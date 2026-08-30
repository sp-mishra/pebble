// ============================================================================
// test_bplus_tree.cpp — Unit tests for pebble::containers::BPlusTree
// ============================================================================

#include <catch_amalgamated.hpp>
#include "containers/tree/bplus_tree.hpp"
#include "mem/smriti.hpp"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace pebble::containers;

struct TinyBPlusTraits {
    static constexpr std::size_t LeafCapacity = 4;
    static constexpr std::size_t InnerCapacity = 4;
    static constexpr bool EnableSIMD = true;
    static constexpr std::size_t MaxRecycleNodes = 64;
};

TEST_CASE("containers::BPlusTree: Basic insertion, find, and update", "[containers][tree][bplus_tree]") {
    BPlusMap<int, std::string> tree;
    REQUIRE(tree.empty());
    REQUIRE(tree.size() == 0);

    auto [it1, ins1] = tree.insert_or_assign(10, "ten");
    REQUIRE(ins1);
    CHECK(it1->first == 10);
    CHECK(it1->second == "ten");
    CHECK(tree.size() == 1);
    CHECK_FALSE(tree.empty());

    auto [it2, ins2] = tree.insert_or_assign(20, "twenty");
    REQUIRE(ins2);
    auto [it3, ins3] = tree.insert_or_assign(5, "five");
    REQUIRE(ins3);

    CHECK(tree.size() == 3);
    CHECK(tree.contains(10));
    CHECK(tree.contains(20));
    CHECK(tree.contains(5));
    CHECK_FALSE(tree.contains(999));

    CHECK(tree.at(10) == "ten");
    CHECK(tree.at(20) == "twenty");
    CHECK(tree.at(5) == "five");

    // Overwrite existing key
    auto [it4, ins4] = tree.insert_or_assign(10, "TEN_UPDATED");
    CHECK_FALSE(ins4);
    CHECK(tree.size() == 3);
    CHECK(tree.at(10) == "TEN_UPDATED");
    CHECK(tree.validate_invariants());
}

TEST_CASE("containers::BPlusTree: Transparent/Heterogeneous string lookups", "[containers][tree][bplus_tree]") {
    // Transparent lookup using std::less<> on std::string keys with std::string_view / const char*
    BPlusTree<std::string, int, std::less<>> tree;
    tree.insert_or_assign("alpha", 1);
    tree.insert_or_assign("beta", 2);
    tree.insert_or_assign("gamma", 3);

    // Lookups using std::string_view (zero allocation)
    std::string_view sv_beta = "beta";
    CHECK(tree.contains(sv_beta));
    CHECK(tree.contains("alpha"));
    CHECK_FALSE(tree.contains("delta"));

    auto it = tree.find(sv_beta);
    REQUIRE(it != tree.end());
    CHECK(it->second == 2);

    CHECK(tree.at(sv_beta) == 2);
    CHECK(tree.erase(sv_beta) == 1);
    CHECK_FALSE(tree.contains(sv_beta));
    CHECK(tree.size() == 2);
    CHECK(tree.validate_invariants());
}

TEST_CASE("containers::BPlusTree: Sequential and random multi-level splits", "[containers][tree][bplus_tree]") {
    BPlusTree<int, int, std::less<int>, TinyBPlusTraits> tree;

    constexpr int kCount = 500;
    for (int i = 0; i < kCount; ++i) {
        tree.insert_or_assign(i, i * 10);
    }

    REQUIRE(tree.size() == kCount);
    REQUIRE(tree.validate_invariants());
    REQUIRE(tree.depth() >= 2);

    for (int i = 0; i < kCount; ++i) {
        CHECK(tree.contains(i));
        CHECK(tree.at(i) == i * 10);
    }

    // Forward iterator validation (strictly ordered)
    int expected_key = 0;
    for (const auto& [k, v] : tree) {
        CHECK(k == expected_key);
        CHECK(v == expected_key * 10);
        ++expected_key;
    }
    CHECK(expected_key == kCount);
}

