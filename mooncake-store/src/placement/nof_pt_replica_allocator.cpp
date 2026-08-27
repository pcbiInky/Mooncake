#include "placement/nof_pt_replica_allocator.h"

#include <algorithm>
#include <numeric>

#include "placement/index.h"
#include "random.h"
#include "replica.h"

namespace mooncake {

tl::expected<std::vector<Replica>, ErrorCode> NofPtReplicaAllocator::Allocate(
    ScopedPlacementAccess& placement,
    const SegmentAllocationRequest& request) {
    if (request.size == 0 || request.replica_count == 0) {
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }

    auto view = view_manager_.GetActiveView();
    if (!view || view->policies.empty() ||
        view->configured_replica_num == 0) {
        return tl::make_unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
    }

    // Defensive row-width validation (RFC 0005 §8.1): single-replica mode
    // uses one uniform-random target within the row; multi-replica mode
    // requires exactly the configured width. The Master already validated
    // 0/1/R_config before entering the PT lane.
    if (request.replica_count != 1 &&
        request.replica_count != view->configured_replica_num) {
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }

    // Size-class policy lookup. Requests beyond the largest class are
    // rejected rather than silently placed in the wrong class.
    const PtPolicyView* policy = view->FindPolicy(request.size);
    if (!policy || policy->entries.empty()) {
        return tl::make_unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
    }

    // Row retry (RFC 0005 §8.2): try up to 3 distinct PT rows. A failed
    // row rolls back cleanly and a different row is drawn without
    // replacement.
    constexpr size_t kMaxRowAttempts = 3;
    const size_t max_attempts =
        std::min<size_t>(kMaxRowAttempts, policy->entries.size());
    std::vector<size_t> row_indices(policy->entries.size());
    std::iota(row_indices.begin(), row_indices.end(), 0);
    for (size_t attempt = 0; attempt < max_attempts; ++attempt) {
        // Draw without replacement by lazily shuffling the untried suffix.
        // This gives bounded work even when most rows have already failed.
        const size_t selected =
            randomUniform(attempt, row_indices.size() - 1);
        std::swap(row_indices[attempt], row_indices[selected]);
        const size_t row_index = row_indices[attempt];
        const PtEntry& entry = policy->entries[row_index];
        if (entry.replicas.size() != view->configured_replica_num) {
            continue;
        }

        std::vector<const PtTarget*> row_targets;
        if (request.replica_count == 1) {
            row_targets.push_back(
                &entry.replicas[randomIndex(entry.replicas.size())]);
        } else {
            row_targets.reserve(entry.replicas.size());
            for (const auto& target : entry.replicas) {
                row_targets.push_back(&target);
            }
        }

        // All-or-nothing allocation (RFC 0005 §8.2): resolve each target
        // through the NoF placement index by name AND region_id; any
        // failure destroys the already created replicas, whose
        // AllocatedBuffer destructors roll the offsets back to their
        // allocators.
        auto read_view = placement.view();
        std::vector<Replica> replicas;
        replicas.reserve(row_targets.size());
        bool row_ok = true;
        for (const PtTarget* target : row_targets) {
            PlacementGroup* group = read_view.Find(target->name);
            if (!group || group->targets.empty()) {
                row_ok = false;
                break;
            }
            // RFC 0005 §8: the PT row names the exact physical segment.
            // Match by region_id so two replicas of one row can never
            // land on the same physical segment even if segments share
            // a PlacementIndex name.
            bool allocated = false;
            for (AllocationTarget* alloc_target : group->targets) {
                if (!(alloc_target->region_id() == target->region_id)) {
                    continue;
                }
                auto buffer = alloc_target->Allocate(request.size);
                if (buffer) {
                    replicas.emplace_back(std::move(buffer),
                                          ReplicaStatus::PROCESSING,
                                          ReplicaType::NOF_SSD);
                    allocated = true;
                    break;
                }
            }
            if (!allocated) {
                row_ok = false;
                break;
            }
        }
        if (row_ok) {
            return replicas;
        }
        // Partial replicas roll back via their AllocatedBuffer destructors
        // as `replicas` goes out of scope; try a different row.
    }
    return tl::make_unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
}

}  // namespace mooncake
