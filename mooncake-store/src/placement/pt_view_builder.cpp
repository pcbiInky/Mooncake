#include "placement/pt_view_builder.h"

#include <cstdint>
#include <string_view>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>

#include "random.h"

namespace mooncake {

namespace {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
constexpr uint64_t kDefaultPtSeed = 0x4D4F4F4E43414B45ULL;

uint64_t FreeBytes(const PtSegmentSnapshot& segment) {
    return segment.capacity > segment.used ? segment.capacity - segment.used
                                           : 0;
}

struct ClassTopology {
    std::vector<const PtSegmentSnapshot*> segments;
    std::vector<size_t> segment_host;
    std::vector<std::vector<size_t>> host_segments;
    std::vector<std::string> host_rack_ids;
    std::vector<std::vector<size_t>> domain_hosts;
    std::vector<std::string> domain_ids;
};

struct IncrementPlan {
    std::vector<double> segment_increments;
    std::vector<double> host_increments;
    std::vector<double> domain_increments;
};

struct SlotPlan {
    std::vector<uint64_t> segment_slots;
    std::vector<uint64_t> host_slots;
    std::vector<uint64_t> domain_slots;
};

// ---------------------------------------------------------------------
// CRUSH-style deterministic weighted sampling (straw2 / Efraimidis-
// Spirakis). Stable seed and candidate keys make a complete rebuild
// reproducible for the same topology and quotas. Row construction still
// consumes remaining quotas sequentially, so an arbitrary row cannot be
// recomputed independently without its preceding quota state.
//
// NOTE: this is a *probabilistic* pick. It is only used where the
// caller does not need to guarantee an exact quota on every individual
// draw (see PickRowGroups for the one place that still does).
// ---------------------------------------------------------------------

// Deterministic pseudo-random helpers for stable placement decisions. Unlike
// threadLocalRandomEngine(), these functions are pure: identical seed/key
// inputs always produce identical results across rebuilds.
inline uint64_t deterministicRandomMix(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

inline uint64_t deterministicRandomHash(uint64_t seed, uint64_t key) {
    return deterministicRandomMix(seed ^ deterministicRandomMix(key));
}

inline uint64_t deterministicRandomHashString(uint64_t seed,
                                              std::string_view key) {
    uint64_t hash = deterministicRandomMix(seed);
    for (const unsigned char byte : key) {
        hash = deterministicRandomMix(hash ^ byte);
    }
    return hash;
}

// Efraimidis-Spirakis / straw2-style score. Pick the candidate with the
// greatest score. A non-positive weight is never selected.
inline double deterministicWeightedScore(uint64_t seed, uint64_t stream,
                                         uint64_t key, double weight) {
    if (!(weight > 0.0) || !std::isfinite(weight)) {
        return -std::numeric_limits<double>::infinity();
    }
    const uint64_t hash = deterministicRandomHash(
        deterministicRandomHash(seed, stream), key);
    constexpr double kTwoTo53 = 9007199254740992.0;
    const double uniform =
        static_cast<double>((hash >> 11) + 1) / kTwoTo53;
    return std::log(uniform) / weight;
}

std::optional<std::vector<double>> DistributeWithCaps(
    const std::vector<double>& weights, const std::vector<double>& caps,
    double total) {
    if (weights.size() != caps.size() || total < 0.0) {
        return std::nullopt;
    }
    constexpr double kEpsilon = 1e-12;
    const double total_cap =
        std::accumulate(caps.begin(), caps.end(), 0.0);
    if (total_cap + kEpsilon < total) {
        return std::nullopt;
    }

    std::vector<double> result(weights.size(), 0.0);
    std::vector<bool> active(weights.size(), false);
    for (size_t i = 0; i < weights.size(); ++i) {
        active[i] = weights[i] > kEpsilon && caps[i] > kEpsilon;
    }

    double remaining = total;
    while (remaining > kEpsilon) {
        double active_weight = 0.0;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (active[i]) {
                active_weight += weights[i];
            }
        }
        if (active_weight <= kEpsilon) {
            return std::nullopt;
        }

        bool capped_any = false;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (!active[i]) {
                continue;
            }
            const double proposed = remaining * weights[i] / active_weight;
            if (proposed > caps[i] + kEpsilon) {
                result[i] = caps[i];
                remaining -= caps[i];
                active[i] = false;
                capped_any = true;
            }
        }
        if (capped_any) {
            continue;
        }
        for (size_t i = 0; i < weights.size(); ++i) {
            if (active[i]) {
                result[i] = remaining * weights[i] / active_weight;
            }
        }
        remaining = 0.0;
    }
    return result;
}

std::optional<std::vector<uint64_t>> RoundQuotas(
    const std::vector<double>& weights, uint64_t total,
    const std::vector<uint64_t>& caps) {
    if (weights.size() != caps.size()) {
        return std::nullopt;
    }
    const uint64_t total_cap =
        std::accumulate(caps.begin(), caps.end(), uint64_t{0});
    const double weight_sum =
        std::accumulate(weights.begin(), weights.end(), 0.0);
    if (total_cap < total || weight_sum <= 0.0) {
        return std::nullopt;
    }

    std::vector<uint64_t> quotas(weights.size(), 0);
    std::vector<std::pair<double, size_t>> remainders;
    remainders.reserve(weights.size());
    uint64_t assigned = 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        const double raw = static_cast<double>(total) * weights[i] /
                           weight_sum;
        quotas[i] = std::min<uint64_t>(
            static_cast<uint64_t>(std::floor(raw)), caps[i]);
        assigned += quotas[i];
        remainders.emplace_back(raw - std::floor(raw), i);
    }
    std::stable_sort(remainders.begin(), remainders.end(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.first > rhs.first;
                     });
    while (assigned < total) {
        bool made_progress = false;
        for (const auto& [remainder, i] : remainders) {
            (void)remainder;
            if (quotas[i] >= caps[i]) {
                continue;
            }
            ++quotas[i];
            ++assigned;
            made_progress = true;
            if (assigned == total) {
                break;
            }
        }
        if (!made_progress) {
            return std::nullopt;
        }
    }
    return quotas;
}