TEST_CASE("containers::BPlusTree: O(N) Bulk Loading from sorted sequence", "[containers][tree][bplus_tree]") {
    std::vector<std::pair<const int, int>> items;
    constexpr int kCount = 1000;
    for (int i = 0; i < kCount; ++i) {
        items.emplace_back(i, i * 100);
    }

    auto tree = BPlusTree<int, int>::from_sorted(items.begin(), items.end());
    REQUIRE(tree.size() == kCount);
    REQUIRE(tree.validate_invariants());

    for (int i = 0; i < kCount; ++i) {
        CHECK(tree.contains(i));
        CHECK(tree.at(i) == i * 100);
    }

    int expected = 0;
    for (const auto& [k, v] : tree) {
        CHECK(k == expected);
        CHECK(v == expected * 100);
        ++expected;
    }
    CHECK(expected == kCount);
}

TEST_CASE("containers::BPlusTree: Lower/Upper bound and Range Scanning with Prefetch", "[containers][tree][bplus_tree]") {
    BPlusMap<int, int> tree;
    for (int i = 10; i <= 100; i += 10) {
        tree.insert_or_assign(i, i * 2);
    }

    // lower_bound
    auto lb1 = tree.lower_bound(25);
    REQUIRE(lb1 != tree.end());
    CHECK(lb1->first == 30);

    auto lb2 = tree.lower_bound(30);
    REQUIRE(lb2 != tree.end());
    CHECK(lb2->first == 30);

    auto lb3 = tree.lower_bound(105);
    CHECK(lb3 == tree.end());

    // upper_bound
    auto ub1 = tree.upper_bound(30);
    REQUIRE(ub1 != tree.end());
    CHECK(ub1->first == 40);

    // High-throughput range scan callback
    std::vector<int> scanned_keys;
    tree.scan(25, 75, [&](int k, int v) {
        scanned_keys.push_back(k);
        CHECK(v == k * 2);
    });

    std::vector<int> expected_scanned = {30, 40, 50, 60, 70};
    CHECK(scanned_keys == expected_scanned);
}

TEST_CASE("containers::BPlusTree: Deletion, Rebalancing, and Node Freelist Recycling", "[containers][tree][bplus_tree]") {
    BPlusTree<int, int, std::less<int>, TinyBPlusTraits> tree;
    constexpr int kCount = 100;

    for (int i = 0; i < kCount; ++i) {
        tree.insert_or_assign(i, i);
    }
    REQUIRE(tree.size() == kCount);
    REQUIRE(tree.validate_invariants());

    // Delete odd numbers
    for (int i = 1; i < kCount; i += 2) {
        std::size_t erased = tree.erase(i);
        CHECK(erased == 1);
        CHECK_FALSE(tree.contains(i));
    }

    CHECK(tree.size() == kCount / 2);
    CHECK(tree.validate_invariants());

    // Re-insert recycled nodes from freelist
    for (int i = 1; i < kCount; i += 2) {
        tree.insert_or_assign(i, i * 10);
    }
    CHECK(tree.size() == kCount);
    CHECK(tree.validate_invariants());

    // Clear all
    tree.clear();
    CHECK(tree.empty());
    CHECK(tree.size() == 0);
    CHECK(tree.validate_invariants());
}

TEST_CASE("containers::BPlusSet: Ordered Set operations and bulk loading", "[containers][tree][bplus_tree]") {
    BPlusSet<std::string> words;
    words.insert("apple");
    words.insert("banana");
    words.insert("cherry");
    words.insert("date");

    CHECK(words.size() == 4);
    CHECK(words.contains("banana"));
    CHECK_FALSE(words.contains("fig"));

    std::vector<std::string> collected;
    for (const auto& w : words) {
        collected.push_back(w);
    }
    std::vector<std::string> expected = {"apple", "banana", "cherry", "date"};
    CHECK(collected == expected);

    words.erase("cherry");
    CHECK(words.size() == 3);
    CHECK_FALSE(words.contains("cherry"));
    CHECK(words.validate_invariants());

    // Set bulk loading
    std::vector<std::string> sorted_words = {"ant", "bee", "cat", "dog", "elk"};
    auto bulk_set = BPlusSet<std::string>::from_sorted(sorted_words.begin(), sorted_words.end());
    CHECK(bulk_set.size() == 5);
    CHECK(bulk_set.contains("cat"));
    CHECK(bulk_set.validate_invariants());
}

