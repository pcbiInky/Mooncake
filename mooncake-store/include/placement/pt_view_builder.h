#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "placement/pt_view.h"
#include "types.h"

namespace mooncake {

class NoFSegmentManager;

// Snapshot of one NoF segment as observed by the builder. The builder runs
// under the NoFSegmentManager pool lock; pointers stay valid for the build
// duration only and are never stored in the published view.
struct PtSegmentSnapshot {
    UUID segment_id;
    std::string name;
    std::string host_id;  // empty => TOPOLOGY_INCOMPLETE, excluded from PT
    uint64_t capacity{0};
    uint64_t used{0};
    uint64_t largest_free{0};
    // Allocator lacks bin-precise capacity reports (e.g. Cachelib); the
    // builder excludes such segments.
    bool unknown_capacity{false};
};

struct PtBuildConfig {
    uint32_t pt_count{128};
    uint32_t replica_num{2};
    double host_increment_skew_k{1.5};
    uint64_t seed{0};
};

struct PtBuildStats {
    size_t total_segments{0};
    size_t topology_incomplete{0};
    size_t eligible_segments{0};
    size_t policies_built{0};
    uint64_t build_duration_ns{0};
};

// Builds immutable PtView snapshots from NoF segment snapshots (RFC 0005 §9).
// All work runs on the caller's thread; publication is done by the owner via
// PtViewManager::Publish.
class PtViewBuilder final {
   public:
    // Returns the fixed size-class upper bounds for the first version:
    // (0,2MiB], (2MiB,4MiB], (4MiB,32MiB], (32MiB,256MiB], (256MiB,1GiB],
    // (1GiB,8GiB].
    static const std::vector<uint64_t>& DefaultSizeClasses();

    static std::optional<PtView> Build(
        const std::vector<PtSegmentSnapshot>& segments,
        const PtBuildConfig& config, PtBuildStats* stats = nullptr);

   private:
    // Fills policy_out with the PT table for one size class. Returns false
    // when the class cannot be built (insufficient failure domains,
    // infeasible slot distribution); the caller must skip that class.
    static bool BuildPolicy(
        const std::vector<const PtSegmentSnapshot*>& eligible,
        uint64_t min_exclusive, uint64_t max_inclusive,
        const PtBuildConfig& config, PtPolicyView* policy_out);
};

}  // namespace mooncake
