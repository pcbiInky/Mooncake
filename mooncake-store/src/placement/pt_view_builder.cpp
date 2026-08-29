#include "placement/pt_view_builder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <utility>

#include "random.h"

namespace mooncake {

namespace {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

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

    std::unordered_map<std::string, size_t> host_index;
    std::unordered_map<std::string, size_t> domain_index;
    for (const auto* segment : eligible) {
        if (segment->largest_free < max_inclusive) {
            continue;
        }
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

std::optional<std::vector<size_t>> PickGroupsForRow(
    const std::vector<size_t>& remaining_slots, size_t rows_left,
    uint32_t replica_num) {
    std::vector<size_t> candidates;
    candidates.reserve(remaining_slots.size());
    for (size_t group = 0; group < remaining_slots.size(); ++group) {
        if (remaining_slots[group] == 0) {
            continue;
        }
        if (remaining_slots[group] > rows_left) {
            return std::nullopt;
        }
        candidates.push_back(group);
    }
    if (candidates.size() < replica_num) {
        return std::nullopt;
    }

    // Havel-Hakimi style construction: consume the groups with the largest
    // remaining quotas first. This deterministically preserves feasibility
    // for the following rows and never selects one group twice in a row.
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&remaining_slots](size_t lhs, size_t rhs) {
                         return remaining_slots[lhs] > remaining_slots[rhs];
                     });
    std::vector<size_t> picked_groups(candidates.begin(),
                                      candidates.begin() + replica_num);
    return picked_groups;
}

std::optional<std::vector<PtEntry>> BuildRows(
    const ClassTopology& topology, const SlotPlan& slots,
    const PtBuildConfig& config) {
    const uint64_t total_slots =
        static_cast<uint64_t>(config.pt_count) * config.replica_num;
    const uint64_t planned_slots = std::accumulate(
        slots.segment_slots.begin(), slots.segment_slots.end(), uint64_t{0});
    if (planned_slots != total_slots) {
        return std::nullopt;
    }

    std::vector<std::vector<size_t>> host_queues(
        topology.host_segments.size());
    for (size_t i = 0; i < topology.segments.size(); ++i) {
        host_queues[topology.segment_host[i]].insert(
            host_queues[topology.segment_host[i]].end(),
            static_cast<size_t>(slots.segment_slots[i]), i);
    }
    for (size_t h = 0; h < host_queues.size(); ++h) {
        randomShuffle(host_queues[h].begin(), host_queues[h].end());
        if (host_queues[h].size() != slots.host_slots[h] ||
            host_queues[h].size() > config.pt_count) {
            return std::nullopt;
        }
    }

    std::vector<size_t> domain_remaining(slots.domain_slots.begin(),
                                         slots.domain_slots.end());
    for (size_t d = 0; d < topology.domain_hosts.size(); ++d) {
        size_t host_slots = 0;
        for (const size_t h : topology.domain_hosts[d]) {
            host_slots += host_queues[h].size();
        }
        if (host_slots != domain_remaining[d] ||
            domain_remaining[d] > config.pt_count) {
            return std::nullopt;
        }
    }

    std::vector<PtEntry> entries;
    entries.reserve(config.pt_count);
    for (uint32_t pt_id = 0; pt_id < config.pt_count; ++pt_id) {
        const size_t rows_left = static_cast<size_t>(config.pt_count) - pt_id;
        auto picked_domains = PickGroupsForRow(
            domain_remaining, rows_left, config.replica_num);
        if (!picked_domains) {
            return std::nullopt;
        }
        PtEntry entry;
        entry.pt_id = pt_id;
        entry.replicas.reserve(config.replica_num);
        for (const size_t d : *picked_domains) {
            size_t selected_host = SIZE_MAX;
            for (const size_t candidate : topology.domain_hosts[d]) {
                if (host_queues[candidate].empty()) {
                    continue;
                }
                if (selected_host == SIZE_MAX ||
                    host_queues[candidate].size() >
                        host_queues[selected_host].size()) {
                    selected_host = candidate;
                }
            }
            if (selected_host == SIZE_MAX) {
                return std::nullopt;
            }
            const size_t segment_index = host_queues[selected_host].back();
            host_queues[selected_host].pop_back();
            --domain_remaining[d];
            const PtSegmentSnapshot* segment = topology.segments[segment_index];
            entry.replicas.push_back(PtTarget{
                segment->segment_id, segment->name, segment->host_id,
                topology.host_rack_ids[selected_host],
                topology.domain_ids[d]});
        }
        entries.push_back(std::move(entry));
    }
    if (std::any_of(host_queues.begin(), host_queues.end(),
                    [](const auto& queue) { return !queue.empty(); })) {
        return std::nullopt;
    }
    if (std::any_of(domain_remaining.begin(), domain_remaining.end(),
                    [](size_t remaining) { return remaining != 0; })) {
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
    view.seed = config.seed ? config.seed : view.epoch;

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

    auto rows = BuildRows(*topology, *slots, config);
    if (!rows) {
        return false;
    }

    policy_out->min_aligned_request_size_exclusive = min_exclusive;
    policy_out->max_aligned_request_size_inclusive = max_inclusive;
    policy_out->entries = std::move(*rows);
    return true;
}

}  // namespace mooncake