TEST_CASE("containers::BPlusTree: Smriti BumpPool Zero-Heap Arena Allocation", "[containers][tree][bplus_tree][smriti]") {
    using PoolType = smriti::pools::BumpPool<smriti::domains::SystemRAMDomain>;
    PoolType pool{65536}; // 64KB memory arena pool

    auto arena_map = make_smriti_bplus_map<int, int>(pool);
    for (int i = 0; i < 200; ++i) {
        arena_map.insert_or_assign(i, i * 5);
    }

    CHECK(arena_map.size() == 200);
    CHECK(arena_map.validate_invariants());
    CHECK(arena_map.contains(100));
    CHECK(arena_map.at(100) == 500);

    // Erase some items
    for (int i = 0; i < 50; ++i) {
        arena_map.erase(i);
    }
    CHECK(arena_map.size() == 150);
    CHECK(arena_map.validate_invariants());
}

// ============================================================================
// Appended: bplus_tree_next design (SIMD search, bottom-up bulk load,
// auto-tuned fanout, Traits concept, pravaha runner parity).
// ============================================================================

#include "containers/tree/bplus_tree_pravaha.hpp"
#include <cstdint>

TEST_CASE("bplus_tree: simd vs scalar search parity", "[bplus_tree][simd]") {
    // Numeric keys exercise the SIMD membership probe; string keys the scalar fallback.
    BPlusMap<int, int> mi;
    BPlusMap<std::uint64_t, int> mu;
    BPlusMap<std::string, int> ms;

    std::vector<int> ref_i;
    for (int i = 0; i < 2000; ++i) {
        const int k = i * 3 - 500; // includes negatives + gaps
        mi.insert_or_assign(k, k * 2);
        mu.insert_or_assign(static_cast<std::uint64_t>(i) * 7ull + 1ull, i);
        ms.insert_or_assign("key_" + std::to_string(i), i);
        ref_i.push_back(k);
    }

    // Membership + value parity against a brute-force reference for the int map.
    for (int probe = -600; probe < 6000; ++probe) {
        const bool expect = std::find(ref_i.begin(), ref_i.end(), probe) != ref_i.end();
        const auto it = mi.find(probe);
        CHECK((it != mi.end()) == expect);
        CHECK(mi.contains(probe) == expect);
        if (expect) {
            CHECK(it->second == probe * 2);
        }
    }

    // lower_bound / upper_bound stay Compare-based and correct regardless of the search path.
    {
        auto lb = mi.lower_bound(100);
        // First key >= 100: keys are 3*i-500; smallest such is 100 (i=200) if present else next.
        CHECK(lb != mi.end());
        CHECK(lb->first >= 100);
        if (lb != mi.begin()) {
            auto prev = lb; --prev;
            CHECK(prev->first < 100);
        }
        auto ub = mi.upper_bound(lb->first);
        CHECK(ub != lb);
        CHECK(ub->first > lb->first);
    }

    // SIMD (uint64) path membership parity.
    for (std::uint64_t i = 0; i < 2000; ++i) {
        const std::uint64_t present = i * 7ull + 1ull;
        CHECK(mu.contains(present));
        CHECK_FALSE(mu.contains(present + 3ull)); // 7-stride guarantees a gap
    }

    // Scalar (string) path membership parity.
    CHECK(ms.contains("key_0"));
    CHECK(ms.contains("key_1999"));
    CHECK_FALSE(ms.contains("key_2000"));
    CHECK_FALSE(ms.contains("nope"));

    CHECK(mi.validate_invariants());
    CHECK(mu.validate_invariants());
    CHECK(ms.validate_invariants());
}

