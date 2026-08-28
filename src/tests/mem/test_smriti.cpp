// ============================================================================
// test_smriti.cpp — Tests for the smriti memory resource framework
// ============================================================================
// Covers: Domains, Pools, Policies, Handles, PageTable, Managers,
//         ManagedResource, SmritiAllocator, Arena, BuddyPool, MmapDomain
// ============================================================================

#include "catch_amalgamated.hpp"

#include "mem/smriti.hpp"
#include "mem/arena.hpp"
#include "mem/buddy.hpp"
#include "mem/mmap_domain.hpp"

#include <algorithm>
#include <list>
#include <map>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>

using namespace smriti;

// ============================================================================
// Helpers
// ============================================================================

struct Trivial {
    int x;
    int y;
};

struct NonTrivial {
    int value;
    bool* destroyed;

    NonTrivial(int v, bool* d) : value{v}, destroyed{d} {}

    ~NonTrivial() { if (destroyed) *destroyed = true; }
};

// ============================================================================
// SECTION 1: detail utilities
// ============================================================================

TEST_CASE (



"detail::align_up"
,
"[detail]"
)
 {
    using smriti::detail::align_up;
    CHECK(align_up(0, 8) == 0);
    CHECK(align_up(1, 8) == 8);
    CHECK(align_up(8, 8) == 8);
    CHECK(align_up(9, 8) == 16);
    CHECK(align_up(15, 16) == 16);
    CHECK(align_up(16, 16) == 16);
    CHECK(align_up(17, 16) == 32);
    CHECK(align_up(1, 1) == 1);
}

TEST_CASE (



"detail::GenId validity"
,
"[detail]"
)
 {
    detail::GenId null_id{};
    CHECK_FALSE(null_id.valid());
    detail::GenId valid_id{1, 1};
    CHECK(valid_id.valid());
    CHECK(valid_id == valid_id);
    CHECK_FALSE(valid_id == null_id);
}

// ============================================================================
// SECTION 2: Domains
// ============================================================================

TEST_CASE (



"SystemRAMDomain: acquire/release"
,
"[domains]"
)
 {
    domains::SystemRAMDomain d;
    void* p = d.acquire(128, 16);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 16 == 0);
    d.release(p, 128);
}

TEST_CASE (



"SystemRAMDomain: large alignment"
,
"[domains]"
)
 {
    domains::SystemRAMDomain d;
    void* p = d.acquire(64, 64);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
    d.release(p, 64);
}

TEST_CASE (



"StackDomain: basic bump allocation"
,
"[domains]"
)
 {
    domains::StackDomain<256> sd;
    void* p1 = sd.acquire(64, 8);
    void* p2 = sd.acquire(64, 8);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    CHECK(p1 != p2);
    // They should be adjacent (no alignment gap for 8-byte aligned)
    CHECK(static_cast<std::byte*>(p2) >= static_cast<std::byte*>(p1) + 64);
}

TEST_CASE (



"StackDomain: returns nullptr when exhausted"
,
"[domains]"
)
 {
    domains::StackDomain<32> sd;
    void* p1 = sd.acquire(32, 1);
    REQUIRE(p1 != nullptr);
    void* p2 = sd.acquire(1, 1);
    CHECK(p2 == nullptr);
}

TEST_CASE (



"StackDomain: alignment respected"
,
"[domains]"
)
 {
    domains::StackDomain<256> sd;
    sd.acquire(1, 1); // offset cursor to 1
    void* p = sd.acquire(16, 16); // must align to 16
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 16 == 0);
}

TEST_CASE (



"NullDomain: always returns nullptr"
,
"[domains]"
)
 {
    domains::NullDomain d;
    CHECK(d.acquire(1, 1) == nullptr);
    CHECK(d.acquire(1024, 64) == nullptr);
    d.release(nullptr, 0); // must not crash
}

// ============================================================================
// SECTION 3: Pools
// ============================================================================

TEST_CASE (



"BumpPool: basic allocation"
,
"[pools]"
)
 {
    pools::BumpPool<domains::SystemRAMDomain> pool{1024};
    void* p = pool.allocate(64, 8);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 8 == 0);
    CHECK(pool.used_bytes() >= 64);
}

TEST_CASE (



"BumpPool: multiple allocations"
,
"[pools]"
)
 {
    pools::BumpPool<domains::SystemRAMDomain> pool{1024};
    void* p1 = pool.allocate(100, 8);
    void* p2 = pool.allocate(100, 8);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    CHECK(p1 != p2);
    // No overlap
    CHECK(static_cast<std::byte*>(p2) >= static_cast<std::byte*>(p1) + 100);
}

TEST_CASE (



"BumpPool: OOM returns nullptr"
,
"[pools]"
)
 {
    pools::BumpPool<domains::SystemRAMDomain> pool{64};
    void* p1 = pool.allocate(64, 1);
    REQUIRE(p1 != nullptr);
    void* p2 = pool.allocate(1, 1);
    CHECK(p2 == nullptr);
}

TEST_CASE (



"BumpPool: reset reuses memory"
,
"[pools]"
)
 {
    pools::BumpPool<domains::SystemRAMDomain> pool{128};
    void* p1 = pool.allocate(64, 8);
    pool.reset();
    CHECK(pool.used_bytes() == 0);
    void* p2 = pool.allocate(64, 8);
    CHECK(p1 == p2); // same start address after reset
}