std::optional<ClassTopology> BuildClassTopology(
    const std::vector<const PtSegmentSnapshot*>& eligible,
    uint64_t max_inclusive, uint32_t replica_num) {
    ClassTopology topology;

    // A Host belongs to at most one explicit rack. If at least one Segment
    // reports the rack, reuse it for sibling Segments whose rack_id is empty.
    // Conflicting explicit rack IDs are a malformed topology.
    std::unordered_map<std::string, std::string> rack_by_host;
    for (const auto* segment : eligible) {
        auto [it, inserted] =
            rack_by_host.emplace(segment->host_id, segment->rack_id);
        if (!inserted && !segment->rack_id.empty()) {
            if (!it->second.empty() && it->second != segment->rack_id) {
                return std::nullopt;
            }
            it->second = segment->rack_id;
        }
    }

    // Canonicalize the class input before assigning vector indices. This
    // keeps quota rounding and score tie-breaking independent of the order in
    // which SegmentSpaceReports happened to be collected.
    std::vector<const PtSegmentSnapshot*> class_segments;
    class_segments.reserve(eligible.size());
    for (const auto* segment : eligible) {
        if (segment->largest_free >= max_inclusive) {
            class_segments.push_back(segment);
        }
    }
    std::sort(class_segments.begin(), class_segments.end(),
              [](const auto* lhs, const auto* rhs) {
                  if (lhs->host_id != rhs->host_id) {
                      return lhs->host_id < rhs->host_id;
                  }
                  if (lhs->segment_id != rhs->segment_id) {
                      return lhs->segment_id < rhs->segment_id;
                  }
                  return lhs->name < rhs->name;
              });

    std::unordered_map<std::string, size_t> host_index;
    std::unordered_map<std::string, size_t> domain_index;
    for (const auto* segment : class_segments) {
        auto [it, inserted] =
            host_index.emplace(segment->host_id, host_index.size());
        if (inserted) {
            topology.host_segments.emplace_back();
            const std::string& rack_id = rack_by_host.at(segment->host_id);
            const std::string domain_id =
                rack_id.empty() ? "host:" + segment->host_id
                                : "rack:" + rack_id;
            auto [domain_it, domain_inserted] =
                domain_index.emplace(domain_id, domain_index.size());
            if (domain_inserted) {
                topology.domain_hosts.emplace_back();
                topology.domain_ids.push_back(domain_id);
            }
            topology.host_rack_ids.push_back(rack_id);
            topology.domain_hosts[domain_it->second].push_back(it->second);
        }
        const size_t segment_index = topology.segments.size();
        topology.segments.push_back(segment);
        topology.segment_host.push_back(it->second);
        topology.host_segments[it->second].push_back(segment_index);
    }
    if (topology.host_segments.size() < replica_num ||
        topology.domain_hosts.size() < replica_num) {
        return std::nullopt;
    }
    return topology;
}

