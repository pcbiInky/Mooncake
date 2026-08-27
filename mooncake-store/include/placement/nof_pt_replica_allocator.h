#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <ylt/util/tl/expected.hpp>

#include "placement/pt_view.h"
#include "replica.h"
#include "types.h"

namespace mooncake {

// PT-lane allocator for NoF replicas (RFC 0005 §8). Selects a PT row from
// the active immutable view and allocates every replica all-or-nothing with
// rollback on partial failure. The manager supplies a resolver while holding
// its shared segment lock, so physical segment identity stays stable.
class NofPtReplicaAllocator final {
   public:
    using AllocateTargetFn = std::function<std::unique_ptr<AllocatedBuffer>(
        const PtTarget&, size_t)>;

    explicit NofPtReplicaAllocator(const PtViewManager& view_manager)
        : view_manager_(view_manager) {}

    // request.replica_count must be 1 (single-replica mode: uniform random
    // target within the row) or exactly view->configured_replica_num
    // (multi-replica mode: the whole row). Any other value is
    // INVALID_PARAMS. Returns NO_AVAILABLE_HANDLE when no usable view or
    // target exists; allocation failures roll back cleanly.
    tl::expected<std::vector<Replica>, ErrorCode> Allocate(
        size_t size, size_t replica_count,
        const AllocateTargetFn& allocate_target);

   private:
    const PtViewManager& view_manager_;
};

}  // namespace mooncake