TEST_CASE (



"BumpPool: alignment correctness"
,
"[pools]"
)
 {
    pools::BumpPool<domains::SystemRAMDomain> pool{4096};
    pool.allocate(1, 1); // misalign intentionally
    void* p = pool.allocate(16, 64);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 64 == 0);
}

TEST_CASE (



"BumpPool: concurrent allocations are non-overlapping"
,
"[pools][concurrent]"
)
 {
    pools::BumpPool<domains::SystemRAMDomain> pool{1024 * 1024};
    constexpr int kThreads = 4;
    constexpr int kAllocsPerThread = 1000;
    constexpr std::size_t kSize = 64;

    std::vector<void*> results(kThreads * kAllocsPerThread, nullptr);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kAllocsPerThread; ++i) {
                results[t * kAllocsPerThread + i] = pool.allocate(kSize, 8);
            }
        });
    }
    for (auto& th : threads) th.join();

    // All allocations must succeed.
    for (auto* p : results)
        REQUIRE(p != nullptr);

    // Sort by address; then adjacent entries must be at least kSize apart,
    // which proves no two allocations overlap. O(n log n) vs the naive O(n²).
    std::vector<std::uintptr_t> addrs;
    addrs.reserve(results.size());
    for (auto* p : results) addrs.push_back(reinterpret_cast<std::uintptr_t>(p));
    std::sort(addrs.begin(), addrs.end());

    for (std::size_t i = 1; i < addrs.size(); ++i)
        REQUIRE(addrs[i] - addrs[i - 1] >= kSize);
}

TEST_CASE (



"FixedPool: basic alloc/dealloc"
,
"[pools]"
)
 {
    constexpr std::size_t kBlock = 64;
    pools::FixedPool<kBlock, domains::SystemRAMDomain> pool{16};
    void* p1 = pool.allocate(kBlock, 8);
    void* p2 = pool.allocate(kBlock, 8);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    CHECK(p1 != p2);
    pool.deallocate(p1, kBlock);
    void* p3 = pool.allocate(kBlock, 8);
    CHECK(p3 == p1); // recycled
}

TEST_CASE (



"FixedPool: reset clears all"
,
"[pools]"
)
 {
    constexpr std::size_t kBlock = 32;
    pools::FixedPool<kBlock, domains::SystemRAMDomain> pool{8};
    pool.allocate(kBlock, 8);
    pool.allocate(kBlock, 8);
    pool.reset();
    // After reset, should be able to alloc again (pool re-initialises on next alloc)
    // reset just drops slabs — next allocate will grow a new slab
    void* p = pool.allocate(kBlock, 8);
    CHECK(p != nullptr);
}

// ============================================================================
// SECTION 4: Policies
// ============================================================================

TEST_CASE (



"UnsafePolicy: pass-through"
,
"[policies]"
)
 {
    using Pool = pools::BumpPool<domains::SystemRAMDomain>;
    using Policy = policies::UnsafePolicy<Pool>;
    Policy p{Pool{512}};
    void* ptr = p.allocate(64, 8);
    REQUIRE(ptr != nullptr);
    p.deallocate(ptr, 64);
    p.reset();
    CHECK(p.pool.used_bytes() == 0);
}

TEST_CASE (



"ThreadSafePolicy: concurrent alloc"
,
"[policies][concurrent]"
)
 {
    using Pool = pools::BumpPool<domains::SystemRAMDomain>;
    using Policy = policies::ThreadSafePolicy<Pool>;
    Policy policy{Pool{1024 * 1024}};

    constexpr int kThreads = 4;
    constexpr int kAllocs = 500;
    std::vector<void*> ptrs(kThreads * kAllocs);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kAllocs; ++i)
                ptrs[t * kAllocs + i] = policy.allocate(32, 8);
        });
    }
    for (auto& th : threads) th.join();

    for (auto* p : ptrs)
        CHECK(p != nullptr);
    // Check uniqueness
    std::sort(ptrs.begin(), ptrs.end());
    CHECK(std::adjacent_find(ptrs.begin(), ptrs.end()) == ptrs.end());
}

TEST_CASE (



"BoundsCheckPolicy: clean alloc/dealloc"
,
"[policies]"
)
 {
    using Pool = pools::BumpPool<domains::SystemRAMDomain>;
    using Policy = policies::BoundsCheckPolicy<Pool>;
    Policy p{Pool{4096}};
    void* ptr = p.allocate(64, 8);
    REQUIRE(ptr != nullptr);
    p.deallocate(ptr, 64); // must not terminate
}

TEST_CASE (



"LockFreePolicy: BumpPool constraint satisfied"
,
"[policies]"
)
 {
    using Pool = pools::BumpPool<domains::SystemRAMDomain>;
    using Policy = policies::LockFreePolicy<Pool>;
    Policy p{Pool{1024}};
    void* ptr = p.allocate(64, 8);
    REQUIRE(ptr != nullptr);
    p.reset();
}

TEST_CASE (



"AuditPolicy: tracks and reports leaks"
,
"[policies]"
)
 {
    using Pool = pools::BumpPool<domains::SystemRAMDomain>;
    using Policy = policies::AuditPolicy<Pool>;
    Policy p{Pool{1024}};
    void* p1 = p.allocate(32, 8);
    void* p2 = p.allocate(32, 8);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    CHECK(p.live_.size() == 2);
    CHECK(p.alloc_count_ == 2);
    p.deallocate(p1, 32);
    CHECK(p.live_.size() == 1);
    p.deallocate(p2, 32);
    CHECK(p.live_.empty());
}

