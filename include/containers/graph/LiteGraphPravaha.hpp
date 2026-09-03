#ifndef LITEGRAPH_PRAVAHA_HPP
#define LITEGRAPH_PRAVAHA_HPP

// =============================================================================
// LiteGraphPravaha.hpp — Pravaha Multi-Core Parallel Graph Algorithms Add-on
//
// Modern C++23/C++26, Header-Only, Zero Virtual Dispatch, Zero Macros.
//
// This add-on provides multi-threaded task-parallel graph traversal, shortest
// paths, PageRank, and centrality algorithms powered by Pravaha's JThreadBackend
// and dynamic chunk scheduling without introducing circular dependencies into
// LiteGraph core.
// =============================================================================

#include "LiteGraphAlgorithms.hpp"
#include <pravaha/pravaha.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <thread>
#include <vector>

namespace litegraph::pravaha {
    // =========================================================================
    // 1. Parallel Level-Synchronous BFS
    // =========================================================================

    /**
     * @brief Parallel level-by-level BFS traversal across LiteGraph models.
     * @tparam GraphT LiteGraph conforming to LiteGraphModel.
     * @tparam Fn Visitor functor taking (NodeId, const node_type&). Must be thread-safe for concurrent invocations across worker threads.
     * @param g The graph to traverse.
     * @param start The origin node ID.
     * @param visit Visitor called for every reachable node concurrently.
     * @param runner Optional Pravaha runner. If default constructed, creates an ephemeral JThread pool.
     */
    template <LiteGraphModel GraphT, typename Fn>
        requires std::invocable<Fn, NodeId, const typename GraphT::node_type&>
    void parallel_bfs(
        const GraphT& g,
        const NodeId start,
        Fn&& visit,
        ::pravaha::Runner<::pravaha::JThreadBackend>& runner
    ) {
        const std::size_t node_cap = g.node_capacity();
        if (node_cap == 0 || !g.valid_node(start)) return;

        std::vector<std::atomic<bool>> visited(node_cap);
        for (std::size_t i = 0; i < node_cap; ++i) {
            visited[i].store(false, std::memory_order_relaxed);
        }

        std::vector<NodeId> current_frontier;
        current_frontier.push_back(start);
        visited[start.value].store(true, std::memory_order_release);

        const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());

