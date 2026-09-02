// ============================================================================
// test_bplus_tree_pravaha.cpp — Unit tests for BPlusTree Pravaha parallel add-on
// ============================================================================

#include <catch_amalgamated.hpp>
#include "containers/tree/bplus_tree.hpp"
#include "containers/tree/bplus_tree_pravaha.hpp"

#include <numeric>
#include <vector>
#include <mutex>

using namespace pebble::containers;
using namespace pebble::containers::pravaha;

TEST_CASE (
"BPlusTree Pravaha Add-on: Parallel Range Scan"
,
"[bplus_tree][pravaha][scan]"
)
 {
    BPlusMap<int, int> tree;
    constexpr int kCount = 1000;
    for (int i = 0; i < kCount; ++i) {
        tree.insert_or_assign(i, i * 2);
    }

    std::atomic<std::size_t> count{0};
    std::atomic<long long> sum{0};

    parallel_scan(tree, 100, 500, [&](int k, int v) {
        count.fetch_add(1, std::memory_order_relaxed);
        sum.fetch_add(v, std::memory_order_relaxed);
    }, 2);

    CHECK(count.load() == 401); // [100, 500] inclusive

    long long expected_sum = 0;
    for (int i = 100; i <= 500; ++i) {
        expected_sum += (i * 2);
    }
    CHECK(sum.load() == expected_sum);
}

TEST_CASE (
"BPlusTree Pravaha Add-on: Parallel Reduce"
,
"[bplus_tree][pravaha][reduce]"
)
 {
    BPlusMap<int, int> tree;
    constexpr int kCount = 2000;
    for (int i = 1; i <= kCount; ++i) {
        tree.insert_or_assign(i, i);
    }

    long long total_sum = parallel_reduce(
        tree,
        1,
        1000,
        0LL,
        [](long long a, long long b) { return a + b; },
        [](int, int v) { return static_cast<long long>(v); },
        2
    );

    long long expected = (1000LL * 1001LL) / 2LL;
    CHECK(total_sum == expected);
}

TEST_CASE (
"BPlusTree Pravaha Add-on: Parallel Batch Find"
,
"[bplus_tree][pravaha][batch_find]"
)
 {
    BPlusMap<int, std::string> tree;
    for (int i = 0; i < 500; ++i) {
        tree.insert_or_assign(i, "val_" + std::to_string(i));
    }

    std::vector<int> queries = {10, 50, 999, 200, 499, -5};
    auto results = parallel_find(tree, std::span<const int>(queries));

    REQUIRE(results.size() == 6);
    CHECK(results[0] == "val_10");
    CHECK(results[1] == "val_50");
    CHECK(results[2] == std::nullopt);
    CHECK(results[3] == "val_200");
    CHECK(results[4] == "val_499");
    CHECK(results[5] == std::nullopt);
}