std::optional<IncrementPlan> PlanIncrements(
    const ClassTopology& topology, const PtBuildConfig& config) {
    IncrementPlan plan;
    plan.segment_increments.resize(topology.segments.size(), 0.0);
    double total_free = 0.0;
    for (size_t i = 0; i < topology.segments.size(); ++i) {
        plan.segment_increments[i] =
            static_cast<double>(FreeBytes(*topology.segments[i]));
        total_free += plan.segment_increments[i];
    }
    if (total_free <= 0.0) {
        return std::nullopt;
    }
    for (auto& increment : plan.segment_increments) {
        increment /= total_free;
    }

    const size_t host_count = topology.host_segments.size();
    const size_t domain_count = topology.domain_hosts.size();
    // Host balance always uses physical Host count. Independently, both a
    // Host and an effective failure domain can contribute at most one slot
    // per PT row.
    const double host_limit =
        std::min(config.host_increment_skew_k /
                     static_cast<double>(host_count),
                 1.0 / static_cast<double>(config.replica_num));
    std::vector<double> raw_host_increments(host_count, 0.0);
    for (size_t i = 0; i < topology.segments.size(); ++i) {
        raw_host_increments[topology.segment_host[i]] +=
            plan.segment_increments[i];
    }

    std::vector<double> raw_domain_increments(domain_count, 0.0);
    std::vector<double> domain_caps(domain_count, 0.0);
    for (size_t d = 0; d < domain_count; ++d) {
        double host_headroom = 0.0;
        for (const size_t h : topology.domain_hosts[d]) {
            raw_domain_increments[d] += raw_host_increments[h];
            if (raw_host_increments[h] > 0.0) {
                host_headroom += host_limit;
            }
        }
        domain_caps[d] =
            std::min(1.0 / static_cast<double>(config.replica_num),
                     host_headroom);
    }
    auto domains =
        DistributeWithCaps(raw_domain_increments, domain_caps, 1.0);
    if (!domains) {
        return std::nullopt;
    }
    plan.domain_increments = std::move(*domains);

    plan.host_increments.assign(host_count, 0.0);
    for (size_t d = 0; d < domain_count; ++d) {
        std::vector<double> weights;
        std::vector<double> caps;
        weights.reserve(topology.domain_hosts[d].size());
        caps.reserve(topology.domain_hosts[d].size());
        for (const size_t h : topology.domain_hosts[d]) {
            weights.push_back(raw_host_increments[h]);
            caps.push_back(raw_host_increments[h] > 0.0 ? host_limit : 0.0);
        }
        auto hosts = DistributeWithCaps(
            weights, caps, plan.domain_increments[d]);
        if (!hosts) {
            return std::nullopt;
        }
        for (size_t i = 0; i < topology.domain_hosts[d].size(); ++i) {
            plan.host_increments[topology.domain_hosts[d][i]] = (*hosts)[i];
        }
    }
    return plan;
}

