#include "placement/replica_allocator.h"
#include "placement/nof_pt_replica_allocator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#include "local_ssd/manager.h"
#include "random.h"
#include "test_buffer_allocator.h"

namespace mooncake::test {
namespace {

constexpr size_t kCapacity = 1U << 20;

class PlacementState {
   public:
    TestBufferAllocator* Add(
        std::string logical_name, std::string endpoint,
        size_t capacity = kCapacity, size_t used = 0,
        AllocationTargetKind kind = AllocationTargetKind::STANDARD,
        std::string cxl_binding = {}, UUID region_id = UUID{0, 0}) {
        auto allocator = std::make_shared<TestBufferAllocator>(
            logical_name, std::move(endpoint), capacity,
            kind == AllocationTargetKind::CXL ? DEFAULT_CXL_BASE + cxl_offset_
                                              : next_base_);
        if (kind == AllocationTargetKind::CXL) {
            cxl_offset_ += capacity + 4096;
        } else {
            next_base_ += capacity + 4096;
        }
        allocator->SetUsed(used);
        auto target = std::make_unique<AllocationTarget>(
            allocator.get(), kind, std::move(cxl_binding), region_id);
        EXPECT_TRUE(index.AddTarget(logical_name, target.get()));
        auto* result = allocator.get();
        allocators.push_back(std::move(allocator));
        targets.push_back(std::move(target));
        return result;
    }

    void SetOwner(const std::string& name, UUID client_id) {
        owners[name] = client_id;
    }

    ScopedPlacementAccess Access() {
        return ScopedPlacementAccess(index, hosts, owners, mutex);
    }

    PlacementIndex index;
    HostRegionIndex hosts;
    ClientByRegionName owners;
    LocalSsdManager local_ssd;
    std::vector<std::shared_ptr<TestBufferAllocator>> allocators;
    std::vector<std::unique_ptr<AllocationTarget>> targets;
    std::shared_mutex mutex;

