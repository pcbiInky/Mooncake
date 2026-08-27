#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "allocator.h"

namespace mooncake {

enum class AllocationTargetKind {
    STANDARD = 0,
    CXL,
};

// A stable, non-owning allocation endpoint published to PlacementIndex.
// The owning RegionResource must outlive every placement reference to it.
class AllocationTarget final {
   public:
    AllocationTarget(BufferAllocatorBase* allocator, AllocationTargetKind kind,
                     std::string cxl_binding_name = {},
                     UUID region_id = UUID{0, 0})
        : allocator_(allocator),
          kind_(kind),
          cxl_binding_name_(std::move(cxl_binding_name)),
          region_id_(region_id) {}

    std::unique_ptr<AllocatedBuffer> Allocate(size_t size) const {
        auto buffer = allocator_->allocate(size);
        if (buffer && kind_ == AllocationTargetKind::CXL) [[unlikely]] {
            buffer->change_to_cxl(cxl_binding_name_);
        }
        return buffer;
    }

    size_t Capacity() const { return allocator_->capacity(); }
    size_t Used() const { return allocator_->size(); }
    size_t LargestFreeRegion() const {
        return allocator_->getLargestFreeRegion();
    }
    AllocationTargetKind kind() const noexcept { return kind_; }
    // Physical region (segment) identity. PT placement (RFC 0005 §8)
    // resolves PtTarget.region_id against this field so a PT row never
    // lands two replicas on the same physical segment even when segments
    // share a PlacementIndex name.
    const UUID& region_id() const noexcept { return region_id_; }

   private:
    BufferAllocatorBase* allocator_;
    AllocationTargetKind kind_;
    std::string cxl_binding_name_;
    UUID region_id_;
};

}  // namespace mooncake