std::optional<SlotPlan> RoundSlots(const ClassTopology& topology,
                                   const IncrementPlan& increments,
                                   const PtBuildConfig& config) {
    const size_t host_count = topology.host_segments.size();
    const uint64_t total_slots =
        static_cast<uint64_t>(config.pt_count) * config.replica_num;
    const uint64_t host_slot_cap = std::min<uint64_t>(
        config.pt_count,
        static_cast<uint64_t>(std::ceil(
            static_cast<double>(total_slots) *
            config.host_increment_skew_k /
            static_cast<double>(topology.host_segments.size()))));

    SlotPlan plan;
    std::vector<uint64_t> domain_caps(topology.domain_hosts.size(),
                                      config.pt_count);
    auto domain_slots =
        RoundQuotas(increments.domain_increments, total_slots, domain_caps);
    if (!domain_slots) {
        return std::nullopt;
    }
    plan.domain_slots = std::move(*domain_slots);

    plan.host_slots.assign(host_count, 0);
    for (size_t d = 0; d < topology.domain_hosts.size(); ++d) {
        std::vector<double> weights;
        std::vector<uint64_t> caps;
        weights.reserve(topology.domain_hosts[d].size());
        caps.reserve(topology.domain_hosts[d].size());
        for (const size_t h : topology.domain_hosts[d]) {
            weights.push_back(increments.host_increments[h]);
            caps.push_back(host_slot_cap);
        }
        auto slots = RoundQuotas(weights, plan.domain_slots[d], caps);
        if (!slots) {
            return std::nullopt;
        }
        for (size_t i = 0; i < topology.domain_hosts[d].size(); ++i) {
            plan.host_slots[topology.domain_hosts[d][i]] = (*slots)[i];
        }
    }

    // Split each integer Host quota among its Segments independently.
    plan.segment_slots.resize(topology.segments.size(), 0);
    for (size_t h = 0; h < host_count; ++h) {
        double host_segment_total = 0.0;
        for (const size_t i : topology.host_segments[h]) {
            host_segment_total += increments.segment_increments[i];
        }
        if (host_segment_total <= 1e-12) {
            return std::nullopt;
        }
        uint64_t assigned_segment_slots = 0;
        std::vector<std::pair<double, size_t>> segment_remainders;
        segment_remainders.reserve(topology.host_segments[h].size());
        for (const size_t i : topology.host_segments[h]) {
            const double raw = static_cast<double>(plan.host_slots[h]) *
                               increments.segment_increments[i] /
                               host_segment_total;
            plan.segment_slots[i] =
                static_cast<uint64_t>(std::floor(raw));
            assigned_segment_slots += plan.segment_slots[i];
            segment_remainders.emplace_back(
                raw - static_cast<double>(plan.segment_slots[i]), i);
        }
        std::stable_sort(segment_remainders.begin(), segment_remainders.end(),
                         [](const auto& a, const auto& b) {
                             return a.first > b.first;
                         });
        for (size_t r = 0;
             assigned_segment_slots < plan.host_slots[h]; ++r) {
            ++plan.segment_slots[segment_remainders[r].second];
            ++assigned_segment_slots;
        }
    }
    return plan;
}

