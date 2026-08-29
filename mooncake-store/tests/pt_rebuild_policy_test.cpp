#include "placement/pt_rebuild_policy.h"

#include <chrono>
#include <vector>

#include <gtest/gtest.h>

namespace mooncake::test {
namespace {

PtBalanceSummary Summary(double spread, uint64_t generation) {
    PtBalanceSummary summary;
    summary.eligible_segments = 4;
    summary.utilization_spread = spread;
    summary.topology_fingerprint = 1;
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
    EXPECT_NEAR(summary.utilization_spread, 0.4, 1e-12);
}

TEST(PtRebuildPolicyTest, SevereImbalanceEntersFastMode) {
    PtRebuildCadence cadence(std::chrono::seconds(60),
                             std::chrono::seconds(1));
    const auto decision = cadence.Observe(Summary(0.06, 1));
    EXPECT_EQ(decision.mode, PtRebuildCadence::Mode::FAST);
    EXPECT_EQ(decision.next_interval, std::chrono::seconds(1));
    EXPECT_TRUE(decision.materially_changed);
}

TEST(PtRebuildPolicyTest, ThreeBalancedRoundsReturnToNormal) {
    PtRebuildCadence cadence(std::chrono::seconds(60),
                             std::chrono::seconds(1));
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
    PtRebuildCadence cadence(std::chrono::seconds(60),
                             std::chrono::seconds(1));
    const auto severe = Summary(0.20, 1);
    EXPECT_EQ(cadence.Observe(severe).mode,
              PtRebuildCadence::Mode::FAST);
    EXPECT_EQ(cadence.Observe(severe).mode,
              PtRebuildCadence::Mode::FAST);
    EXPECT_EQ(cadence.Observe(severe).mode,
              PtRebuildCadence::Mode::FAST);
    const auto backed_off = cadence.Observe(severe);
    EXPECT_EQ(backed_off.mode, PtRebuildCadence::Mode::NORMAL);
    EXPECT_FALSE(backed_off.materially_changed);

    // The same unchanged imbalance does not immediately re-enter FAST.
    EXPECT_EQ(cadence.Observe(severe).mode,
              PtRebuildCadence::Mode::NORMAL);
    // New physical activity makes the imbalance actionable again.
    EXPECT_EQ(cadence.Observe(Summary(0.20, 2)).mode,
              PtRebuildCadence::Mode::FAST);
}

}  // namespace
}  // namespace mooncake::test
