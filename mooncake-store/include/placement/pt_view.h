#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "types.h"

namespace mooncake {

// Immutable placement-table structures for the NoF PT lane (RFC 0005).
// A PtView is published atomically by PtViewBuilder; the foreground
// allocator only takes shared snapshots and never mutates them.

struct PtTarget {
    UUID region_id;  // NoF segment id
    // Segment name used to resolve this target through the NoF placement
    // index (PlacementIndex is keyed by name). Stable for the lifetime of
    // a mounted segment; a renamed/remounted segment invalidates the view
    // via epoch/resource change detection.
    std::string name;
    std::string host_id;
    std::string rack_id;
    // Effective fault domain for this view: "rack:<rack_id>" when rack_id is
    // present, otherwise "host:<host_id>" (best-effort fallback).
    std::string failure_domain_id;
};

struct PtEntry {
    uint32_t pt_id{0};
    std::vector<PtTarget> replicas;
};

struct PtPolicyView {
    // Requests with aligned size in (min_exclusive, max_inclusive] use this
    // policy.
    uint64_t min_aligned_request_size_exclusive{0};
    uint64_t max_aligned_request_size_inclusive{0};
    std::vector<PtEntry> entries;  // entries.size() == pt_count
};

struct PtView {
    uint64_t epoch{0};
    uint64_t resource_epoch{0};
    uint64_t created_at_ns{0};
    uint32_t pt_count{0};
    uint32_t configured_replica_num{0};
    uint64_t seed{0};
    std::vector<PtPolicyView> policies;

    // Returns the policy covering the given aligned request size, or nullptr
    // when the size exceeds the largest class.
    const PtPolicyView* FindPolicy(uint64_t aligned_request_size) const {
        for (const auto& policy : policies) {
            if (aligned_request_size > policy.min_aligned_request_size_exclusive &&
                aligned_request_size <=
                    policy.max_aligned_request_size_inclusive) {
                return &policy;
            }
        }
        return nullptr;
    }
};

// Atomic publisher/reader for PtView snapshots.
class PtViewManager final {
   public:
    PtViewManager() = default;

    PtViewManager(const PtViewManager&) = delete;
    PtViewManager& operator=(const PtViewManager&) = delete;

    std::shared_ptr<const PtView> GetActiveView() const {
        return std::atomic_load(&active_view_);
    }

    void Publish(std::shared_ptr<const PtView> view) {
        std::atomic_store(&active_view_, std::move(view));
    }

   private:
    std::shared_ptr<const PtView> active_view_;
};

}  // namespace mooncake