// ---------------------------------------------------------------------
// Row construction.
//
// Domain level (pick R distinct domains for one row) is the only place
// where a combinatorial "no repeat within this row" constraint exists,
// so it keeps the Havel-Hakimi feasibility guarantee from the original
// implementation: any domain whose remaining quota equals the number of
// rows left *must* be placed now, or a later row becomes infeasible.
// Domains that can still be deferred are chosen among by CRUSH-style
// weighted sampling instead of a flat "largest remaining first" sort --
// this keeps the same worst-case feasibility guarantee while removing
// the artificial bias toward packing high-quota domains into the
// earliest pt_ids.
//
// Host and Segment level each only ever pick *one* item per call, with
// no cross-item constraint. Because a domain's remaining quota always
// equals the sum of its member Hosts' remaining quotas (they are
// decremented together on every pick), and likewise a Host's remaining
// quota always equals the sum of its member Segments' remaining quotas,
// any pick among currently-nonzero candidates is safe: by induction the
// full quota is always exhausted exactly by the final row, regardless of
// which specific candidate is chosen at each step. So no forced/free
// split is needed at these two levels -- a plain CRUSH-style weighted
// draw is both correct and sufficient.
// ---------------------------------------------------------------------

constexpr uint32_t kDomainLevel = 0;
constexpr uint32_t kHostLevel = 1;
constexpr uint32_t kSegmentLevel = 2;

std::optional<std::vector<size_t>> PickRowGroups(
    const std::vector<uint64_t>& remaining_slots, size_t rows_left,
    uint32_t replica_num, uint64_t row_seed,
    const std::vector<std::string>& stable_keys) {
    std::vector<size_t> forced;
    std::vector<size_t> deferrable;
    for (size_t group = 0; group < remaining_slots.size(); ++group) {
        const uint64_t remaining = remaining_slots[group];
        if (remaining == 0) {
            continue;
        }
        if (remaining > rows_left) {
            return std::nullopt;  // infeasible no matter the ordering
        }
        if (remaining == rows_left) {
            forced.push_back(group);
        } else {
            deferrable.push_back(group);
        }
    }
    if (forced.size() > replica_num ||
        forced.size() + deferrable.size() < replica_num) {
        return std::nullopt;
    }

    std::vector<size_t> picked = std::move(forced);
    const size_t need = replica_num - picked.size();
    if (need > 0) {
        std::vector<std::pair<double, size_t>> scored;
        scored.reserve(deferrable.size());
        for (size_t group : deferrable) {
            const uint64_t key =
                deterministicRandomHashString(0, stable_keys[group]);
            const double score = deterministicWeightedScore(
                row_seed, kDomainLevel, key,
                static_cast<double>(remaining_slots[group]));
            scored.emplace_back(score, group);
        }
        std::partial_sort(
            scored.begin(), scored.begin() + need, scored.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.first != rhs.first ? lhs.first > rhs.first
                                              : lhs.second < rhs.second;
            });
        for (size_t i = 0; i < need; ++i) {
            picked.push_back(scored[i].second);
        }
    }
    std::sort(picked.begin(), picked.end());
    return picked;
}