TEST_CASE (



"AuditPolicy: reset clears live map"
,
"[policies]"
)
 {
    using Pool = pools::BumpPool<domains::SystemRAMDomain>;
    using Policy = policies::AuditPolicy<Pool>;
    Policy p{Pool{1024}};
    p.allocate(32, 8); // intentional leak — just test reset doesn't crash
    p.reset();
    CHECK(p.live_.empty());
}

// ============================================================================
// SECTION 5: PageTable
// ============================================================================

TEST_CASE (



"PageTable: alloc_page / resolve"
,
"[pagetable]"
)
 {
    PageTable table;
    int data = 42;
    auto id = table.alloc_page(&data);
    CHECK(id.valid());
    auto result = table.resolve(id);
    REQUIRE(result.has_value());
    CHECK(result.value() == &data);
}

TEST_CASE (



"PageTable: stale generation detection"
,
"[pagetable]"
)
 {
    PageTable table;
    int data = 0;
    auto id = table.alloc_page(&data);
    table.free_page(id);
    auto result = table.resolve(id);
    CHECK_FALSE(result.has_value());
}

TEST_CASE (



"PageTable: null GenId returns NullPage"
,
"[pagetable]"
)
 {
    PageTable table;
    auto result = table.resolve(detail::null_genid);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == PageError::NullPage);
}

TEST_CASE (



"PageTable: pin prevents free_page"
,
"[pagetable]"
)
 {
    PageTable table;
    int data = 0;
    auto id = table.alloc_page(&data);
    REQUIRE(table.pin(id));
    CHECK_FALSE(table.free_page(id)); // pinned — must fail
    table.unpin(id);
    CHECK(table.free_page(id)); // now unpinned — must succeed
}

TEST_CASE (



"PageTable: pin returns false for cold/evicting page"
,
"[pagetable]"
)
 {
    PageTable table;
    int data = 0;
    auto id = table.alloc_page(&data);
    // Transition to Evicting
    table.transition(id, PageState::Resident, PageState::Evicting);
    CHECK_FALSE(table.pin(id));
}

TEST_CASE (



"PageTable: mark_dirty"
,
"[pagetable]"
)
 {
    PageTable table;
    int data = 0;
    auto id = table.alloc_page(&data);
    CHECK(table.mark_dirty(id));
}

TEST_CASE (



"PageTable: state transitions — valid path"
,
"[pagetable]"
)
 {
    PageTable table;
    int data = 0;
    auto id = table.alloc_page(&data);
    CHECK(table.state(id) == PageState::Resident);
    CHECK(table.transition(id, PageState::Resident, PageState::Evicting));
    CHECK(table.state(id) == PageState::Evicting);
    CHECK(table.transition(id, PageState::Evicting, PageState::Cold));
    CHECK(table.state(id) == PageState::Cold);
}

TEST_CASE (



"PageTable: invalid transition rejected"
,
"[pagetable]"
)
 {
    PageTable table;
    int data = 0;
    auto id = table.alloc_page(&data);
    // Cold → Evicting is not valid
    CHECK_FALSE(table.transition(id, PageState::Cold, PageState::Evicting));
    // Resident → Cold is not valid (must go via Evicting)
    CHECK_FALSE(table.transition(id, PageState::Resident, PageState::Cold));
}

TEST_CASE (



"PageTable: for_each_evictable finds unpinned resident pages"
,
"[pagetable]"
)
 {
    PageTable table;
    int a = 0, b = 0, c = 0;
    auto id_a = table.alloc_page(&a);
    auto id_b = table.alloc_page(&b);
    auto id_c = table.alloc_page(&c);

    table.pin(id_b); // pin b — should not appear in evictable list

    std::vector<detail::GenId> evictable;
    table.for_each_evictable([&](detail::GenId id) {
        evictable.push_back(id);
    });

    CHECK(evictable.size() == 2); // a and c
    table.unpin(id_b);
}

// ============================================================================
// SECTION 6: Handles
// ============================================================================

TEST_CASE (



"OwnerHandle: basic construction and dtor"
,
"[handles]"
)
 {
    bool destroyed = false;
    {
        pools::BumpPool<domains::SystemRAMDomain> pool{256};
        void* raw = pool.allocate(sizeof(NonTrivial), alignof(NonTrivial));
        auto* obj = ::new(raw) NonTrivial{99, &destroyed};
        handles::OwnerHandle<NonTrivial> h{
            obj, sizeof(NonTrivial),
            [&pool](void* p, std::size_t n) { pool.deallocate(p, n); }
        };
        CHECK(h->value == 99);
        CHECK(static_cast<bool>(h));
    }
    CHECK(destroyed); // dtor must have been called
}

TEST_CASE (



"OwnerHandle: move transfers ownership"
,
"[handles]"
)
 {
    bool destroyed = false;
    pools::BumpPool<domains::SystemRAMDomain> pool{256};
    void* raw = pool.allocate(sizeof(NonTrivial), alignof(NonTrivial));
    auto* obj = ::new(raw) NonTrivial{42, &destroyed};

    handles::OwnerHandle<NonTrivial> h1{
        obj, sizeof(NonTrivial),
        [](void*, std::size_t) {}
    };
    handles::OwnerHandle<NonTrivial> h2 = std::move(h1);
    CHECK_FALSE(static_cast<bool>(h1));
    CHECK(static_cast<bool>(h2));
    CHECK_FALSE(destroyed);
    // h2 goes out of scope here — dtor called
}

