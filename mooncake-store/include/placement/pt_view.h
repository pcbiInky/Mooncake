#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "types.h"

namespace mooncake {

// Immutable NoF placement table published through atomic shared snapshots.

struct PtTarget {
    UUID region_id;  // NoF segment id
    std::string name;  // Identity check during target resolution
    std::string host_id;
    std::string rack_id;
    // Uses rack_id when present, otherwise host_id.
    std::string failure_domain_id;
};

struct PtEntry {
    uint32_t pt_id{0};
    std::vector<PtTarget> replicas;
};

struct PtView {
    uint64_t epoch{0};
    uint64_t created_at_ns{0};
    uint32_t pt_count{0};
    uint32_t configured_replica_num{0};
    uint64_t seed{0};
    // Segment quotas are weighted by current free bytes.
    std::vector<PtEntry> entries;  // entries.size() == pt_count
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
