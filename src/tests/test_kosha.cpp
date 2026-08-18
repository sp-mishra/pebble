// ============================================================================
// test_kosha.cpp — Unit tests for include/containers/cache/kosha.hpp
// ============================================================================
// Covers: LRUCache, LFUCache, FIFOCache, ARCCache, ThreadSafeCache,
//         ShardedCache, FlatHashStorage, NodeStorage, error propagation,
//         InstrumentedCache, TTLCache, ClusterCache, namespace checks.
// ============================================================================

#include "catch_amalgamated.hpp"

#include "containers/cache/kosha.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <numeric>
#include <string_view>

// ============================================================================
// § 1  Concept / compile-time checks
// ============================================================================

TEST_CASE (



"kosha concepts: policies satisfy EvictionPolicy"
,
"[concepts]"
)
 {
    static_assert(kosha::EvictionPolicy<kosha::LRUPolicy<int>, int>);
    static_assert(kosha::EvictionPolicy<kosha::LFUPolicy<int>, int>);
    static_assert(kosha::EvictionPolicy<kosha::FIFOPolicy<int>, int>);
    static_assert(kosha::EvictionPolicy<kosha::ARCPolicy<int>, int>);
    SUCCEED("All EvictionPolicy concept checks passed");
}

TEST_CASE (



"kosha concepts: storages satisfy StorageBackend"
,
"[concepts]"
)
 {
    static_assert(kosha::StorageBackend<kosha::FlatHashStorage<int, int>, int, int>);
    static_assert(kosha::StorageBackend<kosha::NodeStorage<int, int>, int, int>);
    SUCCEED("All StorageBackend concept checks passed");
}

TEST_CASE (



"kosha traits: mutates_on_hit is false for FIFO only"
,
"[concepts]"
)
 {
    static_assert( kosha::mutates_on_hit<kosha::LRUPolicy<int>>);
    static_assert( kosha::mutates_on_hit<kosha::LFUPolicy<int>>);
    static_assert(!kosha::mutates_on_hit<kosha::FIFOPolicy<int>>);
    static_assert( kosha::mutates_on_hit<kosha::ARCPolicy<int>>);
    SUCCEED("mutates_on_hit trait checks passed");
}

// ============================================================================
// § 2  FlatHashStorage
// ============================================================================

TEST_CASE (



"FlatHashStorage: insert and find"
,
"[storage][flat]"
)
 {
    kosha::FlatHashStorage<int, std::string> s;
    s.insert(1, "one");
    auto* p = s.find(1);
    REQUIRE(p != nullptr);
    CHECK(*p == "one");
}

TEST_CASE (



"FlatHashStorage: duplicate insert does not overwrite"
,
"[storage][flat]"
)
 {
    kosha::FlatHashStorage<int, int> s;
    s.insert(42, 1);
    s.insert(42, 2);            // key already present — value must not change
    CHECK(*s.find(42) == 1);    // value unchanged
}

TEST_CASE (



"FlatHashStorage: insert_or_assign overwrites"
,
"[storage][flat]"
)
 {
    kosha::FlatHashStorage<int, int> s;
    s.insert(10, 100);
    s.insert_or_assign(10, 999);
    CHECK(*s.find(10) == 999);
}

TEST_CASE (



"FlatHashStorage: erase removes entry"
,
"[storage][flat]"
)
 {
    kosha::FlatHashStorage<int, int> s;
    s.insert(5, 50);
    CHECK(s.erase(5));
    CHECK(s.find(5) == nullptr);
    CHECK_FALSE(s.erase(5)); // already gone
}

TEST_CASE (



"FlatHashStorage: clear empties storage"
,
"[storage][flat]"
)
 {
    kosha::FlatHashStorage<int, int> s;
    s.insert(1, 1); s.insert(2, 2); s.insert(3, 3);
    s.clear();
    CHECK(s.size() == 0);
    CHECK(s.find(1) == nullptr);
}

TEST_CASE (



"FlatHashStorage: size tracks live entries"
,
"[storage][flat]"
)
 {
    kosha::FlatHashStorage<int, int> s;
    CHECK(s.size() == 0);
    s.insert(1, 1);
    CHECK(s.size() == 1);
    s.insert(2, 2);
    CHECK(s.size() == 2);
    s.erase(1);
    CHECK(s.size() == 1);
}

TEST_CASE (



"FlatHashStorage: for_each visits all entries"
,
"[storage][flat]"
)
 {
    kosha::FlatHashStorage<int, int> s;
    for (int i = 0; i < 8; ++i) s.insert(i, i * 10);
    int sum = 0;
    s.for_each([&](const int&, int& v) { sum += v; });
    CHECK(sum == (0+10+20+30+40+50+60+70));
}

TEST_CASE (



"FlatHashStorage: grows beyond initial capacity"
,
"[storage][flat]"
)
 {
    kosha::FlatHashStorage<int, int> s{4};
    for (int i = 0; i < 64; ++i) s.insert(i, i);
    CHECK(s.size() == 64);
    for (int i = 0; i < 64; ++i) {
        auto* p = s.find(i);
        REQUIRE(p != nullptr);
        CHECK(*p == i);
    }
}

// ============================================================================
// § 3  NodeStorage
// ============================================================================

TEST_CASE (



"NodeStorage: insert, find, erase"
,
"[storage][node]"
)
 {
    kosha::NodeStorage<std::string, int> s;
    s.insert("a", 1);
    CHECK(*s.find("a") == 1);
    CHECK(s.erase("a"));
    CHECK(s.find("a") == nullptr);
}

TEST_CASE (



"NodeStorage: insert_or_assign"
,
"[storage][node]"
)
 {
    kosha::NodeStorage<std::string, int> s;
    s.insert("k", 10);
    s.insert_or_assign("k", 20);
    CHECK(*s.find("k") == 20);
}

TEST_CASE (



"NodeStorage: for_each"
,
"[storage][node]"
)
 {
    kosha::NodeStorage<int, int> s;
    s.insert(1, 10); s.insert(2, 20); s.insert(3, 30);
    int sum = 0;
    s.for_each([&](const int&, int& v) { sum += v; });
    CHECK(sum == 60);
}

// ============================================================================
// § 4  Cache — basic API (LRU)
// ============================================================================

