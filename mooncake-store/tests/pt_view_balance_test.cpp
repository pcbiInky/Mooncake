// Balance simulation for the NoF PT view builder (RFC 0005 §9).
//
// Scenarios:
//  1. Heterogeneous free space across hosts/segments -> slot distribution
//     tracks free-space water filling with the W/N*k host clamp.
//  2. One dominant host would exceed the W/N*k clamp -> its share is
//     capped near the clamp and the excess is redistributed.
//  3. Failure-domain exclusivity: no PT row ever repeats a Host or Rack.
//  4. Allocation simulation over many rows: per-segment placement
//     frequency tracks each segment's slot count within tolerance.
//  5. R=3 row feasibility after hierarchical Host/Segment rounding.
//  6. Invalid direct builder configuration is rejected.
//  7. Rack-aware placement, missing-Rack Host fallback, and multi-NIC Hosts.
//  8. Rack topology and size-class combinations with no feasible PT view.
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
                               uint64_t used_gib,
                               const std::string& rack = "") {
    PtSegmentSnapshot snapshot;
    snapshot.segment_id.first = std::hash<std::string>{}(name);
    snapshot.segment_id.second = 1;
    snapshot.name = name;
    snapshot.host_id = host;
    snapshot.rack_id = rack;
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

    // ---- Scenario 7: Rack exclusivity and Host balancing are independent.
    // Every row must select one Host from each Rack, while Hosts inside a Rack
    // still split that Rack's slots according to free space.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("a1", "hostA1", 16, 0, "rackA"),
            MakeSegment("a2", "hostA2", 16, 8, "rackA"),
            MakeSegment("b1", "hostB1", 16, 4, "rackB"),
            MakeSegment("b2", "hostB2", 16, 12, "rackB"),
        };
        auto view = PtViewBuilder::Build(segments, config);
        Check(view.has_value(), "scenario7: two-rack view builds");
        if (!view) return failures ? 1 : 0;
        const PtPolicyView* policy = view->FindPolicy(2 * kMiB);
        Check(policy != nullptr, "scenario7: small class policy exists");
        if (policy) {
            bool rows_ok = true;
            for (const auto& entry : policy->entries) {
                std::unordered_set<std::string> hosts;
                std::unordered_set<std::string> domains;
                for (const auto& target : entry.replicas) {
                    hosts.insert(target.host_id);
                    domains.insert(target.failure_domain_id);
                }
                if (hosts.size() != config.replica_num ||
                    domains.size() != config.replica_num) {
                    rows_ok = false;
                }
            }
            Check(rows_ok,
                  "scenario7: every row has distinct Hosts and Racks");
        }
    }

    // ---- Scenario 8: missing rack_id uses Host as a best-effort domain.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("a1", "hostA", 16, 0, "rackA"),
            MakeSegment("a2", "hostB", 16, 0, "rackA"),
            MakeSegment("fallback", "hostC", 16, 0),
        };
        auto view = PtViewBuilder::Build(segments, config);
        Check(view.has_value(), "scenario8: Host fallback view builds");
        if (!view) return failures ? 1 : 0;
        const PtPolicyView* policy = view->FindPolicy(2 * kMiB);
        Check(policy != nullptr, "scenario8: small class policy exists");
        if (policy) {
            bool saw_rack = false;
            bool saw_host_fallback = false;
            for (const auto& entry : policy->entries) {
                for (const auto& target : entry.replicas) {
                    saw_rack |= target.failure_domain_id == "rack:rackA";
                    saw_host_fallback |=
                        target.failure_domain_id == "host:hostC";
                }
            }
            Check(saw_rack && saw_host_fallback,
                  "scenario8: DFX distinguishes Rack and Host fallback");
        }
    }

    // ---- Scenario 9: two NICs on one Host share one Host identity and can
    // never occupy two replica positions in the same row.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("nic0", "hostA", 16, 0, "rackA"),
            MakeSegment("nic1", "hostA", 16, 0),
            MakeSegment("peer", "hostB", 16, 0, "rackB"),
        };
        auto view = PtViewBuilder::Build(segments, config);
        Check(view.has_value(), "scenario9: multi-NIC Host view builds");
        if (!view) return failures ? 1 : 0;
        const PtPolicyView* policy = view->FindPolicy(2 * kMiB);
        Check(policy != nullptr, "scenario9: small class policy exists");
        if (policy) {
            bool hosts_ok = true;
            bool missing_rack_inherited = false;
            for (const auto& entry : policy->entries) {
                std::unordered_set<std::string> hosts;
                for (const auto& target : entry.replicas) {
                    hosts.insert(target.host_id);
                    if (target.name == "nic1") {
                        missing_rack_inherited |=
                            target.rack_id == "rackA" &&
                            target.failure_domain_id == "rack:rackA";
                    }
                }
                hosts_ok &= hosts.size() == entry.replicas.size();
            }
            Check(hosts_ok, "scenario9: no row repeats a physical Host");
            Check(missing_rack_inherited,
                  "scenario9: sibling Segment inherits known Host Rack");
        }
    }

    // ---- Scenario 10: Rack constraints that have no feasible slot plan.
    {
        // Replica count 2 but only one effective Rack.
        std::vector<PtSegmentSnapshot> one_rack = {
            MakeSegment("a", "hostA", 16, 0, "rackA"),
            MakeSegment("b", "hostB", 16, 0, "rackA"),
        };
        Check(!PtViewBuilder::Build(one_rack, config).has_value(),
              "scenario10: one Rack cannot serve R=2");

        // Rack B has only one of four Hosts. With k=1.5, that Host is capped
        // below the 50% Rack share required by R=2, so the joint Host/Rack
        // constraints are mathematically infeasible.
        std::vector<PtSegmentSnapshot> host_cap_conflict = {
            MakeSegment("a1", "hostA1", 16, 0, "rackA"),
            MakeSegment("a2", "hostA2", 16, 0, "rackA"),
            MakeSegment("a3", "hostA3", 16, 0, "rackA"),
            MakeSegment("b1", "hostB1", 16, 0, "rackB"),
        };
        Check(!PtViewBuilder::Build(host_cap_conflict, config).has_value(),
              "scenario10: Rack share conflicts with Host k/N cap");

        // One physical Host cannot claim two explicit Racks.
        std::vector<PtSegmentSnapshot> conflicting_rack = {
            MakeSegment("nic0", "hostA", 16, 0, "rackA"),
            MakeSegment("nic1", "hostA", 16, 0, "rackB"),
            MakeSegment("peer", "hostB", 16, 0, "rackC"),
        };
        Check(!PtViewBuilder::Build(conflicting_rack, config).has_value(),
              "scenario10: conflicting Rack IDs on one Host rejected");

        // Three replicas need three effective failure domains even when the
        // number of physical Hosts is sufficient.
        PtBuildConfig r3_config = config;
        r3_config.replica_num = 3;
        std::vector<PtSegmentSnapshot> only_two_racks = {
            MakeSegment("a1", "hostA1", 16, 0, "rackA"),
            MakeSegment("a2", "hostA2", 16, 0, "rackA"),
            MakeSegment("b1", "hostB1", 16, 0, "rackB"),
        };
        Check(!PtViewBuilder::Build(only_two_racks, r3_config).has_value(),
              "scenario10: two Racks cannot serve R=3");

        // A Rack that exists in topology but cannot serve even the smallest
        // size class is not eligible for any policy. No complete row can be
        // formed, so the builder must keep the old view.
        std::vector<PtSegmentSnapshot> rack_without_contiguous_space = {
            MakeSegment("a", "hostA", 16, 0, "rackA"),
            MakeSegment("b", "hostB", 16, 15, "rackB"),
        };
        Check(!PtViewBuilder::Build(rack_without_contiguous_space, config)
                   .has_value(),
              "scenario10: Rack below smallest size class rejected");
    }

    if (failures == 0) {
        std::printf("ALL PT BALANCE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}