   private:
    uintptr_t next_base_{0x100000000ULL};
    uintptr_t cxl_offset_{0};
};

ReplicaAllocationRequest Request(size_t replicas = 1) {
    ReplicaAllocationRequest request;
    request.size = 4096;
    request.replica_count = replicas;
    return request;
}

UUID RegionId(uint64_t value) { return UUID{value, 1}; }

std::set<std::string> Endpoints(const std::vector<Replica>& replicas) {
    std::set<std::string> result;
    for (const auto& replica : replicas) {
        result.insert(ReplicaEndpoint(replica));
    }
    return result;
}

}  // namespace

TEST(PlacementIndexTest, PointersStayStableAcrossGrowthAndSwapPopRemoval) {
    PlacementState state;
    auto* a = state.Add("a", "a");
    auto* b = state.Add("b", "b");
    auto* c = state.Add("c", "c");
    (void)a;
    (void)b;
    (void)c;
    auto view = state.index.GetView();
    PlacementGroup* a_group = view.Find("a");
    PlacementGroup* c_group = view.Find("c");
    ASSERT_NE(a_group, nullptr);
    ASSERT_NE(c_group, nullptr);

    for (size_t i = 0; i < 64; ++i) {
        state.Add("extra-" + std::to_string(i),
                  "endpoint-" + std::to_string(i));
    }
    EXPECT_EQ(state.index.GetView().Find("a"), a_group);
    EXPECT_EQ(state.index.GetView().Find("c"), c_group);

    ASSERT_TRUE(state.index.RemoveTarget("b", state.targets[1].get()));
    EXPECT_EQ(state.index.GetView().Find("a"), a_group);
    EXPECT_EQ(state.index.GetView().Find("c"), c_group);
    EXPECT_EQ(state.index.GetView().Find("b"), nullptr);
}

TEST(ReplicaAllocatorTest, RejectsEmptyAndInvalidRequests) {
    PlacementState empty;
    ReplicaAllocator allocator;
    auto request = Request();
    auto access = empty.Access();
    EXPECT_EQ(allocator.Allocate(access, PlacementPolicyType::RANDOM, request)
                  .error(),
              ErrorCode::NO_AVAILABLE_HANDLE);

    PlacementState state;
    state.Add("one", "one");
    auto invalid = Request();
    invalid.size = 0;
    auto populated = state.Access();
    EXPECT_EQ(
        allocator.Allocate(populated, PlacementPolicyType::RANDOM, invalid)
            .error(),
        ErrorCode::INVALID_PARAMS);
}

TEST(NofPtReplicaAllocatorTest, RetriesADistinctRowAfterAllocationFailure) {
    PlacementState state;
    auto* bad1 = state.Add("bad1", "bad1", kCapacity, 0,
                           AllocationTargetKind::STANDARD, {}, RegionId(1));
    auto* bad2 = state.Add("bad2", "bad2", kCapacity, 0,
                           AllocationTargetKind::STANDARD, {}, RegionId(2));
    state.Add("good1", "good1", kCapacity, 0,
              AllocationTargetKind::STANDARD, {}, RegionId(3));
    state.Add("good2", "good2", kCapacity, 0,
              AllocationTargetKind::STANDARD, {}, RegionId(4));
    bad1->SetAlwaysFail();
    bad2->SetAlwaysFail();

    PtViewManager view_manager;
    auto view = std::make_shared<PtView>();
    view->configured_replica_num = 2;
    PtPolicyView policy;
    policy.min_aligned_request_size_exclusive = 0;
    policy.max_aligned_request_size_inclusive = 8192;
    policy.entries.resize(2);

    // Put the bad row at the deterministic first draw. The retry must select
    // the only remaining row and return its allocation.
    RandomEngine probe(42);
    const size_t first_row =
        randomUniform<size_t>(0, policy.entries.size() - 1, probe);
    const size_t good_row = 1 - first_row;
    policy.entries[first_row].pt_id = static_cast<uint32_t>(first_row);
    policy.entries[first_row].replicas = {
        PtTarget{RegionId(1), "bad1", "host-a", "host-a"},
        PtTarget{RegionId(2), "bad2", "host-b", "host-b"},
    };
    policy.entries[good_row].pt_id = static_cast<uint32_t>(good_row);
    policy.entries[good_row].replicas = {
        PtTarget{RegionId(3), "good1", "host-c", "host-c"},
        PtTarget{RegionId(4), "good2", "host-d", "host-d"},
    };
    view->policies.push_back(std::move(policy));
    view_manager.Publish(std::move(view));

    threadLocalRandomEngine().seed(42);
    NofPtReplicaAllocator allocator(view_manager);
    SegmentAllocationRequest request;
    request.size = 4096;
    request.replica_count = 1;
    request.replica_type = ReplicaType::NOF_SSD;
    auto access = state.Access();
    auto result = allocator.Allocate(access, request);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_TRUE(ReplicaEndpoint(result->front()) == "good1" ||
                ReplicaEndpoint(result->front()) == "good2");
    EXPECT_EQ(bad1->allocation_calls() + bad2->allocation_calls(), 1U);
}

TEST(NofPtReplicaAllocatorTest, RollsBackPartialMultiReplicaRow) {
    PlacementState state;
    auto* first = state.Add("first", "first", kCapacity, 0,
                            AllocationTargetKind::STANDARD, {}, RegionId(1));
    auto* failed = state.Add("failed", "failed", kCapacity, 0,
                             AllocationTargetKind::STANDARD, {}, RegionId(2));
    failed->SetAlwaysFail();

    PtViewManager view_manager;
    auto view = std::make_shared<PtView>();
    view->configured_replica_num = 2;
    PtPolicyView policy;
    policy.min_aligned_request_size_exclusive = 0;
    policy.max_aligned_request_size_inclusive = 8192;
    PtEntry entry;
    entry.pt_id = 0;
    entry.replicas = {
        PtTarget{RegionId(1), "first", "host-a", "host-a"},
        PtTarget{RegionId(2), "failed", "host-b", "host-b"},
    };
    policy.entries.push_back(std::move(entry));
    view->policies.push_back(std::move(policy));
    view_manager.Publish(std::move(view));

    NofPtReplicaAllocator allocator(view_manager);
    SegmentAllocationRequest request;
    request.size = 4096;
    request.replica_count = 2;
    request.replica_type = ReplicaType::NOF_SSD;
    auto access = state.Access();
    auto result = allocator.Allocate(access, request);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), ErrorCode::NO_AVAILABLE_HANDLE);
    EXPECT_EQ(first->allocation_calls(), 1U);
    EXPECT_EQ(first->size(), 0U);
}