TEST_CASE (



"OwnerHandle: release gives up ownership"
,
"[handles]"
)
 {
    bool destroyed = false;
    pools::BumpPool<domains::SystemRAMDomain> pool{256};
    void* raw = pool.allocate(sizeof(NonTrivial), alignof(NonTrivial));
    auto* obj = ::new(raw) NonTrivial{7, &destroyed};

    handles::OwnerHandle<NonTrivial> h{
        obj, sizeof(NonTrivial),
        [](void*, std::size_t) {}
    };
    NonTrivial* released = h.release();
    CHECK(released == obj);
    CHECK_FALSE(static_cast<bool>(h));
    // destroyed must still be false (dtor not called by handle after release)
    CHECK_FALSE(destroyed);
    released->~NonTrivial();
}

TEST_CASE (



"PinnedPageHandle: basic access and auto-unpin"
,
"[handles]"
)
 {
    PageTable table;
    int data = 123;
    auto id = table.alloc_page(&data);
    table.pin(id);
    {
        handles::PinnedPageHandle<int> h{&data, id, table};
        auto result = h.get();
        REQUIRE(result.has_value());
        CHECK(*result.value() == 123);
    }
    // After handle dtor, pin_count should be 0 → free_page should succeed
    CHECK(table.free_page(id));
}

TEST_CASE (



"PinnedPageHandle: move transfers pin ownership"
,
"[handles]"
)
 {
    PageTable table;
    int data = 0;
    auto id = table.alloc_page(&data);
    table.pin(id);

    handles::PinnedPageHandle<int> h1{&data, id, table};
    handles::PinnedPageHandle<int> h2 = std::move(h1);

    CHECK_FALSE(static_cast<bool>(h1));
    CHECK(static_cast<bool>(h2));

    // After h2 dtor: pin released
    {
        auto tmp = std::move(h2);
    }
    CHECK(table.free_page(id));
}

TEST_CASE (



"PinnedPageHandle: stale access returns error"
,
"[handles]"
)
 {
    PageTable table;
    int data = 0;
    auto id = table.alloc_page(&data);
    table.pin(id);

    handles::PinnedPageHandle<int> h{&data, id, table};
    table.unpin(id);
    table.free_page(id); // invalidate — stale after this

    auto result = h.get();
    // Must return an error (Evicted or Stale) — not a valid pointer
    CHECK_FALSE(result.has_value());
}

// ============================================================================
// SECTION 7: Managers
// ============================================================================

TEST_CASE (



"NullManager: satisfies Manager concept"
,
"[managers]"
)
 {
    static_assert(concepts::Manager<managers::NullManager>);
    managers::NullManager m;
    PageTable t;
    m.attach(t);
    m.on_alloc(detail::null_genid);
    m.on_access(detail::null_genid);
    CHECK_FALSE(m.evict_one().has_value());
    m.shutdown();
}

TEST_CASE (



"LRUCacheManager: evicts LRU page"
,
"[managers]"
)
 {
    PageTable table;
    managers::LRUCacheManager lru{2}; // capacity = 2 pages
    lru.attach(table);

    int d1 = 1, d2 = 2, d3 = 3;
    auto id1 = table.alloc_page(&d1);
    lru.on_alloc(id1);
    auto id2 = table.alloc_page(&d2);
    lru.on_alloc(id2);

    // Access id1 — makes id2 the LRU
    lru.on_access(id1);

    // Adding id3 should trigger eviction of id2 (LRU)
    auto id3 = table.alloc_page(&d3);
    lru.on_alloc(id3);
    // After eviction pressure, id2 should be gone
    // (The manager evicts during on_alloc when over capacity)
    auto res2 = table.resolve(id2);
    // id2 was evicted — resolve returns error
    CHECK_FALSE(res2.has_value());
}

TEST_CASE (



"LRUCacheManager: pinned page not evicted"
,
"[managers]"
)
 {
    PageTable table;
    managers::LRUCacheManager lru{1};
    lru.attach(table);

    int d1 = 1, d2 = 2;
    auto id1 = table.alloc_page(&d1);
    lru.on_alloc(id1);
    table.pin(id1); // pin — must not be evicted

    auto id2 = table.alloc_page(&d2);
    lru.on_alloc(id2);
    // id1 is pinned, so evict_one on id2's alloc cannot evict id1
    // id1 should still be resident
    auto res1 = table.resolve(id1);
    CHECK(res1.has_value());
    table.unpin(id1);
}

// ============================================================================
// SECTION 8: ManagedResource
// ============================================================================

TEST_CASE (



"ManagedResource: make<T> constructs and pins"
,
"[resource]"
)
 {
    using Domain = domains::SystemRAMDomain;
    using Pool = pools::BumpPool<Domain>;
    ManagedResource<Domain, Pool> res{Domain{}, Pool{4096}};

    auto h = res.make<Trivial>(10, 20);
    REQUIRE(static_cast<bool>(h));
    CHECK(h->x == 10);
    CHECK(h->y == 20);
}

TEST_CASE (



"ManagedResource: destructor called on handle dtor"
,
"[resource]"
)
 {
    bool destroyed = false;
    {
        using Domain = domains::SystemRAMDomain;
        using Pool = pools::BumpPool<Domain>;
        ManagedResource<Domain, Pool> res{Domain{}, Pool{4096}};
        auto h = res.make<NonTrivial>(5, &destroyed);
        REQUIRE(static_cast<bool>(h));
        CHECK_FALSE(destroyed);
        // Explicitly destroy: calls ~NonTrivial(), unpins, and reclaims the slot.
        res.destroy(std::move(h));
        CHECK(destroyed);
    }
}

