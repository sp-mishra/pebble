#pragma once
// =============================================================================
// bplus_tree_pravaha.hpp — Pravaha Parallel Algorithms Add-on for BPlusTree
// =============================================================================
// Modern C++23/C++26, Header-Only, Zero Virtual Dispatch, Zero Macros.
//
// Provides multi-threaded task-parallel range scans, parallel reduce/aggregations,
// and concurrent batch lookups powered by the project's Pravaha task runner
// (::pravaha::Runner<::pravaha::JThreadBackend>) — the same orchestration seam the
// Petika adapter uses. Work is partitioned over the leaf chain (or query batch),
// each partition submitted as a Pravaha task, and the runner backend drained before
// results are read. No reliance on thread-destructor timing.
//
// Thread-safety contract: user callables (`fn`, `map_op`, `reduce_op`) run
// concurrently across partitions and MUST be safe to invoke from multiple threads.
// `reduce_op` additionally runs in a final serial fold; it must be associative for
// deterministic results. The tree is only read, never mutated.
// =============================================================================

#include "bplus_tree.hpp"
#include <pravaha/pravaha.hpp>
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace pebble::containers::pravaha {
    using DefaultRunner = ::pravaha::Runner<::pravaha::JThreadBackend>;

    namespace detail {
        // Grain heuristic: at least leaf_grain_size leaves per task, otherwise one task per
        // hardware thread. Keeps task count bounded while honouring the caller's minimum grain.
        [[nodiscard]] inline std::size_t chunk_for(std::size_t work, std::size_t grain) noexcept {
            const std::size_t threads = std::max(1u, std::thread::hardware_concurrency());
            return std::max(grain, (work + threads - 1) / threads);
        }

        // RAII scope drain guard to guarantee task backend draining upon normal exit or exception unwind
        template <typename RunnerType>
        struct DrainGuard {
            RunnerType& runner;
            ~DrainGuard() {
                runner.backend_ref().drain();
            }
        };
    } // namespace detail

    // =========================================================================
    // 1. Parallel Range Scan
    // =========================================================================

    /**
     * @brief Parallel range scan across leaf nodes using the Pravaha runner.
     * Divides matching leaves into partitions processed concurrently. `fn` must be thread-safe.
     * Uses lightweight stride spans without full pointer snapshot array allocation.
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
        std::size_t leaf_grain_size = 4,
        DefaultRunner* runner = nullptr
    ) {
        if (tree.empty()) return;

        auto start_it = tree.lower_bound(min_key);
        if (start_it == tree.end()) return;

        using LeafType = typename BPlusTree<Key, Value, Compare, Traits, Allocator>::LeafType;
        struct PartitionSpan {
            const LeafType* start_leaf{nullptr};
            std::size_t max_leaves{0};
        };

        const std::size_t thread_count = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t est_total_leaves = (tree.size() + Traits::LeafCapacity - 1) / Traits::LeafCapacity;
        const std::size_t chunk_limit = std::max(leaf_grain_size, (est_total_leaves + thread_count * 4 - 1) / (thread_count * 4));

        std::vector<PartitionSpan> partitions;
        partitions.reserve(thread_count * 4);

        const LeafType* curr = start_it.node();
        Compare comp{};

        while (curr != nullptr) {
            if (curr->header.count > 0 && comp(max_key, curr->key_at(0))) break;

            const LeafType* p_start = curr;
            std::size_t step = 0;

            while (curr != nullptr && step < chunk_limit) {
                if (curr->header.count > 0 && comp(max_key, curr->key_at(0))) break;
                curr = curr->next;
                ++step;
            }
            partitions.push_back(PartitionSpan{p_start, step});
        }

        if (partitions.empty()) return;

        DefaultRunner local_runner;
        DefaultRunner& run = runner ? *runner : local_runner;
        detail::DrainGuard<DefaultRunner> drain_guard{run};

        for (const auto& part : partitions) {
            (void)run.submit(::pravaha::task("bplus_parallel_scan",
                [part, &min_key, &max_key, &comp, &fn]() {
                    const LeafType* leaf = part.start_leaf;
                    for (std::size_t i = 0; i < part.max_leaves && leaf != nullptr; ++i) {
                        const auto* keys = leaf->keys();
                        const auto* vals = leaf->values();
                        for (std::size_t j = 0; j < leaf->header.count; ++j) {
                            if (comp(keys[j], min_key)) continue;
                            if (comp(max_key, keys[j])) return;
                            fn(keys[j], vals[j]);
                        }
                        leaf = leaf->next;
                    }
                }));
        }
    }

    // =========================================================================
    // 2. Parallel Reduce / Aggregation
    // =========================================================================

    /**
     * @brief Parallel reduce/aggregation across a key range.
     * `map_op` runs concurrently; `reduce_op` runs concurrently per-partition and then in a
     * serial fold — it must be associative for a deterministic result.
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
        std::size_t leaf_grain_size = 4,
        DefaultRunner* runner = nullptr
    ) {
        if (tree.empty()) return init;

        auto start_it = tree.lower_bound(min_key);
        if (start_it == tree.end()) return init;

        using LeafType = typename BPlusTree<Key, Value, Compare, Traits, Allocator>::LeafType;
        struct PartitionSpan {
            const LeafType* start_leaf{nullptr};
            std::size_t max_leaves{0};
        };

        const std::size_t thread_count = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t est_total_leaves = (tree.size() + Traits::LeafCapacity - 1) / Traits::LeafCapacity;
        const std::size_t chunk_limit = std::max(leaf_grain_size, (est_total_leaves + thread_count * 4 - 1) / (thread_count * 4));

        std::vector<PartitionSpan> partitions;
        partitions.reserve(thread_count * 4);

        const LeafType* curr = start_it.node();
        Compare comp{};

        while (curr != nullptr) {
            if (curr->header.count > 0 && comp(max_key, curr->key_at(0))) break;

            const LeafType* p_start = curr;
            std::size_t step = 0;

            while (curr != nullptr && step < chunk_limit) {
                if (curr->header.count > 0 && comp(max_key, curr->key_at(0))) break;
                curr = curr->next;
                ++step;
            }
            partitions.push_back(PartitionSpan{p_start, step});
        }

        if (partitions.empty()) return init;

        const std::size_t num_parts = partitions.size();
        std::vector<InitT> partial_results(num_parts, init);

        DefaultRunner local_runner;
        DefaultRunner& run = runner ? *runner : local_runner;
        {
            detail::DrainGuard<DefaultRunner> drain_guard{run};

            for (std::size_t chunk_idx = 0; chunk_idx < num_parts; ++chunk_idx) {
                const auto& part = partitions[chunk_idx];
                (void)run.submit(::pravaha::task("bplus_parallel_reduce",
                    [part, chunk_idx, &partial_results, &min_key, &max_key, &comp, &reduce_op, &map_op]() {
                        InitT local_accum = partial_results[chunk_idx];
                        const LeafType* leaf = part.start_leaf;
                        for (std::size_t i = 0; i < part.max_leaves && leaf != nullptr; ++i) {
                            const auto* keys = leaf->keys();
                            const auto* vals = leaf->values();
                            for (std::size_t j = 0; j < leaf->header.count; ++j) {
                                if (comp(keys[j], min_key)) continue;
                                if (comp(max_key, keys[j])) {
                                    partial_results[chunk_idx] = local_accum;
                                    return;
                                }
                                local_accum = reduce_op(local_accum, map_op(keys[j], vals[j]));
                            }
                            leaf = leaf->next;
                        }
                        partial_results[chunk_idx] = local_accum;
                    }));
            }
        }

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
     * @brief Concurrently search for a batch of keys. Results align with query_keys by index.
     * The tree is read-only and BPlusTree::find is const, so concurrent lookups are safe.
     */
    template <
        typename Key, typename Value, typename Compare, typename Traits, typename Allocator,
        typename QueryKey
    >
    [[nodiscard]] std::vector<std::optional<Value>> parallel_find(
        const BPlusTree<Key, Value, Compare, Traits, Allocator>& tree,
        std::span<const QueryKey> query_keys,
        DefaultRunner* runner = nullptr
    ) {
        const std::size_t count = query_keys.size();
        std::vector<std::optional<Value>> results(count, std::nullopt);
        if (count == 0) return results;

        const std::size_t chunk_size = std::max<std::size_t>(16, detail::chunk_for(count, 1));

        DefaultRunner local_runner;
        DefaultRunner& run = runner ? *runner : local_runner;
        detail::DrainGuard<DefaultRunner> drain_guard{run};

        for (std::size_t i = 0; i < count; i += chunk_size) {
            const std::size_t end_idx = std::min(count, i + chunk_size);
            (void)run.submit(::pravaha::task("bplus_parallel_find",
                                             [&tree, &query_keys, &results, i, end_idx]() {
                                                 for (std::size_t j = i; j < end_idx; ++j) {
                                                      auto it = tree.find(query_keys[j]);
                                                      if (it != tree.end()) {
                                                          results[j] = it->second;
                                                      }
                                                 }
                                             }));
        }

        return results;
    }

    // Scale aliases
    template <class... Args>
    void parallel_scan_scale(Args&&... args) {
        parallel_scan(std::forward<Args>(args)...);
    }

    template <class... Args>
    auto parallel_reduce_scale(Args&&... args) {
        return parallel_reduce(std::forward<Args>(args)...);
    }
} // namespace pebble::containers::pravaha