TEST(ReplicaAllocatorTest, SingleGroupFastPathIsBestEffort) {
    PlacementState state;
    state.Add("only", "only");
    ReplicaAllocator allocator;
    auto request = Request(3);
    auto access = state.Access();
    auto result =
        allocator.Allocate(access, PlacementPolicyType::RANDOM, request);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(ReplicaEndpoint(result->front()), "only");
}

TEST(ReplicaAllocatorTest, SameNameTargetsFallbackInsideOneGroup) {
    PlacementState state;
    auto* failing = state.Add("logical", "physical-failing");
    state.Add("logical", "physical-good");
    failing->SetAlwaysFail();

    ReplicaAllocator allocator;
    for (size_t i = 0; i < 64; ++i) {
        auto request = Request();
        auto access = state.Access();
        auto result =
            allocator.Allocate(access, PlacementPolicyType::RANDOM, request);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(ReplicaEndpoint(result->front()), "physical-good");
    }
    EXPECT_GT(failing->allocation_calls(), 0U);
}

TEST(ReplicaAllocatorTest, AtMostOneReplicaPerLogicalGroup) {
    PlacementState state;
    state.Add("shared", "shared-0");
    state.Add("shared", "shared-1");
    state.Add("other", "other");

    ReplicaAllocator allocator;
    auto request = Request(2);
    auto access = state.Access();
    auto result =
        allocator.Allocate(access, PlacementPolicyType::RANDOM, request);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2U);
    auto endpoints = Endpoints(*result);
    EXPECT_TRUE(endpoints.contains("other"));
    EXPECT_EQ(endpoints.count("shared-0") + endpoints.count("shared-1"), 1U);
}

TEST(ReplicaAllocatorTest, FailedPreferenceDoesNotConsumeReplicaSlot) {
    PlacementState state;
    auto* failed = state.Add("failed", "failed");
    failed->SetAlwaysFail();
    state.Add("preferred", "preferred");
    state.Add("fallback", "fallback");
    std::vector<std::string> preferred{"failed", "preferred"};

    ReplicaAllocator allocator;
    auto request = Request(2);
    request.preferred_groups = preferred;
    auto access = state.Access();
    auto result =
        allocator.Allocate(access, PlacementPolicyType::RANDOM, request);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Endpoints(*result),
              (std::set<std::string>{"fallback", "preferred"}));
    EXPECT_GE(failed->allocation_calls(), 1U);
}

TEST(ReplicaAllocatorTest, ExclusionsAndInsufficientGroupsReturnBestEffort) {
    PlacementState state;
    state.Add("a", "a");
    state.Add("b", "b");
    state.Add("c", "c");
    std::vector<std::string> excluded{"c"};

    ReplicaAllocator allocator;
    auto request = Request(4);
    request.excluded_groups = excluded;
    auto access = state.Access();
    auto result =
        allocator.Allocate(access, PlacementPolicyType::RANDOM, request);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Endpoints(*result), (std::set<std::string>{"a", "b"}));
}

TEST(ReplicaAllocatorTest, FreeRatioFirstRanksAndFallsBackAfterFailure) {
    PlacementState state;
    auto* fullest =
        state.Add("fullest", "fullest", kCapacity, kCapacity - 4096);
    auto* best = state.Add("best", "best", kCapacity, 0);
    state.Add("second", "second", kCapacity, kCapacity / 2);
    (void)fullest;
    best->SetAlwaysFail();

    ReplicaAllocator allocator;
    auto request = Request();
    auto access = state.Access();
    auto result = allocator.Allocate(
        access, PlacementPolicyType::FREE_RATIO_FIRST, request);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(ReplicaEndpoint(result->front()), "second");
    EXPECT_GE(best->allocation_calls(), 1U);
}