std::optional<std::vector<PtEntry>> BuildRows(
    const ClassTopology& topology, const SlotPlan& slots,
    const PtBuildConfig& config, uint64_t policy_seed) {
    const uint64_t total_slots =
        static_cast<uint64_t>(config.pt_count) * config.replica_num;
    const uint64_t planned_slots = std::accumulate(
        slots.segment_slots.begin(), slots.segment_slots.end(), uint64_t{0});
    if (planned_slots != total_slots) {
        return std::nullopt;
    }

    std::vector<uint64_t> domain_remaining(slots.domain_slots.begin(),
                                           slots.domain_slots.end());
    std::vector<uint64_t> host_remaining(slots.host_slots.begin(),
                                         slots.host_slots.end());
    std::vector<uint64_t> segment_remaining(slots.segment_slots.begin(),
                                            slots.segment_slots.end());

    // Stable per-host key for hashing: any Segment on the Host carries its
    // host_id, so reuse the first one. Segment-level keys use segment_id
    // directly. Using stable IDs (not vector indices) means the selection
    // pattern for a given physical Host/Segment is comparable across
    // rebuilds, not an artifact of topology-vector ordering.
    std::vector<std::string> host_keys(topology.host_segments.size());
    for (size_t h = 0; h < topology.host_segments.size(); ++h) {
        host_keys[h] =
            topology.segments[topology.host_segments[h].front()]->host_id;
    }

    std::vector<PtEntry> entries;
    entries.reserve(config.pt_count);
    for (uint32_t pt_id = 0; pt_id < config.pt_count; ++pt_id) {
        const size_t rows_left = static_cast<size_t>(config.pt_count) - pt_id;
        const uint64_t row_seed =
            deterministicRandomHash(policy_seed, pt_id);

        auto picked_domains = PickRowGroups(domain_remaining, rows_left,
                                            config.replica_num, row_seed,
                                            topology.domain_ids);
        if (!picked_domains) {
            return std::nullopt;
        }

        PtEntry entry;
        entry.pt_id = pt_id;
        entry.replicas.reserve(config.replica_num);
        for (const size_t d : *picked_domains) {
            size_t selected_host = SIZE_MAX;
            double best_host_score = -std::numeric_limits<double>::infinity();
            for (const size_t h : topology.domain_hosts[d]) {
                if (host_remaining[h] == 0) {
                    continue;
                }
                const uint64_t key =
                    deterministicRandomHashString(0, host_keys[h]);
                const double score = deterministicWeightedScore(
                    row_seed, kHostLevel, key,
                    static_cast<double>(host_remaining[h]));
                if (score > best_host_score ||
                    (score == best_host_score && h < selected_host)) {
                    best_host_score = score;
                    selected_host = h;
                }
            }
            if (selected_host == SIZE_MAX) {
                return std::nullopt;
            }

            size_t selected_segment = SIZE_MAX;
            double best_segment_score =
                -std::numeric_limits<double>::infinity();
            for (const size_t i : topology.host_segments[selected_host]) {
                if (segment_remaining[i] == 0) {
                    continue;
                }
                const UUID& segment_id = topology.segments[i]->segment_id;
                const uint64_t key = deterministicRandomHash(
                    segment_id.first, segment_id.second);
                const double score = deterministicWeightedScore(
                    row_seed, kSegmentLevel, key,
                    static_cast<double>(segment_remaining[i]));
                if (score > best_segment_score ||
                    (score == best_segment_score && i < selected_segment)) {
                    best_segment_score = score;
                    selected_segment = i;
                }
            }
            if (selected_segment == SIZE_MAX) {
                return std::nullopt;
            }

            --domain_remaining[d];
            --host_remaining[selected_host];
            --segment_remaining[selected_segment];

            const PtSegmentSnapshot* segment =
                topology.segments[selected_segment];
            entry.replicas.push_back(PtTarget{
                segment->segment_id, segment->name, segment->host_id,
                topology.host_rack_ids[selected_host],
                topology.domain_ids[d]});
        }
        entries.push_back(std::move(entry));
    }

    // Every remaining-count array must be exactly drained. If this ever
    // fails it means the invariant argued above (domain quota == sum of
    // member Host quotas == sum of member Segment quotas, decremented in
    // lockstep) was violated by a bug upstream in SlotPlan, not by the
    // sampling choices made here.
    const bool fully_drained =
        std::all_of(domain_remaining.begin(), domain_remaining.end(),
                   [](uint64_t r) { return r == 0; }) &&
        std::all_of(host_remaining.begin(), host_remaining.end(),
                   [](uint64_t r) { return r == 0; }) &&
        std::all_of(segment_remaining.begin(), segment_remaining.end(),
                   [](uint64_t r) { return r == 0; });
    if (!fully_drained) {
        return std::nullopt;
    }
    return entries;
}

}  // namespace

