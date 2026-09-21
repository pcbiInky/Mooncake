#include "placement/pt_view_builder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>

#include "random.h"

namespace mooncake {

namespace {

constexpr uint64_t kDefaultPtSeed = 0x4D4F4F4E43414B45ULL;

uint64_t FreeBytes(const PtSegmentSnapshot& segment) {
    return segment.capacity > segment.used ? segment.capacity - segment.used
                                           : 0;
}

double SegmentWeight(const PtSegmentSnapshot& segment, PtSegmentWeightMode mode,
                     double target_utilization) {
    switch (mode) {
        case PtSegmentWeightMode::TARGET_HEADROOM: {
            const double target_bytes =
                static_cast<double>(segment.capacity) * target_utilization;
            return std::max(
                0.0, target_bytes - static_cast<double>(std::min(
                                        segment.used, segment.capacity)));
        }
        case PtSegmentWeightMode::CAPACITY:
            return static_cast<double>(segment.capacity);
    }
    return 0.0;
}

struct PlacementTopology {
    std::vector<const PtSegmentSnapshot*> segments;
    std::vector<size_t> segment_host;
    std::vector<std::vector<size_t>> host_segments;
    std::vector<std::string> host_rack_ids;
    std::vector<std::vector<size_t>> domain_hosts;
    std::vector<std::string> domain_ids;
};

struct SlotPlan {
    std::vector<uint64_t> segment_slots;
    std::vector<uint64_t> host_slots;
    std::vector<uint64_t> domain_slots;
};

// Deterministic weighted sampling. Rows consume quotas sequentially, so an
// individual row cannot be recomputed without the preceding quota state.

// Fixed salt for stable domain and host keys.
constexpr uint64_t kStableKeySalt = 0;

inline uint64_t StableKey(const std::string& value) {
    return deterministicRandomHashString(kStableKeySalt, value);
}

// Bounded proportional apportionment.
// Produces integral quotas with 0 <= quota_i <= cap_i and sum(quota) == total.
// Invalid weights receive zero capacity; floor plus largest remainder preserves
// the exact total after the breakpoint scan.

std::optional<std::vector<uint64_t>> Apportion(
    const std::vector<double>& weights, const std::vector<uint64_t>& caps,
    uint64_t total) {
    if (weights.size() != caps.size()) {
        return std::nullopt;
    }
    const size_t count = weights.size();

    // Effective caps: cap'_i = (w_i > 0) ? cap_i : 0.
    std::vector<uint64_t> effective_caps(count, 0);
    std::vector<double> positive_weights(count, 0.0);
    uint64_t total_effective_cap = 0;
    for (size_t i = 0; i < count; ++i) {
        if (weights[i] > 0.0 && std::isfinite(weights[i])) {
            positive_weights[i] = weights[i];
            effective_caps[i] = caps[i];
            total_effective_cap += caps[i];
        }
    }
    if (total_effective_cap < total) {
        return std::nullopt;
    }

    std::vector<uint64_t> quotas(count, 0);
    if (total == 0) {
        return quotas;
    }
    if (total_effective_cap == total) {
        // Only one feasible answer: every effective cap is saturated.
        for (size_t i = 0; i < count; ++i) {
            quotas[i] = effective_caps[i];
        }
        return quotas;
    }

    // Saturate candidates in cap/weight order; suffix sums avoid subtracting
    // large accumulated weights.
    std::vector<size_t> order;
    order.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        if (effective_caps[i] > 0) {
            order.push_back(i);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) {
        return static_cast<double>(effective_caps[lhs]) /
                   positive_weights[lhs] <
               static_cast<double>(effective_caps[rhs]) / positive_weights[rhs];
    });

    std::vector<double> suffix_weight(order.size() + 1, 0.0);
    for (size_t rank = order.size(); rank > 0; --rank) {
        suffix_weight[rank - 1] =
            suffix_weight[rank] + positive_weights[order[rank - 1]];
    }