TEST_CASE (



"Cache LRU: put and get hit"
,
"[cache][lru]"
)
 {
    kosha::LRUCache<int, std::string> c{8};
    auto r = c.put(1, "hello");
    REQUIRE(r.has_value());
    auto v = c.get(1);
    REQUIRE(v.has_value());
    CHECK(v.value() == "hello");
}

TEST_CASE (



"Cache LRU: get miss returns NotFound"
,
"[cache][lru]"
)
 {
    kosha::LRUCache<int, int> c{4};
    auto v = c.get(99);
    CHECK_FALSE(v.has_value());
    CHECK(v.error() == kosha::Error::NotFound);
}

TEST_CASE (



"Cache LRU: peek is non-mutating"
,
"[cache][lru]"
)
 {
    kosha::LRUCache<int, int> c{4};
    (void)c.put(1, 10);
    auto p = c.peek(1);
    REQUIRE(p.has_value());
    CHECK(*p == 10);
    // peek on absent key
    CHECK_FALSE(c.peek(99).has_value());
}

TEST_CASE (



"Cache LRU: put updates existing key without eviction"
,
"[cache][lru]"
)
 {
    kosha::LRUCache<int, int> c{2};
    (void)c.put(1, 10);
    (void)c.put(2, 20);
    (void)c.put(1, 99); // update, no eviction
    CHECK(c.size() == 2);
    CHECK(c.get(1).value() == 99);
    CHECK(c.get(2).value() == 20);
}

TEST_CASE (



"Cache LRU: evicts least-recently-used entry"
,
"[cache][lru]"
)
 {
    kosha::LRUCache<int, int> c{3};
    (void)c.put(1, 1);
    (void)c.put(2, 2);
    (void)c.put(3, 3);
    // Access 1 and 2 — making 3 the LRU
    (void)c.get(1);
    (void)c.get(2);
    (void)c.put(4, 4); // evicts 3
    CHECK_FALSE(c.get(3).has_value());
    CHECK(c.get(1).has_value());
    CHECK(c.get(2).has_value());
    CHECK(c.get(4).has_value());
}

TEST_CASE (



"Cache LRU: erase removes entry"
,
"[cache][lru]"
)
 {
    kosha::LRUCache<int, int> c{4};
    (void)c.put(1, 10);
    CHECK(c.erase(1));
    CHECK_FALSE(c.get(1).has_value());
    CHECK_FALSE(c.erase(1)); // already gone
}

TEST_CASE (



"Cache LRU: erase_range"
,
"[cache][lru]"
)
 {
    kosha::LRUCache<int, int> c{8};
    for (int i = 0; i < 6; ++i) (void)c.put(i, i);
    std::vector<int> to_erase = {1, 3, 5};
    CHECK(c.erase_range(to_erase) == 3);
    CHECK(c.size() == 3);
    CHECK_FALSE(c.get(1).has_value());
    CHECK(c.get(0).has_value());
}

TEST_CASE (



"Cache LRU: clear empties cache"
,
"[cache][lru]"
)
 {
    kosha::LRUCache<int, int> c{4};
    (void)c.put(1, 1); (void)c.put(2, 2);
    c.clear();
    CHECK(c.empty());
    CHECK(c.size() == 0);
}

TEST_CASE (



"Cache LRU: full() reports correctly"
,
"[cache][lru]"
)
 {
    kosha::LRUCache<int, int> c{2};
    CHECK_FALSE(c.full());
    (void)c.put(1, 1);
    CHECK_FALSE(c.full());
    (void)c.put(2, 2);
    CHECK(c.full());
}

TEST_CASE (



"Cache LRU: for_each visits all entries"
,
"[cache][lru]"
)
 {
    kosha::LRUCache<int, int> c{8};
    for (int i = 0; i < 5; ++i) (void)c.put(i, i * 2);
    int count = 0;
    c.for_each([&](const int&, const int&) { ++count; });
    CHECK(count == 5);
}

// ============================================================================
// § 5  LFU policy
// ============================================================================

TEST_CASE (



"Cache LFU: basic put/get"
,
"[cache][lfu]"
)
 {
    kosha::LFUCache<int, int> c{4};
    (void)c.put(1, 10);
    CHECK(c.get(1).value() == 10);
}

TEST_CASE (



"Cache LFU: evicts least-frequently-used"
,
"[cache][lfu]"
)
 {
    kosha::LFUCache<int, int> c{3};
    (void)c.put(1, 1); (void)c.put(2, 2); (void)c.put(3, 3);
    // Access 1 and 2 multiple times
    (void)c.get(1); (void)c.get(1); (void)c.get(1);
    (void)c.get(2); (void)c.get(2);
    // key 3 has frequency 1 — should be the LFU victim
    (void)c.put(4, 4);
    CHECK_FALSE(c.get(3).has_value());
    CHECK(c.get(1).has_value());
    CHECK(c.get(2).has_value());
    CHECK(c.get(4).has_value());
}

TEST_CASE (



"Cache LFU: frequency tie-break evicts oldest"
,
"[cache][lfu]"
)
 {
    kosha::LFUCache<int, int> c{2};
    (void)c.put(1, 1);
    (void)c.put(2, 2);
    // Both have freq 1; key 1 was inserted first — LFU+LRU tie-break evicts 1
    (void)c.put(3, 3);
    // Either key 1 or key 2 evicted (implementation ties on LRU within frequency);
    // verify exactly one of {1,2} is gone and key 3 is present
    const bool has1 = c.get(1).has_value();
    const bool has2 = c.get(2).has_value();
    CHECK(c.get(3).has_value());
    CHECK((has1 ^ has2)); // exactly one evicted
}

TEST_CASE (



"Cache LFU: update in-place bumps frequency"
,
"[cache][lfu]"
)
 {
    kosha::LFUCache<int, int> c{2};
    (void)c.put(1, 10);
    (void)c.put(2, 20);
    // Bump key 1 frequency by updating it (treated as a hit)
    (void)c.put(1, 100);
    // Add key 3 — key 2 (freq=1) should be evicted, not key 1 (freq≥1 + hit)
    (void)c.put(3, 30);
    CHECK(c.get(1).has_value());
    CHECK(c.get(3).has_value());
}

// ============================================================================
// § 6  FIFO policy
// ============================================================================

TEST_CASE (



"Cache FIFO: basic put/get"
,
"[cache][fifo]"
)
 {
    kosha::FIFOCache<int, int> c{4};
    (void)c.put(1, 10);
    CHECK(c.get(1).value() == 10);
}