TEST(ReplicaAllocatorTest, SsdFreeRatioFirstUsesOwnerMetrics) {
    PlacementState state;
    const UUID low_client{1, 1};
    const UUID high_client{2, 2};
    state.Add("low", "low");
    state.Add("high", "high");
    state.SetOwner("low", low_client);
    state.SetOwner("high", high_client);
    ASSERT_EQ(state.local_ssd.RegisterClient(low_client, true), ErrorCode::OK);
    ASSERT_EQ(state.local_ssd.RegisterClient(high_client, true), ErrorCode::OK);
    ASSERT_TRUE(state.local_ssd.ReportCapacity(low_client, 1000).has_value());
    ASSERT_TRUE(state.local_ssd.ReportCapacity(high_client, 1000).has_value());
    ASSERT_TRUE(state.local_ssd.AdjustUsedBytes(low_client, 900));
    ASSERT_TRUE(state.local_ssd.AdjustUsedBytes(high_client, 100));

    ReplicaAllocator allocator;
    auto request = Request();
    auto access = state.Access();
    auto result =
        allocator.Allocate(access, PlacementPolicyType::SSD_FREE_RATIO_FIRST,
                           request, LocalSSDMetricsView(state.local_ssd));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(ReplicaEndpoint(result->front()), "high");
}

TEST(ReplicaAllocatorTest, LocalFirstConsumesResolvedGroupsInOrder) {
    PlacementState state;
    state.Add("remote", "remote");
    state.Add("local-a", "local-a");
    state.Add("local-b", "local-b");
    auto view = state.index.GetView();
    std::vector<PlacementGroup*> ordered{view.Find("local-b"),
                                         view.Find("local-a")};

    ReplicaAllocator allocator;
    auto request = Request(2);
    request.resolved_preferred_groups = ordered;
    auto access = state.Access();
    auto result =
        allocator.Allocate(access, PlacementPolicyType::LOCAL_FIRST, request);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 2U);
    EXPECT_EQ(ReplicaEndpoint((*result)[0]), "local-b");
    EXPECT_EQ(ReplicaEndpoint((*result)[1]), "local-a");
}

TEST(ReplicaAllocatorTest, CxlRequiresPreferenceAndConvertsDescriptor) {
    PlacementState state;
    state.Add("cxl-group", "global-cxl", kCapacity, 0,
              AllocationTargetKind::CXL, "client-binding");
    ReplicaAllocator allocator;

    auto missing_preference = Request();
    {
        auto access = state.Access();
        EXPECT_EQ(
            allocator
                .Allocate(access, PlacementPolicyType::CXL, missing_preference)
                .error(),
            ErrorCode::INVALID_PARAMS);
    }

    auto preferred = Request();
    preferred.preferred_group = "cxl-group";
    auto access = state.Access();
    auto result =
        allocator.Allocate(access, PlacementPolicyType::CXL, preferred);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    auto descriptor = result->front()
                          .get_descriptor()
                          .get_memory_descriptor()
                          .buffer_descriptor;
    EXPECT_EQ(descriptor.protocol_, "cxl");
    EXPECT_EQ(descriptor.transport_endpoint_, "client-binding");
    EXPECT_LT(descriptor.buffer_address_, kCapacity);
}

TEST(ReplicaAllocatorTest, AllocateFromResolvesOneNameAndUsesTargetFallback) {
    PlacementState state;
    auto* failed = state.Add("logical", "bad");
    failed->SetAlwaysFail();
    state.Add("logical", "good");
    ReplicaAllocator allocator;

    for (size_t i = 0; i < 64; ++i) {
        auto access = state.Access();
        auto result = allocator.AllocateFrom(access, 4096, "logical");
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(ReplicaEndpoint(*result), "good");
    }
    auto access = state.Access();
    EXPECT_EQ(allocator.AllocateFrom(access, 4096, "missing").error(),
              ErrorCode::SEGMENT_NOT_FOUND);
}

TEST(ReplicaAllocatorTest, NoFPolicyMappingPreservesEffectiveDispatch) {
    EXPECT_EQ(
        EffectiveNoFPlacementPolicy(PlacementPolicyType::SSD_FREE_RATIO_FIRST),
        PlacementPolicyType::RANDOM);
    EXPECT_EQ(EffectiveNoFPlacementPolicy(PlacementPolicyType::RANDOM),
              PlacementPolicyType::RANDOM);
    EXPECT_EQ(EffectiveNoFPlacementPolicy(PlacementPolicyType::CXL),
              PlacementPolicyType::CXL);
}

}  // namespace mooncake::test