    double remaining_budget = static_cast<double>(total);
    double lambda_star = 0.0;
    size_t rank = 0;
    const size_t eligible_count = order.size();
    for (; rank < eligible_count; ++rank) {
        const double tail_weight = suffix_weight[rank];
        if (!(tail_weight > 0.0)) {
            return std::nullopt;
        }
        const size_t candidate = order[rank];
        if (remaining_budget / tail_weight >
            static_cast<double>(effective_caps[candidate]) /
                positive_weights[candidate]) {
            remaining_budget -= static_cast<double>(effective_caps[candidate]);
            continue;
        }
        lambda_star = remaining_budget / tail_weight;
        break;
    }

    std::vector<double> raw_slots(count, 0.0);
    if (rank == eligible_count) {
        // Valid inputs cannot reach this non-finite branch.
        return std::nullopt;
    }
    for (size_t r = 0; r < rank; ++r) {
        raw_slots[order[r]] = static_cast<double>(effective_caps[order[r]]);
    }
    for (size_t r = rank; r < eligible_count; ++r) {
        const size_t candidate = order[r];
        raw_slots[candidate] =
            std::min(lambda_star * positive_weights[candidate],
                     static_cast<double>(effective_caps[candidate]));
    }

    // Floor then distribute the residue by largest remainder.
    std::vector<std::pair<double, size_t>> remainders;
    remainders.reserve(count);
    uint64_t assigned = 0;
    for (size_t i = 0; i < count; ++i) {
        const double raw = raw_slots[i];
        const double floored = std::floor(std::max(0.0, raw));
        quotas[i] = std::min<uint64_t>(static_cast<uint64_t>(floored),
                                       effective_caps[i]);
        assigned += quotas[i];
        if (effective_caps[i] > quotas[i]) {
            remainders.emplace_back(raw - floored, i);
        }
    }
    if (assigned > total) {
        return std::nullopt;
    }
    std::stable_sort(
        remainders.begin(), remainders.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    while (assigned < total) {
        bool made_progress = false;
        for (const auto& [remainder, i] : remainders) {
            (void)remainder;
            if (quotas[i] >= effective_caps[i]) {
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
            return std::nullopt;  // Unreachable with valid inputs.
        }
    }
    if (assigned != total) {
        return std::nullopt;
    }
    return quotas;
}

std::optional<PlacementTopology> BuildTopology(
    const std::vector<const PtSegmentSnapshot*>& eligible,
    uint32_t replica_num) {
    PlacementTopology topology;

    // Propagate each host's explicit rack ID; reject conflicting IDs.
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

    // Canonicalize usable segments so placement is independent of input order.
    std::vector<const PtSegmentSnapshot*> weighted_segments;
    weighted_segments.reserve(eligible.size());
    for (const auto* segment : eligible) {
        if (FreeBytes(*segment) > 0) {
            weighted_segments.push_back(segment);
        }
    }
    std::sort(weighted_segments.begin(), weighted_segments.end(),
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
    for (const auto* segment : weighted_segments) {
        auto [it, inserted] =
            host_index.emplace(segment->host_id, host_index.size());
        if (inserted) {
            topology.host_segments.emplace_back();
            const std::string& rack_id = rack_by_host.at(segment->host_id);
            const std::string domain_id = rack_id.empty()
                                              ? "host:" + segment->host_id
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

// Allocate slots from domain to host to segment using the selected
// segment weight. Host cap: min(P, ceil(P*R*k/N)); domain cap: min(P,
// positive host caps).

std::optional<SlotPlan> ComputeSlotPlanWithMode(
    const PlacementTopology& topology, const PtBuildConfig& config,
    PtSegmentWeightMode weight_mode, double target_utilization) {
    const size_t host_count = topology.host_segments.size();
    const size_t domain_count = topology.domain_hosts.size();
    const size_t segment_count = topology.segments.size();
    const uint64_t total_slots =
        static_cast<uint64_t>(config.pt_count) * config.replica_num;

    const uint64_t host_slot_cap = std::min<uint64_t>(
        config.pt_count,
        static_cast<uint64_t>(std::ceil(static_cast<double>(total_slots) *
                                        config.host_increment_skew_k /
                                        static_cast<double>(host_count))));

    SlotPlan plan;
    plan.segment_slots.assign(segment_count, 0);
    plan.host_slots.assign(host_count, 0);
    plan.domain_slots.assign(domain_count, 0);

    // Segment weights are aggregated bottom-up so steady-state capacity
    // weighting also makes each Host receive traffic proportional to its
    // provisioned capacity.
    std::vector<double> segment_weights(segment_count, 0.0);
    std::vector<double> host_weights(host_count, 0.0);
    std::vector<double> domain_weights(domain_count, 0.0);
    for (size_t i = 0; i < segment_count; ++i) {
        segment_weights[i] = SegmentWeight(
            *topology.segments[i], weight_mode, target_utilization);
        host_weights[topology.segment_host[i]] += segment_weights[i];
    }
    for (size_t d = 0; d < domain_count; ++d) {
        for (const size_t h : topology.domain_hosts[d]) {
            domain_weights[d] += host_weights[h];
        }
    }

    // Level 1: domain quotas with aggregated Host-cap headroom.
    std::vector<uint64_t> domain_caps(domain_count, 0);
    for (size_t d = 0; d < domain_count; ++d) {
        uint64_t headroom = 0;
        for (const size_t h : topology.domain_hosts[d]) {
            if (host_weights[h] > 0.0) {
                headroom += host_slot_cap;
            }
        }
        domain_caps[d] = std::min<uint64_t>(config.pt_count, headroom);
    }
    auto domain_slots = Apportion(domain_weights, domain_caps, total_slots);
    if (!domain_slots) {
        return std::nullopt;
    }
    plan.domain_slots = std::move(*domain_slots);

    // Level 2: Host quotas within each domain.
    for (size_t d = 0; d < domain_count; ++d) {
        if (plan.domain_slots[d] == 0) {
            continue;
        }
        std::vector<double> weights;
        std::vector<uint64_t> caps;
        weights.reserve(topology.domain_hosts[d].size());
        caps.reserve(topology.domain_hosts[d].size());
        for (const size_t h : topology.domain_hosts[d]) {
            weights.push_back(host_weights[h]);
            caps.push_back(host_weights[h] > 0.0 ? host_slot_cap : uint64_t{0});
        }
        auto slots = Apportion(weights, caps, plan.domain_slots[d]);
        if (!slots) {
            return std::nullopt;
        }
        for (size_t i = 0; i < topology.domain_hosts[d].size(); ++i) {
            plan.host_slots[topology.domain_hosts[d][i]] = (*slots)[i];
        }
    }

    // Level 3: Segment quotas within each Host.
    for (size_t h = 0; h < host_count; ++h) {
        if (plan.host_slots[h] == 0) {
            continue;
        }
        const std::vector<size_t>& members = topology.host_segments[h];
        std::vector<double> weights;
        std::vector<uint64_t> caps;
        weights.reserve(members.size());
        caps.reserve(members.size());
        for (const size_t i : members) {
            weights.push_back(segment_weights[i]);
            caps.push_back(config.pt_count);  // Per-row exclusivity bound.
        }
        auto slots = Apportion(weights, caps, plan.host_slots[h]);
        if (!slots) {
            return std::nullopt;
        }
        for (size_t i = 0; i < members.size(); ++i) {
            plan.segment_slots[members[i]] = (*slots)[i];
        }
    }
    return plan;
}

std::optional<SlotPlan> ComputeSlotPlan(
    const PlacementTopology& topology, const PtBuildConfig& config,
    bool* used_full_target_fallback) {
    if (used_full_target_fallback) {
        *used_full_target_fallback = false;
    }
    auto plan = ComputeSlotPlanWithMode(
        topology, config, config.segment_weight_mode,
        config.target_utilization);
    if (plan ||
        config.segment_weight_mode != PtSegmentWeightMode::TARGET_HEADROOM) {
        return plan;
    }

    // Strict target-headroom weights may leave fewer positive Hosts or failure
    // domains than replica_num. target=1.0 is the same formula with all actual
    // free bytes as headroom, preserving availability without a second mode.
    if (used_full_target_fallback) {
        *used_full_target_fallback = true;
    }
    return ComputeSlotPlanWithMode(
        topology, config, PtSegmentWeightMode::TARGET_HEADROOM, 1.0);
}

// Build rows while draining exact quotas. Domains with quota equal to the
// remaining row count are forced; other domains use deterministic sampling.
// Parent and child quotas are decremented together.

constexpr uint32_t kDomainLevel = 0;
constexpr uint32_t kHostLevel = 1;
constexpr uint32_t kSegmentLevel = 2;

// Select distinct domains using precomputed keys and caller-owned scratch.
bool PickRowGroups(const std::vector<uint64_t>& remaining_slots,
                   size_t rows_left, uint32_t replica_num,
                   uint64_t premixed_domain_seed,
                   const std::vector<uint64_t>& stable_keys,
                   std::vector<size_t>& deferrable_scratch,
                   std::vector<std::pair<double, size_t>>& top_scratch,
                   std::vector<size_t>& picked_out) {
    deferrable_scratch.clear();
    size_t forced_count = 0;
    for (size_t group = 0; group < remaining_slots.size(); ++group) {
        const uint64_t remaining = remaining_slots[group];
        if (remaining == 0) {
            continue;
        }
        if (remaining > rows_left) {
            return false;  // infeasible no matter the ordering
        }
        if (remaining == rows_left) {
            ++forced_count;
        } else {
            deferrable_scratch.push_back(group);
        }
    }
    if (forced_count > replica_num ||
        forced_count + deferrable_scratch.size() < replica_num) {
        return false;
    }

    picked_out.clear();
    const size_t need = replica_num - forced_count;
    if (need > 0) {
        // Select the highest-scoring deferred domains in one pass.
        const auto better = [](const std::pair<double, size_t>& lhs,
                               const std::pair<double, size_t>& rhs) {
            return lhs.first != rhs.first ? lhs.first > rhs.first
                                          : lhs.second < rhs.second;
        };
        top_scratch.clear();
        for (const size_t group : deferrable_scratch) {
            const std::pair<double, size_t> candidate(
                deterministicWeightedScorePremixed(
                    premixed_domain_seed, stable_keys[group],
                    static_cast<double>(remaining_slots[group])),
                group);
            if (top_scratch.size() < need ||
                better(candidate, top_scratch.back())) {
                if (top_scratch.size() == need) {
                    top_scratch.pop_back();
                }
                auto position = top_scratch.begin();
                while (position != top_scratch.end() &&
                       better(*position, candidate)) {
                    ++position;
                }
                top_scratch.insert(position, candidate);
            }
        }
        for (const auto& [score, group] : top_scratch) {
            (void)score;
            picked_out.push_back(group);
        }
    }
    for (size_t group = 0; group < remaining_slots.size(); ++group) {
        if (remaining_slots[group] == rows_left) {
            picked_out.push_back(group);
        }
    }
    std::sort(picked_out.begin(), picked_out.end());
    return true;
}

std::optional<std::vector<PtEntry>> BuildRows(const PlacementTopology& topology,
                                              const SlotPlan& slots,
                                              const PtBuildConfig& config,
                                              uint64_t view_seed) {
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

    // Precompute stable keys once per view.
    std::vector<uint64_t> domain_keys(topology.domain_ids.size());
    for (size_t d = 0; d < topology.domain_ids.size(); ++d) {
        domain_keys[d] = StableKey(topology.domain_ids[d]);
    }
    // Stable physical IDs keep selection independent of vector indices.
    std::vector<uint64_t> host_keys(topology.host_segments.size());
    for (size_t h = 0; h < topology.host_segments.size(); ++h) {
        host_keys[h] = StableKey(
            topology.segments[topology.host_segments[h].front()]->host_id);
    }
    std::vector<uint64_t> segment_keys(topology.segments.size());
    for (size_t i = 0; i < topology.segments.size(); ++i) {
        const UUID& segment_id = topology.segments[i]->segment_id;
        segment_keys[i] =
            deterministicRandomHash(segment_id.first, segment_id.second);
    }

    // Scratch reused across rows: no steady-state per-row allocation.
    std::vector<size_t> deferrable_scratch;
    deferrable_scratch.reserve(topology.domain_ids.size());
    std::vector<std::pair<double, size_t>> top_scratch;
    top_scratch.reserve(config.replica_num);
    std::vector<size_t> picked_domains;
    picked_domains.reserve(config.replica_num);

    std::vector<PtEntry> entries;
    entries.reserve(config.pt_count);
    for (uint32_t pt_id = 0; pt_id < config.pt_count; ++pt_id) {
        const size_t rows_left = static_cast<size_t>(config.pt_count) - pt_id;
        const uint64_t row_seed = deterministicRandomHash(view_seed, pt_id);
        // Premix each row/level seed once.
        const uint64_t domain_level_seed =
            deterministicRandomHash(row_seed, kDomainLevel);
        const uint64_t host_level_seed =
            deterministicRandomHash(row_seed, kHostLevel);
        const uint64_t segment_level_seed =
            deterministicRandomHash(row_seed, kSegmentLevel);

        if (!PickRowGroups(domain_remaining, rows_left, config.replica_num,
                           domain_level_seed, domain_keys, deferrable_scratch,
                           top_scratch, picked_domains)) {
            return std::nullopt;
        }

        PtEntry entry;
        entry.pt_id = pt_id;
        entry.replicas.reserve(config.replica_num);
        for (const size_t d : picked_domains) {
            size_t selected_host = SIZE_MAX;
            double best_host_score = -std::numeric_limits<double>::infinity();
            for (const size_t h : topology.domain_hosts[d]) {
                if (host_remaining[h] == 0) {
                    continue;
                }
                const double score = deterministicWeightedScorePremixed(
                    host_level_seed, host_keys[h],
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
                const double score = deterministicWeightedScorePremixed(
                    segment_level_seed, segment_keys[i],
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
                topology.host_rack_ids[selected_host], topology.domain_ids[d]});
        }
        entries.push_back(std::move(entry));
    }

    // Every planned quota must be drained exactly.
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

std::optional<PtView> PtViewBuilder::Build(
    const std::vector<PtSegmentSnapshot>& segments, const PtBuildConfig& config,
    PtBuildStats* stats) {
    const auto start = std::chrono::steady_clock::now();

    if (stats) {
        stats->total_segments = segments.size();
        stats->topology_incomplete = 0;
        stats->eligible_segments = 0;
        stats->used_full_target_fallback = false;
        stats->build_duration_ns = 0;
    }

    if (config.pt_count == 0 || config.replica_num == 0 ||
        !std::isfinite(config.host_increment_skew_k) ||
        config.host_increment_skew_k < 1.0 ||
        !std::isfinite(config.target_utilization) ||
        config.target_utilization <= 0.0 || config.target_utilization > 1.0) {
        return std::nullopt;
    }

    // Require a stable host ID and precise capacity; rack ID is optional.
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

    auto topology = BuildTopology(eligible, config.replica_num);
    if (!topology) {
        return std::nullopt;
    }
    bool used_full_target_fallback = false;
    auto slots = ComputeSlotPlan(*topology, config, &used_full_target_fallback);
    if (stats) {
        stats->used_full_target_fallback = used_full_target_fallback;
    }
    if (!slots) {
        return std::nullopt;
    }

    PtView view;
    view.epoch = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    view.created_at_ns = view.epoch;
    view.pt_count = config.pt_count;
    view.configured_replica_num = config.replica_num;
    view.seed = config.seed ? config.seed : kDefaultPtSeed;

    auto rows = BuildRows(*topology, *slots, config, view.seed);
    if (!rows) {
        return std::nullopt;
    }
    view.entries = std::move(*rows);

    if (stats) {
        stats->build_duration_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start)
                .count());
    }
    return view;
}

}  // namespace mooncake
