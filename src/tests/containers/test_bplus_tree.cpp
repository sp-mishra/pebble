// ============================================================================
// test_bplus_tree.cpp — Unit tests for pebble::containers::BPlusTree
// ============================================================================

#include <catch_amalgamated.hpp>
#include "containers/tree/bplus_tree.hpp"
#include "mem/smriti.hpp"

#include <numeric>
#include <random>
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