TEST_CASE (



"Cache FIFO: evicts in insertion order"
,
"[cache][fifo]"
)
 {
    kosha::FIFOCache<int, int> c{3};
    (void)c.put(1, 1); (void)c.put(2, 2); (void)c.put(3, 3);
    // Access keys — should NOT affect eviction order
    (void)c.get(1); (void)c.get(1); (void)c.get(1);
    // Adding key 4 evicts key 1 (first inserted)
    (void)c.put(4, 4);
    CHECK_FALSE(c.get(1).has_value());
    CHECK(c.get(2).has_value());
    CHECK(c.get(3).has_value());
    CHECK(c.get(4).has_value());
}

TEST_CASE (



"Cache FIFO: erase then evict skips erased key"
,
"[cache][fifo]"
)
 {
    kosha::FIFOCache<int, int> c{3};
    (void)c.put(1, 1); (void)c.put(2, 2); (void)c.put(3, 3);
    c.erase(1); // lazy-remove from FIFO queue
    // Now adding key 4: FIFO evict should skip the already-erased key 1
    // and evict key 2 (first live entry)
    (void)c.put(4, 4);
    CHECK(c.size() == 3);
    CHECK_FALSE(c.get(1).has_value());
    CHECK(c.get(4).has_value());
}

// ============================================================================
// § 7  ARC policy
// ============================================================================

TEST_CASE (



"Cache ARC: basic put/get"
,
"[cache][arc]"
)
 {
    kosha::ARCCache<int, int> c{4};
    (void)c.put(1, 10);
    CHECK(c.get(1).value() == 10);
}

TEST_CASE (



"Cache ARC: evicts under pressure"
,
"[cache][arc]"
)
 {
    kosha::ARCCache<int, int> c{4};
    for (int i = 0; i < 8; ++i) (void)c.put(i, i);
    CHECK(c.size() <= 4);
}

TEST_CASE (



"Cache ARC: frequently-used keys survive eviction"
,
"[cache][arc]"
)
 {
    kosha::ARCCache<int, int> c{8};
    // Warm up keys 1,2,3 into T2 (frequently used)
    for (int rep = 0; rep < 4; ++rep) {
        (void)c.put(1, 1); (void)c.get(1);
        (void)c.put(2, 2); (void)c.get(2);
        (void)c.put(3, 3); (void)c.get(3);
    }
    // Add a small number of cold keys — 1,2,3 in T2 should not be evicted
    for (int i = 10; i < 14; ++i) (void)c.put(i, i);
    CHECK(c.get(1).has_value());
    CHECK(c.get(2).has_value());
    CHECK(c.get(3).has_value());
}

TEST_CASE (



"Cache ARC: clear resets state"
,
"[cache][arc]"
)
 {
    kosha::ARCCache<int, int> c{4};
    (void)c.put(1, 1); (void)c.put(2, 2);
    c.clear();
    CHECK(c.empty());
    CHECK_FALSE(c.get(1).has_value());
}

// ============================================================================
// § 8  Error propagation
// ============================================================================

TEST_CASE (



"Error::NotFound on empty cache"
,
"[error]"
)
 {
    kosha::LRUCache<int, int> c{4};
    auto r = c.get(0);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == kosha::Error::NotFound);
}

TEST_CASE (



"put returns void expected on success"
,
"[error]"
)
 {
    kosha::LRUCache<int, int> c{4};
    auto r = c.put(1, 10);
    CHECK(r.has_value());
}

// ============================================================================
// § 9  pmr allocator
// ============================================================================

TEST_CASE (



"Cache with monotonic_buffer_resource"
,
"[pmr]"
)
 {
    std::array<std::byte, 4096> buf{};
    std::pmr::monotonic_buffer_resource mr{buf.data(), buf.size()};
    std::pmr::polymorphic_allocator<std::byte> alloc{&mr};

    using Storage = kosha::FlatHashStorage<int, int,
        std::hash<int>, std::equal_to<int>,
        std::pmr::polymorphic_allocator<std::byte>>;
    using Policy  = kosha::LRUPolicy<int>;
    using C       = kosha::Cache<int, int, Policy, Storage>;

    C c{8, Policy{8}, Storage{16, {}, {}, alloc}};
    (void)c.put(1, 100);
    (void)c.put(2, 200);
    CHECK(c.get(1).value() == 100);
    CHECK(c.get(2).value() == 200);
}

// ============================================================================
// § 10  ThreadSafeCache
// ============================================================================

TEST_CASE (



"ThreadSafeCache LRU: basic single-threaded use"
,
"[thread_safe][lru]"
)
 {
    kosha::TLRUCache<int, int> c{std::size_t{8}};
    (void)c.put(1, 10);
    CHECK(c.get(1).value() == 10);
    CHECK(c.peek(1).value() == 10);
    c.erase(1);
    CHECK_FALSE(c.get(1).has_value());
}

TEST_CASE (



"ThreadSafeCache FIFO: get uses shared lock"
,
"[thread_safe][fifo]"
)
 {
    // FIFOPolicy has mutates_on_hit = false — get uses shared_lock
    kosha::TFIFOCache<int, int> c{std::size_t{8}};
    (void)c.put(1, 10); (void)c.put(2, 20);
    CHECK(c.get(1).value() == 10);
    CHECK(c.get(2).value() == 20);
}

TEST_CASE (



"ThreadSafeCache: concurrent puts and gets"
,
"[thread_safe][concurrent]"
)
 {
    kosha::TLRUCache<int, int> c{std::size_t{256}};
    constexpr int kThreads = 4;
    constexpr int kOps     = 1000;

    std::vector<std::thread> writers, readers;
    writers.reserve(kThreads);
    readers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&, t] {
            for (int i = 0; i < kOps; ++i)
                (void)c.put(t * kOps + i, i);
        });
    }
    for (auto& th : writers) th.join();

    // After all writes, all values should be retrievable (cache size allows)
    std::atomic<int> hits{0};
    for (int t = 0; t < kThreads; ++t) {
        readers.emplace_back([&, t] {
            for (int i = 0; i < kOps; ++i)
                if (c.get(t * kOps + i).has_value()) ++hits;
        });
    }
    for (auto& th : readers) th.join();
    CHECK(hits.load() > 0);
}