TEST_CASE (



"ManagedResource: multiple allocations"
,
"[resource]"
)
 {
    using Domain = domains::SystemRAMDomain;
    using Pool = pools::BumpPool<Domain>;
    ManagedResource<Domain, Pool> res{Domain{}, Pool{65536}};

    std::vector<handles::PinnedPageHandle<int>> handles;
    for (int i = 0; i < 10; ++i) {
        auto h = res.make<int>(i);
        REQUIRE(static_cast<bool>(h));
        auto val = h.get();
        REQUIRE(val.has_value());
        CHECK(*val.value() == i);
        handles.push_back(std::move(h));
    }
    CHECK(handles.size() == 10);
}

TEST_CASE (



"ManagedResource: with LRUCacheManager"
,
"[resource]"
)
 {
    using Domain = domains::SystemRAMDomain;
    using Pool = pools::BumpPool<Domain>;
    using Manager = managers::LRUCacheManager;

    Manager mgr{5};
    ManagedResource<Domain, Pool, Manager> res{
        Domain{}, Pool{65536}, std::move(mgr)
    };

    // Make 5 objects
    std::vector<handles::PinnedPageHandle<int>> handles;
    for (int i = 0; i < 5; ++i)
        handles.push_back(res.make<int>(i));

    CHECK(handles.size() == 5);
}

TEST_CASE (



"ManagedResource: raw allocate/deallocate"
,
"[resource]"
)
 {
    using Domain = domains::SystemRAMDomain;
    using Pool = pools::BumpPool<Domain>;
    ManagedResource<Domain, Pool> res{Domain{}, Pool{4096}};

    void* p = res.allocate(64, 8);
    REQUIRE(p != nullptr);
    res.deallocate(p, 64);
}

// ============================================================================
// SECTION 9: SmritiAllocator (STL compatibility)
// ============================================================================

TEST_CASE (



"SmritiAllocator: use with std::vector"
,
"[allocator]"
)
 {
    using Domain = domains::SystemRAMDomain;
    using Pool = pools::BumpPool<Domain>;
    ManagedResource<Domain, Pool> res{Domain{}, Pool{65536}};

    SmritiAllocator<int, decltype(res)> alloc{res};
    std::vector<int, SmritiAllocator<int, decltype(res)>> v{alloc};

    for (int i = 0; i < 100; ++i) v.push_back(i);
    REQUIRE(v.size() == 100);
    for (int i = 0; i < 100; ++i)
        CHECK(v[i] == i);
}

TEST_CASE (



"SmritiAllocator: equality"
,
"[allocator]"
)
 {
    using Domain = domains::SystemRAMDomain;
    using Pool = pools::BumpPool<Domain>;
    ManagedResource<Domain, Pool> res{Domain{}, Pool{4096}};

    SmritiAllocator<int, decltype(res)> a1{res};
    SmritiAllocator<int, decltype(res)> a2{res};
    CHECK(a1 == a2);
}

// ============================================================================
// SECTION 10: Arena (arena.hpp)
// ============================================================================

TEST_CASE (



"ScopedArena: stack-backed allocation"
,
"[arena]"
)
 {
    pools::ScopedArena<512> arena;
    void* p = arena.allocate(64, 8);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 8 == 0);
}

TEST_CASE (



"ScopedArena: OOM when stack exhausted"
,
"[arena]"
)
 {
    pools::ScopedArena<128> arena;
    void* p1 = arena.allocate(128, 1);
    REQUIRE(p1 != nullptr);
    void* p2 = arena.allocate(1, 1);
    CHECK(p2 == nullptr);
}

TEST_CASE (



"LinearArena: checkpoint and rollback"
,
"[arena]"
)
 {
    pools::LinearArena arena{4096};
    auto cp = arena.checkpoint();
    CHECK(cp.offset == 0);

    void* p1 = arena.allocate(64, 8);
    void* p2 = arena.allocate(64, 8);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    CHECK(arena.used_bytes() >= 128);

    arena.rollback(cp);
    CHECK(arena.used_bytes() == 0);

    // After rollback, next allocation should be at the same address as p1
    void* p3 = arena.allocate(64, 8);
    CHECK(p3 == p1);
}

TEST_CASE (



"LinearArena: checkpoint mid-stream"
,
"[arena]"
)
 {
    pools::LinearArena arena{4096};
    arena.allocate(100, 8);
    auto cp = arena.checkpoint();
    arena.allocate(200, 8);
    CHECK(arena.used_bytes() >= 300);
    arena.rollback(cp);
    CHECK(arena.used_bytes() <= 104); // only the first 100 bytes remain
}

TEST_CASE (



"TwoPhaseArena: primary then overflow"
,
"[arena]"
)
 {
    // Primary holds 64 bytes, overflow holds 256
    pools::TwoPhaseArena arena{64, 256};

    void* p1 = arena.allocate(64, 1);
    REQUIRE(p1 != nullptr);

    // This should spill to overflow
    void* p2 = arena.allocate(64, 1);
    REQUIRE(p2 != nullptr);
    CHECK(p1 != p2);
}

