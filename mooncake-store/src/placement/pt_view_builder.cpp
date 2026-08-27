#include "placement/pt_view_builder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
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
};

struct IncrementPlan {
    std::vector<double> segment_increments;
    std::vector<double> host_increments;
};

struct SlotPlan {
    std::vector<uint64_t> segment_slots;
    std::vector<uint64_t> host_slots;
};

std::optional<ClassTopology> BuildClassTopology(
    const std::vector<const PtSegmentSnapshot*>& eligible,
    uint64_t max_inclusive, uint32_t replica_num) {
    ClassTopology topology;
    std::unordered_map<std::string, size_t> host_index;
    for (const auto* segment : eligible) {
        if (segment->largest_free < max_inclusive) {
            continue;
        }
        auto [it, inserted] =
            host_index.emplace(segment->host_id, host_index.size());
        if (inserted) {
            topology.host_segments.emplace_back();
        }
        const size_t segment_index = topology.segments.size();
        topology.segments.push_back(segment);
        topology.segment_host.push_back(it->second);
        topology.host_segments[it->second].push_back(segment_index);
    }
    if (topology.host_segments.size() < replica_num) {
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
    // A host receives at most W/N*k of the planned increment. The 1/R cap
    // is the structural limit imposed by one failure domain per PT row.
    const double limit_per_host =
        std::min(config.host_increment_skew_k /
                     static_cast<double>(host_count),
                 1.0 / static_cast<double>(config.replica_num));
    plan.host_increments.resize(host_count, 0.0);
    for (size_t i = 0; i < topology.segments.size(); ++i) {
        plan.host_increments[topology.segment_host[i]] +=
            plan.segment_increments[i];
    }

    // Clamp W/N*k and redistribute the excess until every host is below
    // both the skew limit and the one-slot-per-row feasibility limit.
    for (size_t round = 0; round <= host_count; ++round) {
        double excess = 0.0;
        double headroom_total = 0.0;
        for (double& host_increment : plan.host_increments) {
            if (host_increment > limit_per_host) {
                excess += host_increment - limit_per_host;
                host_increment = limit_per_host;
            } else {
                headroom_total += limit_per_host - host_increment;
            }
        }
        if (excess <= 1e-12 || headroom_total <= 1e-12) {
            break;
        }
        for (double& host_increment : plan.host_increments) {
            if (host_increment < limit_per_host) {
                const double share =
                    (limit_per_host - host_increment) / headroom_total;
                host_increment += excess * share;
            }
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
    const double host_weight_sum = std::accumulate(
        increments.host_increments.begin(), increments.host_increments.end(),
        0.0);
    if (host_weight_sum <= 0.0) {
        return std::nullopt;
    }

    // Round host quotas first so Segment-level remainders cannot push their
    // owning Host beyond the k/N or one-slot-per-row cap.
    const uint64_t skew_slot_cap = static_cast<uint64_t>(std::ceil(
        static_cast<double>(total_slots) * config.host_increment_skew_k /
        static_cast<double>(host_count)));
    const uint64_t host_slot_cap =
        std::min<uint64_t>(config.pt_count, skew_slot_cap);
    SlotPlan plan;
    plan.host_slots.resize(host_count, 0);
    uint64_t assigned_host_slots = 0;
    std::vector<std::pair<double, size_t>> host_remainders;
    host_remainders.reserve(host_count);
    for (size_t h = 0; h < host_count; ++h) {
        const double raw = static_cast<double>(total_slots) *
                           increments.host_increments[h] / host_weight_sum;
        plan.host_slots[h] =
            std::min<uint64_t>(static_cast<uint64_t>(std::floor(raw)),
                               host_slot_cap);
        assigned_host_slots += plan.host_slots[h];
        host_remainders.emplace_back(
            raw - static_cast<double>(std::floor(raw)), h);
    }
    std::stable_sort(host_remainders.begin(), host_remainders.end(),
                     [](const auto& a, const auto& b) {
                         return a.first > b.first;
                     });
    while (assigned_host_slots < total_slots) {
        bool made_progress = false;
        for (const auto& [remainder, h] : host_remainders) {
            (void)remainder;
            if (plan.host_slots[h] >= host_slot_cap) {
                continue;
            }
            ++plan.host_slots[h];
            ++assigned_host_slots;
            made_progress = true;
            if (assigned_host_slots == total_slots) {
                break;
            }
        }
        if (!made_progress) {
            return std::nullopt;
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

std::optional<std::vector<size_t>> PickHostsForRow(
    const std::vector<std::vector<size_t>>& host_queues, size_t rows_left,
    uint32_t replica_num) {
    std::vector<size_t> picked_hosts;
    picked_hosts.reserve(replica_num);
    const auto already_picked = [&picked_hosts](size_t h) {
        return std::find(picked_hosts.begin(), picked_hosts.end(), h) !=
               picked_hosts.end();
    };

    // A Host with one slot for every remaining row is mandatory. Selecting
    // all mandatory Hosts preserves remaining_slots[h] <= rows_left - 1.
    for (size_t h = 0; h < host_queues.size(); ++h) {
        if (host_queues[h].size() < rows_left) {
            continue;
        }
        if (picked_hosts.size() >= replica_num) {
            return std::nullopt;
        }
        picked_hosts.push_back(h);
    }
    while (picked_hosts.size() < replica_num) {
        size_t total = 0;
        for (size_t h = 0; h < host_queues.size(); ++h) {
            if (!host_queues[h].empty() && !already_picked(h)) {
                total += host_queues[h].size();
            }
        }
        if (total == 0) {
            return std::nullopt;
        }
        size_t draw = randomIndex(total);
        size_t selected = SIZE_MAX;
        for (size_t h = 0; h < host_queues.size(); ++h) {
            if (host_queues[h].empty() || already_picked(h)) {
                continue;
            }
            if (draw < host_queues[h].size()) {
                selected = h;
                break;
            }
            draw -= host_queues[h].size();
        }
        picked_hosts.push_back(selected);
    }
    randomShuffle(picked_hosts.begin(), picked_hosts.end());
    return picked_hosts;
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

    std::vector<PtEntry> entries;
    entries.reserve(config.pt_count);
    for (uint32_t pt_id = 0; pt_id < config.pt_count; ++pt_id) {
        const size_t rows_left = static_cast<size_t>(config.pt_count) - pt_id;
        auto picked_hosts =
            PickHostsForRow(host_queues, rows_left, config.replica_num);
        if (!picked_hosts) {
            return std::nullopt;
        }
        PtEntry entry;
        entry.pt_id = pt_id;
        entry.replicas.reserve(config.replica_num);
        for (const size_t h : *picked_hosts) {
            const size_t segment_index = host_queues[h].back();
            host_queues[h].pop_back();
            const PtSegmentSnapshot* segment = topology.segments[segment_index];
            entry.replicas.push_back(PtTarget{
                segment->segment_id, segment->name, segment->host_id,
                segment->host_id});
        }
        entries.push_back(std::move(entry));
    }
    if (std::any_of(host_queues.begin(), host_queues.end(),
                    [](const auto& queue) { return !queue.empty(); })) {
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

    // 1. Filter: only segments with explicit host_id participate (RFC 0005
    // §7.1: TOPOLOGY_INCOMPLETE segments are excluded, never guessed).
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

    // Need at least replica_num distinct failure domains overall.
    {
        std::unordered_set<std::string> domains;
        for (const auto* segment : eligible) {
            domains.insert(segment->host_id);
        }
        if (domains.size() < config.replica_num) {
            return std::nullopt;
        }
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
