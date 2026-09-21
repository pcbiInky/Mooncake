// Placement-table balance and topology tests.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
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
constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

PtSegmentSnapshot MakeSegment(const std::string& name, const std::string& host,
                              uint64_t capacity_gib, uint64_t used_gib,
                              const std::string& rack = "") {
    PtSegmentSnapshot snapshot;
    snapshot.segment_id.first = std::hash<std::string>{}(name);
    snapshot.segment_id.second = 1;
    snapshot.name = name;
    snapshot.host_id = host;
    snapshot.rack_id = rack;
    snapshot.capacity = capacity_gib * kGiB;
    snapshot.used = used_gib * kGiB;
    snapshot.largest_free = snapshot.capacity > snapshot.used
                                ? snapshot.capacity - snapshot.used
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

// Compare slot share with single-replica selection frequency.
struct PlacementStats {
    std::unordered_map<std::string, size_t> slots;
    std::unordered_map<std::string, size_t> picks;
};

PlacementStats TallyView(const PtView& view, uint64_t rng_seed,
                         size_t simulate_rows) {
    PlacementStats stats;
    for (const auto& entry : view.entries) {
        for (const auto& target : entry.replicas) {
            ++stats.slots[target.name];
        }
    }
    std::mt19937_64 rng(rng_seed);
    for (size_t i = 0; i < simulate_rows; ++i) {
        const PtEntry& entry = view.entries[rng() % view.entries.size()];
        const PtTarget& target = entry.replicas[rng() % entry.replicas.size()];
        ++stats.picks[target.name];
    }
    return stats;
}

std::vector<std::string> RowDomains(const PtEntry& entry) {
    std::vector<std::string> domains;
    domains.reserve(entry.replicas.size());
    for (const auto& target : entry.replicas) {
        domains.push_back(target.failure_domain_id);
    }
    std::sort(domains.begin(), domains.end());
    return domains;
}

std::vector<std::string> RowTargets(const PtEntry& entry) {
    std::vector<std::string> targets;
    targets.reserve(entry.replicas.size());
    for (const auto& target : entry.replicas) {
        targets.push_back(target.failure_domain_id + "/" + target.host_id +
                          "/" + target.name);
    }
    std::sort(targets.begin(), targets.end());
    return targets;
}

bool SameViewLayout(const PtView& lhs, const PtView& rhs) {
    if (lhs.entries.size() != rhs.entries.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.entries.size(); ++i) {
        if (RowTargets(lhs.entries[i]) != RowTargets(rhs.entries[i])) {
            return false;
        }
    }
    return true;
}

struct PairDiversityStats {
    size_t unique_pairs{0};
    size_t max_pair_rows{0};
};

PairDiversityStats MeasurePairDiversity(const PtView& view) {
    std::map<std::pair<std::string, std::string>, size_t> pair_rows;
    for (const auto& entry : view.entries) {
        const auto domains = RowDomains(entry);
        if (domains.size() == 2) {
            ++pair_rows[{domains[0], domains[1]}];
        }
    }
    PairDiversityStats stats;
    stats.unique_pairs = pair_rows.size();
    for (const auto& [pair, rows] : pair_rows) {
        (void)pair;
        stats.max_pair_rows = std::max(stats.max_pair_rows, rows);
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

    // Scenario 1: row constraints cap both hosts at 50%; segment shares
    // within each host follow free bytes.
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
        Check(view->entries.size() == config.pt_count,
              "scenario1: row count == pt_count");

        // Domain exclusivity within every row.
        bool domains_ok = true;
        for (const auto& entry : view->entries) {
            std::unordered_set<std::string> domains;
            for (const auto& target : entry.replicas) {
                domains.insert(target.failure_domain_id);
            }
            if (domains.size() != entry.replicas.size()) {
                domains_ok = false;
            }
        }
        Check(domains_ok, "scenario1: rows are domain-exclusive");

        // Row width fixes host shares at 50/50.
        const auto tally = TallyView(*view, config.seed, 0);
        const size_t total_slots =
            static_cast<size_t>(config.pt_count) * config.replica_num;
        const size_t host_a_slots =
            tally.slots.at("a1") + tally.slots.at("a2") + tally.slots.at("a3");
        const double a_share = static_cast<double>(host_a_slots) / total_slots;
        Check(std::abs(a_share - 0.50) < 0.02,
              "scenario1: hostA share is the row cap 50% (got " +
                  std::to_string(a_share) + ")");
        // Segment shares follow their host-local weights.
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

    // Scenario 2: a dominant host is limited by the k/N cap.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("big1", "hostA", 16, 0),
            MakeSegment("big2", "hostA", 16, 3),
            MakeSegment("s1", "hostB", 16, 12),
            MakeSegment("s2", "hostB", 16, 13),
            MakeSegment("s3", "hostC", 16, 14),
        };
        // Free bytes: A=29 GiB, B=7 GiB, C=2 GiB.
        PtBuildStats stats;
        auto view = PtViewBuilder::Build(segments, config, &stats);
        Check(view.has_value(), "scenario2: view builds");
        if (!view) return failures ? 1 : 0;
        const auto tally = TallyView(*view, config.seed, 0);
        const size_t total_slots =
            static_cast<size_t>(config.pt_count) * config.replica_num;
        double host_a_share = 0.0;
        for (const auto& [name, count] : tally.slots) {
            if (name == "big1" || name == "big2") {
                host_a_share += static_cast<double>(count) / total_slots;
            }
        }
        // The cap limits host A to 50% plus rounding slack.
        Check(host_a_share <= 0.52,
              "scenario2: hostA share <= W/N*k clamp (got " +
                  std::to_string(host_a_share) + ")");
        Check(host_a_share >= 0.45,
              "scenario2: hostA share still reflects free space (got " +
                  std::to_string(host_a_share) + ")");
    }

    // Scenario 3: sampled single-replica frequency tracks slot share.
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
        const size_t kSimulatedPicks = 200000;
        const auto tally = TallyView(*view, config.seed + 1, kSimulatedPicks);
        const size_t total_slots =
            static_cast<size_t>(config.pt_count) * config.replica_num;
        for (const auto& [name, slot_count] : tally.slots) {
            const double expected =
                static_cast<double>(slot_count) / total_slots;
            const double actual =
                static_cast<double>(tally.picks.at(name)) / kSimulatedPicks;
            Check(std::abs(expected - actual) < 0.02,
                  "scenario3: pick rate tracks slot share for " + name +
                      " (expected " + std::to_string(expected) + ", actual " +
                      std::to_string(actual) + ")");
        }
    }

    // Scenario 4: reject infeasible topologies.
    {
        // One host cannot provide two failure domains.
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("x1", "hostA", 16, 0),
            MakeSegment("x2", "hostA", 16, 0),
        };
        auto view = PtViewBuilder::Build(segments, config);
        Check(!view.has_value(), "scenario4: single-host rejected");

        // Missing host identity makes the segment ineligible.
        std::vector<PtSegmentSnapshot> partial = {
            MakeSegment("y1", "", 16, 0),
            MakeSegment("y2", "hostB", 16, 0),
        };
        auto view2 = PtViewBuilder::Build(partial, config);
        Check(!view2.has_value(), "scenario4: incomplete topology rejected");
    }

    // Scenario 5: R=3 rounding preserves the per-host row cap.
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
        std::unordered_map<std::string, size_t> host_slots;
        bool domains_ok = true;
        for (const auto& entry : view->entries) {
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

    // Scenario 6: reject invalid builder configuration.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("a", "hostA", 16, 0),
            MakeSegment("b", "hostB", 16, 0),
        };
        PtBuildConfig invalid = config;
        invalid.replica_num = 0;
        Check(!PtViewBuilder::Build(segments, invalid).has_value(),
              "scenario6: zero replicas rejected");

        invalid = config;
        invalid.pt_count = 0;
        Check(!PtViewBuilder::Build(segments, invalid).has_value(),
              "scenario6: zero pt_count rejected");

        invalid = config;
        invalid.host_increment_skew_k = 0.5;
        Check(!PtViewBuilder::Build(segments, invalid).has_value(),
              "scenario6: skew below 1.0 rejected");

        invalid = config;
        invalid.host_increment_skew_k = std::nan("");
        Check(!PtViewBuilder::Build(segments, invalid).has_value(),
              "scenario6: NaN skew rejected");

        invalid = config;
        invalid.host_increment_skew_k = std::numeric_limits<double>::infinity();
        Check(!PtViewBuilder::Build(segments, invalid).has_value(),
              "scenario6: infinite skew rejected");
    }

    // Scenario 7: rack exclusivity preserves host weighting within each rack.
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
        bool rows_ok = true;
        for (const auto& entry : view->entries) {
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
        Check(rows_ok, "scenario7: every row has distinct Hosts and Racks");
    }

    // Scenario 8: missing rack_id falls back to host_id.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("a1", "hostA", 16, 0, "rackA"),
            MakeSegment("a2", "hostB", 16, 0, "rackA"),
            MakeSegment("fallback", "hostC", 16, 0),
        };
        auto view = PtViewBuilder::Build(segments, config);
        Check(view.has_value(), "scenario8: Host fallback view builds");
        if (!view) return failures ? 1 : 0;
        bool saw_rack = false;
        bool saw_host_fallback = false;
        for (const auto& entry : view->entries) {
            for (const auto& target : entry.replicas) {
                saw_rack |= target.failure_domain_id == "rack:rackA";
                saw_host_fallback |= target.failure_domain_id == "host:hostC";
            }
        }
        Check(saw_rack && saw_host_fallback,
              "scenario8: DFX distinguishes Rack and Host fallback");
    }

    // Scenario 9: multiple NICs on one host cannot share a row.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("nic0", "hostA", 16, 0, "rackA"),
            MakeSegment("nic1", "hostA", 16, 0),
            MakeSegment("peer", "hostB", 16, 0, "rackB"),
        };
        auto view = PtViewBuilder::Build(segments, config);
        Check(view.has_value(), "scenario9: multi-NIC Host view builds");
        if (!view) return failures ? 1 : 0;
        bool hosts_ok = true;
        bool missing_rack_inherited = false;
        for (const auto& entry : view->entries) {
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

    // Scenario 10: reject infeasible rack constraints.
    {
        // Two replicas require two effective racks.
        std::vector<PtSegmentSnapshot> one_rack = {
            MakeSegment("a", "hostA", 16, 0, "rackA"),
            MakeSegment("b", "hostB", 16, 0, "rackA"),
        };
        Check(!PtViewBuilder::Build(one_rack, config).has_value(),
              "scenario10: one Rack cannot serve R=2");

        // Rack B cannot meet the required share within its host cap.
        std::vector<PtSegmentSnapshot> host_cap_conflict = {
            MakeSegment("a1", "hostA1", 16, 0, "rackA"),
            MakeSegment("a2", "hostA2", 16, 0, "rackA"),
            MakeSegment("a3", "hostA3", 16, 0, "rackA"),
            MakeSegment("b1", "hostB1", 16, 0, "rackB"),
        };
        Check(!PtViewBuilder::Build(host_cap_conflict, config).has_value(),
              "scenario10: Rack share conflicts with Host k/N cap");

        // A host cannot belong to two racks.
        std::vector<PtSegmentSnapshot> conflicting_rack = {
            MakeSegment("nic0", "hostA", 16, 0, "rackA"),
            MakeSegment("nic1", "hostA", 16, 0, "rackB"),
            MakeSegment("peer", "hostB", 16, 0, "rackC"),
        };
        Check(!PtViewBuilder::Build(conflicting_rack, config).has_value(),
              "scenario10: conflicting Rack IDs on one Host rejected");

        // Three replicas require three failure domains.
        PtBuildConfig r3_config = config;
        r3_config.replica_num = 3;
        std::vector<PtSegmentSnapshot> only_two_racks = {
            MakeSegment("a1", "hostA1", 16, 0, "rackA"),
            MakeSegment("a2", "hostA2", 16, 0, "rackA"),
            MakeSegment("b1", "hostB1", 16, 0, "rackB"),
        };
        Check(!PtViewBuilder::Build(only_two_racks, r3_config).has_value(),
              "scenario10: two Racks cannot serve R=3");

        // A zero-free rack cannot contribute to a complete row.
        std::vector<PtSegmentSnapshot> rack_without_contiguous_space = {
            MakeSegment("a", "hostA", 16, 0, "rackA"),
            MakeSegment("b", "hostB", 16, 16, "rackB"),
        };
        Check(!PtViewBuilder::Build(rack_without_contiguous_space, config)
                   .has_value(),
              "scenario10: zero-free-space Rack rejected");
    }

    // Scenario 11: placement is reproducible and input-order independent.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("a0", "hostA", 16, 0, "rackA"),
            MakeSegment("a1", "hostA", 16, 0, "rackA"),
            MakeSegment("b0", "hostB", 16, 0, "rackB"),
            MakeSegment("b1", "hostB", 16, 0, "rackB"),
            MakeSegment("c0", "hostC", 16, 0, "rackC"),
            MakeSegment("c1", "hostC", 16, 0, "rackC"),
            MakeSegment("d0", "hostD", 16, 0, "rackD"),
            MakeSegment("d1", "hostD", 16, 0, "rackD"),
        };
        PtBuildConfig default_seed = config;
        default_seed.seed = 0;
        auto first = PtViewBuilder::Build(segments, default_seed);
        auto second = PtViewBuilder::Build(segments, default_seed);
        std::reverse(segments.begin(), segments.end());
        auto reversed = PtViewBuilder::Build(segments, default_seed);
        Check(first.has_value() && second.has_value() && reversed.has_value(),
              "scenario11: all deterministic builds succeed");
        if (first && second && reversed) {
            Check(first->seed != 0 && first->seed == second->seed &&
                      first->seed == reversed->seed,
                  "scenario11: default PT seed is stable");
            Check(SameViewLayout(*first, *second),
                  "scenario11: repeated build preserves every PT row");
            Check(SameViewLayout(*first, *reversed),
                  "scenario11: input order preserves every PT row");
        }
    }

    // Scenario 12: fixed-seed sampling spreads pairs and limits remapping.
    {
        std::vector<PtSegmentSnapshot> four_racks;
        for (size_t i = 0; i < 4; ++i) {
            const std::string suffix(1, static_cast<char>('A' + i));
            four_racks.push_back(MakeSegment(
                "segment" + suffix, "host" + suffix, 16, 0, "rack" + suffix));
        }
        auto four_view = PtViewBuilder::Build(four_racks, config);
        Check(four_view.has_value(), "scenario12: four-Rack view builds");
        if (!four_view) return failures ? 1 : 0;
        const auto diversity = MeasurePairDiversity(*four_view);
        Check(diversity.unique_pairs == 6,
              "scenario12: all four-Rack pairs are represented");
        Check(diversity.max_pair_rows <= 32,
              "scenario12: no Rack pair dominates more than 25% rows");

        std::vector<PtSegmentSnapshot> five_racks = four_racks;
        five_racks.push_back(MakeSegment("segmentE", "hostE", 16, 0, "rackE"));
        auto five_view = PtViewBuilder::Build(five_racks, config);
        Check(five_view.has_value(), "scenario12: scale-out view builds");
        if (five_view) {
            size_t changed_rows = 0;
            size_t new_rack_rows = 0;
            for (size_t i = 0; i < four_view->entries.size(); ++i) {
                const auto before = RowDomains(four_view->entries[i]);
                const auto after = RowDomains(five_view->entries[i]);
                changed_rows += before != after;
                new_rack_rows += std::find(after.begin(), after.end(),
                                           "rack:rackE") != after.end();
            }
            Check(new_rack_rows >= 50 && new_rack_rows <= 52,
                  "scenario12: new Rack receives its exact share");
            Check(changed_rows <= new_rack_rows + 16,
                  "scenario12: scale-out avoids broad old-Rack churn");
        }
    }

    // Scenario 13: proportional capacity scaling preserves placement.
    {
        std::vector<PtSegmentSnapshot> baseline_segments;
        for (size_t i = 0; i < 4; ++i) {
            const std::string suffix(1, static_cast<char>('A' + i));
            baseline_segments.push_back(MakeSegment(
                "scaled" + suffix, "host" + suffix, 16, 0, "rack" + suffix));
        }
        std::vector<PtSegmentSnapshot> large_segments = baseline_segments;
        for (auto& segment : large_segments) {
            segment.capacity = 1024 * kGiB;
            segment.used = 0;
            segment.largest_free = segment.capacity;
        }

        auto baseline_view = PtViewBuilder::Build(baseline_segments, config);
        auto large_view = PtViewBuilder::Build(large_segments, config);
        Check(baseline_view.has_value() && large_view.has_value(),
              "scenario13: 16 GiB and 1 TiB views build");
        if (baseline_view && large_view) {
            Check(SameViewLayout(*baseline_view, *large_view),
                  "scenario13: capacity scaling preserves PT layout");
            const auto tally = TallyView(*large_view, config.seed, 0);
            for (const auto& segment : large_segments) {
                Check(tally.slots.at(segment.name) == 64,
                      "scenario13: each 1 TiB Segment gets 64 slots");
            }
        }
    }

    // Scenario 14: skewed byte-scale weights conserve slots and host caps.
    {
        std::vector<PtSegmentSnapshot> skewed_segments = {
            MakeSegment("skewA", "hostA", 1024, 0, "rackA"),
            MakeSegment("skewB", "hostB", 1, 0, "rackB"),
            MakeSegment("skewC", "hostC", 1, 0, "rackC"),
            MakeSegment("skewD", "hostD", 1, 0, "rackD"),
        };
        // The two smallest racks each have 100 MiB free.
        for (size_t i = 2; i < skewed_segments.size(); ++i) {
            skewed_segments[i].used = kGiB - 100 * kMiB;
            skewed_segments[i].largest_free = 100 * kMiB;
        }

        auto view = PtViewBuilder::Build(skewed_segments, config);
        Check(view.has_value(), "scenario14: skewed topology builds");
        if (view) {
            const auto tally = TallyView(*view, config.seed, 0);
            size_t total_slots = 0;
            for (const auto& [name, slots] : tally.slots) {
                total_slots += slots;
            }
            Check(total_slots ==
                      static_cast<size_t>(config.pt_count) * config.replica_num,
                  "scenario14: slots conserve the PT budget");
            Check(tally.slots.at("skewA") == 96,
                  "scenario14: dominant Rack saturates the Host cap");
            Check(tally.slots.at("skewB") == 96,
                  "scenario14: GiB Rack saturates the Host cap");
            Check(tally.slots.at("skewC") == 32,
                  "scenario14: MiB Rack gets its exact share");
            Check(tally.slots.at("skewD") == 32,
                  "scenario14: MiB Rack gets its exact share");
            for (const auto& [name, slots] : tally.slots) {
                Check(slots <= 96, "scenario14: no Host exceeds the k/N cap");
            }
        }
    }

    // Scenario 15: total free bytes, not largest extent, determine weight.
    {
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("a_big", "hostA", 16, 4),
            MakeSegment("a_small", "hostA", 16, 12),
            MakeSegment("peer", "hostB", 16, 0),
        };
        for (auto& segment : segments) {
            segment.largest_free = 1;
        }

        auto view = PtViewBuilder::Build(segments, config);
        Check(view.has_value(),
              "scenario15: tiny largest_free does not exclude free capacity");
        if (view) {
            const auto tally = TallyView(*view, config.seed, 0);
            Check(tally.slots.at("a_big") == 96,
                  "scenario15: 12 GiB free receives 3/4 of Host slots");
            Check(tally.slots.at("a_small") == 32,
                  "scenario15: 4 GiB free receives 1/4 of Host slots");
            Check(tally.slots.at("peer") == 128,
                  "scenario15: peer Host fills one replica per row");
        }
    }

    // Scenario 16: steady-state capacity weights ignore utilization skew.
    {
        PtBuildConfig capacity_config = config;
        capacity_config.segment_weight_mode = PtSegmentWeightMode::CAPACITY;
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("capA", "hostA", 16, 15, "rackA"),
            MakeSegment("capB", "hostB", 24, 23, "rackB"),
            MakeSegment("capC", "hostC", 16, 8, "rackC"),
            MakeSegment("capD", "hostD", 16, 0, "rackD"),
        };

        auto view = PtViewBuilder::Build(segments, capacity_config);
        Check(view.has_value(), "scenario16: capacity-weighted view builds");
        if (view) {
            const auto tally = TallyView(*view, config.seed, 0);
            Check(tally.slots.at("capA") == 57,
                  "scenario16: 16 GiB segment A gets 57 slots");
            Check(tally.slots.at("capB") == 85,
                  "scenario16: 24 GiB segment gets 85 slots");
            Check(tally.slots.at("capC") == 57,
                  "scenario16: 16 GiB segment C gets 57 slots");
            Check(tally.slots.at("capD") == 57,
                  "scenario16: 16 GiB segment D gets 57 slots");
        }
    }

    // Scenario 17: catch-up weights use bytes below the 90% target.
    {
        PtBuildConfig catch_up_config = config;
        catch_up_config.segment_weight_mode =
            PtSegmentWeightMode::TARGET_HEADROOM;
        catch_up_config.target_utilization = 0.90;
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("targetA", "hostA", 100, 90, "rackA"),
            MakeSegment("targetB", "hostB", 100, 80, "rackB"),
            MakeSegment("targetC", "hostC", 100, 80, "rackC"),
            MakeSegment("targetD", "hostD", 100, 80, "rackD"),
        };

        PtBuildStats stats;
        auto view = PtViewBuilder::Build(segments, catch_up_config, &stats);
        Check(view.has_value(), "scenario17: target-headroom view builds");
        Check(!stats.used_full_target_fallback,
              "scenario17: target-headroom weights are feasible");
        if (view) {
            const auto tally = TallyView(*view, config.seed, 0);
            Check(tally.slots.count("targetA") == 0,
                  "scenario17: segment at 90% receives no catch-up slots");
            for (const char suffix : {'B', 'C', 'D'}) {
                const std::string name = "target" + std::string(1, suffix);
                const size_t slots = tally.slots.at(name);
                Check(slots >= 85 && slots <= 86,
                      "scenario17: below-target segments split catch-up slots");
            }
        }
    }

    // Scenario 18: strict headroom with too few positive failure domains falls
    // back to target=1.0 instead of failing the rebuild.
    {
        PtBuildConfig catch_up_config = config;
        catch_up_config.segment_weight_mode =
            PtSegmentWeightMode::TARGET_HEADROOM;
        catch_up_config.target_utilization = 0.90;
        std::vector<PtSegmentSnapshot> segments = {
            MakeSegment("oldA", "hostA", 100, 95, "rackA"),
            MakeSegment("oldB", "hostB", 100, 95, "rackB"),
            MakeSegment("oldC", "hostC", 100, 95, "rackC"),
            MakeSegment("newD", "hostD", 100, 0, "rackD"),
        };

        PtBuildStats stats;
        auto view = PtViewBuilder::Build(segments, catch_up_config, &stats);
        Check(view.has_value(), "scenario18: fallback view builds");
        Check(stats.used_full_target_fallback,
              "scenario18: infeasible headroom retries with target=1.0");
        if (view) {
            const auto tally = TallyView(*view, config.seed, 0);
            Check(tally.slots.at("newD") == 96,
                  "scenario18: empty segment saturates the Host cap");
        }
    }

    if (failures == 0) {
        std::printf("ALL PT BALANCE TESTS PASSED\n");
        return 0;
    }
    std::printf("%d failures\n", failures);
    return 1;
}