TEST_CASE (



"TwoPhaseArena: reset clears both arenas"
,
"[arena]"
)
 {
    pools::TwoPhaseArena arena{64, 256};
    arena.allocate(64, 1);
    arena.allocate(64, 1); // triggers overflow
    arena.reset();
    CHECK(arena.used_bytes() == 0);
}

// ============================================================================
// SECTION 11: BuddyPool (buddy.hpp)
// ============================================================================

TEST_CASE (



"BuddyPool: basic alloc/dealloc"
,
"[buddy]"
)
 {
    pools::BuddyPool<5, 20> pool;
    void* p = pool.allocate(32, 8);
    REQUIRE(p != nullptr);
    pool.deallocate(p, 32);
}

TEST_CASE (



"BuddyPool: multiple blocks"
,
"[buddy]"
)
 {
    pools::BuddyPool<5, 20> pool;
    void* p1 = pool.allocate(32, 8);
    void* p2 = pool.allocate(32, 8);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    CHECK(p1 != p2);
    pool.deallocate(p1, 32);
    pool.deallocate(p2, 32);
}

TEST_CASE (



"BuddyPool: reuse after dealloc"
,
"[buddy]"
)
 {
    pools::BuddyPool<5, 20> pool;
    void* p1 = pool.allocate(32, 8);
    REQUIRE(p1 != nullptr);
    pool.deallocate(p1, 32);
    void* p2 = pool.allocate(32, 8);
    CHECK(p2 == p1); // buddy merges; same block reused
}

TEST_CASE (



"BuddyPool: varied sizes"
,
"[buddy]"
)
 {
    pools::BuddyPool<5, 20> pool;
    void* p1 = pool.allocate(32, 8);
    void* p2 = pool.allocate(64, 8);
    void* p3 = pool.allocate(128, 8);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p3 != nullptr);
    pool.deallocate(p3, 128);
    pool.deallocate(p2, 64);
    pool.deallocate(p1, 32);
}

TEST_CASE (



"BuddyPool: reset reinitialises"
,
"[buddy]"
)
 {
    pools::BuddyPool<5, 20> pool;
    for (int i = 0; i < 8; ++i) pool.allocate(32, 8);
    pool.reset();
    void* p = pool.allocate(1 << 20, 8); // full slab
    CHECK(p != nullptr);
}

TEST_CASE (



"BuddyPool: OOM on over-allocation"
,
"[buddy]"
)
 {
    pools::BuddyPool<5, 10> pool; // max 1024 bytes
    void* p = pool.allocate(2048, 8); // too large
    CHECK(p == nullptr);
}

// ============================================================================
// SECTION 12: MappedFileDomain (mmap_domain.hpp)
// ============================================================================

TEST_CASE (



"MappedFileDomain: anonymous mapping"
,
"[mmap]"
)
 {
    auto dom = domains::MappedFileDomain::anonymous(4096);
    CHECK(dom.valid());
    void* p = dom.acquire(128, 16);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 16 == 0);
    // Write and read back
    std::memset(p, 0xAB, 128);
    std::byte buf[128];
    std::memcpy(buf, p, 128);
    for (auto b : buf)
        CHECK(static_cast<unsigned char>(b) == 0xAB);
}

TEST_CASE (



"MappedFileDomain: OOM when region exhausted"
,
"[mmap]"
)
 {
    auto dom = domains::MappedFileDomain::anonymous(4096);
    REQUIRE(dom.valid());
    void* p1 = dom.acquire(4096, 1);
    REQUIRE(p1 != nullptr);
    void* p2 = dom.acquire(1, 1);
    CHECK(p2 == nullptr);
}

TEST_CASE (



"MappedFileDomain: invalid domain is not valid"
,
"[mmap]"
)
 {
    domains::MappedFileDomain dom;
    CHECK_FALSE(dom.valid());
    CHECK(dom.acquire(64, 8) == nullptr);
}

TEST_CASE (



"MappedFileDomain: move transfers ownership"
,
"[mmap]"
)
 {
    auto dom1 = domains::MappedFileDomain::anonymous(4096);
    REQUIRE(dom1.valid());
    auto dom2 = std::move(dom1);
    CHECK_FALSE(dom1.valid());
    CHECK(dom2.valid());
    void* p = dom2.acquire(64, 8);
    CHECK(p != nullptr);
}

TEST_CASE (



"MappedRegion: span view and flush"
,
"[mmap]"
)
 {
    auto dom = domains::MappedFileDomain::anonymous(4096);
    REQUIRE(dom.valid());
    void* p = dom.acquire(256, 8);
    REQUIRE(p != nullptr);
    std::span<std::byte> view{static_cast<std::byte*>(p), 256};
    domains::MappedRegion region{view, false};
    CHECK(region.size() == 256);
    region.flush(); // must not crash
}

TEST_CASE (



"NumaDomain: acquire/release"
,
"[numa]"
)
 {
    auto dom = domains::NumaDomain::local();
    void* p = dom.acquire(256, 16);
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(p) % 16 == 0);
    dom.release(p, 256);
}

// ============================================================================
// SECTION 13: BumpPool with MappedFileDomain
// ============================================================================

TEST_CASE (



"BumpPool over MappedFileDomain: basic use"
,
"[integration]"
)
 {
    auto dom = domains::MappedFileDomain::anonymous(65536);
    REQUIRE(dom.valid());

    // BumpPool directly using a pre-acquired region from the mmap domain
    // (Using SystemRAMDomain here for the BumpPool backing; mmap tested separately)
    pools::BumpPool<domains::SystemRAMDomain> pool{65536};
    void* p = pool.allocate(128, 16);
    REQUIRE(p != nullptr);
    std::memset(p, 0, 128);
}