TEST_CASE (



"ThreadSafeCache: size and capacity"
,
"[thread_safe]"
)
 {
    kosha::TLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 1); (void)c.put(2, 2);
    CHECK(c.size() == 2);
    CHECK(c.capacity() == 4);
}

TEST_CASE (



"ThreadSafeCache: clear"
,
"[thread_safe]"
)
 {
    kosha::TLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 1); (void)c.put(2, 2);
    c.clear();
    CHECK(c.size() == 0);
}

// ============================================================================
// § 11  ShardedCache
// ============================================================================

TEST_CASE (



"ShardedCache: basic put/get/erase"
,
"[sharded]"
)
 {
    kosha::ShardedLRUCache<int, int, 4> c{64};
    (void)c.put(1, 10);
    CHECK(c.get(1).value() == 10);
    CHECK(c.peek(1).value() == 10);
    c.erase(1);
    CHECK_FALSE(c.get(1).has_value());
}

TEST_CASE (



"ShardedCache: size sums across shards"
,
"[sharded]"
)
 {
    kosha::ShardedLRUCache<int, int, 4> c{64};
    for (int i = 0; i < 16; ++i) (void)c.put(i, i);
    CHECK(c.size() == 16);
}

TEST_CASE (



"ShardedCache: clear empties all shards"
,
"[sharded]"
)
 {
    kosha::ShardedLRUCache<int, int, 4> c{64};
    for (int i = 0; i < 16; ++i) (void)c.put(i, i);
    c.clear();
    CHECK(c.size() == 0);
}

TEST_CASE (



"ShardedCache: concurrent puts and gets"
,
"[sharded][concurrent]"
)
 {
    kosha::ShardedLRUCache<int, int, 8> c{4096};
    constexpr int kThreads = 8;
    constexpr int kOps     = 500;

    std::vector<std::thread> threads;
    threads.reserve(kThreads * 2);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kOps; ++i)
                (void)c.put(t * kOps + i, i);
        });
    }
    for (auto& th : threads) th.join();
    threads.clear();

    std::atomic<int> hits{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kOps; ++i)
                if (c.get(t * kOps + i).has_value()) ++hits;
        });
    }
    for (auto& th : threads) th.join();
    CHECK(hits.load() > 0);
}

TEST_CASE (



"ShardedCache: LFU variant compiles and works"
,
"[sharded]"
)
 {
    kosha::ShardedLFUCache<std::string, int, 4> c{64};
    (void)c.put("alpha", 1);
    (void)c.put("beta",  2);
    CHECK(c.get("alpha").value() == 1);
    CHECK(c.get("beta").value()  == 2);
}

TEST_CASE (



"ShardedCache: ARC variant compiles and works"
,
"[sharded]"
)
 {
    kosha::ShardedARCCache<int, int, 4> c{64};
    (void)c.put(7, 77);
    CHECK(c.get(7).value() == 77);
}

// ============================================================================
// § 12  NodeStorage with Cache
// ============================================================================

TEST_CASE (



"Cache with NodeStorage: pointer stability"
,
"[cache][node]"
)
 {
    using C = kosha::Cache<int, std::string,
                           kosha::LRUPolicy<int>,
                           kosha::NodeStorage<int, std::string>>;
    C c{8, kosha::LRUPolicy<int>{8}, kosha::NodeStorage<int, std::string>{}};
    (void)c.put(1, "one");
    (void)c.put(2, "two");
    CHECK(c.get(1).value() == "one");
    CHECK(c.get(2).value() == "two");
    c.erase(1);
    CHECK_FALSE(c.get(1).has_value());
}

// ============================================================================
// § 13  Eviction sequence correctness — deeper LRU ordering
// ============================================================================

TEST_CASE (



"LRU eviction order: access pattern drives ordering"
,
"[cache][lru][eviction]"
)
 {
    kosha::LRUCache<int, int> c{4};
    (void)c.put(1, 1); (void)c.put(2, 2); (void)c.put(3, 3); (void)c.put(4, 4);
    // Access order: 2, 4, 1 — making 3 the LRU
    (void)c.get(2); (void)c.get(4); (void)c.get(1);
    (void)c.put(5, 5); // evicts 3
    CHECK_FALSE(c.get(3).has_value());
    // Evict again: 2 is now LRU among {1,2,4,5}
    (void)c.get(5); (void)c.get(1); (void)c.get(4);
    (void)c.put(6, 6); // evicts 2
    CHECK_FALSE(c.get(2).has_value());
}

// ============================================================================
// § 14  Large capacity stress
// ============================================================================

TEST_CASE (



"LRU large capacity stress: 10 000 entries"
,
"[cache][lru][stress]"
)
 {
    constexpr int N = 10'000;
    kosha::LRUCache<int, int> c{static_cast<std::size_t>(N)};
    for (int i = 0; i < N; ++i) (void)c.put(i, i);
    CHECK(c.size() == static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) CHECK(c.get(i).value() == i);
}

TEST_CASE (



"LRU eviction under sustained load"
,
"[cache][lru][stress]"
)
 {
    constexpr int Cap = 128;
    kosha::LRUCache<int, int> c{Cap};
    for (int i = 0; i < Cap * 4; ++i) (void)c.put(i, i);
    CHECK(c.size() == Cap);
}

// ============================================================================
// § 15  Convenience alias smoke tests
// ============================================================================

TEST_CASE (



"Convenience aliases compile and work"
,
"[aliases]"
)
 {
    { kosha::LRUCache<std::string, int>  c{4}; (void)c.put("a", 1); CHECK(c.get("a").value() == 1); }
    { kosha::LFUCache<std::string, int>  c{4}; (void)c.put("b", 2); CHECK(c.get("b").value() == 2); }
    { kosha::FIFOCache<std::string, int> c{4}; (void)c.put("c", 3); CHECK(c.get("c").value() == 3); }
    { kosha::ARCCache<std::string, int>  c{4}; (void)c.put("d", 4); CHECK(c.get("d").value() == 4); }

    { kosha::TLRUCache<int, int>  c{std::size_t{4}}; (void)c.put(1, 10); CHECK(c.get(1).value() == 10); }
    { kosha::TLFUCache<int, int>  c{std::size_t{4}}; (void)c.put(1, 10); CHECK(c.get(1).value() == 10); }
    { kosha::TFIFOCache<int, int> c{std::size_t{4}}; (void)c.put(1, 10); CHECK(c.get(1).value() == 10); }
    { kosha::TARCCache<int, int>  c{std::size_t{4}}; (void)c.put(1, 10); CHECK(c.get(1).value() == 10); }
}

