#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "placement/pt_view_builder.h"

namespace mooncake {

// Physical-state summary used to choose the next rebuild interval.
struct PtBalanceSummary {
    size_t eligible_segments{0};
    double min_utilization{0.0};
    double max_utilization{0.0};
    double utilization_spread{0.0};
    uint64_t topology_fingerprint{0};
    uint64_t space_fingerprint{0};
};

namespace pt_rebuild_policy_detail {

inline void HashBytes(uint64_t* hash, const void* data, size_t size) {
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        *hash ^= bytes[i];
        *hash *= kFnvPrime;
    }
}

template <typename T>
inline void HashValue(uint64_t* hash, const T& value) {
    HashBytes(hash, &value, sizeof(value));
}

inline void HashString(uint64_t* hash, std::string_view value) {
    HashBytes(hash, value.data(), value.size());
    const uint8_t separator = 0xff;
    HashValue(hash, separator);
}

}  // namespace pt_rebuild_policy_detail

inline PtBalanceSummary ComputePtBalanceSummary(
    const std::vector<PtSegmentSnapshot>& segments) {
    std::vector<const PtSegmentSnapshot*> eligible;
    eligible.reserve(segments.size());
    for (const auto& segment : segments) {
        if (EffectiveFailureDomain(segment).empty() ||
            segment.unknown_capacity || segment.capacity == 0) {
            continue;
        }
        eligible.push_back(&segment);
    }
    std::sort(
        eligible.begin(), eligible.end(), [](const auto* lhs, const auto* rhs) {
            if (lhs->segment_id != rhs->segment_id) {
                return lhs->segment_id < rhs->segment_id;
            }
            if (lhs->name != rhs->name) {
                return lhs->name < rhs->name;
            }
            if (lhs->host_id != rhs->host_id) {
                return lhs->host_id < rhs->host_id;
            }
            return EffectiveFailureDomain(*lhs) < EffectiveFailureDomain(*rhs);
        });

    constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
    PtBalanceSummary summary;
    summary.eligible_segments = eligible.size();
    summary.topology_fingerprint = kFnvOffset;
    summary.space_fingerprint = kFnvOffset;

    double min_utilization = 1.0;
    double max_utilization = 0.0;
    for (const auto* segment : eligible) {
        const uint64_t used = std::min(segment->used, segment->capacity);
        const double utilization =
            static_cast<double>(used) / static_cast<double>(segment->capacity);
        min_utilization = std::min(min_utilization, utilization);
        max_utilization = std::max(max_utilization, utilization);

        using namespace pt_rebuild_policy_detail;
        HashValue(&summary.topology_fingerprint, segment->segment_id.first);
        HashValue(&summary.topology_fingerprint, segment->segment_id.second);
        HashString(&summary.topology_fingerprint, segment->name);
        HashString(&summary.topology_fingerprint, segment->host_id);
        HashString(&summary.topology_fingerprint,
                   EffectiveFailureDomain(*segment));
        HashValue(&summary.topology_fingerprint, segment->capacity);

        HashValue(&summary.space_fingerprint, segment->segment_id.first);
        HashValue(&summary.space_fingerprint, segment->segment_id.second);
        HashValue(&summary.space_fingerprint, used);
        HashValue(&summary.space_fingerprint, segment->largest_free);
    }
    if (!eligible.empty()) {
        summary.min_utilization = min_utilization;
        summary.max_utilization = max_utilization;
        summary.utilization_spread = max_utilization - min_utilization;
    }
    return summary;
}

// Keeps scale-out catch-up separate from steady-state traffic weighting.
// Topology changes enter TARGET_HEADROOM when spread reaches
// kCatchUpEnterSpread. On unchanged topology, eviction/key deletion drift
// re-enters only when the minimum utilization is below the target as well.
// Once caught up, CAPACITY is restored after kCaughtUpRoundsToCapacity
// consecutive balanced rounds; the enter/exit spread gap provides hysteresis
// so ordinary eviction churn does not oscillate the mode.
class PtPlacementWeightPolicy final {
   public:
    struct Decision {
        PtSegmentWeightMode weight_mode{PtSegmentWeightMode::CAPACITY};
        bool mode_changed{false};
    };

    explicit PtPlacementWeightPolicy(double target_utilization)
        : target_utilization_(target_utilization) {
        if (!std::isfinite(target_utilization_) || target_utilization_ <= 0.0 ||
            target_utilization_ > 1.0) {
            throw std::invalid_argument(
                "PT target utilization must be in (0, 1]");
        }
    }