// ============================================================================
// SECTION 14: Concept satisfaction checks (compile-time)
// ============================================================================

TEST_CASE (



"Concepts: Domain satisfied by standard domains"
,
"[concepts]"
)
 {
    static_assert(concepts::Domain<domains::SystemRAMDomain>);
    static_assert(concepts::Domain<domains::StackDomain<64>>);
    static_assert(concepts::Domain<domains::NullDomain>);
    static_assert(concepts::DomainWithContext<domains::MappedFileDomain>);
    static_assert(concepts::DomainWithContext<domains::NumaDomain>);
    SUCCEED("All Domain concept checks passed");
}

TEST_CASE (



"Concepts: Pool satisfied by BumpPool and FixedPool"
,
"[concepts]"
)
 {
    using BP = pools::BumpPool<domains::SystemRAMDomain>;
    using FP = pools::FixedPool<64, domains::SystemRAMDomain>;
    static_assert(concepts::Pool<BP>);
    static_assert(concepts::Pool<FP>);
    static_assert(concepts::BulkPool<BP>);
    SUCCEED("All Pool concept checks passed");
}

TEST_CASE (



"Concepts: Pool satisfied by policies"
,
"[concepts]"
)
 {
    using Inner = pools::BumpPool<domains::SystemRAMDomain>;
    static_assert(concepts::Pool<policies::UnsafePolicy<Inner>>);
    static_assert(concepts::Pool<policies::ThreadSafePolicy<Inner>>);
    static_assert(concepts::Pool<policies::BoundsCheckPolicy<Inner>>);
    static_assert(concepts::Pool<policies::LockFreePolicy<Inner>>);
    static_assert(concepts::Pool<policies::AuditPolicy<Inner>>);
    static_assert(concepts::PolicyWrapper<policies::UnsafePolicy<Inner>>);
    SUCCEED("All Policy concept checks passed");
}

TEST_CASE (



"Concepts: Manager satisfied"
,
"[concepts]"
)
 {
    static_assert(concepts::Manager<managers::NullManager>);
    static_assert(concepts::Manager<managers::LRUCacheManager>);
    static_assert(concepts::Manager<managers::RecoveryManager>);
    SUCCEED("All Manager concept checks passed");
}

// ============================================================================
// SECTION 15: SmritiAllocator — STL adaptor completeness
// ============================================================================

// Helper alias: ManagedResource backed by BumpPool over SystemRAMDomain
using DefaultResource = ManagedResource<domains::SystemRAMDomain,
                                        pools::BumpPool<domains::SystemRAMDomain>>;

TEST_CASE (



"SmritiAllocator: rebind<U>::other produces correct type"
,
"[allocator][rebind]"
)
 {
    // Compile-time check: rebind from int to double yields SmritiAllocator<double, R>
    using IntAlloc = SmritiAllocator<int, DefaultResource>;
    using DblAlloc = IntAlloc::rebind<double>::other;
    static_assert(std::is_same_v<DblAlloc, SmritiAllocator<double, DefaultResource>>);

    // allocator_traits::rebind_alloc must agree
    using TraitsRebind = std::allocator_traits<IntAlloc>::rebind_alloc<double>;
    static_assert(std::is_same_v<TraitsRebind, SmritiAllocator<double, DefaultResource>>);

    SUCCEED("rebind types are correct");
}

TEST_CASE (



"SmritiAllocator: allocator_traits value_type and pointer"
,
"[allocator][traits]"
)
 {
    using Alloc = SmritiAllocator<int, DefaultResource>;
    using Traits = std::allocator_traits<Alloc>;
    static_assert(std::is_same_v<Traits::value_type, int>);
    static_assert(std::is_same_v<Traits::pointer, int*>);
    static_assert(std::is_same_v<Traits::size_type, std::size_t>);
    SUCCEED("allocator_traits satisfied");
}

TEST_CASE (



"SmritiAllocator: inequality for different resources"
,
"[allocator]"
)
 {
    DefaultResource res1{
        domains::SystemRAMDomain{},
        pools::BumpPool<domains::SystemRAMDomain>{4096}
    };
    DefaultResource res2{
        domains::SystemRAMDomain{},
        pools::BumpPool<domains::SystemRAMDomain>{4096}
    };

    SmritiAllocator<int, DefaultResource> a1{res1};
    SmritiAllocator<int, DefaultResource> a2{res2};
    CHECK_FALSE(a1 == a2); // different backing resources
}

TEST_CASE (



"SmritiAllocator: rebind copy constructor transfers resource pointer"
,
"[allocator][rebind]"
)
 {
    DefaultResource res{
        domains::SystemRAMDomain{},
        pools::BumpPool<domains::SystemRAMDomain>{4096}
    };

    SmritiAllocator<int, DefaultResource> int_alloc{res};
    SmritiAllocator<char, DefaultResource> char_alloc{int_alloc}; // rebind copy ctor

    CHECK(int_alloc.resource() == char_alloc.resource());
}

TEST_CASE (



"make_allocator: deduces ResourceT"
,
"[allocator]"
)
 {
    DefaultResource res{
        domains::SystemRAMDomain{},
        pools::BumpPool<domains::SystemRAMDomain>{4096}
    };

    auto alloc = make_allocator<int>(res);
    static_assert(std::is_same_v<decltype(alloc),
                                 SmritiAllocator<int, DefaultResource>>);
    CHECK(alloc.resource() == &res);
}

