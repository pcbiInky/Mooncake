#pragma once

#include <chrono>

#include <glog/logging.h>

#include "background_worker.h"
#include "segment.h"
#include "placement/pt_view.h"
#include "placement/pt_view_builder.h"
#include "placement/pt_rebuild_policy.h"
#include "types.h"

namespace mooncake {

// Runs single-flight PT rebuilds on adaptive intervals and topology events.
class PtRebuildScheduler final {
   public:
    // Publishes through manager. Requires 0 < fast_interval <= normal_interval.
    PtRebuildScheduler(NoFSegmentManager& manager,
                       const PtBuildConfig& build_config,
                       std::chrono::milliseconds normal_interval,
                       std::chrono::milliseconds fast_interval)
        : manager_(manager),
          build_config_(build_config),
          placement_policy_(build_config.target_utilization),
          cadence_(normal_interval, fast_interval),
          worker_([this] { RebuildOnce(); }, normal_interval) {}
    ~PtRebuildScheduler() { Stop(); }

    PtRebuildScheduler(const PtRebuildScheduler&) = delete;
    PtRebuildScheduler& operator=(const PtRebuildScheduler&) = delete;

    void Start() { worker_.Start(); }

    void Stop() { worker_.Stop(); }

    // Queues an out-of-band rebuild through the single-flight worker.
    void RequestRebuild() { worker_.Schedule(); }

    std::shared_ptr<const PtView> GetActiveView() const {
        return manager_.GetPtViewManager().GetActiveView();
    }

    // Copies segment topology and capacity; unknown capacities are excluded.
    std::vector<PtSegmentSnapshot> CollectSegments() {
        std::vector<PtSegmentSnapshot> snapshots;
        std::vector<NoFSegmentManager::SegmentSpaceReport> reports;
        manager_.GetSegmentSpaceReports(reports);
        snapshots.reserve(reports.size());
        for (const auto& report : reports) {
            PtSegmentSnapshot snapshot;
            snapshot.segment_id = report.segment_id;
            snapshot.name = report.name;
            snapshot.host_id = report.host_id;
            snapshot.rack_id = report.rack_id;
            snapshot.unknown_capacity = report.unknown_capacity;
            snapshot.capacity = report.capacity;
            snapshot.used = report.used;
            snapshot.largest_free = report.largest_free;
            snapshots.push_back(std::move(snapshot));
        }
        return snapshots;
    }

   private:
    void RebuildOnce() {
        std::vector<PtSegmentSnapshot> segments = CollectSegments();
        const PtBalanceSummary balance = ComputePtBalanceSummary(segments);
        const auto placement = placement_policy_.Observe(balance);
        const auto cadence = cadence_.Observe(balance);
        worker_.SetPeriodicInterval(cadence.next_interval);

        auto& view_manager = manager_.GetPtViewManager();
        if (view_manager.GetActiveView() && !cadence.materially_changed &&
            !placement.mode_changed) {
            VLOG(1) << "PtViewBuilder: skip unchanged physical state, mode="
                    << PtRebuildModeName(cadence.mode)
                    << ", utilization_spread=" << balance.utilization_spread
                    << ", eligible=" << balance.eligible_segments
                    << ", weight_mode="
                    << PtSegmentWeightModeName(placement.weight_mode);
            return;
        }

        PtBuildConfig effective_config = build_config_;
        effective_config.segment_weight_mode = placement.weight_mode;
        PtBuildStats stats;
        auto view = PtViewBuilder::Build(segments, effective_config, &stats);
        if (!view) {
            LOG(WARNING) << "PtViewBuilder: no feasible view, total="
                         << stats.total_segments << ", topology_incomplete="
                         << stats.topology_incomplete
                         << ", eligible=" << stats.eligible_segments
                         << ", mode=" << PtRebuildModeName(cadence.mode)
                         << ", utilization_spread="
                         << balance.utilization_spread << ", weight_mode="
                         << PtSegmentWeightModeName(placement.weight_mode)
                         << ", full_target_fallback="
                         << stats.used_full_target_fallback;
            return;
        }
        view_manager.Publish(std::make_shared<const PtView>(std::move(*view)));
        LOG(INFO) << "PtViewBuilder: published view, epoch="
                  << view_manager.GetActiveView()->epoch
                  << ", rows=" << view_manager.GetActiveView()->entries.size()
                  << ", topology_incomplete=" << stats.topology_incomplete
                  << ", eligible=" << stats.eligible_segments
                  << ", duration_ns=" << stats.build_duration_ns
                  << ", mode=" << PtRebuildModeName(cadence.mode)
                  << ", next_interval_ms=" << cadence.next_interval.count()
                  << ", utilization_spread=" << balance.utilization_spread
                  << ", min_utilization=" << balance.min_utilization
                  << ", weight_mode="
                  << PtSegmentWeightModeName(placement.weight_mode)
                  << ", full_target_fallback=" << stats.used_full_target_fallback;
    }

    NoFSegmentManager& manager_;
    const PtBuildConfig build_config_;
    PtPlacementWeightPolicy placement_policy_;
    PtRebuildCadence cadence_;
    BackgroundWorker worker_;
};

}  // namespace mooncake