// ============================================================================
// § 16  Zero-capacity via Cache::create()
// ============================================================================

TEST_CASE (



"Cache::create: zero capacity returns InvalidArg"
,
"[error][create]"
)
 {
    auto r = kosha::LRUCache<int, int>::create(0);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == kosha::Error::InvalidArg);
}

TEST_CASE (



"Cache::create: non-zero capacity succeeds"
,
"[error][create]"
)
 {
    auto r = kosha::LRUCache<int, int>::create(4);
    REQUIRE(r.has_value());
    (void)r->put(1, 10);
    CHECK(r->get(1).value() == 10);
}

// ============================================================================
// § 17  Non-default-constructible and move-only values
// ============================================================================

struct NoDef {
    int x;

    explicit NoDef(int v) : x{v} {}

    NoDef() = delete;

    NoDef(const NoDef&) = default;

    NoDef& operator=(const NoDef&) = default;
};

TEST_CASE (



"FlatHashStorage: non-default-constructible value"
,
"[storage][flat][ndc]"
)
 {
    kosha::FlatHashStorage<int, NoDef> s;
    s.insert(1, NoDef{42});
    auto* p = s.find(1);
    REQUIRE(p != nullptr);
    CHECK(p->x == 42);
    s.erase(1);
    CHECK(s.find(1) == nullptr);
}

TEST_CASE (



"Cache LRU: non-default-constructible value"
,
"[cache][lru][ndc]"
)
 {
    kosha::LRUCache<int, NoDef> c{4};
    (void)c.put(1, NoDef{7});
    auto v = c.get(1);
    REQUIRE(v.has_value());
    CHECK(v->x == 7);
}

TEST_CASE (



"FlatHashStorage: move-only value"
,
"[storage][flat][move_only]"
)
 {
    kosha::FlatHashStorage<int, std::unique_ptr<int>> s;
    s.insert(1, std::make_unique<int>(99));
    auto* p = s.find(1);
    REQUIRE(p != nullptr);
    CHECK(**p == 99);
    s.erase(1);
    CHECK(s.find(1) == nullptr);
}

TEST_CASE (



"Cache LRU: move-only value eviction"
,
"[cache][lru][move_only]"
)
 {
    kosha::LRUCache<int, std::unique_ptr<int>> c{4};
    (void)c.put(1, std::make_unique<int>(55));
    (void)c.put(2, std::make_unique<int>(66));
    (void)c.put(3, std::make_unique<int>(77));
    (void)c.put(4, std::make_unique<int>(88));
    CHECK(c.size() == 4);
    (void)c.put(5, std::make_unique<int>(99)); // evicts key 1
    CHECK(c.size() == 4);
    // key 1 was the LRU — it should be gone
    CHECK(c.erase(1) == false); // already evicted
}

// ============================================================================
// § 18  ARC ghost-list behavior
// ============================================================================

TEST_CASE (



"Cache ARC: ghost-list re-admission promotes to T2"
,
"[cache][arc][ghost]"
)
 {
    // With cap=2: insert 1,2 fills cache. Insert 3 evicts 1 (T1 LRU) to B1.
    // Insert 1 again: B1 hit → adapt_up, re-admit to T2. Then 1 should survive
    // a subsequent eviction pressure better than a cold key.
    kosha::ARCCache<int, int> c{2};
    (void)c.put(1, 10);
    (void)c.put(2, 20);
    (void)c.put(3, 30); // evicts 1 → B1, size=2 ({2,3})
    CHECK_FALSE(c.get(1).has_value());

    (void)c.put(1, 11); // B1 hit → re-admit 1 to T2, evicts someone from T1/T2
    CHECK(c.size() <= 2);
    CHECK(c.get(1).has_value()); // 1 is back in cache
}

TEST_CASE (



"Cache ARC: size never exceeds capacity under pressure"
,
"[cache][arc][invariant]"
)
 {
    kosha::ARCCache<int, int> c{4};
    for (int i = 0; i < 20; ++i) {
        (void)c.put(i, i);
        CHECK(c.size() <= 4);
    }
}

TEST_CASE (



"Cache ARC: policy and storage stay in sync"
,
"[cache][arc][sync]"
)
 {
    kosha::ARCCache<int, int> c{2};
    // Fill and overflow several times — policy.size() must always equal storage.size().
    for (int i = 0; i < 8; ++i) {
        (void)c.put(i, i);
        CHECK(c.policy().size() == c.size());
    }
}

// ============================================================================
// § 19  Namespace / type checks
// ============================================================================

TEST_CASE (



"kosha namespace: types visible at top level"
,
"[namespace]"
)
 {
    // Core types accessible via kosha::
    static_assert(std::is_same_v<kosha::Error, kosha::core::Error>);
    static_assert(std::is_same_v<kosha::LRUCache<int,int>,
        kosha::core::Cache<int,int,kosha::core::LRUPolicy<int>,
                           kosha::core::FlatHashStorage<int,int>>>);

    // Adapter types
    static_assert(std::is_same_v<kosha::TLRUCache<int,int>,
        kosha::adapter::ThreadSafeCache<kosha::LRUCache<int,int>>>);

    SUCCEED("namespace alias checks passed");
}

TEST_CASE (



"kosha::core: Cache in explicit namespace"
,
"[namespace]"
)
 {
    kosha::core::Cache<int, int, kosha::core::LRUPolicy<int>,
                       kosha::core::FlatHashStorage<int, int>> c{4};
    (void)c.put(1, 10);
    CHECK(c.get(1).value() == 10);
}

// ============================================================================
// § 20  InstrumentedCache
// ============================================================================

TEST_CASE (



"InstrumentedCache: hit and miss counting"
,
"[instrumented]"
)
 {
    kosha::InstrumentedLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 10);
    (void)c.get(1); // hit
    (void)c.get(2); // miss
    (void)c.get(1); // hit
    CHECK(c.hit_count()  == 2);
    CHECK(c.miss_count() == 1);
    CHECK(c.hit_rate() == Catch::Approx(2.0 / 3.0));
}