    Decision Observe(const PtBalanceSummary& summary) {
        const bool topology_changed =
            !previous_topology_fingerprint_ ||
            *previous_topology_fingerprint_ != summary.topology_fingerprint;
        const PtSegmentWeightMode previous_mode = weight_mode_;

        if (topology_changed) {
            caught_up_rounds_ = 0;
            weight_mode_ =
                summary.eligible_segments > 0 &&
                        summary.utilization_spread >= kCatchUpEnterSpread
                    ? PtSegmentWeightMode::TARGET_HEADROOM
                    : PtSegmentWeightMode::CAPACITY;
        } else if (weight_mode_ == PtSegmentWeightMode::CAPACITY &&
                   summary.eligible_segments > 0 &&
                   summary.min_utilization < target_utilization_ &&
                   summary.utilization_spread >= kCatchUpEnterSpread) {
            // Eviction or key deletion opened a large utilization gap below
            // the target on an unchanged topology: leave steady state and
            // rebalance. Gaps entirely above the target remain capacity-
            // weighted because TARGET_HEADROOM cannot reduce them.
            weight_mode_ = PtSegmentWeightMode::TARGET_HEADROOM;
            caught_up_rounds_ = 0;
        } else if (weight_mode_ == PtSegmentWeightMode::TARGET_HEADROOM) {
            const bool caught_up =
                summary.eligible_segments > 0 &&
                (summary.min_utilization >= target_utilization_ ||
                 summary.utilization_spread <= kCatchUpExitSpread);
            if (caught_up) {
                if (++caught_up_rounds_ >= kCaughtUpRoundsToCapacity) {
                    weight_mode_ = PtSegmentWeightMode::CAPACITY;
                    caught_up_rounds_ = 0;
                }
            } else {
                caught_up_rounds_ = 0;
            }
        }

        previous_topology_fingerprint_ = summary.topology_fingerprint;
        return Decision{weight_mode_, weight_mode_ != previous_mode};
    }

    static constexpr double kCatchUpEnterSpread = 0.05;
    static constexpr double kCatchUpExitSpread = 0.02;
    static constexpr uint32_t kCaughtUpRoundsToCapacity = 3;

   private:
    const double target_utilization_;
    std::optional<uint64_t> previous_topology_fingerprint_;
    PtSegmentWeightMode weight_mode_{PtSegmentWeightMode::CAPACITY};
    uint32_t caught_up_rounds_{0};
};

class PtRebuildCadence final {
   public:
    enum class Mode { NORMAL, FAST };

    struct Decision {
        Mode mode{Mode::NORMAL};
        std::chrono::milliseconds next_interval{0};
        bool materially_changed{true};
    };

    PtRebuildCadence(std::chrono::milliseconds normal_interval,
                     std::chrono::milliseconds fast_interval)
        : normal_interval_(normal_interval), fast_interval_(fast_interval) {
        if (normal_interval.count() <= 0 || fast_interval.count() <= 0) {
            throw std::invalid_argument(
                "PT rebuild intervals must be positive");
        }
        if (fast_interval > normal_interval) {
            throw std::invalid_argument(
                "PT fast rebuild interval must be <= normal interval");
        }
    }

    Decision Observe(const PtBalanceSummary& summary) {
        const bool materially_changed =
            !previous_ ||
            previous_->topology_fingerprint != summary.topology_fingerprint ||
            previous_->space_fingerprint != summary.space_fingerprint;

        if (!materially_changed) {
            balanced_rounds_ = 0;
            if (mode_ == Mode::FAST &&
                ++unchanged_rounds_ >= kUnchangedRoundsToBackoff) {
                mode_ = Mode::NORMAL;
                unchanged_rounds_ = 0;
            }
        } else {
            unchanged_rounds_ = 0;
            if (summary.utilization_spread >= kFastEnterSpread) {
                mode_ = Mode::FAST;
                balanced_rounds_ = 0;
            } else if (mode_ == Mode::FAST &&
                       summary.utilization_spread <= kFastExitSpread) {
                if (++balanced_rounds_ >= kBalancedRoundsToNormal) {
                    mode_ = Mode::NORMAL;
                    balanced_rounds_ = 0;
                }
            } else if (mode_ == Mode::FAST) {
                balanced_rounds_ = 0;
            }
        }
        previous_ = summary;
        return Decision{mode_,
                        mode_ == Mode::FAST ? fast_interval_ : normal_interval_,
                        materially_changed};
    }

    static constexpr double kFastEnterSpread = 0.05;
    static constexpr double kFastExitSpread = 0.02;
    static constexpr uint32_t kBalancedRoundsToNormal = 3;
    static constexpr uint32_t kUnchangedRoundsToBackoff = 3;

   private:
    const std::chrono::milliseconds normal_interval_;
    const std::chrono::milliseconds fast_interval_;
    std::optional<PtBalanceSummary> previous_;
    Mode mode_{Mode::NORMAL};
    uint32_t balanced_rounds_{0};
    uint32_t unchanged_rounds_{0};
};

inline const char* PtRebuildModeName(PtRebuildCadence::Mode mode) {
    return mode == PtRebuildCadence::Mode::FAST ? "FAST" : "NORMAL";
}

}  // namespace mooncake
