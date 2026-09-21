#include "placement/pt_rebuild_policy.h"

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

namespace mooncake::test {
namespace {

PtBalanceSummary Summary(double spread, uint64_t generation,
                         double min_utilization = 0.0,
                         uint64_t topology_fingerprint = 1) {
    PtBalanceSummary summary;
    summary.eligible_segments = 4;
    summary.min_utilization = min_utilization;
    summary.max_utilization = min_utilization + spread;
    summary.utilization_spread = spread;
    summary.topology_fingerprint = topology_fingerprint;
    summary.space_fingerprint = generation;
    return summary;
}

TEST(PtRebuildPolicyTest, ComputesSegmentUtilizationSpread) {
    std::vector<PtSegmentSnapshot> segments(3);
    segments[0].segment_id = {1, 1};
    segments[0].name = "a";
    segments[0].host_id = "host-a";
    segments[0].capacity = 100;
    segments[0].used = 10;
    segments[0].largest_free = 90;

    segments[1].segment_id = {2, 1};
    segments[1].name = "b";
    segments[1].host_id = "host-b";
    segments[1].capacity = 200;
    segments[1].used = 100;
    segments[1].largest_free = 100;

    // Topology-incomplete segments do not participate.
    segments[2].segment_id = {3, 1};
    segments[2].name = "excluded";
    segments[2].capacity = 100;
    segments[2].used = 100;

    const auto summary = ComputePtBalanceSummary(segments);
    EXPECT_EQ(summary.eligible_segments, 2U);
    EXPECT_NEAR(summary.min_utilization, 0.1, 1e-12);
    EXPECT_NEAR(summary.max_utilization, 0.5, 1e-12);
    EXPECT_NEAR(summary.utilization_spread, 0.4, 1e-12);
}

TEST(PtPlacementWeightPolicyTest, TopologyChangeEntersCatchUp) {
    PtPlacementWeightPolicy policy(0.90);

    const auto initial = policy.Observe(Summary(0.01, 1, 0.20, 1));
    EXPECT_EQ(initial.weight_mode, PtSegmentWeightMode::CAPACITY);

    const auto scale_out = policy.Observe(Summary(0.30, 2, 0.0, 2));
    EXPECT_EQ(scale_out.weight_mode, PtSegmentWeightMode::TARGET_HEADROOM);
    EXPECT_TRUE(scale_out.mode_changed);
}

TEST(PtPlacementWeightPolicyTest, ThreeCaughtUpRoundsLatchCapacity) {
    PtPlacementWeightPolicy policy(0.90);
    EXPECT_EQ(policy.Observe(Summary(0.30, 1, 0.0, 1)).weight_mode,
              PtSegmentWeightMode::TARGET_HEADROOM);

    EXPECT_EQ(policy.Observe(Summary(0.01, 2, 0.89, 1)).weight_mode,
              PtSegmentWeightMode::TARGET_HEADROOM);
    EXPECT_EQ(policy.Observe(Summary(0.01, 3, 0.89, 1)).weight_mode,
              PtSegmentWeightMode::TARGET_HEADROOM);
    const auto steady = policy.Observe(Summary(0.01, 4, 0.89, 1));
    EXPECT_EQ(steady.weight_mode, PtSegmentWeightMode::CAPACITY);
    EXPECT_TRUE(steady.mode_changed);

    // Small write/eviction skew below the enter threshold stays latched.
    const auto skewed = policy.Observe(Summary(0.04, 5, 0.86, 1));
    EXPECT_EQ(skewed.weight_mode, PtSegmentWeightMode::CAPACITY);
    EXPECT_FALSE(skewed.mode_changed);
}

TEST(PtPlacementWeightPolicyTest, DriftReentersCatchUpWithoutTopologyChange) {
    PtPlacementWeightPolicy policy(0.90);
    EXPECT_EQ(policy.Observe(Summary(0.01, 1, 0.90, 1)).weight_mode,
              PtSegmentWeightMode::CAPACITY);

    // Eviction or key deletion opens a large gap on the same topology.
    const auto drifted = policy.Observe(Summary(0.30, 2, 0.60, 1));
    EXPECT_EQ(drifted.weight_mode, PtSegmentWeightMode::TARGET_HEADROOM);
    EXPECT_TRUE(drifted.mode_changed);

    // Catch-up runs headroom-weighted rounds until balance is restored.
    EXPECT_EQ(policy.Observe(Summary(0.10, 3, 0.80, 1)).weight_mode,
              PtSegmentWeightMode::TARGET_HEADROOM);
    EXPECT_EQ(policy.Observe(Summary(0.01, 4, 0.89, 1)).weight_mode,
              PtSegmentWeightMode::TARGET_HEADROOM);
    EXPECT_EQ(policy.Observe(Summary(0.01, 5, 0.89, 1)).weight_mode,
              PtSegmentWeightMode::TARGET_HEADROOM);
    const auto relatched = policy.Observe(Summary(0.01, 6, 0.89, 1));
    EXPECT_EQ(relatched.weight_mode, PtSegmentWeightMode::CAPACITY);
    EXPECT_TRUE(relatched.mode_changed);
}

TEST(PtPlacementWeightPolicyTest, DriftBelowEnterSpreadStaysCapacity) {
    PtPlacementWeightPolicy policy(0.90);
    EXPECT_EQ(policy.Observe(Summary(0.01, 1, 0.90, 1)).weight_mode,
              PtSegmentWeightMode::CAPACITY);

    const auto mild = policy.Observe(Summary(0.049, 2, 0.87, 1));
    EXPECT_EQ(mild.weight_mode, PtSegmentWeightMode::CAPACITY);
    EXPECT_FALSE(mild.mode_changed);
}

TEST(PtPlacementWeightPolicyTest, DriftEntirelyAboveTargetStaysCapacity) {
    PtPlacementWeightPolicy policy(0.90);
    EXPECT_EQ(policy.Observe(Summary(0.01, 1, 0.90, 1)).weight_mode,
              PtSegmentWeightMode::CAPACITY);

    // Headroom at the 90% target is zero for every segment, so re-entering
    // catch-up cannot reduce this spread and would only cause mode oscillation.
    const auto above_target = policy.Observe(Summary(0.08, 2, 0.90, 1));
    EXPECT_EQ(above_target.weight_mode, PtSegmentWeightMode::CAPACITY);
    EXPECT_FALSE(above_target.mode_changed);
}

TEST(PtPlacementWeightPolicyTest, NinetyPercentTargetEndsCatchUp) {
    PtPlacementWeightPolicy policy(0.90);
    EXPECT_EQ(policy.Observe(Summary(0.30, 1, 0.0, 1)).weight_mode,
              PtSegmentWeightMode::TARGET_HEADROOM);

    EXPECT_EQ(policy.Observe(Summary(0.08, 2, 0.90, 1)).weight_mode,
              PtSegmentWeightMode::TARGET_HEADROOM);
    EXPECT_EQ(policy.Observe(Summary(0.08, 3, 0.90, 1)).weight_mode,
              PtSegmentWeightMode::TARGET_HEADROOM);
    EXPECT_EQ(policy.Observe(Summary(0.08, 4, 0.90, 1)).weight_mode,
              PtSegmentWeightMode::CAPACITY);
}

TEST(PtPlacementWeightPolicyTest, RejectsInvalidTarget) {
    EXPECT_THROW(PtPlacementWeightPolicy(0.0), std::invalid_argument);
    EXPECT_THROW(PtPlacementWeightPolicy(1.01), std::invalid_argument);
}

TEST(PtRebuildPolicyTest, SevereImbalanceEntersFastMode) {
    PtRebuildCadence cadence(std::chrono::seconds(60), std::chrono::seconds(1));
    const auto decision = cadence.Observe(Summary(0.06, 1));
    EXPECT_EQ(decision.mode, PtRebuildCadence::Mode::FAST);
    EXPECT_EQ(decision.next_interval, std::chrono::seconds(1));
    EXPECT_TRUE(decision.materially_changed);
}

TEST(PtRebuildPolicyTest, ThreeBalancedRoundsReturnToNormal) {
    PtRebuildCadence cadence(std::chrono::seconds(60), std::chrono::seconds(1));
    EXPECT_EQ(cadence.Observe(Summary(0.06, 1)).mode,
              PtRebuildCadence::Mode::FAST);
    EXPECT_EQ(cadence.Observe(Summary(0.01, 2)).mode,
              PtRebuildCadence::Mode::FAST);
    EXPECT_EQ(cadence.Observe(Summary(0.01, 3)).mode,
              PtRebuildCadence::Mode::FAST);
    const auto normal = cadence.Observe(Summary(0.01, 4));
    EXPECT_EQ(normal.mode, PtRebuildCadence::Mode::NORMAL);
    EXPECT_EQ(normal.next_interval, std::chrono::seconds(60));
}

TEST(PtRebuildPolicyTest, UnchangedFastStateBacksOff) {
    PtRebuildCadence cadence(std::chrono::seconds(60), std::chrono::seconds(1));
    const auto severe = Summary(0.20, 1);
    EXPECT_EQ(cadence.Observe(severe).mode, PtRebuildCadence::Mode::FAST);
    EXPECT_EQ(cadence.Observe(severe).mode, PtRebuildCadence::Mode::FAST);
    EXPECT_EQ(cadence.Observe(severe).mode, PtRebuildCadence::Mode::FAST);
    const auto backed_off = cadence.Observe(severe);
    EXPECT_EQ(backed_off.mode, PtRebuildCadence::Mode::NORMAL);
    EXPECT_FALSE(backed_off.materially_changed);

    // The same unchanged imbalance does not immediately re-enter FAST.
    EXPECT_EQ(cadence.Observe(severe).mode, PtRebuildCadence::Mode::NORMAL);
    // New physical activity makes the imbalance actionable again.
    EXPECT_EQ(cadence.Observe(Summary(0.20, 2)).mode,
              PtRebuildCadence::Mode::FAST);
}

TEST(PtRebuildPolicyTest, RejectsInvalidIntervals) {
    EXPECT_THROW(PtRebuildCadence(std::chrono::milliseconds(0),
                                  std::chrono::milliseconds(1)),
                 std::invalid_argument);
    EXPECT_THROW(PtRebuildCadence(std::chrono::milliseconds(60),
                                  std::chrono::milliseconds(0)),
                 std::invalid_argument);
    EXPECT_THROW(PtRebuildCadence(std::chrono::milliseconds(-1),
                                  std::chrono::milliseconds(-1)),
                 std::invalid_argument);
    EXPECT_THROW(PtRebuildCadence(std::chrono::milliseconds(100),
                                  std::chrono::milliseconds(200)),
                 std::invalid_argument);
}

}  // namespace
}  // namespace mooncake::test