TEST_CASE("bplus_tree: bottom-up from_sorted correctness", "[bplus_tree][bulkload]") {
    // Empty range → empty tree.
    {
        std::vector<std::pair<const int, int>> empty;
        auto t = BPlusMap<int, int>::from_sorted(empty.begin(), empty.end());
        CHECK(t.empty());
        CHECK(t.size() == 0);
        CHECK(t.validate_invariants());
        CHECK(t.begin() == t.end());
    }

    // Single leaf (few elements) → root is a leaf, depth 1.
    {
        std::vector<std::pair<const int, int>> few = {{1, 10}, {2, 20}, {3, 30}};
        auto t = BPlusMap<int, int>::from_sorted(few.begin(), few.end());
        CHECK(t.size() == 3);
        CHECK(t.depth() == 1);
        CHECK(t.validate_invariants());
        CHECK(t.at(2) == 20);
    }

    // Large sorted input → valid, balanced, exact contents, ordered iteration.
    {
        constexpr int N = 5000;
        std::vector<std::pair<const int, int>> src;
        src.reserve(N);
        for (int i = 0; i < N; ++i) src.emplace_back(i, i * 5);

        auto t = BPlusMap<int, int>::from_sorted(src.begin(), src.end());
        CHECK(t.size() == static_cast<std::size_t>(N));
        CHECK(t.validate_invariants());

        int expect = 0;
        for (const auto& kv : t) {
            CHECK(kv.first == expect);
            CHECK(kv.second == expect * 5);
            ++expect;
        }
        CHECK(expect == N);

        CHECK(t.contains(0));
        CHECK(t.contains(N - 1));
        CHECK(t.at(1234) == 1234 * 5);

        // Depth must be minimal-ish: far below a naive log2 blow-up.
        CHECK(t.depth() >= 1);
        CHECK(t.depth() <= 5);
    }
}

TEST_CASE("bplus_tree: fanout auto-tunes to node bytes", "[bplus_tree][traits]") {
    STATIC_REQUIRE(BPlusTreeTraits<DefaultBPlusTreeTraits<int, int>>);
    STATIC_REQUIRE(BPlusTreeTraits<DefaultBPlusTreeTraits<std::uint64_t, double>>);
    STATIC_REQUIRE(BPlusTreeTraits<TinyBPlusTraits>);

    // Derived node sizes stay within a bounded multiple of the byte budget (header slack allowed).
    using SmallTree = BPlusMap<int, int>;
    using WideTree  = BPlusMap<std::uint64_t, double>;

    STATIC_REQUIRE(sizeof(typename SmallTree::LeafType) <= 512);
    STATIC_REQUIRE(sizeof(typename SmallTree::InnerType) <= 512);
    STATIC_REQUIRE(sizeof(typename WideTree::LeafType) <= 512);
    STATIC_REQUIRE(sizeof(typename WideTree::InnerType) <= 512);

    // Fanout auto-derives (not the old hardcoded 16) and respects the >=3 floor.
    STATIC_REQUIRE(SmallTree::LeafCapacity >= 3);
    STATIC_REQUIRE(SmallTree::InnerCapacity >= 3);
}

TEST_CASE("bplus_tree: pravaha parallel parity", "[bplus_tree][pravaha]") {
    BPlusMap<int, int> tree;
    constexpr int N = 4000;
    long long serial_sum = 0;
    for (int i = 0; i < N; ++i) {
        tree.insert_or_assign(i, i);
        serial_sum += i;
    }

    // parallel_reduce parity with serial fold over [0, N).
    long long par_sum = pebble::containers::pravaha::parallel_reduce(
        tree, 0, N - 1, 0LL,
        [](long long a, long long b) { return a + b; },
        [](int, int v) { return static_cast<long long>(v); });
    CHECK(par_sum == serial_sum);

    // parallel_scan visits exactly the range (thread-safe accumulation via atomic).
    std::atomic<long long> scan_sum{0};
    std::atomic<int> scan_count{0};
    pebble::containers::pravaha::parallel_scan(
        tree, 0, N - 1,
        [&](int, int v) { scan_sum.fetch_add(v); scan_count.fetch_add(1); });
    CHECK(scan_count.load() == N);
    CHECK(scan_sum.load() == serial_sum);

    // parallel_find parity: present + absent keys.
    std::vector<int> queries;
    for (int i = 0; i < N; i += 2) queries.push_back(i);
    queries.push_back(N + 100); // absent
    auto found = pebble::containers::pravaha::parallel_find(
        tree, std::span<const int>(queries));
    REQUIRE(found.size() == queries.size());
    for (std::size_t i = 0; i + 1 < queries.size(); ++i) {
        REQUIRE(found[i].has_value());
        CHECK(*found[i] == queries[i]);
    }
    CHECK_FALSE(found.back().has_value());
}


