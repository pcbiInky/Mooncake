#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <ylt/util/tl/expected.hpp>

#include "placement/pt_view.h"
#include "replica.h"
#include "types.h"

namespace mooncake {

// Allocates NoF replicas from an immutable PT row with rollback on failure.
class NofPtReplicaAllocator final {
   public:
    using AllocateTargetFn = std::function<std::unique_ptr<AllocatedBuffer>(
        const PtTarget&, size_t)>;

    explicit NofPtReplicaAllocator(const PtViewManager& view_manager)
        : view_manager_(view_manager) {}

    // Accepts one replica or the configured row width. Tries up to three
    // distinct rows; partial allocations roll back automatically.
    tl::expected<std::vector<Replica>, ErrorCode> Allocate(
        size_t size, size_t replica_count,
        const AllocateTargetFn& allocate_target);

   private:
    const PtViewManager& view_manager_;
};

}  // namespace mooncake
