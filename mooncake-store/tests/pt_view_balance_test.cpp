// Balance simulation for the NoF PT view builder (RFC 0005 §9).
//
// Scenarios:
//  1. Heterogeneous free space across hosts/segments -> slot distribution
//     tracks free-space water filling with the W/N*k host clamp.
//  2. One dominant host would exceed the W/N*k clamp -> its share is
//     capped near the clamp and the excess is redistributed.
//  3. Domain exclusivity: no PT row ever repeats a failure domain.
//  4. Allocation simulation over many rows: per-segment placement
//     frequency tracks each segment's slot count within tolerance.
//  5. R=3 row feasibility after hierarchical Host/Segment rounding.
//  6. Invalid direct builder configuration is rejected.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "placement/pt_view_builder.h"

using namespace mooncake;

namespace {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;

PtSegmentSnapshot MakeSegment(const std::string& name,
                               const std::string& host, uint64_t capacity_gib,
                               uint64_t used_gib) {
    PtSegmentSnapshot snapshot;
    snapshot.segment_id.first = std::hash<std::string>{}(name);
    snapshot.segment_id.second = 1;
    snapshot.name = name;
    snapshot.host_id = host;
    snapshot.capacity = capacity_gib * kMiB;
    snapshot.used = used_gib * kMiB;
    snapshot.largest_free =
        snapshot.capacity > snapshot.used ? snapshot.capacity - snapshot.used
                                          : 0;
    return snapshot;
}

PtBuildConfig MakeConfig() {
    PtBuildConfig config;
    config.pt_count = 128;
    config.replica_num = 2;
    config.host_increment_skew_k = 1.5;
    config.seed = 42;
    return config;
}

// Counts how many replica slots each segment occupies across all rows of a
// policy, and how often each segment is picked by uniform row + random
// target selection (single-replica mode).
struct PlacementStats {
    std::unordered_map<std::string, size_t> slots;
    std::unordered_map<std::string, size_t> picks;
};

PlacementStats TallyPolicy(const PtPolicyView& policy, uint64_t rng_seed,
                            size_t simulate_rows) {
    PlacementStats stats;
    for (const auto& entry : policy.entries) {
        for (const auto& target : entry.replicas) {
            ++stats.slots[target.name];
        }
    }
    std::mt19937_64 rng(rng_seed);
    for (size_t i = 0; i < simulate_rows; ++i) {
        const PtEntry& entry = policy.entries[rng() % policy.entries.size()];
        const PtTarget& target =
            entry.replicas[rng() % entry.replicas.size()];
        ++stats.picks[target.name];
    }
    return stats;
}

int failures = 0;
void Check(bool ok, const std::string& what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

}  // namespace

int main() {
    const PtBuildConfig config = MakeConfig();

    // ---- Scenario 1: heterogeneous free space ----
    // 2 hosts; host A has 3 segments with 9/6/3 GiB free (18 total),
    // host B has 2 segments with 6/6 GiB free (12 total). Free-space
    // water filling would put 60% of slots on host A, but with only 2
    // failure domains and replica_num=2 every row must take exactly one
    // slot from each host, so both hosts are structurally capped at
    // 50% (the 1/replica_num row-feasibility bound binds before
    // W/N*k = 0.75). Within each host the slot split still tracks free
    // space: a1:a2:a3 = 9:6:3, b1:b2 = 6:6.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("a1", "hostA", 16, 7),
            MakeSegment("a2", "hostA", 16, 10),
            MakeSegment("a3", "hostA", 16, 13),
            MakeSegment("b1", "hostB", 16, 10),
            MakeSegment("b2", "hostB", 16, 10),
        };
        PtBuildStats stats;
        auto view = PtViewBuilder::Build(segments, config, &stats);
        Check(view.has_value(), "scenario1: view builds");
        if (!view) return failures ? 1 : 0;
        Check(stats.eligible_segments == 5, "scenario1: 5 eligible");
        Check(view->policies.size() >= 1, "scenario1: policies built");