        while (!current_frontier.empty()) {
            // 1. Parallel visitor invocation on the current frontier
            const std::size_t frontier_size = current_frontier.size();
            const std::size_t chunk_sz = std::max<std::size_t>(1, (frontier_size + hw_threads - 1) / hw_threads);
            const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(frontier_size, chunk_sz);

            for (const auto& ch : chunks) {
                auto task_fn = [&current_frontier, &g, &visit, begin = ch.begin, end = ch.end]() {
                    for (std::size_t i = begin; i < end; ++i) {
                        const NodeId u = current_frontier[i];
                        visit(u, g.node_data(u));
                    }
                };
                (void)runner.submit(::pravaha::task("litegraph_bfs_visit", std::move(task_fn)));
            }
            runner.backend_ref().drain();

            // 2. Parallel next-frontier discovery
            std::vector<std::vector<NodeId>> local_next_frontiers(chunks.size());

            for (std::size_t c_idx = 0; c_idx < chunks.size(); ++c_idx) {
                const auto& ch = chunks[c_idx];
                auto* local_frontier = &local_next_frontiers[c_idx];

                auto task_fn = [&current_frontier, &g, &visited, local_frontier, begin = ch.begin, end = ch.end]() {
                    for (std::size_t i = begin; i < end; ++i) {
                        const NodeId u = current_frontier[i];
                        for (NodeId v : g.neighbors(u)) {
                            bool expected = false;
                            if (visited[v.value].compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                                local_frontier->push_back(v);
                            }
                        }
                    }
                };
                (void)runner.submit(::pravaha::task("litegraph_bfs_expand", std::move(task_fn)));
            }
            runner.backend_ref().drain();

            // 3. Assemble next frontier
            current_frontier.clear();
            for (auto& local : local_next_frontiers) {
                current_frontier.insert(current_frontier.end(), local.begin(), local.end());
            }
        }
    }

    template <LiteGraphModel GraphT, typename Fn>
    void parallel_bfs(const GraphT& g, const NodeId start, Fn&& visit) {
        ::pravaha::Runner<::pravaha::JThreadBackend> runner;
        parallel_bfs(g, start, std::forward<Fn>(visit), runner);
    }

    // =========================================================================
    // =========================================================================
    // 2. Parallel CSR PageRank Algorithm
    // =========================================================================

    namespace policy {
        template <typename RunnerT>
        struct PravahaExec {
            RunnerT& runner;

            template <typename Idx, typename Fn>
            void for_each(Idx begin, Idx end, Fn&& fn) const {
                const std::size_t total = static_cast<std::size_t>(end - begin);
                if (total == 0) return;
                const unsigned int threads = std::max(1u, std::thread::hardware_concurrency());
                const std::size_t chunk_sz = std::max<std::size_t>(1, (total + threads - 1) / threads);
                const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(total, chunk_sz);

                for (const auto& ch : chunks) {
                    (void)runner.submit(::pravaha::task("pr_for_each", [&fn, b = begin + ch.begin, e = begin + ch.end]() {
                        for (auto i = b; i < e; ++i) fn(i);
                    }));
                }
                runner.backend_ref().drain();
            }

            template <typename Idx, typename TransformFn, typename ReduceFn, typename T>
            T transform_reduce(Idx begin, Idx end, T init, TransformFn&& trans, ReduceFn&& red) const {
                const std::size_t total = static_cast<std::size_t>(end - begin);
                if (total == 0) return init;
                const unsigned int threads = std::max(1u, std::thread::hardware_concurrency());
                const std::size_t chunk_sz = std::max<std::size_t>(1, (total + threads - 1) / threads);
                const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(total, chunk_sz);

                std::vector<T> partials(chunks.size(), init);
                for (std::size_t c = 0; c < chunks.size(); ++c) {
                    const auto& ch = chunks[c];
                    auto* out = &partials[c];
                    (void)runner.submit(::pravaha::task("pr_reduce", [&trans, &red, out, b = begin + ch.begin, e = begin + ch.end]() {
                        T acc = *out;
                        for (auto i = b; i < e; ++i) acc = red(acc, trans(i));
                        *out = acc;
                    }));
                }
                runner.backend_ref().drain();
                for (const auto& part : partials) init = red(init, part);
                return init;
            }
        };
    } // namespace policy

    template <typename EdgeT, DirectednessTag Directedness>
    CsrPageRankResult parallel_pagerank(
        const CsrGraph<EdgeT, Directedness>& g,
        const CsrPageRankOptions& options,
        ::pravaha::Runner<::pravaha::JThreadBackend>& runner
    ) {
        return pagerank_engine(g, options, policy::PravahaExec{runner}, litegraph::policy::ScalarVectorOps{});
    }

    template <typename EdgeT, DirectednessTag Directedness>
    CsrPageRankResult parallel_pagerank(
        const CsrGraph<EdgeT, Directedness>& g,
        const CsrPageRankOptions& options = {}
    ) {
        ::pravaha::Runner<::pravaha::JThreadBackend> runner;
        return parallel_pagerank(g, options, runner);
    }

    // =========================================================================
    // 3. Parallel Multi-Source Dijkstra Shortest Paths
    // =========================================================================

    /**
     * @brief Computes shortest paths from multiple sources in parallel across Pravaha workers.
     * @tparam GraphT LiteGraph conforming to LiteGraphModel.
     * @tparam WeightFn Weight extractor function.
     * @param g The graph.
     * @param sources Vector of source NodeIds.
     * @param weight_fn Weight projection.
     * @param runner Optional Pravaha runner.
     * @return Vector of pairs (distances, predecessors) for each source in order.
     */
    template <LiteGraphModel GraphT, std::invocable<const typename GraphT::edge_type&> WeightFn>
    std::vector<std::pair<std::vector<double>, std::vector<std::optional<NodeId>>>>
    parallel_multi_source_dijkstra(
        const GraphT& g,
        const std::vector<NodeId>& sources,
        WeightFn&& weight_fn,
        ::pravaha::Runner<::pravaha::JThreadBackend>& runner
    ) {
        const std::size_t num_sources = sources.size();
        std::vector<std::pair<std::vector<double>, std::vector<std::optional<NodeId>>>> results(num_sources);

        if (num_sources == 0) return results;

        const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t chunk_sz = std::max<std::size_t>(1, (num_sources + hw_threads - 1) / hw_threads);
        const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(num_sources, chunk_sz);

        for (const auto& ch : chunks) {
            auto task_fn = [&g, &sources, &results, &weight_fn, begin = ch.begin, end = ch.end]() {
                for (std::size_t s_idx = begin; s_idx < end; ++s_idx) {
                    results[s_idx] = litegraph::dijkstra(g, sources[s_idx], weight_fn);
                }
            };
            (void)runner.submit(::pravaha::task("litegraph_multi_dijkstra", std::move(task_fn)));
        }
        runner.backend_ref().drain();

        return results;
    }

    template <LiteGraphModel GraphT>
        requires std::is_arithmetic_v<typename GraphT::edge_type>
    auto parallel_multi_source_dijkstra(
        const GraphT& g,
        const std::vector<NodeId>& sources,
        ::pravaha::Runner<::pravaha::JThreadBackend>& runner
    ) {
        return parallel_multi_source_dijkstra(g, sources, [](const auto& e) {
            return static_cast<double>(e);
        }, runner);
    }

    template <LiteGraphModel GraphT>
        requires std::is_arithmetic_v<typename GraphT::edge_type>
    auto parallel_multi_source_dijkstra(
        const GraphT& g,
        const std::vector<NodeId>& sources
    ) {
        ::pravaha::Runner<::pravaha::JThreadBackend> runner;
        return parallel_multi_source_dijkstra(g, sources, runner);
    }

    // =========================================================================
    // 4. Parallel Betweenness Centrality (Parallel Brandes Algorithm)
    // =========================================================================

    /**
     * @brief Computes exact betweenness centrality using a parallelized Brandes algorithm.
     *        Distributes independent single-source shortest path DAG accumulations across Pravaha workers.
     */
    template <LiteGraphModel GraphT>
    std::vector<double> parallel_betweenness_centrality(
        const GraphT& g,
        ::pravaha::Runner<::pravaha::JThreadBackend>& runner
    ) {
        const std::size_t node_cap = g.node_capacity();
        const std::size_t node_count = g.node_count();
        if (node_cap == 0 || node_count <= 2) {
            return std::vector<double>(node_cap, 0.0);
        }

        std::vector<std::size_t> active_node_indices;
        active_node_indices.reserve(node_count);
        for (const auto& [nid_val, _] : g.nodes()) {
            active_node_indices.push_back(nid_val);
        }

        const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t chunk_sz = std::max<std::size_t>(1, (node_count + hw_threads - 1) / hw_threads);
        const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(node_count, chunk_sz);

        std::vector<std::vector<double>> thread_centralities(chunks.size(), std::vector<double>(node_cap, 0.0));

        for (std::size_t c_idx = 0; c_idx < chunks.size(); ++c_idx) {
            const auto& ch = chunks[c_idx];
            auto* local_cent = &thread_centralities[c_idx];

            auto task_fn = [&g, &active_node_indices, local_cent, node_cap, begin = ch.begin, end = ch.end]() {
                // Thread-local scratch buffers
                std::vector<std::vector<NodeId>> P(node_cap);
                std::vector<double> sigma(node_cap);
                std::vector<int> d(node_cap);
                std::vector<double> delta(node_cap);

                for (std::size_t idx = begin; idx < end; ++idx) {
                    const NodeId s{active_node_indices[idx]};
                    std::stack<NodeId> S;

                    for (auto& v : P) v.clear();
                    std::ranges::fill(sigma, 0.0);
                    std::ranges::fill(d, -1);
                    std::ranges::fill(delta, 0.0);

                    sigma[s.value] = 1.0;
                    d[s.value] = 0;

                    std::queue<NodeId> Q;
                    Q.push(s);

                    while (!Q.empty()) {
                        NodeId v = Q.front();
                        Q.pop();
                        S.push(v);

                        for (auto w : g.neighbors(v)) {
                            if (d[w.value] < 0) {
                                Q.push(w);
                                d[w.value] = d[v.value] + 1;
                            }
                            if (d[w.value] == d[v.value] + 1) {
                                sigma[w.value] += sigma[v.value];
                                P[w.value].push_back(v);
                            }
                        }
                    }

                    while (!S.empty()) {
                        const NodeId w = S.top();
                        S.pop();
                        for (const NodeId v : P[w.value]) {
                            if (sigma[w.value] != 0.0) {
                                delta[v.value] += (sigma[v.value] / sigma[w.value]) * (1.0 + delta[w.value]);
                            }
                        }
                        if (w.value != s.value) {
                            (*local_cent)[w.value] += delta[w.value];
                        }
                    }
                }
            };
            (void)runner.submit(::pravaha::task("litegraph_brandes_accum", std::move(task_fn)));
        }
        runner.backend_ref().drain();

        // Accumulate partial sums across threads
        std::vector<double> centrality(node_cap, 0.0);
        for (const auto& local : thread_centralities) {
            for (std::size_t i = 0; i < node_cap; ++i) {
                centrality[i] += local[i];
            }
        }

        // Normalize
        double normalizer = 0.0;
        if constexpr (std::is_same_v<typename GraphT::directed_tag, Directed>) {
            normalizer = static_cast<double>(node_count - 1) * static_cast<double>(node_count - 2);
        }
        else {
            normalizer = static_cast<double>(node_count - 1) * static_cast<double>(node_count - 2) / 2.0;
        }

        if (normalizer > 0.0) {
            for (std::size_t i = 0; i < node_cap; ++i) {
                centrality[i] /= normalizer;
            }
        }

        return centrality;
    }

    template <LiteGraphModel GraphT>
    std::vector<double> parallel_betweenness_centrality(const GraphT& g) {
        ::pravaha::Runner<::pravaha::JThreadBackend> runner;
        return parallel_betweenness_centrality(g, runner);
    }
} // namespace litegraph::pravaha

#endif // LITEGRAPH_PRAVAHA_HPP