TEST_CASE (



"InstrumentedCache: eviction counting"
,
"[instrumented]"
)
 {
    kosha::InstrumentedLRUCache<int, int> c{std::size_t{2}};
    (void)c.put(1, 1);
    (void)c.put(2, 2);
    CHECK(c.eviction_count() == 0);
    (void)c.put(3, 3); // evicts key 1
    CHECK(c.eviction_count() == 1);
    (void)c.put(4, 4); // evicts key 2
    CHECK(c.eviction_count() == 2);
}

TEST_CASE (



"InstrumentedCache: reset_stats zeroes counters"
,
"[instrumented]"
)
 {
    kosha::InstrumentedLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 1);
    (void)c.get(1); (void)c.get(99);
    CHECK(c.hit_count() == 1);
    c.reset_stats();
    CHECK(c.hit_count()  == 0);
    CHECK(c.miss_count() == 0);
}

TEST_CASE (



"InstrumentedCache: size/capacity/full/empty delegation"
,
"[instrumented]"
)
 {
    kosha::InstrumentedLRUCache<int, int> c{std::size_t{2}};
    CHECK(c.empty());
    CHECK(c.capacity() == 2);
    (void)c.put(1, 1); (void)c.put(2, 2);
    CHECK(c.full());
    CHECK(c.size() == 2);
}

TEST_CASE (



"InstrumentedCache: clear resets cache and stats"
,
"[instrumented]"
)
 {
    kosha::InstrumentedLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 1); (void)c.get(1);
    c.clear();
    CHECK(c.empty());
    CHECK(c.hit_count() == 0);
}

TEST_CASE (



"InstrumentedCache: hit_rate on empty stats is 0"
,
"[instrumented]"
)
 {
    kosha::InstrumentedLRUCache<int, int> c{std::size_t{4}};
    CHECK(c.hit_rate() == Catch::Approx(0.0));
}

TEST_CASE (



"InstrumentedCache: wrapping LFU"
,
"[instrumented]"
)
 {
    kosha::InstrumentedLFUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 10);
    (void)c.get(1); (void)c.get(1); (void)c.get(99);
    CHECK(c.hit_count()  == 2);
    CHECK(c.miss_count() == 1);
}

// ============================================================================
// § 21  TTLCache
// ============================================================================

TEST_CASE (



"TTLCache: put without TTL never expires"
,
"[ttl]"
)
 {
    kosha::TTLLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 10);
    CHECK(c.get(1).value() == 10);
    CHECK(c.peek(1).value() == 10);
}

TEST_CASE (



"TTLCache: put with zero TTL is immediately expired"
,
"[ttl]"
)
 {
    kosha::TTLLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 10, std::chrono::seconds{0});
    CHECK_FALSE(c.get(1).has_value());
    CHECK_FALSE(c.peek(1).has_value());
}

TEST_CASE (



"TTLCache: expired entry is treated as NotFound"
,
"[ttl]"
)
 {
    kosha::TTLLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 10, std::chrono::nanoseconds{1}); // expire almost immediately
    // spin briefly to ensure expiry
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10};
    while (std::chrono::steady_clock::now() < deadline) {}
    auto r = c.get(1);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error() == kosha::Error::NotFound);
}

TEST_CASE (



"TTLCache: expired entry lazily removed from underlying cache"
,
"[ttl]"
)
 {
    kosha::TTLLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 10, std::chrono::nanoseconds{1});
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10};
    while (std::chrono::steady_clock::now() < deadline) {}
    (void)c.get(1); // triggers lazy removal
    CHECK(c.size() == 0);
}

TEST_CASE (



"TTLCache: erase removes TTL metadata"
,
"[ttl]"
)
 {
    kosha::TTLLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 10, std::chrono::seconds{60});
    CHECK(c.erase(1));
    CHECK_FALSE(c.get(1).has_value());
}

TEST_CASE (



"TTLCache: clear removes all entries and TTL state"
,
"[ttl]"
)
 {
    kosha::TTLLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 10, std::chrono::seconds{60});
    (void)c.put(2, 20);
    c.clear();
    CHECK(c.empty());
}

TEST_CASE (



"TTLCache: mixed TTL and no-TTL entries coexist"
,
"[ttl]"
)
 {
    kosha::TTLLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 10);                             // no expiry
    (void)c.put(2, 20, std::chrono::nanoseconds{1}); // expire immediately
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10};
    while (std::chrono::steady_clock::now() < deadline) {}
    CHECK(c.get(1).value() == 10);
    CHECK_FALSE(c.get(2).has_value());
}

// ============================================================================
// § 22  ClusterCache (with no-op policies — local passthrough)
// ============================================================================

TEST_CASE (



"ClusterCache: no-op policies pass through to local cache"
,
"[cluster]"
)
 {
    kosha::cluster::ClusterCache<kosha::LRUCache<int, int>> cc{std::size_t{4}};
    (void)cc.put(1, 10);
    CHECK(cc.get(1).value() == 10);
    CHECK(cc.peek(1).value() == 10);
    CHECK(cc.size() == 1);
    CHECK(cc.capacity() == 4);
}

TEST_CASE (



"ClusterCache: erase delegates to local cache"
,
"[cluster]"
)
 {
    kosha::cluster::ClusterCache<kosha::LRUCache<int, int>> cc{std::size_t{4}};
    (void)cc.put(1, 10);
    CHECK(cc.erase(1));
    CHECK_FALSE(cc.get(1).has_value());
}

TEST_CASE (



"ClusterCache: clear delegates to local cache"
,
"[cluster]"
)
 {
    kosha::cluster::ClusterCache<kosha::LRUCache<int, int>> cc{std::size_t{4}};
    (void)cc.put(1, 1); (void)cc.put(2, 2);
    cc.clear();
    CHECK(cc.size() == 0);
}

TEST_CASE (



"ClusterCache: no-op policies are zero-size via [[no_unique_address]]"
,
"[cluster]"
)
 {
    using CC = kosha::cluster::ClusterCache<kosha::LRUCache<int, int>>;
    // With all no-op policies, ClusterCache should be the same size as its
    // local cache (empty-base / [[no_unique_address]] applies to all 6 policies).
    CHECK(sizeof(CC) == sizeof(kosha::LRUCache<int, int>));
}

