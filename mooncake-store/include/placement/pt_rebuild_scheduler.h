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

// Background PT view scheduler (RFC 0005 §9.1). Runs a single-flight builder
// thread on an adaptive interval and on-demand events (mount/unmount/topology
// change). It is created only when foreground PT allocation is enabled.
class PtRebuildScheduler final {
   public:
    // The scheduler publishes into the manager's PtViewManager so the
    // foreground PT allocator (which reads manager.GetPtViewManager())
    // observes the rebuilt views. The scheduler itself never keeps a
    // private view manager.
    PtRebuildScheduler(NoFSegmentManager& manager,
                       const PtBuildConfig& build_config,
                       std::chrono::milliseconds rebuild_interval)
        : manager_(manager),
          build_config_(build_config),
          cadence_(rebuild_interval,
                   std::min(rebuild_interval,
                            std::chrono::milliseconds(1000))),
          worker_([this] { RebuildOnce(); }, rebuild_interval) {}
    ~PtRebuildScheduler() { Stop(); }

    PtRebuildScheduler(const PtRebuildScheduler&) = delete;
    PtRebuildScheduler& operator=(const PtRebuildScheduler&) = delete;

    void Start() { worker_.Start(); }

    void Stop() { worker_.Stop(); }

    // Request an out-of-band rebuild (e.g. segment mounted/unmounted). The
    // rebuild itself still goes through the single-flight loop.
    void RequestRebuild() { worker_.Schedule(); }

    std::shared_ptr<const PtView> GetActiveView() const {
        return manager_.GetPtViewManager().GetActiveView();
    }

    // Snapshot of the NoF segments for the builder. Must be called while the
    // manager's pool lock is not held by the caller. Segments without
    // bin-precise capacity reports (Cachelib) are reported with
    // unknown_capacity and excluded from PT eligibility by the builder.
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
        const auto cadence = cadence_.Observe(balance);
        worker_.SetPeriodicInterval(cadence.next_interval);

        auto& view_manager = manager_.GetPtViewManager();
        if (view_manager.GetActiveView() && !cadence.materially_changed) {
            VLOG(1) << "PtViewBuilder: skip unchanged physical state, mode="
                    << PtRebuildModeName(cadence.mode)
                    << ", utilization_spread="
                    << balance.utilization_spread
                    << ", eligible=" << balance.eligible_segments;
            return;
        }

        PtBuildStats stats;
        auto view = PtViewBuilder::Build(segments, build_config_, &stats);
        if (!view) {
            LOG(WARNING) << "PtViewBuilder: no feasible view, total="
                         << stats.total_segments
                         << ", topology_incomplete="
                         << stats.topology_incomplete
                         << ", eligible=" << stats.eligible_segments
                         << ", mode=" << PtRebuildModeName(cadence.mode)
                         << ", utilization_spread="
                         << balance.utilization_spread;
            return;
        }
        view_manager.Publish(std::make_shared<const PtView>(std::move(*view)));
        LOG(INFO) << "PtViewBuilder: published view, epoch="
                  << view_manager.GetActiveView()->epoch
                  << ", policies=" << stats.policies_built
                  << ", topology_incomplete=" << stats.topology_incomplete
                  << ", eligible=" << stats.eligible_segments
                  << ", duration_ns=" << stats.build_duration_ns
                  << ", mode=" << PtRebuildModeName(cadence.mode)
                  << ", next_interval_ms=" << cadence.next_interval.count()
                  << ", utilization_spread="
                  << balance.utilization_spread;
    }

    NoFSegmentManager& manager_;
    const PtBuildConfig build_config_;
    PtRebuildCadence cadence_;
    BackgroundWorker worker_;
};

}  // namespace mooncake