const std::vector<uint64_t>& PtViewBuilder::DefaultSizeClasses() {
    static const std::vector<uint64_t> kClasses = {
        2 * kMiB, 4 * kMiB, 32 * kMiB, 256 * kMiB, kGiB, 8 * kGiB};
    return kClasses;
}

std::optional<PtView> PtViewBuilder::Build(
    const std::vector<PtSegmentSnapshot>& segments,
    const PtBuildConfig& config, PtBuildStats* stats) {
    const auto start = std::chrono::steady_clock::now();

    if (stats) {
        stats->total_segments = segments.size();
        stats->topology_incomplete = 0;
        stats->eligible_segments = 0;
        stats->policies_built = 0;
    }

    if (config.pt_count == 0 || config.replica_num == 0 ||
        !std::isfinite(config.host_increment_skew_k) ||
        config.host_increment_skew_k < 1.0) {
        return std::nullopt;
    }

    // 1. A stable Host identity is required for k/N balance and same-Host
    // exclusion. rack_id is optional and falls back to Host best-effort.
    // Segments without bin-precise capacity reports are excluded as well.
    std::vector<const PtSegmentSnapshot*> eligible;
    for (const auto& segment : segments) {
        if (segment.host_id.empty()) {
            if (stats) {
                ++stats->topology_incomplete;
            }
            continue;
        }
        if (segment.unknown_capacity) {
            if (stats) {
                ++stats->topology_incomplete;
            }
            continue;
        }
        eligible.push_back(&segment);
    }
    if (stats) {
        stats->eligible_segments = eligible.size();
    }

    // 2. The first version uses fixed size classes. Keeping the policy set
    // internal avoids an external configuration and compatibility surface.
    const auto& class_bounds = DefaultSizeClasses();

    // 3. One policy per size class. Views whose policies all fail are not
    // published; partially failed classes are skipped (RFC 0005 §9.6 keeps
    // the old view only when *no* feasible table exists).
    PtView view;
    view.epoch = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    view.created_at_ns = view.epoch;
    view.pt_count = config.pt_count;
    view.configured_replica_num = config.replica_num;
    view.seed = config.seed ? config.seed : kDefaultPtSeed;

    uint64_t min_exclusive = 0;
    for (const uint64_t max_inclusive : class_bounds) {
        PtPolicyView policy;
        if (BuildPolicy(eligible, min_exclusive, max_inclusive, config,
                        &policy)) {
            view.policies.push_back(std::move(policy));
            if (stats) {
                ++stats->policies_built;
            }
        }
        min_exclusive = max_inclusive;
    }
    if (view.policies.empty()) {
        return std::nullopt;
    }

    if (stats) {
        stats->build_duration_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
    }
    return view;
}

bool PtViewBuilder::BuildPolicy(
    const std::vector<const PtSegmentSnapshot*>& eligible,
    uint64_t min_exclusive, uint64_t max_inclusive,
    const PtBuildConfig& config, PtPolicyView* policy_out) {
    auto topology =
        BuildClassTopology(eligible, max_inclusive, config.replica_num);
    if (!topology) {
        return false;
    }
    auto increments = PlanIncrements(*topology, config);
    if (!increments) {
        return false;
    }

    auto slots = RoundSlots(*topology, *increments, config);
    if (!slots) {
        return false;
    }

    // Salt the seed with this policy's upper bound so that different size
    // classes never draw from identical (row_seed, level_tag, key) tuples
    // even when their eligible-topology subsets overlap.
    const uint64_t view_seed = config.seed ? config.seed : kDefaultPtSeed;
    const uint64_t policy_seed =
        deterministicRandomHash(view_seed, max_inclusive);
    auto rows = BuildRows(*topology, *slots, config, policy_seed);
    if (!rows) {
        return false;
    }

    policy_out->min_aligned_request_size_exclusive = min_exclusive;
    policy_out->max_aligned_request_size_inclusive = max_inclusive;
    policy_out->entries = std::move(*rows);
    return true;
}

}  // namespace mooncake
