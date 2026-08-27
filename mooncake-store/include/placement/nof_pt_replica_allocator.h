#pragma once

#include <memory>
#include <vector>

#include <ylt/util/tl/expected.hpp>

#include "placement/pt_view.h"
#include "segment/pool_types.h"
#include "types.h"

namespace mooncake {

class ScopedPlacementAccess;

// PT-lane allocator for NoF replicas (RFC 0005 §8). Selects a PT row from
// the active immutable view, resolves its targets against the mounted NoF
// segments, and allocates every replica all-or-nothing with rollback on
// partial failure. Callers must already hold the NoFSegmentManager pool
// lock via ScopedPlacementAccess/ScopedNoFSegmentAccess so target pointers
// stay alive for the call duration.
class NofPtReplicaAllocator final {
   public:
    explicit NofPtReplicaAllocator(const PtViewManager& view_manager)
        : view_manager_(view_manager) {}

    // request.replica_count must be 1 (single-replica mode: uniform random
    // target within the row) or exactly view->configured_replica_num
    // (multi-replica mode: the whole row). Any other value is
    // INVALID_PARAMS. Returns NO_AVAILABLE_HANDLE when no usable view or
    // target exists; allocation failures roll back cleanly.
    tl::expected<std::vector<Replica>, ErrorCode> Allocate(
        ScopedPlacementAccess& placement,
        const SegmentAllocationRequest& request);

   private:
    const PtViewManager& view_manager_;
};

}  // namespace mooncake
