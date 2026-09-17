#include "placement/nof_pt_replica_allocator.h"

#include <algorithm>
#include <numeric>

#include "random.h"

namespace mooncake {

namespace {

constexpr size_t kMaxRowAttempts = 3;

}  // namespace

tl::expected<std::vector<Replica>, ErrorCode> NofPtReplicaAllocator::Allocate(
    size_t size, size_t replica_count,
    const AllocateTargetFn& allocate_target) {
    if (size == 0 || replica_count == 0 || !allocate_target) {
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }

    auto view = view_manager_.GetActiveView();
    if (!view || view->entries.empty() || view->configured_replica_num == 0) {
        return tl::make_unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
    }

    // Accept one target or the configured row width.
    if (replica_count != 1 && replica_count != view->configured_replica_num) {
        return tl::make_unexpected(ErrorCode::INVALID_PARAMS);
    }

    // Visit up to three distinct rows without allocating an index vector.
    const size_t row_count = view->entries.size();
    const size_t max_attempts = std::min(kMaxRowAttempts, row_count);
    const size_t start_row = randomIndex(row_count);
    size_t row_stride = 1;
    if (row_count > 1) {
        do {
            row_stride = randomUniform<size_t>(1, row_count - 1);
        } while (std::gcd(row_stride, row_count) != 1);
    }
    for (size_t attempt = 0, row_index = start_row; attempt < max_attempts;
         ++attempt, row_index = row_index >= row_count - row_stride
                                    ? row_index - (row_count - row_stride)
                                    : row_index + row_stride) {
        const PtEntry& entry = view->entries[row_index];
        if (entry.replicas.size() != view->configured_replica_num) {
            continue;
        }

        std::vector<const PtTarget*> row_targets;
        if (replica_count == 1) {
            row_targets.push_back(
                &entry.replicas[randomIndex(entry.replicas.size())]);
        } else {
            row_targets.reserve(entry.replicas.size());
            for (const auto& target : entry.replicas) {
                row_targets.push_back(&target);
            }
        }

        // Resolve and allocate the whole row; Replica destruction rolls back
        // partial failures.
        std::vector<Replica> replicas;
        replicas.reserve(row_targets.size());
        bool row_ok = true;
        for (const PtTarget* target : row_targets) {
            auto buffer = allocate_target(*target, size);
            if (!buffer) {
                row_ok = false;
                break;
            }
            replicas.emplace_back(std::move(buffer), ReplicaStatus::PROCESSING,
                                  ReplicaType::NOF_SSD);
        }
        if (row_ok) {
            return replicas;
        }
        // Try another row after rollback.
    }
    return tl::make_unexpected(ErrorCode::NO_AVAILABLE_HANDLE);
}

}  // namespace mooncake