        // Every policy covering small sizes must exist. Use the (0,2MiB]
        // class: all segments have >= 2MiB largest_free.
        const PtPolicyView* policy = view->FindPolicy(2 * kMiB);
        Check(policy != nullptr, "scenario1: small class policy exists");
        if (policy) {
            Check(policy->entries.size() == config.pt_count,
                  "scenario1: row count == pt_count");
            // Domain exclusivity within every row.
            bool domains_ok = true;
            for (const auto& entry : policy->entries) {
                std::unordered_set<std::string> domains;
                for (const auto& target : entry.replicas) {
                    domains.insert(target.failure_domain_id);
                }
                if (domains.size() != entry.replicas.size()) {
                    domains_ok = false;
                }
            }
            Check(domains_ok, "scenario1: rows are domain-exclusive");

            // Host shares are structurally 50/50 (row cap); within
            // each host the split tracks free space.
            const auto tally = TallyPolicy(*policy, config.seed, 0);
            const size_t total_slots =
                static_cast<size_t>(config.pt_count) * config.replica_num;
            const size_t host_a_slots = tally.slots.at("a1") +
                                        tally.slots.at("a2") +
                                        tally.slots.at("a3");
            const double a_share =
                static_cast<double>(host_a_slots) / total_slots;
            Check(std::abs(a_share - 0.50) < 0.02,
                  "scenario1: hostA share is the row cap 50% (got " +
                      std::to_string(a_share) + ")");
            // a1 gets 9/18 of host A's slots; b1 gets 6/12 of host B's.
            const double a1_host_share =
                static_cast<double>(tally.slots.at("a1")) / host_a_slots;
            const double b1_host_share =
                static_cast<double>(tally.slots.at("b1")) /
                (tally.slots.at("b1") + tally.slots.at("b2"));
            Check(std::abs(a1_host_share - 0.50) < 0.05,
                  "scenario1: a1 gets 50% of hostA slots (got " +
                      std::to_string(a1_host_share) + ")");
            Check(std::abs(b1_host_share - 0.50) < 0.05,
                  "scenario1: b1 gets 50% of hostB slots (got " +
                      std::to_string(b1_host_share) + ")");
        }
    }

    // ---- Scenario 2: W/N*k clamp binds ----
    // 3 hosts; host A has 80% of free space. Clamp = 1/3*1.5 = 0.5, so
    // host A's slot share must be <= ~0.5 instead of 0.8.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("big1", "hostA", 16, 0),
            MakeSegment("big2", "hostA", 16, 3),
            MakeSegment("s1", "hostB", 16, 12),
            MakeSegment("s2", "hostB", 16, 13),
            MakeSegment("s3", "hostC", 16, 14),
        };
        // free: hostA = 29 GiB, hostB = 7 GiB, hostC = 2 GiB, total 38.
        PtBuildStats stats;
        auto view = PtViewBuilder::Build(segments, config, &stats);
        Check(view.has_value(), "scenario2: view builds");
        if (!view) return failures ? 1 : 0;
        const PtPolicyView* policy = view->FindPolicy(2 * kMiB);
        Check(policy != nullptr, "scenario2: small class policy exists");
        if (policy) {
            const auto tally = TallyPolicy(*policy, config.seed, 0);
            const size_t total_slots =
                static_cast<size_t>(config.pt_count) * config.replica_num;
            double host_a_share = 0.0;
            for (const auto& [name, count] : tally.slots) {
                if (name == "big1" || name == "big2") {
                    host_a_share +=
                        static_cast<double>(count) / total_slots;
                }
            }
            // Without the clamp host A would get ~76% of slots; with the
            // clamp it must be <= 0.5 + rounding slack.
            Check(host_a_share <= 0.52,
                  "scenario2: hostA share <= W/N*k clamp (got " +
                      std::to_string(host_a_share) + ")");
            Check(host_a_share >= 0.45,
                  "scenario2: hostA share still reflects free space (got " +
                      std::to_string(host_a_share) + ")");
        }
    }

    // ---- Scenario 3: allocation simulation over many single-replica
    // picks. The empirical pick frequency per segment must track its slot
    // share within a generous tolerance (uniform random row + target).
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("a1", "hostA", 16, 4),
            MakeSegment("a2", "hostA", 16, 8),
            MakeSegment("b1", "hostB", 16, 6),
            MakeSegment("b2", "hostB", 16, 2),
            MakeSegment("c1", "hostC", 16, 12),
        };
        auto view = PtViewBuilder::Build(segments, config);
        Check(view.has_value(), "scenario3: view builds");
        if (!view) return failures ? 1 : 0;
        const PtPolicyView* policy = view->FindPolicy(4 * kMiB);
        Check(policy != nullptr, "scenario3: mid class policy exists");
        if (policy) {
            const size_t kSimulatedPicks = 200000;
            const auto tally =
                TallyPolicy(*policy, config.seed + 1, kSimulatedPicks);
            const size_t total_slots =
                static_cast<size_t>(config.pt_count) * config.replica_num;
            for (const auto& [name, slot_count] : tally.slots) {
                const double expected =
                    static_cast<double>(slot_count) / total_slots;
                const double actual =
                    static_cast<double>(tally.picks.at(name)) /
                    kSimulatedPicks;
                Check(std::abs(expected - actual) < 0.02,
                      "scenario3: pick rate tracks slot share for " + name +
                          " (expected " + std::to_string(expected) +
                          ", actual " + std::to_string(actual) + ")");
            }
        }
    }

    // ---- Scenario 4: infeasible topologies are rejected ----
    {
        // Only one host: cannot satisfy replica_num=2 domain exclusivity.
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("x1", "hostA", 16, 0),
            MakeSegment("x2", "hostA", 16, 0),
        };
        auto view = PtViewBuilder::Build(segments, config);
        Check(!view.has_value(), "scenario4: single-host rejected");

        // Empty host_id => TOPOLOGY_INCOMPLETE, excluded entirely.
        std::vector<PtSegmentSnapshot> partial = {
            MakeSegment("y1", "", 16, 0),
            MakeSegment("y2", "hostB", 16, 0),
        };
        auto view2 = PtViewBuilder::Build(partial, config);
        Check(!view2.has_value(), "scenario4: incomplete topology rejected");
    }

    // ---- Scenario 5: R=3 keeps integer host quotas feasible ----
    // The dominant host is capped at one slot per row. Host-level rounding
    // happens before segment-level rounding, so no collection of segment
    // remainders can push a host above pt_count.
    {
        PtBuildConfig r3_config = config;
        r3_config.replica_num = 3;
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("a1", "hostA", 64, 0),
            MakeSegment("a2", "hostA", 32, 0),
            MakeSegment("a3", "hostA", 16, 0),
            MakeSegment("b1", "hostB", 32, 16),
            MakeSegment("c1", "hostC", 32, 20),
            MakeSegment("d1", "hostD", 32, 24),
        };
        auto view = PtViewBuilder::Build(segments, r3_config);
        Check(view.has_value(), "scenario5: R=3 view builds");
        if (!view) return failures ? 1 : 0;
        const PtPolicyView* policy = view->FindPolicy(2 * kMiB);
        Check(policy != nullptr, "scenario5: R=3 policy exists");
        if (policy) {
            std::unordered_map<std::string, size_t> host_slots;
            bool domains_ok = true;
            for (const auto& entry : policy->entries) {
                std::unordered_set<std::string> domains;
                for (const auto& target : entry.replicas) {
                    ++host_slots[target.host_id];
                    domains.insert(target.failure_domain_id);
                }
                if (entry.replicas.size() != r3_config.replica_num ||
                    domains.size() != r3_config.replica_num) {
                    domains_ok = false;
                }
            }
            Check(domains_ok, "scenario5: every R=3 row has 3 domains");
            size_t total = 0;
            for (const auto& [host, count] : host_slots) {
                (void)host;
                total += count;
                Check(count <= r3_config.pt_count,
                      "scenario5: integer host quota <= pt_count");
            }
            Check(total == static_cast<size_t>(r3_config.pt_count) *
                               r3_config.replica_num,
                  "scenario5: integer host quotas preserve all slots");
        }
    }

    // ---- Scenario 6: malformed direct builder config is rejected ----
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("a", "hostA", 16, 0),
            MakeSegment("b", "hostB", 16, 0),
        };
        PtBuildConfig invalid = config;
        invalid.replica_num = 0;
        Check(!PtViewBuilder::Build(segments, invalid).has_value(),
              "scenario6: zero replicas rejected");
    }

    if (failures == 0) {
        std::printf("ALL PT BALANCE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}