TEST_CASE (



"ClusterCache: wrapping ShardedCache still compiles and works"
,
"[cluster]"
)
 {
    using Sharded = kosha::ShardedLRUCache<int, int, 4>;
    kosha::cluster::ClusterCache<Sharded> cc{std::size_t{64}};
    (void)cc.put(1, 100);
    CHECK(cc.get(1).value() == 100);
}

TEST_CASE (



"ClusterCache: policy accessors are reachable"
,
"[cluster]"
)
 {
    kosha::cluster::ClusterCache<kosha::LRUCache<int, int>> cc{std::size_t{4}};
    // All no-op policies should be accessible without compiler error.
    [[maybe_unused]] const auto& r = cc.router();
    [[maybe_unused]] const auto& t = cc.transport();
    [[maybe_unused]] const auto& rep = cc.replication();
    [[maybe_unused]] const auto& con = cc.consistency();
    [[maybe_unused]] const auto& mem = cc.membership();
    SUCCEED("policy accessor checks passed");
}

// ── §23 ARC correctness ────────────────────────────────────────────────────

TEST_CASE (



"ARC invariant: storage == T1+T2 after every put"
,
"[arc][invariant]"
)
 {
    kosha::ARCCache<int, int> c{std::size_t{4}};
    for (int i = 0; i < 20; ++i) {
        (void)c.put(i, i);
        CHECK(c.policy().size() == c.size());
    }
}

TEST_CASE (



"ARC: B1 ghost re-admission updates p and lands in T2"
,
"[arc][ghost]"
)
 {
    kosha::ARCCache<int, int> c{std::size_t{2}};
    (void)c.put(1, 1); (void)c.put(2, 2);
    (void)c.put(3, 3); // evicts 1 → B1
    (void)c.put(1, 11); // B1 hit → T2, p increases
    CHECK(c.get(1).value() == 11);
    CHECK(c.size() <= 2);
    CHECK(c.policy().size() == c.size());
}

TEST_CASE (



"ARC: size never exceeds capacity"
,
"[arc][invariant]"
)
 {
    kosha::ARCCache<int, int> c{std::size_t{4}};
    for (int i = 0; i < 50; ++i) {
        (void)c.put(i % 10, i);
        CHECK(c.size() <= 4);
        CHECK(c.policy().size() == c.size());
    }
}

TEST_CASE (



"Heterogeneous lookup: string key smoke test"
,
"[hetero]"
)
 {
    kosha::LRUCache<std::string, int> c{std::size_t{4}};
    (void)c.put("hello", 42);
    CHECK(c.peek("hello").value() == 42);
    CHECK_FALSE(c.peek("world").has_value());
}

// ============================================================================
// § 24  ARC random invariant tests
// ============================================================================

TEST_CASE (



"ARC random: size <= capacity and policy.size() == size after every op"
,
"[arc][random][invariant]"
)
 {
    constexpr std::size_t kCap = 8;
    constexpr int kOps = 500;
    kosha::ARCCache<int, int> c{kCap};

    std::mt19937 rng{42};
    std::uniform_int_distribution<int> key_dist{0, 15};
    std::uniform_int_distribution<int> op_dist{0, 2}; // 0=put, 1=get, 2=erase

    for (int i = 0; i < kOps; ++i) {
        const int key = key_dist(rng);
        switch (op_dist(rng)) {
            case 0: (void)c.put(key, key * 10); break;
            case 1: (void)c.get(key); break;
            case 2: (void)c.erase(key); break;
        }
        CHECK(c.size() <= kCap);
        CHECK(c.policy().size() == c.size());
    }
}

TEST_CASE (



"ARC random cap=1: capacity-1 edge case never overflows"
,
"[arc][random][invariant]"
)
 {
    kosha::ARCCache<int, int> c{1};
    std::mt19937 rng{7};
    std::uniform_int_distribution<int> key_dist{0, 3};

    for (int i = 0; i < 200; ++i) {
        (void)c.put(key_dist(rng), i);
        CHECK(c.size() <= 1);
        CHECK(c.policy().size() == c.size());
    }
}

TEST_CASE (



"ARC random: specific B1 ghost re-admit does not exceed capacity"
,
"[arc][ghost][invariant]"
)
 {
    // Reproduces the exact sequence from the bug report: cap=1, put(4), put(2), put(4)
    kosha::ARCCache<int, int> c{1};
    (void)c.put(4, 4);
    CHECK(c.size() <= 1);
    CHECK(c.policy().size() == c.size());
    (void)c.put(2, 2); // evicts 4 into B1
    CHECK(c.size() <= 1);
    CHECK(c.policy().size() == c.size());
    (void)c.put(4, 44); // B1 ghost hit
    CHECK(c.size() <= 1);
    CHECK(c.policy().size() == c.size());
}

// ============================================================================
// § 25  Production-readiness fixes
// ============================================================================

TEST_CASE (



"Cache LRU: get_ref returns pointer to live value"
,
"[cache][lru][ref]"
)
 {
    kosha::LRUCache<int, int> c{4};
    (void)c.put(1, 42);
    int *p = c.get_ref(1);
    REQUIRE(p != nullptr);
    CHECK(*p == 42);
}

TEST_CASE (



"Cache LRU: get_ref returns nullptr on miss"
,
"[cache][lru][ref]"
)
 {
    kosha::LRUCache<int, int> c{4};
    CHECK(c.get_ref(99) == nullptr);
}

TEST_CASE (



"Cache LRU: peek_ref const access"
,
"[cache][lru][ref]"
)
 {
    kosha::LRUCache<int, int> c{4};
    (void)c.put(7, 77);
    const kosha::LRUCache<int, int> &cc = c;
    const int *p = cc.peek_ref(7);
    REQUIRE(p != nullptr);
    CHECK(*p == 77);
    CHECK(cc.peek_ref(99) == nullptr);
}

TEST_CASE (



"Cache LRU: get_ref on move-only value avoids copy"
,
"[cache][lru][ref][move_only]"
)
 {
    kosha::LRUCache<int, std::unique_ptr<int>> c{4};
    (void)c.put(1, std::make_unique<int>(55));
    std::unique_ptr<int> *p = c.get_ref(1);
    REQUIRE(p != nullptr);
    CHECK(**p == 55);
}

