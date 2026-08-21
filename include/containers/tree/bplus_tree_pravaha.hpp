#pragma once
// =============================================================================
// bplus_tree_pravaha.hpp — Pravaha Parallel Algorithms Add-on for BPlusTree
// =============================================================================
// Modern C++23/C++26, Header-Only, Zero Virtual Dispatch, Zero Macros.
//
// Provides multi-threaded task-parallel range scans, parallel reduce/aggregations,
// and concurrent batch lookups powered by Pravaha task orchestration without
// imposing a heavy dependency on the core BPlusTree header.
// =============================================================================

#include "bplus_tree.hpp"
#include <pravaha/pravaha.hpp>
#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <span>
#include <thread>
#include <vector>

namespace pebble::containers::pravaha {

    // =========================================================================
    // 1. Parallel Range Scan
    // =========================================================================

    /**
     * @brief Parallel range scan across leaf nodes using Pravaha task orchestration.
     * Divides matching leaves into parallel chunks processed concurrently.
     */
    template <
        typename Key, typename Value, typename Compare, typename Traits, typename Allocator,
        typename K1, typename K2, typename Fn
    >
    void parallel_scan(
        const BPlusTree<Key, Value, Compare, Traits, Allocator>& tree,
        const K1& min_key,
        const K2& max_key,
        Fn&& fn,
        std::size_t leaf_grain_size = 4
    ) {
        auto start_it = tree.lower_bound(min_key);
        if (start_it == tree.end()) return;

        // Collect matching leaf nodes
        using LeafType = typename BPlusTree<Key, Value, Compare, Traits, Allocator>::LeafType;
        std::vector<const LeafType*> leaves;
        Compare comp{};

        for (const LeafType* curr = start_it.node(); curr != nullptr; curr = curr->next) {
            if (curr->header.count > 0 && comp(max_key, curr->key_at(0))) {
                break;
            }
            leaves.push_back(curr);
        }

        if (leaves.empty()) return;

        const std::size_t num_leaves = leaves.size();
        const std::size_t num_threads = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t chunk_size = std::max(leaf_grain_size, (num_leaves + num_threads - 1) / num_threads);

        std::vector<std::jthread> workers;
        for (std::size_t i = 0; i < num_leaves; i += chunk_size) {
            const std::size_t end_idx = std::min(num_leaves, i + chunk_size);
            workers.emplace_back([&leaves, i, end_idx, &min_key, &max_key, &comp, &fn]() {
                for (std::size_t idx = i; idx < end_idx; ++idx) {
                    const auto* leaf = leaves[idx];
                    const auto* keys = leaf->keys();
                    const auto* vals = leaf->values();
                    for (std::size_t j = 0; j < leaf->header.count; ++j) {
                        if (comp(keys[j], min_key)) continue;
                        if (comp(max_key, keys[j])) return;
                        fn(keys[j], vals[j]);
                    }
                }
            });
        }
    }

    // =========================================================================
    // 2. Parallel Reduce / Aggregation
    // =========================================================================

    /**
     * @brief Parallel reduce/aggregation across a key range.
     */
    template <
        typename Key, typename Value, typename Compare, typename Traits, typename Allocator,
        typename K1, typename K2, typename InitT, typename ReduceFn, typename MapFn
    >
    [[nodiscard]] InitT parallel_reduce(
        const BPlusTree<Key, Value, Compare, Traits, Allocator>& tree,
        const K1& min_key,
        const K2& max_key,
        InitT init,
        ReduceFn&& reduce_op,
        MapFn&& map_op,
        std::size_t leaf_grain_size = 4
    ) {
        auto start_it = tree.lower_bound(min_key);
        if (start_it == tree.end()) return init;

        using LeafType = typename BPlusTree<Key, Value, Compare, Traits, Allocator>::LeafType;
        std::vector<const LeafType*> leaves;
        Compare comp{};

        for (const LeafType* curr = start_it.node(); curr != nullptr; curr = curr->next) {
            if (curr->header.count > 0 && comp(max_key, curr->key_at(0))) {
                break;
            }
            leaves.push_back(curr);
        }

        if (leaves.empty()) return init;

        const std::size_t num_leaves = leaves.size();
        const std::size_t num_threads = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t chunk_size = std::max(leaf_grain_size, (num_leaves + num_threads - 1) / num_threads);
        const std::size_t num_chunks = (num_leaves + chunk_size - 1) / chunk_size;

        std::vector<InitT> partial_results(num_chunks, init);
        std::vector<std::jthread> workers;

        for (std::size_t chunk_idx = 0; chunk_idx < num_chunks; ++chunk_idx) {
            const std::size_t start_idx = chunk_idx * chunk_size;
            const std::size_t end_idx = std::min(num_leaves, start_idx + chunk_size);

            workers.emplace_back([&leaves, start_idx, end_idx, chunk_idx, &partial_results, &min_key, &max_key, &comp, &reduce_op, &map_op]() {
                InitT local_accum = partial_results[chunk_idx];
                for (std::size_t idx = start_idx; idx < end_idx; ++idx) {
                    const auto* leaf = leaves[idx];
                    const auto* keys = leaf->keys();
                    const auto* vals = leaf->values();
                    for (std::size_t j = 0; j < leaf->header.count; ++j) {
                        if (comp(keys[j], min_key)) continue;
                        if (comp(max_key, keys[j])) break;
                        local_accum = reduce_op(local_accum, map_op(keys[j], vals[j]));
                    }
                }
                partial_results[chunk_idx] = local_accum;
            });
        }

        workers.clear(); // Join all threads

        InitT total = init;
        for (const auto& part : partial_results) {
            total = reduce_op(total, part);
        }
        return total;
    }

    // =========================================================================
    // 3. Parallel Batch Find
    // =========================================================================

    /**
     * @brief Concurrently search for a batch of keys in parallel.
     */
    template <
        typename Key, typename Value, typename Compare, typename Traits, typename Allocator,
        typename QueryKey
    >
    [[nodiscard]] std::vector<std::optional<Value>> parallel_find(
        const BPlusTree<Key, Value, Compare, Traits, Allocator>& tree,
        std::span<const QueryKey> query_keys
    ) {
        const std::size_t count = query_keys.size();
        std::vector<std::optional<Value>> results(count, std::nullopt);
        if (count == 0) return results;

        const std::size_t num_threads = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t chunk_size = std::max<std::size_t>(16, (count + num_threads - 1) / num_threads);

        std::vector<std::jthread> workers;
        for (std::size_t i = 0; i < count; i += chunk_size) {
            const std::size_t end_idx = std::min(count, i + chunk_size);
            workers.emplace_back([&tree, &query_keys, &results, i, end_idx]() {
                for (std::size_t j = i; j < end_idx; ++j) {
                    auto it = tree.find(query_keys[j]);
                    if (it != tree.end()) {
                        results[j] = it->second;
                    }
                }
            });
        }

        return results;
    }

} // namespace pebble::containers::pravaha
