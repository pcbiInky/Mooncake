#include "nof_segment_manager.h"

#include <algorithm>
#include <numeric>

#include "master_metric_manager.h"
#include "placement/nof_pt_replica_allocator.h"

namespace mooncake {

NoFSegmentManager::NoFSegmentManager(
    BufferAllocatorType memory_allocator)
    : memory_allocator_(memory_allocator) {}

NoFSegmentManager::~NoFSegmentManager() = default;

void NoFSegmentManager::SetPtEnabled(bool enabled) {
    if (enabled && !pt_allocator_) {
        pt_allocator_ = std::make_unique<NofPtReplicaAllocator>(pt_view_manager_);
    }
    pt_enabled_.store(enabled, std::memory_order_release);
}

tl::expected<std::vector<Replica>, ErrorCode> NoFSegmentManager::Allocate(
    PlacementPolicyType policy_type, const SegmentAllocationRequest& request) {
    ScopedPlacementAccess placement(placement_index_, pool_mutex_);
    if (pt_enabled_.load(std::memory_order_acquire)) {
        return pt_allocator_->Allocate(placement, request);
    }
    const ReplicaAllocationRequest resolved{
        .size = request.size,
        .replica_count = request.replica_count,
        .preferred_group = request.preferred_group,
        .preferred_groups = request.preferred_groups,
        .resolved_preferred_groups = {},
        .excluded_groups = request.excluded_groups,
        .replica_type = request.replica_type,
    };
    return replica_allocator_.Allocate(placement, policy_type, resolved);
}

ErrorCode ScopedNoFSegmentAccess::MountSegment(const NoFSegment& segment,
                                               const UUID& client_id) {
    if (segment.size == 0) {
        return ErrorCode::INVALID_PARAMS;
    }
    if (nof_segment_manager_->mounted_segments_.contains(segment.id)) {
        return ErrorCode::SEGMENT_ALREADY_EXISTS;
    }
    for (const auto& [_, mounted] : nof_segment_manager_->mounted_segments_) {
        if (mounted.status == SegmentStatus::OK &&
            mounted.segment.te_endpoint == segment.te_endpoint) {
            return ErrorCode::SEGMENT_ALREADY_EXISTS;
        }
    }

    std::shared_ptr<BufferAllocatorBase> allocator;
    try {
        if (nof_segment_manager_->memory_allocator_ ==
            BufferAllocatorType::CACHELIB) {
            allocator = std::make_shared<CachelibBufferAllocator>(
                segment.name, segment.base, segment.size, segment.te_endpoint,
                ReplicaType::NOF_SSD);
        } else if (nof_segment_manager_->memory_allocator_ ==
                   BufferAllocatorType::OFFSET) {
            allocator = std::make_shared<OffsetBufferAllocator>(
                segment.name, segment.base, segment.size, segment.te_endpoint,
                ReplicaType::NOF_SSD);
        } else {
            return ErrorCode::INVALID_PARAMS;
        }
    } catch (...) {
        return ErrorCode::INVALID_PARAMS;
    }
    auto target = std::make_unique<AllocationTarget>(
        allocator.get(), AllocationTargetKind::STANDARD, std::string(),
        segment.id);
    nof_segment_manager_->placement_index_.AddTarget(segment.name,
                                                     target.get());
    nof_segment_manager_->client_segments_[client_id].push_back(segment.id);
    nof_segment_manager_->client_by_name_[segment.name] = client_id;
    nof_segment_manager_->mounted_segments_.emplace(
        segment.id, MountedNoFSegment{segment, client_id, SegmentStatus::OK,
                                      std::move(allocator), std::move(target)});
    MasterMetricManager::instance().inc_total_nof_capacity(segment.name,
                                                           segment.size);
    return ErrorCode::OK;
}

ErrorCode ScopedNoFSegmentAccess::ReMountSegment(
    const std::vector<NoFSegment>& segments, const UUID& client_id) {
    for (const auto& segment : segments) {
        auto result = MountSegment(segment, client_id);
        if (result == ErrorCode::UNAVAILABLE_IN_CURRENT_STATUS ||
            result == ErrorCode::INTERNAL_ERROR) {
            return result;
        }
    }
    return ErrorCode::OK;
}

ErrorCode ScopedNoFSegmentAccess::PrepareUnmountSegment(
    const UUID& segment_id, size_t& metrics_dec_capacity) {
    auto mounted = nof_segment_manager_->mounted_segments_.find(segment_id);
    if (mounted == nof_segment_manager_->mounted_segments_.end()) {
        return ErrorCode::SEGMENT_NOT_FOUND;
    }
    if (mounted->second.status == SegmentStatus::UNMOUNTING) {
        return ErrorCode::UNAVAILABLE_IN_CURRENT_STATUS;
    }
    metrics_dec_capacity = mounted->second.segment.size;
    if (mounted->second.allocation_target) {
        nof_segment_manager_->placement_index_.RemoveTarget(
            mounted->second.segment.name,
            mounted->second.allocation_target.get());
    }
    mounted->second.allocation_target.reset();
    mounted->second.buf_allocator.reset();
    mounted->second.status = SegmentStatus::UNMOUNTING;
    return ErrorCode::OK;
}

ErrorCode ScopedNoFSegmentAccess::CommitUnmountSegment(
    const UUID& segment_id, const UUID& client_id,
    const size_t& metrics_dec_capacity) {
    auto mounted = nof_segment_manager_->mounted_segments_.find(segment_id);
    if (mounted == nof_segment_manager_->mounted_segments_.end()) {
        return ErrorCode::SEGMENT_NOT_FOUND;
    }
    const std::string name = mounted->second.segment.name;
    auto client = nof_segment_manager_->client_segments_.find(client_id);
    if (client != nof_segment_manager_->client_segments_.end()) {
        auto id =
            std::find(client->second.begin(), client->second.end(), segment_id);
        if (id != client->second.end()) {
            client->second.erase(id);
        }
        if (client->second.empty()) {
            nof_segment_manager_->client_segments_.erase(client);
        }
    }
    nof_segment_manager_->mounted_segments_.erase(mounted);
    bool name_exists = false;
    for (const auto& [_, other] : nof_segment_manager_->mounted_segments_) {
        if (other.segment.name == name) {
            nof_segment_manager_->client_by_name_[name] = other.client_id;
            name_exists = true;
            break;
        }
    }
    if (!name_exists) {
        nof_segment_manager_->client_by_name_.erase(name);
    }
    MasterMetricManager::instance().dec_total_nof_capacity(
        name, metrics_dec_capacity);
    MasterMetricManager::instance().remove_nof_segment_metrics(name);
    return ErrorCode::OK;
}

ErrorCode ScopedNoFSegmentAccess::GetClientSegments(
    const UUID& client_id, std::vector<NoFSegment>& segments) const {
    auto client = nof_segment_manager_->client_segments_.find(client_id);
    if (client == nof_segment_manager_->client_segments_.end()) {
        return ErrorCode::SEGMENT_NOT_FOUND;
    }
    segments.clear();
    for (const auto& id : client->second) {
        auto mounted = nof_segment_manager_->mounted_segments_.find(id);
        if (mounted != nof_segment_manager_->mounted_segments_.end()) {
            segments.push_back(mounted->second.segment);
        }
    }
    return ErrorCode::OK;
}

ErrorCode ScopedNoFSegmentAccess::GetMountedSegments(
    std::vector<MountedNoFSegmentSnapshot>& segments) const {
    segments.clear();
    for (const auto& [id, mounted] : nof_segment_manager_->mounted_segments_) {
        segments.push_back(
            {id, mounted.client_id, mounted.segment, mounted.status});
    }
    return ErrorCode::OK;
}

void NoFSegmentManager::GetSegmentSpaceReports(
    std::vector<SegmentSpaceReport>& reports) const {
    std::shared_lock lock(pool_mutex_);
    reports.clear();
    reports.reserve(mounted_segments_.size());
    for (const auto& [id, mounted] : mounted_segments_) {
        SegmentSpaceReport report;
        report.segment_id = id;
        report.name = mounted.segment.name;
        report.host_id = mounted.segment.host_id;
        if (mounted.buf_allocator) {
            const size_t capacity = mounted.buf_allocator->capacity();
            if (capacity == kAllocatorUnknownFreeSpace) {
                report.unknown_capacity = true;
            } else {
                report.capacity = capacity;
                // Prefer the bin-precise total free space from the offset
                // allocator over capacity - Used(): cur_size_ counts
                // requested bytes only and misses bin fragmentation.
                if (const auto* offset_allocator =
                        dynamic_cast<const OffsetBufferAllocator*>(
                            mounted.buf_allocator.get())) {
                    const auto storage =
                        offset_allocator->getOffsetAllocator()
                            ->storageReport();
                    report.used =
                        capacity > storage.totalFreeSpace
                            ? capacity - storage.totalFreeSpace
                            : 0;
                    report.largest_free = storage.largestFreeRegion;
                } else {
                    const size_t used = mounted.buf_allocator->size();
                    report.used = used < capacity ? used : capacity;
                    report.largest_free =
                        mounted.buf_allocator->getLargestFreeRegion();
                    if (report.largest_free == kAllocatorUnknownFreeSpace) {
                        report.unknown_capacity = true;
                    }
                }
            }
        } else {
            report.unknown_capacity = true;
        }
        reports.push_back(std::move(report));
    }
}

ErrorCode ScopedNoFSegmentAccess::GetAllSegments(
    std::vector<std::string>& all_segments) {
    all_segments.clear();
    for (const auto& [_, mounted] : nof_segment_manager_->mounted_segments_) {
        if (mounted.status == SegmentStatus::OK) {
            all_segments.push_back(mounted.segment.name);
        }
    }
    return ErrorCode::OK;
}

ErrorCode ScopedNoFSegmentAccess::QuerySegments(const std::string& name,
                                                size_t& used,
                                                size_t& capacity) {
    auto* group = nof_segment_manager_->placement_index_.GetView().Find(name);
    if (!group) {
        return ErrorCode::SEGMENT_NOT_FOUND;
    }
    used = 0;
    capacity = 0;
    for (const auto* target : group->targets) {
        used += target->Used();
        capacity += target->Capacity();
    }
    return capacity == 0 ? ErrorCode::SEGMENT_NOT_FOUND : ErrorCode::OK;
}

void NoFSegmentManager::GetMountedSegmentsSnapshot(
    std::vector<MountedNoFSegmentSnapshot>& segments) const {
    std::shared_lock lock(pool_mutex_);
    segments.clear();
    segments.reserve(mounted_segments_.size());
    for (const auto& [id, mounted] : mounted_segments_) {
        segments.push_back(
            {id, mounted.client_id, mounted.segment, mounted.status});
    }
}

}  // namespace mooncake