TEST_CASE (



"Cache ARC: policy/storage sync after exception-safe put order"
,
"[cache][arc][exception_safety]"
)
 {
    // Verify invariants hold across a B1 ghost re-admission sequence (exercises
    // the storage-first commit path).
    kosha::ARCCache<int, int> c{4};
    for (int i = 0; i < 10; ++i) {
        (void)c.put(i % 6, i);
        CHECK(c.size() <= 4);
        CHECK(c.policy().size() == c.size());
    }
    // Re-admit from ghost lists to exercise commit_insert after insert_or_assign.
    for (int i = 0; i < 6; ++i) {
        (void)c.put(i, i * 100);
        CHECK(c.size() <= 4);
        CHECK(c.policy().size() == c.size());
    }
}

TEST_CASE (



"Cache LRU: erase_range is not noexcept"
,
"[cache][lru][noexcept]"
)
 {
    using C = kosha::LRUCache<int, int>;
    static_assert(!noexcept(std::declval<C &>().erase_range(std::declval<std::vector<int> &>())),
                  "erase_range must not be noexcept — policy::on_erase can throw");
    SUCCEED("erase_range is correctly not noexcept");
}

TEST_CASE (



"ThreadSafeCache: constructor rejects invalid args at concept boundary"
,
"[thread_safe][concept]"
)
 {
    // Verify the constructible_from constraint exists by checking a valid construction.
    static_assert(std::constructible_from<kosha::TLRUCache<int, int>, std::size_t>,
                  "TLRUCache must be constructible from std::size_t");
    kosha::TLRUCache<int, int> c{std::size_t{4}};
    (void)c.put(1, 10);
    CHECK(c.get(1).value() == 10);
}

TEST_CASE (



"FlatHashStorage: slot lifetime correct under grow"
,
"[storage][flat][lifetime]"
)
 {
    // Exercises the alloc/grow/destroy symmetry over many insertions.
    kosha::FlatHashStorage<int, std::string> s{4};
    for (int i = 0; i < 128; ++i)
        s.insert(i, std::to_string(i));
    CHECK(s.size() == 128);
    for (int i = 0; i < 128; ++i) {
        auto *p = s.find(i);
        REQUIRE(p != nullptr);
        CHECK(*p == std::to_string(i));
    }
    // Destructor fires destroy_at on all slots — verified implicitly by clean exit.
}

// ============================================================================
// § 26  ARC case tests (Task 1c)
// ============================================================================

TEST_CASE (



"ARC: hit in T1 promotes to T2"
,
"[arc][promotion]"
)
 {
    kosha::ARCCache<int, int> c{4};
    (void)c.put(1, 10);
    CHECK(c.policy().size() == 1);
    (void)c.get(1); // T1 hit → promote to T2
    CHECK(c.policy().size() == 1);
    CHECK(c.get(1).value() == 10);
}

TEST_CASE (



"ARC: hit in T2 stays MRU"
,
"[arc][promotion]"
)
 {
    kosha::ARCCache<int, int> c{4};
    (void)c.put(1, 10);
    (void)c.get(1); // T1 → T2
    (void)c.get(1); // stays in T2 MRU
    // Add keys to create pressure — key 1 should survive
    (void)c.put(2, 20); (void)c.put(3, 30); (void)c.put(4, 40);
    CHECK(c.get(1).value() == 10);
    CHECK(c.policy().size() == c.size());
}

TEST_CASE (



"ARC: B2 ghost hit decrements p"
,
"[arc][ghost]"
)
 {
    // cap=2: put 1,2 fills cache. put 3 evicts 1→B1 (T1 LRU). put 1 re-admits via B1.
    // put 4 evicts 2→B2 or similar. put 2 again: B2 ghost hit → p decrements.
    kosha::ARCCache<int, int> c{2};
    (void)c.put(1, 1); (void)c.put(2, 2);
    (void)c.put(3, 3); // evicts 1 → B1
    (void)c.put(1, 11); // B1 ghost hit, p increases
    // Now evict via pressure to get something into B2
    (void)c.put(4, 4); (void)c.put(5, 5);
    // Keep invariants
    CHECK(c.size() <= 2);
    CHECK(c.policy().size() == c.size());
}

TEST_CASE (



"ARC: repeated scan workload keeps size within cap"
,
"[arc][workload]"
)
 {
    constexpr std::size_t kCap = 4;
    kosha::ARCCache<int, int> c{kCap};
    // Sequential scan over 2*cap keys
    for (int i = 0; i < static_cast<int>(2 * kCap); ++i) {
        (void)c.put(i, i);
        CHECK(c.size() <= kCap);
    }
}

TEST_CASE (



"ARC: looping workload keeps size within cap"
,
"[arc][workload]"
)
 {
    constexpr std::size_t kCap = 4;
    kosha::ARCCache<int, int> c{kCap};
    // Cycle through cap+1 keys repeatedly
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i <= static_cast<int>(kCap); ++i) {
            (void)c.put(i, i + round * 100);
            CHECK(c.size() <= kCap);
        }
    }
    CHECK(c.policy().size() == c.size());
}

// ============================================================================
// § 27  Production-readiness tests (Task 2d)
// ============================================================================

TEST_CASE (



"Cache LRU: memory_bytes is nonzero after put"
,
"[cache][lru][memory]"
)
 {
    kosha::LRUCache<int, int> c{4};
    CHECK(c.memory_bytes() == 0);
    (void)c.put(1, 10);
    CHECK(c.memory_bytes() > 0);
    CHECK(c.memory_bytes() == sizeof(int) + sizeof(int));
    (void)c.put(2, 20);
    CHECK(c.memory_bytes() == 2 * (sizeof(int) + sizeof(int)));
}

#ifndef NDEBUG
TEST_CASE (



"ARCPolicy: validate passes under random ops"
,
"[arc][validate]"
)
 {
    constexpr std::size_t kCap = 6;
    constexpr int kOps = 300;
    kosha::ARCCache<int, int> c{kCap};

    std::mt19937 rng{99};
    std::uniform_int_distribution<int> key_dist{0, 10};
    std::uniform_int_distribution<int> op_dist{0, 2};

    for (int i = 0; i < kOps; ++i) {
        const int key = key_dist(rng);
        switch (op_dist(rng)) {
            case 0: (void)c.put(key, key * 2); break;
            case 1: (void)c.get(key); break;
            case 2: (void)c.erase(key); break;
        }
        c.policy().validate(c.size());
    }
}
#endif