TEST_CASE (



"SmritiAllocator: std::vector with ManagedResource"
,
"[allocator][stl]"
)
 {
    DefaultResource res{
        domains::SystemRAMDomain{},
        pools::BumpPool<domains::SystemRAMDomain>{1 << 17}
    };

    auto alloc = make_allocator<int>(res);
    std::vector<int, decltype(alloc)> v{alloc};

    for (int i = 0; i < 256; ++i) v.push_back(i);
    REQUIRE(v.size() == 256);
    for (int i = 0; i < 256; ++i)
        CHECK(v[i] == i);
}

TEST_CASE (



"SmritiAllocator: std::list with ManagedResource"
,
"[allocator][stl]"
)
 {
    // std::list heavily exercises rebind (allocates list_node<T>, not T)
    DefaultResource res{
        domains::SystemRAMDomain{},
        pools::BumpPool<domains::SystemRAMDomain>{1 << 17}
    };

    auto alloc = make_allocator<int>(res);
    std::list<int, decltype(alloc)> lst{alloc};

    for (int i = 0; i < 64; ++i) lst.push_back(i);
    REQUIRE(lst.size() == 64);
    int expected = 0;
    for (int v : lst)
        CHECK(v == expected++);
}

TEST_CASE (



"SmritiAllocator: std::map with ManagedResource"
,
"[allocator][stl]"
)
 {
    // std::map uses rebind to allocate tree nodes (pair<const K,V> internal nodes)
    using Pair = std::pair<const int, int>;
    DefaultResource res{
        domains::SystemRAMDomain{},
        pools::BumpPool<domains::SystemRAMDomain>{1 << 17}
    };

    auto alloc = make_allocator<Pair>(res);
    std::map<int, int, std::less<int>, decltype(alloc)> m{std::less<int>{}, alloc};

    for (int i = 0; i < 32; ++i) m[i] = i * 2;
    REQUIRE(m.size() == 32);
    for (int i = 0; i < 32; ++i)
        CHECK(m.at(i) == i * 2);
}

TEST_CASE (



"SmritiAllocator: std::basic_string with ManagedResource"
,
"[allocator][stl]"
)
 {
    DefaultResource res{
        domains::SystemRAMDomain{},
        pools::BumpPool<domains::SystemRAMDomain>{1 << 16}
    };

    auto alloc = make_allocator<char>(res);
    std::basic_string<char, std::char_traits<char>, decltype(alloc)> s{alloc};

    // Append enough to exceed SSO buffer and force a heap (pool) allocation
    for (int i = 0; i < 10; ++i) s += "hello world!";
    CHECK(s.size() == 120);
    CHECK(s.starts_with("hello world!"));
}

TEST_CASE (



"SmritiAllocator: FixedPool-backed std::vector"
,
"[allocator][stl]"
)
 {
    // FixedPool satisfies MemoryResource directly via its allocate/deallocate members
    pools::FixedPool<64, domains::SystemRAMDomain> pool{128};

    SmritiAllocator<int, decltype(pool)> alloc{pool};
    std::vector<int, decltype(alloc)> v{alloc};

    // Each int alloc rounds up to a 64-byte block — small vector to stay in budget
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    REQUIRE(v.size() == 3);
    CHECK(v[0] == 1);
    CHECK(v[1] == 2);
    CHECK(v[2] == 3);
}

TEST_CASE (



"SmritiAllocator: BumpPool-backed std::list (no dealloc needed)"
,
"[allocator][stl]"
)
 {
    // BumpPool satisfies MemoryResource directly; deallocate is a no-op
    pools::BumpPool<domains::SystemRAMDomain> pool{1 << 16};

    SmritiAllocator<int, decltype(pool)> alloc{pool};
    std::list<int, decltype(alloc)> lst{alloc};

    for (int i = 0; i < 32; ++i) lst.push_back(i * 3);
    REQUIRE(lst.size() == 32);
    int expected = 0;
    for (int v : lst) {
        CHECK(v == expected * 3);
        ++expected;
    }
}

TEST_CASE (



"SmritiAllocator: ScopedArena-backed std::vector"
,
"[allocator][stl]"
)
 {
    // ScopedArena satisfies MemoryResource directly
    pools::ScopedArena<8192> arena;

    SmritiAllocator<int, decltype(arena)> alloc{arena};
    std::vector<int, decltype(alloc)> v{alloc};

    // Reserve first to avoid multiple reallocs exhausting the arena
    v.reserve(32);
    for (int i = 0; i < 32; ++i) v.push_back(i);
    REQUIRE(v.size() == 32);
    for (int i = 0; i < 32; ++i)
        CHECK(v[i] == i);
}

TEST_CASE (



"SmritiAllocator: concept MemoryResource satisfied by pools and arenas"
,
"[allocator][concepts]"
)
 {
    static_assert(concepts::MemoryResource<pools::BumpPool<domains::SystemRAMDomain>>);
    static_assert(concepts::MemoryResource<pools::FixedPool<64, domains::SystemRAMDomain>>);
    static_assert(concepts::MemoryResource<pools::ScopedArena<512>>);
    static_assert(concepts::MemoryResource<pools::LinearArena>);
    static_assert(concepts::MemoryResource<pools::TwoPhaseArena>);
    static_assert(concepts::MemoryResource<DefaultResource>);
    SUCCEED("All MemoryResource concept checks passed");
}
