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

// Copy of one NoF segment's topology and capacity state.
struct PtSegmentSnapshot {
    UUID segment_id;
    std::string name;
    std::string host_id;  // empty => TOPOLOGY_INCOMPLETE, excluded from PT
    // Optional rack or power-domain ID; empty falls back to host_id.
    std::string rack_id;
    uint64_t capacity{0};
    uint64_t used{0};
    uint64_t largest_free{0};
    // True when precise capacity is unavailable; such segments are excluded.
    bool unknown_capacity{false};
};

// Prefix IDs to keep rack and host namespaces distinct.
inline std::string EffectiveFailureDomain(
    const PtSegmentSnapshot& segment) {
    if (!segment.rack_id.empty()) {
        return "rack:" + segment.rack_id;
    }
    return segment.host_id.empty() ? std::string()
                                   : "host:" + segment.host_id;
}

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
    uint64_t build_duration_ns{0};
};

// Builds immutable PtView snapshots; the owner publishes successful results.
class PtViewBuilder final {
   public:
    static std::optional<PtView> Build(
        const std::vector<PtSegmentSnapshot>& segments,
        const PtBuildConfig& config, PtBuildStats* stats = nullptr);
};

}  // namespace mooncake
