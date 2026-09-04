#pragma once


#include "LiteGraph.hpp"
#include <queue>
#include <stack>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>
#include <set>
#include <ranges>
#include <execution>
#include <expected>
#include <atomic>
#include <thread>
#include <span>
#include <deque>
#include <cstdint>

namespace litegraph {
    // Additional error types for algorithms
    enum class AlgorithmError {
        CycleDetected,
        NotBipartite,
        IncompatibleGraphs,
        NoPath
    };

    // Node reordering strategy applied at freeze time. The compact-index space is
    // permuted, but original<->compact id maps stay consistent so results remain
    // invariant under relabeling.
    enum class Reorder {
        Original, // active_node_ids() order (default; zero cost).
        DegreeDesc, // highest out-degree first — improves locality for gather kernels.
        Bfs // BFS discovery order from the lowest compact node — cache-friendly.
    };

    // Opt-in freeze configuration. Defaults reproduce the historical behaviour
    // (outgoing-only, no reordering) so no caller pays for what it does not use.
    struct CsrFreezeOptions {
        bool build_incoming = false;
        Reorder reorder = Reorder::Original;
    };

    // ----------- Frozen CSR Graph -----------
    template <typename EdgeT, DirectednessTag Directedness = Directed>
    class CsrGraph {
    public:
        using edge_type = EdgeT;
        using directed_tag = Directedness;
        using weight_type = double;

        [[nodiscard]] std::size_t node_count() const noexcept { return compact_to_original_.size(); }
        [[nodiscard]] std::size_t edge_count() const noexcept { return targets_.size(); }

        [[nodiscard]] std::optional<std::size_t> compact_index(const NodeId original) const noexcept {
            if (original.value >= original_to_compact_.size()) return std::nullopt;
            return original_to_compact_[original.value];
        }

        [[nodiscard]] NodeId original_node_id(const std::size_t compact_node_index) const {
            if (compact_node_index >= compact_to_original_.size()) {
                throw std::out_of_range("CSR compact node index out of range.");
            }
            return compact_to_original_[compact_node_index];
        }

        [[nodiscard]] const std::vector<std::optional<std::size_t>>& original_to_compact() const noexcept {
            return original_to_compact_;
        }

        [[nodiscard]] const std::vector<NodeId>& compact_to_original() const noexcept {
            return compact_to_original_;
        }

        [[nodiscard]] std::span<const NodeId> out_neighbors(const std::size_t compact_node_index) const {
            if (compact_node_index >= node_count()) {
                throw std::out_of_range("CSR compact node index out of range.");
            }
            const std::size_t begin = offsets_[compact_node_index];
            const std::size_t end = offsets_[compact_node_index + 1];
            return std::span(targets_).subspan(begin, end - begin);
        }

        [[nodiscard]] std::span<const EdgeId> out_edges(const std::size_t compact_node_index) const {
            if (compact_node_index >= node_count()) {
                throw std::out_of_range("CSR compact node index out of range.");
            }
            const std::size_t begin = offsets_[compact_node_index];
            const std::size_t end = offsets_[compact_node_index + 1];
            return std::span(edge_ids_).subspan(begin, end - begin);
        }

        // Edge payloads are only materialised for non-arithmetic EdgeT; arithmetic
        // payloads live in edge_weights() (as double) to avoid duplicate storage.
        template <typename T = EdgeT>
        [[nodiscard]] std::enable_if_t<!std::is_arithmetic_v<T>, std::span<const EdgeT>>
        out_edge_data(const std::size_t compact_node_index) const {
            if (compact_node_index >= node_count()) {
                throw std::out_of_range("CSR compact node index out of range.");
            }
            const std::size_t begin = offsets_[compact_node_index];
            const std::size_t end = offsets_[compact_node_index + 1];
            return std::span<const EdgeT>(edge_data_).subspan(begin, end - begin);
        }

        [[nodiscard]] bool has_edge_weights() const noexcept { return edge_weights_enabled_; }

        template <typename T = EdgeT>
        [[nodiscard]] std::enable_if_t<std::is_arithmetic_v<T>, std::span<const double>> edge_weights() const {
            return edge_weights_;
        }

        [[nodiscard]] const std::vector<std::size_t>& offsets() const noexcept { return offsets_; }
        [[nodiscard]] const std::vector<NodeId>& targets() const noexcept { return targets_; }

        // ---- Incoming (reverse) CSR — present only when built via CsrFreezeOptions ----
        [[nodiscard]] bool has_incoming() const noexcept { return incoming_enabled_; }

        [[nodiscard]] const std::vector<std::size_t>& in_offsets() const noexcept { return in_offsets_; }

        // Compact source node indices, one per incoming arc.
        [[nodiscard]] const std::vector<NodeId>& in_sources() const noexcept { return in_sources_; }

        // For each incoming arc, the position of the mirrored outgoing arc — lets
        // gather kernels read edge_weights()/edge_ids() without duplicating them.
        [[nodiscard]] const std::vector<std::size_t>& in_edge_pos() const noexcept { return in_edge_pos_; }

        [[nodiscard]] std::size_t in_degree_csr(const std::size_t compact_node_index) const {
            if (!incoming_enabled_ || compact_node_index >= node_count()) return 0;
            return in_offsets_[compact_node_index + 1] - in_offsets_[compact_node_index];
        }

        [[nodiscard]] std::span<const NodeId> in_neighbors(const std::size_t compact_node_index) const {
            if (!incoming_enabled_ || compact_node_index >= node_count()) return {};
            const std::size_t begin = in_offsets_[compact_node_index];
            const std::size_t end = in_offsets_[compact_node_index + 1];
            return std::span(in_sources_).subspan(begin, end - begin);
        }

        // Structural self-check on the frozen arrays.
        [[nodiscard]] std::expected<void, GraphError> validate() const {
            const std::size_t n = node_count();
            if (offsets_.size() != n + 1) return std::unexpected(GraphError::OffsetSizeMismatch);
            for (std::size_t i = 0; i < n; ++i) {
                if (offsets_[i] > offsets_[i + 1]) return std::unexpected(GraphError::OffsetsNotMonotonic);
            }
            if (offsets_.back() != targets_.size()) return std::unexpected(GraphError::OffsetSizeMismatch);
            if (edge_ids_.size() != targets_.size()) return std::unexpected(GraphError::ParallelArrayLengthMismatch);
            if (edge_weights_enabled_ && edge_weights_.size() != targets_.size()) {
                return std::unexpected(GraphError::ParallelArrayLengthMismatch);
            }
            for (const NodeId t : targets_) {
                if (t.value >= n) return std::unexpected(GraphError::TargetOutOfRange);
            }
            if (incoming_enabled_) {
                if (in_offsets_.size() != n + 1) return std::unexpected(GraphError::IncomingInconsistent);
                if (in_offsets_.back() != in_sources_.size()) return std::unexpected(GraphError::IncomingInconsistent);
                if (in_sources_.size() != targets_.size() || in_edge_pos_.size() != targets_.size()) {
                    return std::unexpected(GraphError::IncomingInconsistent);
                }
                for (std::size_t i = 0; i < n; ++i) {
                    if (in_offsets_[i] > in_offsets_[i + 1]) return std::unexpected(GraphError::IncomingInconsistent);
                }
                for (const NodeId s : in_sources_) {
                    if (s.value >= n) return std::unexpected(GraphError::IncomingInconsistent);
                }
                for (const std::size_t p : in_edge_pos_) {
                    if (p >= targets_.size()) return std::unexpected(GraphError::IncomingInconsistent);
                }
            }
            return {};
        }

    private:
        std::vector<std::size_t> offsets_;
        std::vector<NodeId> targets_; // compact target node indices encoded as NodeId{compact_index}
        std::vector<EdgeId> edge_ids_;
        std::vector<EdgeT> edge_data_; // materialised only for non-arithmetic EdgeT
        std::vector<double> edge_weights_;
        bool edge_weights_enabled_ = false;
        std::vector<std::optional<std::size_t>> original_to_compact_;
        std::vector<NodeId> compact_to_original_;

        // Incoming CSR (opt-in). in_sources_/in_edge_pos_ are parallel to each other;
        // in_edge_pos_[k] indexes the outgoing arrays for the mirrored arc.
        bool incoming_enabled_ = false;
        std::vector<std::size_t> in_offsets_;
        std::vector<NodeId> in_sources_;
        std::vector<std::size_t> in_edge_pos_;

        template <Hashable NodeT, Hashable E, DirectednessTag D>
        friend CsrGraph<E, D> freeze_to_csr(const Graph<NodeT, E, D>& g, const CsrFreezeOptions&);
    };

    template <Hashable NodeT, Hashable EdgeT, DirectednessTag Directedness>
    CsrGraph<EdgeT, Directedness> freeze_to_csr(const Graph<NodeT, EdgeT, Directedness>& g,
                                                const CsrFreezeOptions& options = {}) {
        CsrGraph<EdgeT, Directedness> csr;

        // Step 1: collect active nodes in native order.
        std::vector<NodeId> active;
        for (NodeId nid : g.active_node_ids()) active.push_back(nid);
        const std::size_t compact_nodes = active.size();

        // Step 2: choose the compact ordering (permutation of `active`).
        std::vector<NodeId> ordered = active;
        if (options.reorder == Reorder::DegreeDesc && compact_nodes > 0) {
            std::vector<std::size_t> deg(compact_nodes, 0);
            for (std::size_t i = 0; i < compact_nodes; ++i) {
                std::size_t d = 0;
                g.for_each_out_edge(active[i], [&](EdgeId, NodeId, NodeId, const auto&) { ++d; });
                deg[i] = d;
            }
            std::vector<std::size_t> idx(compact_nodes);
            for (std::size_t i = 0; i < compact_nodes; ++i) idx[i] = i;
            std::stable_sort(idx.begin(), idx.end(),
                             [&](std::size_t a, std::size_t b) { return deg[a] > deg[b]; });
            for (std::size_t i = 0; i < compact_nodes; ++i) ordered[i] = active[idx[i]];
        }
        else if (options.reorder == Reorder::Bfs && compact_nodes > 0) {
            // Discovery order from each unvisited seed (lowest native id first).
            std::vector<std::uint8_t> seen(g.node_capacity(), 0);
            ordered.clear();
            std::queue<NodeId> q;
            for (NodeId seed : active) {
                if (seen[seed.value]) continue;
                seen[seed.value] = 1;
                q.push(seed);
                while (!q.empty()) {
                    const NodeId u = q.front();
                    q.pop();
                    ordered.push_back(u);
                    g.for_each_out_edge(u, [&](EdgeId, NodeId, NodeId target, const auto&) {
                        if (!seen[target.value]) {
                            seen[target.value] = 1;
                            q.push(target);
                        }
                    });
                }
            }
        }

        // Step 3: assign compact indices from the chosen order.
        csr.original_to_compact_.assign(g.node_capacity(), std::nullopt);
        csr.compact_to_original_.reserve(compact_nodes);
        for (NodeId nid : ordered) {
            csr.original_to_compact_[nid.value] = csr.compact_to_original_.size();
            csr.compact_to_original_.push_back(nid);
        }

        csr.offsets_.assign(compact_nodes + 1, 0);

        // First pass: count active outgoing adjacency entries per compact node.
        for (std::size_t c = 0; c < compact_nodes; ++c) {
            const NodeId original = csr.compact_to_original_[c];
            std::size_t count = 0;
            g.for_each_out_edge(original, [&](EdgeId, NodeId, NodeId target, const auto&) {
                if (csr.original_to_compact_[target.value].has_value()) ++count;
            });
            csr.offsets_[c + 1] = csr.offsets_[c] + count;
        }

        const std::size_t arc_count = csr.offsets_.back();
        csr.targets_.resize(arc_count);
        csr.edge_ids_.resize(arc_count);
        if constexpr (std::is_arithmetic_v<EdgeT>) {
            csr.edge_weights_enabled_ = true;
            csr.edge_weights_.resize(arc_count);
        }
        else {
            csr.edge_data_.resize(arc_count);
        }

        std::vector<std::size_t> cursor = csr.offsets_;

        // Second pass: fill compact adjacency arrays (zero heap alloc per node).
        for (std::size_t c = 0; c < compact_nodes; ++c) {
            const NodeId original = csr.compact_to_original_[c];
            g.for_each_out_edge(original, [&](EdgeId eid, NodeId, NodeId target, const auto& data) {
                const auto compact_target = csr.original_to_compact_[target.value];
                if (!compact_target.has_value()) return;

                const std::size_t pos = cursor[c]++;
                csr.targets_[pos] = NodeId{*compact_target};
                csr.edge_ids_[pos] = eid;
                if constexpr (std::is_arithmetic_v<EdgeT>) {
                    csr.edge_weights_[pos] = static_cast<double>(data);
                }
                else {
                    csr.edge_data_[pos] = data;
                }
            });
        }

        // Optional incoming CSR: reverse of the outgoing arcs (transpose), storing
        // indices back into the outgoing arrays rather than duplicating weights.
        if (options.build_incoming) {
            csr.incoming_enabled_ = true;
            csr.in_offsets_.assign(compact_nodes + 1, 0);
            for (const NodeId t : csr.targets_) {
                ++csr.in_offsets_[t.value + 1];
            }
            for (std::size_t c = 0; c < compact_nodes; ++c) {
                csr.in_offsets_[c + 1] += csr.in_offsets_[c];
            }
            csr.in_sources_.resize(arc_count);
            csr.in_edge_pos_.resize(arc_count);
            std::vector<std::size_t> in_cursor = csr.in_offsets_;
            for (std::size_t u = 0; u < compact_nodes; ++u) {
                for (std::size_t i = csr.offsets_[u]; i < csr.offsets_[u + 1]; ++i) {
                    const std::size_t v = csr.targets_[i].value;
                    const std::size_t pos = in_cursor[v]++;
                    csr.in_sources_[pos] = NodeId{u};
                    csr.in_edge_pos_[pos] = i;
                }
            }
        }

        return csr;
    }

    // Move-only builder wrapping freeze_to_csr — the graph compiler front-end.
    // Chain option setters, then call build() (or freeze()) to produce the CsrGraph.
    template <Hashable NodeT, Hashable EdgeT, DirectednessTag Directedness>
    class CsrCompiler {
    public:
        explicit CsrCompiler(const Graph<NodeT, EdgeT, Directedness>& g) noexcept : graph_(&g) {}

        CsrCompiler(const CsrCompiler&) = delete;
        CsrCompiler& operator=(const CsrCompiler&) = delete;
        CsrCompiler(CsrCompiler&&) noexcept = default;
        CsrCompiler& operator=(CsrCompiler&&) noexcept = default;

        CsrCompiler& with_incoming(const bool on = true) noexcept {
            options_.build_incoming = on;
            return *this;
        }

        CsrCompiler& with_reorder(const Reorder r) noexcept {
            options_.reorder = r;
            return *this;
        }

        CsrCompiler& with_options(const CsrFreezeOptions& opts) noexcept {
            options_ = opts;
            return *this;
        }

        [[nodiscard]] CsrGraph<EdgeT, Directedness> build() const {
            return freeze_to_csr(*graph_, options_);
        }

        [[nodiscard]] CsrGraph<EdgeT, Directedness> freeze() const { return build(); }

    private:
        const Graph<NodeT, EdgeT, Directedness>* graph_;
        CsrFreezeOptions options_{};
    };

    template <Hashable NodeT, Hashable EdgeT, DirectednessTag Directedness>
    [[nodiscard]] CsrCompiler<NodeT, EdgeT, Directedness>
    compile_graph(const Graph<NodeT, EdgeT, Directedness>& g) noexcept {
        return CsrCompiler<NodeT, EdgeT, Directedness>(g);
    }

    struct CsrBfsResult {
        std::vector<std::size_t> distances;
        std::vector<std::optional<std::size_t>> predecessors;
    };

    struct CsrPageRankOptions {
        double damping_factor = 0.85;
        std::size_t max_iterations = 100;
        double tolerance = 1e-9;
    };

    struct CsrPageRankResult {
        std::vector<double> ranks;
        std::size_t iterations = 0;
        bool converged = false;
    };

    template <typename EdgeT, DirectednessTag Directedness>
    CsrBfsResult bfs(const CsrGraph<EdgeT, Directedness>& g, const std::size_t start_compact_index) {
        if (start_compact_index >= g.node_count()) {
            throw std::out_of_range("CSR BFS start index out of range.");
        }

        constexpr std::size_t INF = std::numeric_limits<std::size_t>::max();
        CsrBfsResult result{
            .distances = std::vector<std::size_t>(g.node_count(), INF),
            .predecessors = std::vector<std::optional<std::size_t>>(g.node_count(), std::nullopt)
        };

        std::vector<std::uint8_t> visited(g.node_count(), static_cast<std::uint8_t>(0));
        std::queue<std::size_t> q;

        visited[start_compact_index] = static_cast<std::uint8_t>(1);
        result.distances[start_compact_index] = 0;
        q.push(start_compact_index);

        const auto& offsets = g.offsets();
        const auto& targets = g.targets();

        while (!q.empty()) {
            const std::size_t u = q.front();
            q.pop();

            for (std::size_t i = offsets[u]; i < offsets[u + 1]; ++i) {
                const std::size_t v = targets[i].value;
                if (visited[v]) continue;

                visited[v] = static_cast<std::uint8_t>(1);
                result.distances[v] = result.distances[u] + 1;
                result.predecessors[v] = u;
                q.push(v);
            }
        }

        return result;
    }

    template <typename EdgeT, DirectednessTag Directedness>
    CsrBfsResult bfs(const CsrGraph<EdgeT, Directedness>& g, const NodeId start_original_node) {
        const auto compact_index = g.compact_index(start_original_node);
        if (!compact_index) {
            throw std::out_of_range("CSR BFS start node is not active in the frozen graph.");
        }
        return bfs(g, *compact_index);
    }

    namespace policy {
        struct SerialExec {
            template <typename Idx, typename Fn>
            static void for_each(Idx begin, Idx end, Fn&& fn) {
                for (Idx i = begin; i < end; ++i) fn(i);
            }

            template <typename Idx, typename TransformFn, typename ReduceFn, typename T>
            static T transform_reduce(Idx begin, Idx end, T init, TransformFn&& trans, ReduceFn&& red) {
                for (Idx i = begin; i < end; ++i) init = red(init, trans(i));
                return init;
            }
        };

        struct ScalarVectorOps {
            static void fill(std::span<double> v, double val) noexcept {
                std::ranges::fill(v, val);
            }

            static void add_scaled(std::span<double> dst, std::span<const double> src, double scale) noexcept {
                for (std::size_t i = 0; i < dst.size(); ++i) dst[i] += src[i] * scale;
            }

            [[nodiscard]] static double l1_delta(std::span<const double> a, std::span<const double> b) noexcept {
                double sum = 0.0;
                for (std::size_t i = 0; i < a.size(); ++i) sum += std::abs(a[i] - b[i]);
                return sum;
            }
        };
    } // namespace policy

    // Concept for coarse-grained algorithm phase observers
    template <typename O>
    concept GraphObserver = requires(O o, std::string_view phase, std::size_t count) {
        { o.on_phase_start(phase) };
        { o.on_phase_end(phase) };
        { o.on_iteration(count) };
    };

    // Zero-overhead default observer (fully elided by compiler)
    struct NullObserver {
        static constexpr void on_phase_start(std::string_view) noexcept {}
        static constexpr void on_phase_end(std::string_view) noexcept {}
        static constexpr void on_iteration(std::size_t) noexcept {}
    };

    template <
        typename EdgeT,
        DirectednessTag Directedness,
        typename ExecPolicy = policy::SerialExec,
        typename VectorOps = policy::ScalarVectorOps,
        GraphObserver Observer = NullObserver>
    CsrPageRankResult pagerank_engine(
        const CsrGraph<EdgeT, Directedness>& g,
        const CsrPageRankOptions& options = {},
        ExecPolicy&& exec = {},
        VectorOps&& vops = {},
        Observer&& observer = {}
    ) {
        if (options.damping_factor < 0.0 || options.damping_factor > 1.0) {
            throw std::invalid_argument("PageRank damping_factor must be in [0, 1].");
        }
        const std::size_t n = g.node_count();
        if (n == 0) return {};

        observer.on_phase_start("pagerank");

        const auto& offsets = g.offsets();
        const auto& targets = g.targets();
        std::vector<double> rank(n, 1.0 / static_cast<double>(n));
        std::vector<double> next_rank(n, 0.0);
        const double d = options.damping_factor;
        const bool gather = g.has_incoming();
        std::vector<double> contrib;
        if (gather) contrib.assign(n, 0.0);

        CsrPageRankResult result;
        for (std::size_t iter = 0; iter < options.max_iterations; ++iter) {
            observer.on_iteration(iter);

            const double dangling_mass = exec.transform_reduce(
                std::size_t{0}, n, 0.0,
                [&](std::size_t u) noexcept {
                    return (offsets[u] == offsets[u + 1]) ? rank[u] : 0.0;
                },
                std::plus<>{}
            );

            const double base = (1.0 - d) / static_cast<double>(n) + d * dangling_mass / static_cast<double>(n);

            if (gather) {
                const auto& in_offsets = g.in_offsets();
                const auto& in_sources = g.in_sources();
                exec.for_each(std::size_t{0}, n, [&](std::size_t u) noexcept {
                    const std::size_t out_deg = offsets[u + 1] - offsets[u];
                    contrib[u] = (out_deg == 0) ? 0.0 : (d * rank[u] / static_cast<double>(out_deg));
                });
                exec.for_each(std::size_t{0}, n, [&](std::size_t v) noexcept {
                    double acc = base;
                    for (std::size_t k = in_offsets[v]; k < in_offsets[v + 1]; ++k) {
                        acc += contrib[in_sources[k].value];
                    }
                    next_rank[v] = acc;
                });
            }
            else {
                vops.fill(next_rank, base);
                for (std::size_t u = 0; u < n; ++u) {
                    const std::size_t out_deg = offsets[u + 1] - offsets[u];
                    if (out_deg == 0) continue;
                    const double c = d * rank[u] / static_cast<double>(out_deg);
                    for (std::size_t i = offsets[u]; i < offsets[u + 1]; ++i) {
                        next_rank[targets[i].value] += c;
                    }
                }
            }

            const double delta = vops.l1_delta(next_rank, rank);
            rank.swap(next_rank);
            result.iterations = iter + 1;
            if (delta <= options.tolerance) {
                result.converged = true;
                break;
            }
        }
        result.ranks = std::move(rank);
        observer.on_phase_end("pagerank");
        return result;
    }

    template <typename EdgeT, DirectednessTag Directedness>
    CsrPageRankResult pagerank(const CsrGraph<EdgeT, Directedness>& g,
                               const CsrPageRankOptions& options = {}) {
        return pagerank_engine(g, options, policy::SerialExec{}, policy::ScalarVectorOps{});
    }

    // ----------- Breadth-First Search (BFS) -----------
    template <LiteGraphModel GraphT, typename Fn>
    void bfs(const GraphT& g, const NodeId start, Fn&& visit) {
        std::vector<std::uint8_t> visited(g.node_capacity(), 0);
        std::queue<NodeId> q;
        q.push(start);
        visited[start.value] = 1;
        while (!q.empty()) {
            NodeId u = q.front();
            q.pop();
            visit(u, g.node_data(u));
            for (auto v : g.neighbors(u)) {
                if (!visited[v.value]) {
                    visited[v.value] = 1;
                    q.push(v);
                }
            }
        }
    }

    // ----------- Depth-First Search (DFS) -----------
    template <LiteGraphModel GraphT, typename Fn>
    void dfs(const GraphT& g, const NodeId start, Fn&& visit) {
        std::vector<std::uint8_t> visited(g.node_capacity(), 0);
        std::stack<NodeId> stk;
        std::vector<NodeId> nbrs;
        stk.push(start);
        while (!stk.empty()) {
            NodeId u = stk.top();
            stk.pop();
            if (visited[u.value]) continue;
            visited[u.value] = 1;
            visit(u, g.node_data(u));
            // Add neighbors in reverse order for predictable ordering
            nbrs.clear();
            for (auto v : g.neighbors(u)) nbrs.push_back(v);
            std::ranges::reverse(nbrs);
            for (auto v : nbrs) stk.push(v);
        }
    }

    // ----------- Dijkstra's Shortest Path (numeric edge weight) -----------
    // Returns distance map and predecessor map
    template <LiteGraphModel GraphT, std::invocable<const typename GraphT::edge_type&> WeightFn>
    auto dijkstra(const GraphT& g, NodeId source, WeightFn&& weight_fn) {
        using DistT = double;
        constexpr DistT INF = std::numeric_limits<DistT>::infinity();

        std::vector<DistT> dist(g.node_capacity(), INF);
        std::vector<std::optional<NodeId>> pred(g.node_capacity());
        using QEntry = std::pair<DistT, NodeId>;
        auto cmp = [](const QEntry& a, const QEntry& b) { return a.first > b.first; };
        std::priority_queue<QEntry, std::vector<QEntry>, decltype(cmp)> pq(cmp);

        dist[source.value] = 0;
        pq.emplace(0, source);

        while (!pq.empty()) {
            auto [d, u] = pq.top();
            pq.pop();
            if (d > dist[u.value]) continue; // outdated
            for (auto eid : g.out_edges(u)) {
                const auto& edge = g.get_edge(eid);
                NodeId v = edge.to;
                auto w = weight_fn(edge.data);
                if (dist[u.value] + w < dist[v.value]) {
                    dist[v.value] = dist[u.value] + w;
                    pred[v.value] = u;
                    pq.emplace(dist[v.value], v);
                }
            }
        }
        return std::make_pair(dist, pred);
    }

    // Overload with default weight function for arithmetic edge types
    template <LiteGraphModel GraphT>
    auto dijkstra(const GraphT& g, NodeId source) {
        return dijkstra(g, source, [](const typename GraphT::edge_type& e) {
            return static_cast<double>(e);
        });
    }

    // ----------- Generic edge-tracking least-cost path -----------
    // A least-cost path that records the *predecessor edge* (not just the
    // predecessor node) so callers with parallel edges can recover exactly which
    // edge was taken. Parametric on:
    //   Cost     — the accumulator type (double, uint32, ...), any regular type.
    //   WeightFn — const edge_type& -> Cost, the per-edge weight.
    //   Combine  — (Cost running, Cost edge) -> Cost. Defaults to std::plus<Cost>.
    //              Pass a saturating adder to clamp integer overflow.
    //   Less     — strict-weak order used both for the frontier and for
    //              tie-breaking. It orders tuples of (cost, target NodeId, EdgeId)
    //              so equal-cost paths resolve deterministically.
    // Existing dijkstra()/a_star_search()/bellman_ford() are left untouched; this
    // is an additive symbol. Distances/predecessors are dense over
    // node_capacity() — sparse callers should intern their ids first.
    template <class Cost>
    struct DijkstraPathResult {
        std::vector<Cost> dist; // per node_capacity slot
        std::vector<std::optional<EdgeId>> pred_edge; // edge entering each node

        // Reconstruct the ordered EdgeId path source->target.
        // Returns empty vector iff target == source (cost-0 path) OR target is
        // unreachable; callers distinguish the two via dist[target].
        [[nodiscard]] std::vector<EdgeId>
        reconstruct(const NodeId source, const NodeId target) const {
            std::vector<EdgeId> path;
            if (target.value == source.value) return path;
            NodeId at = target;
            while (at.value != source.value) {
                const auto& pe = pred_edge[at.value];
                if (!pe) return {}; // unreachable
                path.push_back(*pe);
                at = from_of_[pe->value];
            }
            std::ranges::reverse(path);
            return path;
        }

        // Endpoint bookkeeping so reconstruct() can walk edges without the graph.
        std::vector<NodeId> from_of_; // source node of each edge slot
    };

    template <LiteGraphModel GraphT,
        class Cost,
        class WeightFn,
        class Combine = std::plus<Cost>,
        class Less = std::less<>>
        requires std::invocable<WeightFn, const typename GraphT::edge_type&>
    DijkstraPathResult<Cost>
    dijkstra_path(const GraphT& g, NodeId source, WeightFn weight_fn,
                  Combine combine = {}, Less less = {}) {
        DijkstraPathResult<Cost> r;
        const std::size_t n = g.node_capacity();
        r.dist.assign(n, std::numeric_limits<Cost>::max());
        r.pred_edge.assign(n, std::nullopt);
        r.from_of_.assign(g.edge_capacity(), NodeId{});

        // Frontier key: (cost, vertex, edge-into-vertex). The vertex/edge fields
        // make ordering total, so pops are a pure function of the graph.
        struct Key {
            Cost cost;
            NodeId vertex;
            EdgeId via;
        };
        auto key_less = [&](const Key& a, const Key& b) {
            if (!(a.cost == b.cost)) return less(a.cost, b.cost);
            if (a.vertex.value != b.vertex.value) return a.vertex.value < b.vertex.value;
            return a.via.value < b.via.value;
        };
        // priority_queue is a max-heap; invert so the "least" Key sits on top.
        auto pq_greater = [&](const Key& a, const Key& b) { return key_less(b, a); };
        std::priority_queue<Key, std::vector<Key>, decltype(pq_greater)> pq(pq_greater);

        r.dist[source.value] = Cost{};
        pq.push(Key{Cost{}, source, EdgeId{}});

        while (!pq.empty()) {
            const Key cur = pq.top();
            pq.pop();
            if (less(r.dist[cur.vertex.value], cur.cost)) continue; // stale
            g.for_each_out_edge(cur.vertex, [&](EdgeId eid, NodeId, NodeId to,
                                                const typename GraphT::edge_type& data) {
                const Cost ncost = combine(cur.cost, static_cast<Cost>(weight_fn(data)));
                if (less(ncost, r.dist[to.value])) {
                    r.dist[to.value] = ncost;
                    r.pred_edge[to.value] = eid;
                    r.from_of_[eid.value] = cur.vertex;
                    pq.push(Key{ncost, to, eid});
                }
            });
        }
        return r;
    }

    // ----------- A* Search Algorithm -----------
    /**
     * @brief Finds the shortest path from a source to a target node using the A* algorithm.
     *
     * A* is an extension of Dijkstra's algorithm that uses a heuristic to guide the search,
     * often resulting in better performance. The heuristic must be "admissible," meaning it
     * never overestimates the actual cost to reach the target.
     *
     * @tparam GraphT The type of the graph, which must conform to the LiteGraphModel concept.
     * @param g The graph to search within.
     * @param source The starting node for the path.
     * @param target The destination node for the path.
     * @param weight_fn A function `double(const EdgeType&)` that returns the cost (weight) of an edge.
     * @param heuristic_fn A function `double(NodeId)` that estimates the cost from a given node to the target.
     * @return A pair containing a vector of distances (g-costs) from the source and a vector of predecessors to reconstruct the path.
     */
    template <LiteGraphModel GraphT,
        std::invocable<const typename GraphT::edge_type&> WeightFn,
        std::invocable<NodeId> HeuristicFn>
    auto a_star_search(
        const GraphT& g,
        NodeId source,
        NodeId target,
        WeightFn&& weight_fn,
        HeuristicFn&& heuristic_fn
    ) {
        using DistT = double;
        constexpr DistT INF = std::numeric_limits<DistT>::infinity();

        // g_costs stores the cost of the cheapest path from the source to each node found so far.
        std::vector<DistT> g_costs(g.node_capacity(), INF);
        std::vector<std::optional<NodeId>> pred(g.node_capacity());

        // The priority queue stores pairs of {f_cost, node_id}, where f_cost = g_cost + h_cost.
        using QEntry = std::pair<DistT, NodeId>;
        auto cmp = [](const QEntry& a, const QEntry& b) { return a.first > b.first; };
        std::priority_queue<QEntry, std::vector<QEntry>, decltype(cmp)> pq(cmp);

        // Initialize the source node
        g_costs[source.value] = 0;
        pq.emplace(heuristic_fn(source), source); // f_cost for source is just the heuristic

        while (!pq.empty()) {
            auto [current_f_cost, u] = pq.top();
            pq.pop();

            // If we reached the target, we have found the shortest path.
            if (u.value == target.value) {
                break;
            }

            // This check handles outdated entries in the priority queue.
            if (current_f_cost > g_costs[u.value] + heuristic_fn(u)) {
                continue;
            }

            for (auto eid : g.out_edges(u)) {
                const auto& edge = g.get_edge(eid);
                NodeId v = edge.to;
                DistT weight = weight_fn(edge.data);

                // Calculate the tentative g_cost for the neighbor node v.

                if (const DistT tentative_g_cost = g_costs[u.value] + weight; tentative_g_cost < g_costs[v.value]) {
                    // This path to v is better than any previously found. Record it.
                    pred[v.value] = u;
                    g_costs[v.value] = tentative_g_cost;
                    DistT f_cost = tentative_g_cost + heuristic_fn(v);
                    pq.emplace(f_cost, v);
                }
            }
        }

        return std::make_pair(g_costs, pred);
    }

    // ----------- Reconstruct shortest path from predecessor map -----------
    // Suggested fix in LitegraphAlgorithms.hpp
    inline std::vector<NodeId> reconstruct_path(const NodeId target, const std::vector<std::optional<NodeId>>& pred) {
        std::vector<NodeId> path;
        NodeId at = target;

        // If there is no predecessor for the target, it is unreachable (except if it's the source itself)
        if (!pred[at.value]) {
            return {};
        }

        // The source node will not have a predecessor.
        // So we loop as long as a predecessor exists.
        while (pred[at.value]) {
            path.push_back(at);
            at = *pred[at.value];
        }
        // Add the source node itself, which was the final value of 'at'.
        path.push_back(at);

        std::ranges::reverse(path);

        // Add a check to ensure a valid path was found to the target
        if (!path.empty() && path.back().value == target.value) {
            return path;
        }
        return {}; // Return empty path if target was unreachable
    }

    // In LiteGraph.hpp, after the Graph class definition

    // ----------- Display Functions -----------

    /**
     * @brief Exports the graph to the DOT format as a free function.
     * @tparam NodeT The node data type.
     * @tparam EdgeT The edge data type.
     * @tparam Directedness The directedness of the graph.
     * @param g The graph to export.
     * @param os The output stream.
     */
    template <typename NodeT, typename EdgeT, typename Directedness>
    void to_dot(const Graph<NodeT, EdgeT, Directedness>& g, std::ostream& os) {
        // Check directedness to determine graph type (digraph or graph)
        os << (std::is_same_v<Directedness, Directed> ? "digraph" : "graph") << " G {\n";

        // Iterate through all active nodes to define them in DOT format
        for (auto [nid, node] : g.nodes()) {
            os << "  " << nid << " [label=\"" << nid << "\"];\n";
        }

        // Iterate through all active edges to define the connections
        for (auto [eid, edge] : g.edges()) {
            // Check directedness to determine edge operator (-> or --)
            os << "  " << edge.from.value
                << (std::is_same_v<Directedness, Directed> ? " -> " : " -- ")
                << edge.to.value << ";\n";
        }
        os << "}\n";
    }

    /**
     * @brief Displays the graph as an ASCII art adjacency list as a free function.
     * @param g The graph to display.
     * @param os The output stream.
     * @param node_formatter A function to format the node data.
     * @param edge_formatter A function to format the edge data.
     */
    template <
        typename NodeT, typename EdgeT, typename Directedness,
        typename NodeFormatter, typename EdgeFormatter
    >
    void to_ascii(
        const Graph<NodeT, EdgeT, Directedness>& g,
        std::ostream& os,
        NodeFormatter&& node_formatter,
        EdgeFormatter&& edge_formatter
    ) {
        os << "--- LiteGraph ASCII Display ---\n";
        // Iterate through each active node in the graph
        for (auto [nid_val, node] : g.nodes()) {
            NodeId nid{nid_val};
            // Format the node data using the provided formatter
            std::string node_str = node_formatter(g.node_data(nid));
            os << nid.value << (node_str.empty() ? "" : " [" + node_str + "]") << "\n";

            // Determine the arrow style based on graph directedness
            const char* arrow = std::is_same_v<Directedness, Directed> ? "->" : "--";

            // Iterate through the outgoing edges for this node
            for (auto eid : g.out_edges(nid)) {
                // Get the full edge object to access its data and destination
                const auto& edge = g.get_edge(eid);
                const NodeId target_node = edge.to;

                // Format the edge data using the provided formatter
                std::string edge_str = edge_formatter(edge.data);

                os << "  " << arrow << " " << target_node.value
                    << (edge_str.empty() ? "" : " (" + edge_str + ")") << "\n";
            }
        }
        os << "---------------------------\n";
    }

    /**
     * @brief Displays the graph as an ASCII art adjacency list with default formatting.
     */
    template <typename NodeT, typename EdgeT, typename Directedness>
    void to_ascii(const Graph<NodeT, EdgeT, Directedness>& g, std::ostream& os) {
        to_ascii(g, os,
                 [](const NodeT&) { return std::string(""); },
                 [](const EdgeT&) { return std::string(""); }
        );
    }


    // ----------- VF2 Subgraph Isomorphism -----------

    namespace detail {
        // A helper class to manage the state of the VF2 algorithm.
        // This encapsulates the core mappings and terminal sets required for the matching process.
        template <
            typename PatternGraph,
            typename TargetGraph,
            typename NodeComp = std::equal_to<>,
            typename EdgeComp = std::equal_to<>>
        class VF2State {
        public:
            // Type aliases for node and edge data
            using PatternNode = typename PatternGraph::node_type;
            using TargetNode = typename TargetGraph::node_type;
            using PatternEdge = typename PatternGraph::edge_type;
            using TargetEdge = typename TargetGraph::edge_type;

            using NodeComparator = NodeComp;
            using EdgeComparator = EdgeComp;

            VF2State(const PatternGraph& p, const TargetGraph& t, NodeComp nc = {}, EdgeComp ec = {})
                : g1(p), g2(t), node_comp(nc), edge_comp(ec), candidate_levels(p.node_capacity()) {
                // Initialize mappings and depth vectors
                core_1.resize(g1.node_capacity(), std::nullopt);
                core_2.resize(g2.node_capacity(), std::nullopt);
                depth_1.resize(g1.node_capacity(), 0);
                depth_2.resize(g2.node_capacity(), 0);

                // The mapping M: V_1 -> V_2
                // We use a vector for direct lookup from pattern node ID to target node ID
                mapping.resize(g1.node_capacity(), std::nullopt);

                core_len = 0;
            }

            // The main recursive matching function.
            // Candidate vectors are reused per depth level to avoid per-call allocations.
            void match(std::vector<std::unordered_map<std::size_t, std::size_t>>& results) {
                // If the mapping is complete, we found a solution
                if (core_len == g1.node_count()) {
                    // Store the current mapping as a result
                    std::unordered_map<std::size_t, std::size_t> current_match;
                    for (size_t i = 0; i < mapping.size(); ++i) {
                        if (mapping[i]) {
                            current_match[i] = mapping[i]->value;
                        }
                    }
                    results.push_back(std::move(current_match));
                    return;
                }

                if (core_len >= candidate_levels.size()) {
                    candidate_levels.resize(core_len + 1);
                }

                auto& candidates = candidate_levels[core_len];
                generate_candidate_pairs(candidates);

                for (auto [p_id, t_id] : candidates) {
                    // Check if the pair is feasible before recursing
                    if (is_feasible(p_id, t_id)) {
                        // If feasible, update the state and recurse
                        update_state(p_id, t_id);
                        match(results);
                        // Backtrack: restore the state to explore other possibilities
                        restore_state(p_id, t_id);
                    }
                }
            }

        private:
            // Fills `pairs` (clearing it first) with candidate (pattern, target) node pairs.
            void generate_candidate_pairs(std::vector<std::pair<NodeId, NodeId>>& pairs) const {
                pairs.clear();

                // Find the first unmatched pattern node
                NodeId p_unmatched = INVALID_NODE_ID;
                for (size_t i = 0; i < g1.node_capacity(); ++i) {
                    if (g1.valid_node(NodeId{i}) && !core_1[i]) {
                        p_unmatched = NodeId{i};
                        break;
                    }
                }

                if (!p_unmatched.is_valid()) return;

                // Case 1: T1_out and T2_out are not empty
                if (depth_1[p_unmatched.value] > 0) {
                    for (size_t i = 0; i < g2.node_capacity(); ++i) {
                        if (g2.valid_node(NodeId{i}) && !core_2[i] && depth_2[i] > 0) {
                            pairs.emplace_back(p_unmatched, NodeId{i});
                        }
                    }
                    return;
                }

                // Case 2: Neither set has terminal nodes, pick any unmatched node in G2
                for (size_t i = 0; i < g2.node_capacity(); ++i) {
                    if (g2.valid_node(NodeId{i}) && !core_2[i]) {
                        pairs.emplace_back(p_unmatched, NodeId{i});
                    }
                }
            }

            // Checks the syntactic and semantic feasibility of adding (p_id, t_id) to the current mapping
            bool is_feasible(NodeId p_id, NodeId t_id) const {
                // 1. Semantic Feasibility (User-defined comparators)
                if (!node_comp(g1.node_data(p_id), g2.node_data(t_id))) {
                    return false;
                }

                auto get_other_p = [&](NodeId cur, const auto& edge, bool is_outgoing) {
                    if constexpr (std::is_same_v<typename PatternGraph::directed_tag, Directed>) {
                        return is_outgoing ? edge.to : edge.from;
                    }
                    else {
                        return edge.from.value == cur.value ? edge.to : edge.from;
                    }
                };

                auto get_other_t = [&](NodeId cur, const auto& edge, bool is_outgoing) {
                    if constexpr (std::is_same_v<typename TargetGraph::directed_tag, Directed>) {
                        return is_outgoing ? edge.to : edge.from;
                    }
                    else {
                        return edge.from.value == cur.value ? edge.to : edge.from;
                    }
                };

                // Helper lambda to check edge consistency in one direction (outgoing or incoming)
                auto check_edges = [&](auto p_edges, auto const& g1_graph, auto const& g2_graph,
                                       const auto& p_core, const auto& t_core, bool is_outgoing) {
                    for (auto p_eid : p_edges) {
                        const auto& p_edge = g1_graph.get_edge(p_eid);
                        NodeId p_other = get_other_p(p_id, p_edge, is_outgoing);

                        // Case a: The other pattern node is already in the mapping
                        if (p_core[p_other.value]) {
                            NodeId t_other = *p_core[p_other.value];
                            bool edge_found = false;

                            auto check_target_edges = [&](auto t_edges) {
                                for (auto t_eid : t_edges) {
                                    const auto& t_edge = g2_graph.get_edge(t_eid);
                                    if (get_other_t(t_id, t_edge, is_outgoing).value == t_other.value) {
                                        if (edge_comp(p_edge.data, t_edge.data)) {
                                            edge_found = true;
                                            break;
                                        }
                                    }
                                }
                            };

                            if (is_outgoing) {
                                check_target_edges(g2_graph.out_edges(t_id));
                            }
                            else {
                                if constexpr (std::is_same_v<typename TargetGraph::directed_tag, Directed>) {
                                    check_target_edges(g2_graph.in_edges(t_id));
                                }
                            }

                            if (!edge_found) return false;
                        }
                    }
                    return true;
                };

                // Check outgoing edges from p_id and incoming edges to p_id
                if (!check_edges(g1.out_edges(p_id), g1, g2, core_1, core_2, true)) return false;

                if constexpr (std::is_same_v<typename PatternGraph::directed_tag, Directed>) {
                    if (!check_edges(g1.in_edges(p_id), g1, g2, core_1, core_2, false)) return false;
                }

                // 2. Syntactic Feasibility: 1-Lookahead / Cutoff rules
                int r_out = 0;
                for (auto eid : g1.out_edges(p_id)) {
                    NodeId other = get_other_p(p_id, g1.get_edge(eid), true);
                    if (!core_1[other.value] && depth_1[other.value] > 0) r_out++;
                }

                int t_r_out = 0;
                for (auto eid : g2.out_edges(t_id)) {
                    NodeId other = get_other_t(t_id, g2.get_edge(eid), true);
                    if (!core_2[other.value] && depth_2[other.value] > 0) t_r_out++;
                }

                if (r_out > t_r_out) return false;

                return true;
            }

            // Updates the state to include the new mapping (p_id -> t_id)
            void update_state(NodeId p_id, NodeId t_id) {
                core_len++;
                mapping[p_id.value] = t_id;
                core_1[p_id.value] = t_id;
                core_2[t_id.value] = p_id;

                if (depth_1[p_id.value] == 0) depth_1[p_id.value] = core_len;
                if (depth_2[t_id.value] == 0) depth_2[t_id.value] = core_len;

                auto get_other_node = [](NodeId current, const auto& edge) {
                    return edge.from.value == current.value ? edge.to : edge.from;
                };

                // Update depths for neighbors
                auto update_depth = [&]<typename T0>(auto& depth_vec, T0& graph, NodeId u) {
                    for (auto eid : graph.out_edges(u)) {
                        if (NodeId neighbor = get_other_node(u, graph.get_edge(eid)); depth_vec[neighbor.value] == 0)
                            depth_vec[neighbor.value] = core_len;
                    }
                    if constexpr (std::is_same_v<typename std::decay_t<T0>::directed_tag, Directed>) {
                        for (auto eid : graph.in_edges(u)) {
                            if (depth_vec[graph.get_edge(eid).from.value] == 0)
                                depth_vec[graph.get_edge(eid).from.value] = core_len;
                        }
                    }
                };
                update_depth(depth_1, g1, p_id);
                update_depth(depth_2, g2, t_id);
            }

            void restore_state(NodeId p_id, NodeId t_id) {
                mapping[p_id.value] = std::nullopt;
                core_1[p_id.value] = std::nullopt;
                core_2[t_id.value] = std::nullopt;

                auto get_other_node = [](NodeId current, const auto& edge) {
                    return edge.from.value == current.value ? edge.to : edge.from;
                };

                // Restore depths for neighbors
                auto restore_depth = [&]<typename T0>(auto& depth_vec, T0& graph, NodeId u) {
                    for (auto eid : graph.out_edges(u)) {
                        if (NodeId neighbor = get_other_node(u, graph.get_edge(eid));
                            depth_vec[neighbor.value] == core_len)
                            depth_vec[neighbor.value] = 0;
                    }
                    if constexpr (std::is_same_v<typename std::decay_t<T0>::directed_tag, Directed>) {
                        for (auto eid : graph.in_edges(u)) {
                            if (depth_vec[graph.get_edge(eid).from.value] == core_len)
                                depth_vec[graph.get_edge(eid).from.value] = 0;
                        }
                    }
                };

                restore_depth(depth_1, g1, p_id);
                restore_depth(depth_2, g2, t_id);

                core_len--;
            }

            const PatternGraph& g1;
            const TargetGraph& g2;
            NodeComp node_comp;
            EdgeComp edge_comp;

            // --- State variables ---
            size_t core_len = 0, t1_len = 0, t2_len = 0;
            std::vector<std::optional<NodeId>> core_1;
            std::vector<std::optional<NodeId>> core_2;
            std::vector<int> depth_1;
            std::vector<int> depth_2;
            std::vector<std::optional<NodeId>> mapping;
            std::vector<std::vector<std::pair<NodeId, NodeId>>> candidate_levels;
        };
    } // namespace detail

    /**
     * @brief Finds all occurrences of a pattern graph within a target graph (subgraph isomorphism).
     */
    template <
        LiteGraphModel PatternGraph,
        LiteGraphModel TargetGraph,
        typename NodeComp = std::equal_to<>,
        typename EdgeComp = std::equal_to<>>
    auto vf2_subgraph_isomorphism(
        const PatternGraph& pattern,
        const TargetGraph& target,
        NodeComp node_comp = {},
        EdgeComp edge_comp = {}
    ) -> std::vector<std::unordered_map<std::size_t, std::size_t>> {
        // Basic pre-check: pattern cannot have more nodes or edges than the target.
        if (pattern.node_count() > target.node_count() || pattern.edge_count() > target.edge_count()) {
            return {};
        }

        // In-edge checks require a directed graph model
        static_assert(
            std::is_same_v<typename PatternGraph::directed_tag, typename TargetGraph::directed_tag>,
            "Pattern and Target graphs must have the same directedness."
        );

        std::vector<std::unordered_map<std::size_t, std::size_t>> results;
        detail::VF2State<PatternGraph, TargetGraph, NodeComp, EdgeComp> state(pattern, target, node_comp, edge_comp);

        state.match(results);

        return results;
    }

    // ----------- Bellman-Ford Algorithm -----------
    /**
     * @brief Finds the shortest paths from a source node using the Bellman-Ford algorithm.
     *
     * This algorithm can handle edges with negative weights. It can also detect
     * negative-weight cycles that are reachable from the source.
     *
     * @tparam GraphT The type of the graph, which must conform to the LiteGraphModel concept.
     * @param g The graph to search within.
     * @param source The starting node.
     * @param weight_fn A function `double(const EdgeType&)` that returns the cost (weight) of an edge.
     * @return A tuple containing:
     * 1. `std::vector<double>`: A vector of distances from the source.
     * 2. `std::vector<std::optional<NodeId>>`: A vector of predecessors to reconstruct paths.
     * 3. `bool`: A flag that is `true` if a negative-weight cycle is detected, `false` otherwise.
     */
    template <LiteGraphModel GraphT, std::invocable<const typename GraphT::edge_type&> WeightFn>
    auto bellman_ford(
        const GraphT& g,
        const NodeId source,
        WeightFn&& weight_fn
    ) -> std::tuple<std::vector<double>, std::vector<std::optional<NodeId>>, bool> {
        using DistT = double;
        constexpr DistT INF = std::numeric_limits<DistT>::infinity();
        const size_t node_cap = g.node_capacity();

        std::vector dist(node_cap, INF);
        std::vector<std::optional<NodeId>> pred(node_cap, std::nullopt);

        dist[source.value] = 0;

        // Relax all edges |V| - 1 times.
        // A simple shortest path can have at most |V| - 1 edges.
        for (size_t i = 1; i < g.node_count(); ++i) {
            bool updated_in_pass = false;
            for (const auto& [eid_val, edge] : g.edges()) {
                if (dist[edge.from.value] != INF) {
                    DistT new_dist = dist[edge.from.value] + weight_fn(edge.data);
                    if (new_dist < dist[edge.to.value]) {
                        dist[edge.to.value] = new_dist;
                        pred[edge.to.value] = edge.from;
                        updated_in_pass = true;
                    }
                }
            }
            // Optimization: if no distances were updated in a pass, we can stop early.
            if (!updated_in_pass) {
                break;
            }
        }

        // Check for negative-weight cycles.
        // If we can still relax an edge, a negative cycle exists.
        for (const auto& [eid_val, edge] : g.edges()) {
            if (dist[edge.from.value] != INF && dist[edge.from.value] + weight_fn(edge.data) < dist[edge.to.value]) {
                return std::make_tuple(std::move(dist), std::move(pred), true); // Negative cycle detected
            }
        }

        return std::make_tuple(std::move(dist), std::move(pred), false); // No negative cycle
    }

    // ----------- Floyd-Warshall Algorithm -----------
    /**
     * @brief Finds the shortest paths between all pairs of nodes using the Floyd-Warshall algorithm.
     *
     * @tparam GraphT The type of the graph, which must conform to the LiteGraphModel concept.
     * @param g The graph to process.
     * @param weight_fn A function `double(const EdgeType&)` that returns the cost of an edge.
     * @return A pair containing:
     * 1. `std::vector<std::vector<double>>`: A 2D matrix where dist[i][j] is the shortest distance from node i to j.
     * 2. `std::vector<std::vector<std::optional<NodeId>>>`: A 2D matrix where next[i][j] is the next node on the path from i to j.
     */
    template <LiteGraphModel GraphT, std::invocable<const typename GraphT::edge_type&> WeightFn>
    auto floyd_warshall(
        const GraphT& g,
        WeightFn&& weight_fn
    ) -> std::pair<std::vector<std::vector<double>>, std::vector<std::vector<std::optional<NodeId>>>> {
        using DistT = double;
        constexpr DistT INF = std::numeric_limits<DistT>::infinity();
        const size_t N = g.node_capacity();

        // Flat 1D storage for cache-friendly access in the k-pivot triple loop.
        // Indexed as flat_dist[i*N + j] and flat_next[i*N + j].
        std::vector<DistT> flat_dist(N * N, INF);
        std::vector<std::optional<NodeId>> flat_next(N * N, std::nullopt);

        for (size_t i = 0; i < N; ++i) {
            flat_dist[i * N + i] = 0;
            flat_next[i * N + i] = NodeId{i};
        }

        for (const auto& [eid_val, edge] : g.edges()) {
            const size_t u = edge.from.value;
            const size_t v = edge.to.value;
            flat_dist[u * N + v] = weight_fn(edge.data);
            flat_next[u * N + v] = NodeId{v};
        }

        // Main algorithm loop — flat indexing eliminates pointer chasing on each row access
        for (size_t k = 0; k < N; ++k) {
            for (size_t i = 0; i < N; ++i) {
                const DistT d_ik = flat_dist[i * N + k];
                if (d_ik == INF) continue;
                for (size_t j = 0; j < N; ++j) {
                    const DistT d_kj = flat_dist[k * N + j];
                    if (d_kj == INF) continue;
                    if (d_ik + d_kj < flat_dist[i * N + j]) {
                        flat_dist[i * N + j] = d_ik + d_kj;
                        flat_next[i * N + j] = flat_next[i * N + k];
                    }
                }
            }
        }

        // Convert back to vector-of-vectors to preserve the existing API / test expectations.
        std::vector<std::vector<double>> dist(N, std::vector<double>(N));
        std::vector<std::vector<std::optional<NodeId>>> next(N, std::vector<std::optional<NodeId>>(N));
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j) {
                dist[i][j] = flat_dist[i * N + j];
                next[i][j] = flat_next[i * N + j];
            }
        }

        return std::make_pair(std::move(dist), std::move(next));
    }

    /**
     * @brief Reconstructs a path from the output of the Floyd-Warshall algorithm.
     * @param u The source node ID.
     * @param v The target node ID.
     * @param next The 'next' matrix produced by the floyd_warshall function.
     * @return A vector of NodeIds representing the path, or an empty vector if no path exists.
     */
    inline std::vector<NodeId> reconstruct_path(
        const NodeId u,
        const NodeId v,
        const std::vector<std::vector<std::optional<NodeId>>>& next
    ) {
        if (!next[u.value][v.value]) {
            return {}; // No path exists
        }

        std::vector<NodeId> path;
        NodeId at = u;
        while (at.value != v.value) {
            path.push_back(at);
            at = *next[at.value][v.value];
        }
        path.push_back(v);

        return path;
    }

    // ----------- Cycle Detection -----------
    namespace detail {
        // Iterative DFS cycle detection for directed graphs.
        // Uses an explicit stack and a "grey/black" visited encoding:
        //   visited[v] == 0 → unvisited
        //   visited[v] == 1 → on the current DFS path (grey)
        //   visited[v] == 2 → fully processed (black)
        template <LiteGraphModel GraphT>
        bool has_cycle_directed(const GraphT& g) {
            const std::size_t node_cap = g.node_capacity();
            std::vector<std::uint8_t> visited(node_cap, 0);

            // DFS stack frame: node + index into materialised neighbour list.
            struct Frame {
                NodeId node;
                std::vector<NodeId> nbrs;
                std::size_t idx{};
            };
            std::stack<Frame> stk;

            for (const auto& [nid_val, node_obj] : g.nodes()) {
                NodeId start{nid_val};
                if (visited[start.value] != 0) continue;

                visited[start.value] = 1; // grey
                std::vector<NodeId> init_nbrs;
                for (auto v : g.neighbors(start)) init_nbrs.push_back(v);
                stk.push(Frame{start, std::move(init_nbrs), 0});

                while (!stk.empty()) {
                    Frame& f = stk.top();
                    if (f.idx < f.nbrs.size()) {
                        NodeId v = f.nbrs[f.idx++];
                        if (visited[v.value] == 1) return true; // back edge
                        if (visited[v.value] == 0) {
                            visited[v.value] = 1; // grey
                            std::vector<NodeId> vnbrs;
                            for (auto w : g.neighbors(v)) vnbrs.push_back(w);
                            stk.push(Frame{v, std::move(vnbrs), 0});
                        }
                    }
                    else {
                        visited[f.node.value] = 2; // black
                        stk.pop();
                    }
                }
            }
            return false;
        }

        // Helper for undirected graph cycle detection (recursive is fine for
        // moderate depth; kept for undirected because the iterative directed
        // version does not handle the "parent edge skip" gracefully).
        template <LiteGraphModel GraphT>
        bool has_cycle_undirected_util(const GraphT& g, NodeId u,
                                       std::vector<std::uint8_t>& visited,
                                       std::optional<NodeId> parent) {
            visited[u.value] = 1;

            for (auto v : g.neighbors(u)) {
                if (parent && v.value == parent->value) {
                    continue; // Skip the edge back to the parent
                }
                if (visited[v.value]) {
                    return true; // Found a back edge to a visited node that isn't the direct parent
                }
                if (has_cycle_undirected_util(g, v, visited, u)) {
                    return true;
                }
            }
            return false;
        }
    } // namespace detail


    /**
     * @brief Detects if the graph contains a cycle.
     *
     * This function dispatches to a specific implementation for directed or undirected graphs.
     * @tparam GraphT The type of the graph, conforming to LiteGraphModel.
     * @param g The graph to check.
     * @return `true` if a cycle is found, `false` otherwise.
     */
    template <LiteGraphModel GraphT>
    bool has_cycle(const GraphT& g) {
        if constexpr (std::is_same_v<typename GraphT::directed_tag, Directed>) {
            return detail::has_cycle_directed(g);
        }
        else {
            const std::size_t cap = g.node_capacity();
            std::vector<std::uint8_t> visited(cap, 0);
            for (const auto& [nid_val, node] : g.nodes()) {
                if (NodeId u{nid_val}; !visited[u.value]) {
                    if (detail::has_cycle_undirected_util(g, u, visited, std::nullopt))
                        return true;
                }
            }
            return false;
        }
    }

    // ----------- Topological Sort -----------

    /**
     * @brief Computes a topological sort of a directed acyclic graph (DAG) using
     *        Kahn's iterative algorithm (BFS-based).
     *
     * Kahn's algorithm is inherently iterative: it processes nodes in waves of
     * in-degree zero, completely avoiding recursion-depth issues on deep graphs.
     * It also naturally detects cycles: if the output order is shorter than the
     * active node count, a cycle was detected and an empty vector is returned.
     *
     * Time complexity: O(V + E).
     *
     * @tparam GraphT The type of the graph, must be a directed graph.
     * @param g The graph to sort.
     * @return A vector of NodeIds in topological order, or empty if the graph
     *         contains a cycle.
     */
    template <LiteGraphModel GraphT>
    std::vector<NodeId> topological_sort(const GraphT& g) {
        static_assert(std::is_same_v<typename GraphT::directed_tag, Directed>,
                      "Topological sort is only defined for directed graphs.");

        const std::size_t node_cap = g.node_capacity();

        // Compute in-degrees for all active nodes.
        std::vector<std::size_t> in_deg(node_cap, 0);
        for (const auto& [nid_val, node_obj] : g.nodes()) {
            // Each active node contributes its active_out_degree to the in-degree
            // of its targets.  We walk out-edges for accuracy.
            for (NodeId u{nid_val}; EdgeId eid : g.out_edges(u)) {
                if (const auto& edge = g.get_edge(eid); g.valid_node(edge.to)) ++in_deg[edge.to.value];
            }
        }

        // Seed the queue with every node whose in-degree is zero.
        std::queue<NodeId> q;
        for (const auto& [nid_val, node_obj] : g.nodes()) {
            if (in_deg[nid_val] == 0) q.push(NodeId{nid_val});
        }

        std::vector<NodeId> order;
        order.reserve(g.node_count());

        while (!q.empty()) {
            NodeId u = q.front();
            q.pop();
            order.push_back(u);

            for (EdgeId eid : g.out_edges(u)) {
                const auto& edge = g.get_edge(eid);
                if (!g.valid_node(edge.to)) continue;
                if (--in_deg[edge.to.value] == 0) q.push(edge.to);
            }
        }

        // If order.size() < g.node_count() the graph has a cycle.
        if (order.size() != g.node_count()) return {};
        return order;
    }

    // ----------- Strongly Connected Components (Tarjan's Algorithm) -----------

    /**
     * @brief Finds the Strongly Connected Components (SCCs) of a directed graph
     *        using an iterative Tarjan's algorithm.
     *
     * The iterative formulation uses an explicit DFS stack and avoids the
     * recursion-depth limit that the naive recursive version hits on deep graphs
     * (e.g., long chains or deep DAGs with |V| > ~10 000 on typical platforms).
     *
     * Each stack frame stores:
     *   - the node being visited
     *   - an iterator index into its out-edge list so we can resume after
     *     processing each child
     *
     * This is O(V + E) — identical asymptotic complexity to the recursive version.
     *
     * @tparam GraphT The type of the graph, must be a directed graph.
     * @param g The graph to process.
     * @return A vector of vectors, where each inner vector contains the NodeIds of one SCC.
     */
    template <LiteGraphModel GraphT>
    std::vector<std::vector<NodeId>> strongly_connected_components(const GraphT& g) {
        static_assert(std::is_same_v<typename GraphT::directed_tag, Directed>,
                      "Strongly Connected Components are only defined for directed graphs.");

        const std::size_t node_cap = g.node_capacity();
        std::vector<int> disc(node_cap, -1);
        std::vector<int> low(node_cap, -1);
        std::vector<std::uint8_t> on_stack(node_cap, 0);
        std::vector<std::vector<NodeId>> sccs;
        std::stack<NodeId> st; // Tarjan's node stack
        int timer = 0;

        // DFS stack frame: node + index of the next neighbour to visit.
        struct Frame {
            NodeId node;
            std::vector<NodeId> nbrs; // materialised once, then iterated
            std::size_t nbr_idx{};
        };

        std::stack<Frame> dfs;

        auto enter_node = [&](NodeId u) {
            disc[u.value] = low[u.value] = ++timer;
            st.push(u);
            on_stack[u.value] = 1;
            // Materialise neighbours into the frame so we can resume mid-loop.
            std::vector<NodeId> nbrs;
            for (auto v : g.neighbors(u)) nbrs.push_back(v);
            dfs.push(Frame{u, std::move(nbrs), 0});
        };

        auto emit_scc = [&](const NodeId root) {
            std::vector<NodeId> scc;
            while (true) {
                NodeId top = st.top();
                st.pop();
                on_stack[top.value] = 0;
                scc.push_back(top);
                if (top.value == root.value) break;
            }
            sccs.push_back(std::move(scc));
        };

        for (const auto& [nid_val, node_obj] : g.nodes()) {
            NodeId start{nid_val};
            if (disc[start.value] != -1) continue;

            enter_node(start);

            while (!dfs.empty()) {
                Frame& frame = dfs.top();
                NodeId u = frame.node;

                if (frame.nbr_idx < frame.nbrs.size()) {
                    NodeId v = frame.nbrs[frame.nbr_idx++];

                    if (disc[v.value] == -1) {
                        // Tree edge: descend into v.
                        enter_node(v);
                        // We'll propagate low[v] → low[u] when we return.
                    }
                    else if (on_stack[v.value]) {
                        // Back edge: update low.
                        low[u.value] = std::min(low[u.value], disc[v.value]);
                    }
                    // Cross / forward edges: no action needed.
                }
                else {
                    // All neighbours processed — "return" from u.
                    dfs.pop();

                    if (!dfs.empty()) {
                        // Propagate low value to parent.
                        const NodeId parent = dfs.top().node;
                        low[parent.value] = std::min(low[parent.value], low[u.value]);
                    }

                    // SCC root check.
                    if (low[u.value] == disc[u.value]) {
                        emit_scc(u);
                    }
                }
            }
        }

        return sccs;
    }

    // =========================================================================
    // Enhanced SCC Diagnostics & Condensation Graph
    // =========================================================================

    struct scc_analysis {
        std::vector<std::vector<NodeId>> components;
        std::vector<NodeId> offending_cycle;
        bool has_self_loop{false};
    };

    template <LiteGraphModel GraphT>
    scc_analysis analyze_scc(const GraphT& g) {
        auto components = strongly_connected_components(g);
        scc_analysis analysis;
        analysis.components = std::move(components);

        // Check for self-loops and offending cycles (>1 node in component)
        for (const auto& [nid_val, node_obj] : g.nodes()) {
            NodeId u{nid_val};
            for (auto v : g.neighbors(u)) {
                if (u == v) {
                    analysis.has_self_loop = true;
                    if (analysis.offending_cycle.empty()) {
                        analysis.offending_cycle = {u, u};
                    }
                }
            }
        }

        for (const auto& comp : analysis.components) {
            if (comp.size() > 1 && analysis.offending_cycle.empty()) {
                analysis.offending_cycle = comp;
            }
        }

        return analysis;
    }

    template <LiteGraphModel GraphT>
    Graph<std::size_t, std::monostate, Directed>
    make_condensation_graph(const GraphT& g, const std::vector<std::vector<NodeId>>& components) {
        Graph<std::size_t, std::monostate, Directed> condensation;
        std::vector<std::size_t> node_to_comp(g.node_capacity(), static_cast<std::size_t>(-1));

        for (std::size_t comp_idx = 0; comp_idx < components.size(); ++comp_idx) {
            condensation.add_node(comp_idx);
            for (auto u : components[comp_idx]) {
                if (u.value < node_to_comp.size()) {
                    node_to_comp[u.value] = comp_idx;
                }
            }
        }

        for (std::size_t comp_idx = 0; comp_idx < components.size(); ++comp_idx) {
            for (auto u : components[comp_idx]) {
                for (auto v : g.neighbors(u)) {
                    if (v.value < node_to_comp.size()) {
                        std::size_t target_comp = node_to_comp[v.value];
                        if (target_comp != comp_idx && target_comp != static_cast<std::size_t>(-1)) {
                            (void)condensation.add_edge(NodeId{comp_idx}, NodeId{target_comp});
                        }
                    }
                }
            }
        }

        return condensation;
    }

    // ----------- Network Flow Algorithms -----------

    /**
     * @brief Computes the maximum flow from a source to a sink in a flow network using the Edmonds-Karp algorithm.
     *
     * The Edmonds-Karp algorithm is an implementation of the Ford-Fulkerson method that uses
     * Breadth-First Search (BFS) to find augmenting paths in the residual graph.
     *
     * @tparam GraphT The type of the graph, must be a directed graph.
     * @param g The graph representing the flow network.
     * @param source The source node of the flow.
     * @param sink The sink (destination) node of the flow.
     * @param capacity_fn A function `double(const EdgeType&)` that returns the capacity of an edge.
     * @return The maximum possible flow from the source to the sink as a double.
     */
    template <LiteGraphModel GraphT, std::invocable<const typename GraphT::edge_type&> CapacityFn>
    double edmonds_karp_max_flow(
        const GraphT& g,
        NodeId source,
        const NodeId sink,
        CapacityFn&& capacity_fn
    ) {
        static_assert(std::is_same_v<typename GraphT::directed_tag, Directed>,
                      "Network flow algorithms are defined for directed graphs.");

        const size_t node_cap = g.node_capacity();

        // Sparse residual graph: residual[u] maps target v -> remaining capacity.
        // This avoids the O(V²) dense matrix of the previous implementation.
        std::vector<std::unordered_map<std::size_t, double>> residual(node_cap);

        for (const auto& [eid_val, edge] : g.edges()) {
            residual[edge.from.value][edge.to.value] += capacity_fn(edge.data);
        }

        double max_flow = 0.0;

        while (true) {
            // Find an augmenting path from source to sink using BFS.
            std::vector<std::optional<NodeId>> parent(node_cap, std::nullopt);
            std::queue<std::pair<NodeId, double>> q;

            q.emplace(source, std::numeric_limits<double>::infinity());
            parent[source.value] = source; // Mark source as visited

            double path_flow = 0.0;

            while (!q.empty() && path_flow == 0.0) {
                auto [u, flow] = q.front();
                q.pop();

                if (u.value == sink.value) {
                    path_flow = flow;
                    break;
                }

                // Iterate only actual neighbors in the sparse residual graph
                for (const auto& [v_val, cap] : residual[u.value]) {
                    if (!parent[v_val] && cap > 0) {
                        NodeId v{v_val};
                        parent[v.value] = u;
                        double new_flow = std::min(flow, cap);
                        q.emplace(v, new_flow);
                    }
                }
            }

            // If no augmenting path was found, we are done.
            if (path_flow == 0.0) {
                break;
            }

            // Add the path flow to the total max flow.
            max_flow += path_flow;

            // Update residual capacities along the path.
            NodeId current = sink;
            while (current.value != source.value) {
                const NodeId prev = *parent[current.value];
                residual[prev.value][current.value] -= path_flow;
                residual[current.value][prev.value] += path_flow;
                current = prev;
            }
        }

        return max_flow;
    }

    // ----------- Graph Matching and Coloring -----------

    /**
     * @brief Assigns a color to each node in the graph using a greedy algorithm.
     *
     * The algorithm iterates through each node and assigns it the smallest integer color
     * that is not used by any of its already-colored neighbors. This provides a valid
     * coloring but does not guarantee the use of the minimum possible number of colors
     * (which is an NP-hard problem). For coloring purposes, edges are treated as undirected.
     *
     * @tparam GraphT The type of the graph, conforming to LiteGraphModel.
     * @param g The graph to color.
     * @return A vector of optional integers, where the value at index `i` corresponds
     * to the color of the node with ID `i`. std::nullopt if a node wasn't processed.
     */
    template <LiteGraphModel GraphT>
    std::vector<std::optional<int>> greedy_graph_coloring(const GraphT& g) {
        const size_t node_cap = g.node_capacity();
        std::vector<std::optional<int>> colors(node_cap, std::nullopt);

        // Use a deterministic node order for coloring: sort by node id
        std::vector<std::size_t> node_ids;
        for (const auto& [nid_val, node_obj] : g.nodes()) {
            node_ids.push_back(nid_val);
        }
        std::ranges::sort(node_ids);

        for (const std::size_t nid_val : node_ids) {
            NodeId u{nid_val};
            std::unordered_set<int> neighbor_colors;

            // For undirected graphs, neighbors() already returns all adjacent nodes
            // (the graph stores each edge in out_edges on both endpoints), so a single
            // pass over neighbors() is sufficient and correct.
            // For directed graphs, neighbors() only returns successors, so we also
            // inspect in_edges to treat the graph as undirected for coloring purposes.
            for (auto v : g.neighbors(u)) {
                if (colors[v.value]) {
                    neighbor_colors.insert(*colors[v.value]);
                }
            }
            if constexpr (std::is_same_v<typename GraphT::directed_tag, Directed>) {
                for (auto eid : g.in_edges(u)) {
                    if (const auto& edge = g.get_edge(eid); colors[edge.from.value]) {
                        neighbor_colors.insert(*colors[edge.from.value]);
                    }
                }
            }

            // Find the smallest non-negative integer color not in use by neighbors
            int current_color = 0;
            while (neighbor_colors.contains(current_color)) {
                current_color++;
            }
            colors[u.value] = current_color;
        }

        return colors;
    }


    namespace detail {
        // Helper DFS function for finding an augmenting path in a bipartite graph.
        template <LiteGraphModel GraphT>
        bool can_find_augmenting_path_dfs(
            const GraphT& g,
            NodeId u,
            std::vector<std::optional<NodeId>>& match,
            std::unordered_set<std::size_t>& visited
        ) {
            for (auto v : g.neighbors(u)) {
                if (!visited.contains(v.value)) {
                    visited.insert(v.value);
                    // If v is unmatched, or if its current match can find an alternative partner
                    if (!match[v.value] || can_find_augmenting_path_dfs(g, *match[v.value], match, visited)) {
                        match[v.value] = u;
                        return true;
                    }
                }
            }
            return false;
        }
    } // namespace detail


    /**
     * @brief Finds the maximum matching in a bipartite graph.
     *
     * A matching is a set of edges where no two edges share a common node. This
     * function finds the largest possible such set. It first verifies the graph is
     * bipartite.
     *
     * @tparam GraphT The type of the graph, must be undirected.
     * @param g The graph to process.
     * @return A vector of EdgeId that form the maximum matching. Returns an empty
     * vector if the graph is not bipartite.
     */
    template <LiteGraphModel GraphT>
    std::vector<EdgeId> max_bipartite_matching(const GraphT& g) {
        static_assert(std::is_same_v<typename GraphT::directed_tag, Undirected>,
                      "Bipartite matching is typically defined for undirected graphs.");

        const size_t node_cap = g.node_capacity();
        std::vector<std::optional<int>> part(node_cap, std::nullopt); // 0 or 1 for partition
        std::vector<NodeId> partition_u;

        // 1. Check if the graph is bipartite and get the first partition (U).
        bool is_bipartite = true;
        for (const auto& [nid_val, node_obj] : g.nodes()) {
            if (!part[nid_val]) {
                // If node not yet visited
                std::queue<NodeId> q;
                q.push(NodeId{nid_val});
                part[nid_val] = 0;

                while (!q.empty()) {
                    NodeId u = q.front();
                    q.pop();

                    if (*part[u.value] == 0) partition_u.push_back(u);

                    for (auto v : g.neighbors(u)) {
                        if (!part[v.value]) {
                            part[v.value] = 1 - *part[u.value];
                            q.push(v);
                        }
                        else if (*part[v.value] == *part[u.value]) {
                            is_bipartite = false;
                            break;
                        }
                    }
                    if (!is_bipartite) break;
                }
            }
            if (!is_bipartite) break;
        }

        if (!is_bipartite) {
            return {}; // Not bipartite, return empty matching
        }

        // 2. Find the maximum matching using augmenting paths (Ford-Fulkerson method).
        std::vector<std::optional<NodeId>> match(node_cap, std::nullopt);
        int result = 0;
        for (NodeId u : partition_u) {
            if (std::unordered_set<std::size_t> visited; detail::can_find_augmenting_path_dfs(g, u, match, visited)) {
                result++;
            }
        }

        // 3. Construct the final set of edges from the match vector.
        std::vector<EdgeId> matching_edges;
        matching_edges.reserve(result);
        // Only add edges for nodes in partition 1 (the "right" side), to avoid duplicates
        for (const auto& [nid_val, node_obj] : g.nodes()) {
            if (part[nid_val] && part[nid_val].value() == 1 && match[nid_val]) {
                NodeId u_node = match[nid_val].value();
                auto [value] = NodeId{nid_val};
                // Find the corresponding edge
                for (auto eid : g.out_edges(u_node)) {
                    if (g.get_edge(eid).to.value == value) {
                        matching_edges.push_back(eid);
                        break;
                    }
                }
            }
        }

        return matching_edges;
    }

    // ----------- Graph Centrality Measures -----------

    /**
     * @brief Calculates the normalized degree centrality for each node in the graph.
     *
     * Degree centrality is defined as the number of links incident upon a node.
     * For directed graphs, this implementation uses the total degree (in-degree + out-degree).
     * The result is normalized by dividing by (N-1), where N is the number of nodes.
     *
     * @tparam GraphT The type of the graph, conforming to LiteGraphModel.
     * @param g The graph to analyze.
     * @return A vector of doubles, where the value at index `i` is the degree centrality of node `i`.
     */
    template <LiteGraphModel GraphT>
    std::vector<double> degree_centrality(const GraphT& g) {
        const size_t node_cap = g.node_capacity();
        const size_t node_count = g.node_count();
        std::vector centrality(node_cap, 0.0);

        if (node_count <= 1) {
            return centrality;
        }

        const auto normalizer = static_cast<double>(node_count - 1);

        for (const auto& [nid_val, node_obj] : g.nodes()) {
            NodeId u{nid_val};
            centrality[u.value] = static_cast<double>(g.degree(u)) / normalizer;
        }

        return centrality;
    }


    /**
     * @brief Calculates the closeness centrality for each node in an unweighted graph.
     *
     * Closeness centrality measures the reciprocal of the sum of the shortest path
     * distances from a node to all other reachable nodes in its component. A higher
     * value indicates a more "central" position.
     *
     * @tparam GraphT The type of the graph, conforming to LiteGraphModel.
     * @param g The graph to analyze.
     * @return A vector of doubles, where the value at index `i` is the closeness centrality of node `i`.
     */
    template <LiteGraphModel GraphT>
    std::vector<double> closeness_centrality(const GraphT& g) {
        const size_t node_cap = g.node_capacity();
        std::vector centrality(node_cap, 0.0);

        for (const auto& [nid_val, node_obj] : g.nodes()) {
            NodeId source{nid_val};

            // Run BFS from source to find all-pairs shortest paths in unweighted graph
            std::vector dist(node_cap, -1);
            dist[source.value] = 0;

            double sum_of_distances = 0;
            size_t reachable_nodes = 0;

            std::queue<NodeId> bfs_q;
            bfs_q.push(source);
            std::vector<std::uint8_t> visited(node_cap, static_cast<std::uint8_t>(0));
            visited[source.value] = static_cast<std::uint8_t>(1);

            while (!bfs_q.empty()) {
                NodeId u = bfs_q.front();
                bfs_q.pop();

                reachable_nodes++;
                sum_of_distances += dist[u.value];

                for (auto v : g.neighbors(u)) {
                    if (visited[v.value] == 0) {
                        visited[v.value] = static_cast<std::uint8_t>(1);
                        dist[v.value] = dist[u.value] + 1;
                        bfs_q.push(v);
                    }
                }
            }

            if (sum_of_distances > 0 && reachable_nodes > 1) {
                // Standard definition: (Number of reachable nodes - 1) / Sum of distances
                centrality[source.value] = static_cast<double>(reachable_nodes - 1) / sum_of_distances;
            }
        }
        return centrality;
    }


    /**
     * @brief Calculates the betweenness centrality for each node using Brandes' algorithm.
     *
     * Betweenness centrality measures the extent to which a node lies on the shortest
     * paths between other pairs of nodes. It acts as a bridge in the network.
     * The result is normalized by dividing by the number of pairs of nodes.
     *
     * @tparam GraphT The type of the graph, conforming to LiteGraphModel.
     * @param g The graph to analyze.
     * @return A vector of doubles, where the value at index `i` is the betweenness centrality of node `i`.
     */
    template <LiteGraphModel GraphT>
    std::vector<double> betweenness_centrality(const GraphT& g) {
        const size_t node_cap = g.node_capacity();
        std::vector centrality(node_cap, 0.0);

        // Scratch buffers hoisted outside the per-source loop to avoid O(V²) allocations.
        std::vector<std::vector<NodeId>> P(node_cap);
        std::vector<double> sigma(node_cap);
        std::vector<int> d(node_cap);
        std::vector<double> delta(node_cap);

        for (const auto& [s_val, s_node_obj] : g.nodes()) {
            NodeId s{s_val};
            std::stack<NodeId> S;

            // Reset scratch buffers for this source
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
                        // Path discovery
                        Q.push(w);
                        d[w.value] = d[v.value] + 1;
                    }
                    if (d[w.value] == d[v.value] + 1) {
                        // Path counting
                        sigma[w.value] += sigma[v.value];
                        P[w.value].push_back(v);
                    }
                }
            }

            // Accumulation phase (Brandes' algorithm)
            while (!S.empty()) {
                const NodeId w = S.top();
                S.pop();
                for (const NodeId v : P[w.value]) {
                    if (sigma[w.value] != 0) {
                        delta[v.value] += (sigma[v.value] / sigma[w.value]) * (1.0 + delta[w.value]);
                    }
                }
                if (w.value != s.value) {
                    centrality[w.value] += delta[w.value];
                }
            }
        }

        // Normalize the results
        if (const size_t N = g.node_count(); N > 2) {
            double normalizer = 0;
            if constexpr (std::is_same_v<typename GraphT::directed_tag, Directed>) {
                normalizer = static_cast<double>(N - 1) * (N - 2);
            }
            else {
                normalizer = static_cast<double>(N - 1) * (N - 2) / 2.0;
            }

            if (normalizer > 0) {
                for (size_t i = 0; i < node_cap; ++i) {
                    centrality[i] /= normalizer;
                }
            }
        }

        return centrality;
    }

    // ----------- Minimum Spanning Tree (MST) Algorithms -----------

    namespace detail {
        // A Disjoint Set Union (DSU) data structure with path compression and union by size.
        // Required for an efficient implementation of Kruskal's algorithm.
        class DisjointSetUnion {
        public:
            explicit DisjointSetUnion(const size_t n) : parent(n), size(n, 1) {
                for (size_t i = 0; i < n; ++i) parent[i] = i;
            }

            // Finds the representative (root) of the set containing element i, with path compression.
            size_t find(const size_t i) {
                if (parent[i] == i) {
                    return i;
                }
                return parent[i] = find(parent[i]);
            }

            // Merges the sets containing elements i and j, using union by size.
            void unite(const size_t i, const size_t j) {
                size_t root_i = find(i);
                if (size_t root_j = find(j); root_i != root_j) {
                    if (size[root_i] < size[root_j]) std::swap(root_i, root_j);
                    parent[root_j] = root_i;
                    size[root_i] += size[root_j];
                }
            }

        private:
            std::vector<size_t> parent;
            std::vector<size_t> size;
        };
    } // namespace detail


    /**
     * @brief Finds an MST of an undirected, weighted graph using Kruskal's algorithm.
     *
     * Kruskal's algorithm works by sorting all edges by weight and adding them to the
     * MST if they do not form a cycle with already-added edges.
     *
     * @tparam GraphT The type of the graph, must be an undirected graph.
     * @param g The graph to process.
     * @param weight_fn A function `double(const EdgeType&)` that returns the cost of an edge.
     * @return A vector of EdgeId that form the Minimum Spanning Tree.
     */
    template <LiteGraphModel GraphT, std::invocable<const typename GraphT::edge_type&> WeightFn>
    std::vector<EdgeId> kruskal_mst(
        const GraphT& g,
        WeightFn&& weight_fn
    ) {
        static_assert(std::is_same_v<typename GraphT::directed_tag, Undirected>,
                      "MST algorithms are typically defined for undirected graphs.");

        // Collect and sort edges by weight
        std::vector<std::pair<double, EdgeId>> sorted_edges;
        for (const auto& [eid_val, edge] : g.edges()) {
            sorted_edges.push_back({weight_fn(edge.data), EdgeId{eid_val}});
        }

        std::sort(sorted_edges.begin(), sorted_edges.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        std::vector<EdgeId> mst_edges;
        detail::DisjointSetUnion dsu(g.node_capacity());

        // Process each edge in sorted order
        for (const auto& [weight, eid] : sorted_edges) {
            const auto& edge = g.get_edge(eid);

            // Check if the two endpoints are in different components
            const size_t root_from = dsu.find(edge.from.value);
            const size_t root_to = dsu.find(edge.to.value);

            if (root_from != root_to) {
                mst_edges.push_back(eid);
                dsu.unite(edge.from.value, edge.to.value);
            }
        }

        return mst_edges;
    }


    /**
     * @brief Finds an MST of an undirected, weighted graph using Prim's algorithm.
     *
     * Prim's algorithm grows the MST from an arbitrary starting node by iteratively
     * adding the cheapest edge that connects a node in the MST to a node outside the MST.
     *
     * @tparam GraphT The type of the graph, must be an undirected graph.
     * @param g The graph to process.
     * @param weight_fn A function `double(const EdgeType&)` that returns the cost of an edge.
     * @param start_node An optional starting node for the algorithm. If not provided, the first available node is used.
     * @return A vector of EdgeId that form the Minimum Spanning Tree.
     */
    template <LiteGraphModel GraphT, std::invocable<const typename GraphT::edge_type&> WeightFn>
    std::vector<EdgeId> prim_mst(
        const GraphT& g,
        WeightFn&& weight_fn,
        const std::optional<NodeId> start_node = std::nullopt
    ) {
        static_assert(std::is_same_v<typename GraphT::directed_tag, Undirected>,
                      "MST algorithms are typically defined for undirected graphs.");

        if (g.node_count() == 0) return {};

        // **FIXED LINE:** Changed -> to (*...).
        const NodeId start = start_node.value_or(NodeId{(*g.nodes().begin()).first});

        std::vector<EdgeId> mst_edges;
        mst_edges.reserve(g.node_count() > 0 ? g.node_count() - 1 : 0);

        using QEntry = std::pair<double, EdgeId>;
        auto cmp = [](const QEntry& a, const QEntry& b) { return a.first > b.first; };
        std::priority_queue<QEntry, std::vector<QEntry>, decltype(cmp)> pq(cmp);

        std::vector<std::uint8_t> in_mst(g.node_capacity(), static_cast<std::uint8_t>(0));
        size_t mst_size = 0;

        auto add_node_to_mst = [&](NodeId u) {
            if (in_mst[u.value] != 0) return;

            in_mst[u.value] = static_cast<std::uint8_t>(1);
            mst_size++;
            for (auto eid : g.out_edges(u)) {
                const auto& edge = g.get_edge(eid);
                // Add edge to PQ if it leads to a node not yet in the MST
                if (in_mst[edge.to.value] == 0) {
                    pq.emplace(weight_fn(edge.data), eid);
                }
            }
        };

        add_node_to_mst(start);

        while (!pq.empty() && mst_size < g.node_count()) {
            auto [weight, eid] = pq.top();
            pq.pop();

            const auto& edge = g.get_edge(eid);
            // Both endpoints of the edge could be in the MST if we've processed a denser part of the graph.
            // We only care about edges that expand the MST.
            if (in_mst[edge.from.value] != 0 && in_mst[edge.to.value] != 0) {
                continue;
            }

            mst_edges.push_back(eid);

            // Find which node is new and add it to the MST
            NodeId new_node = in_mst[edge.from.value] != 0 ? edge.to : edge.from;
            add_node_to_mst(new_node);
        }

        return mst_edges;
    }

    // ----------- Graph Edit Distance (GED) using A* Search -----------

    namespace detail {
        template <typename Graph1, typename Graph2>
        struct GEDSearchNode {
            std::vector<std::optional<NodeId>> g1_to_g2_mapping;
            std::vector<std::uint8_t> g2_is_mapped;
            double cost_so_far = 0.0; // g_cost
            double estimated_total_cost = 0.0; // f_cost = g_cost + h_cost

            bool operator>(const GEDSearchNode& other) const {
                return estimated_total_cost > other.estimated_total_cost;
            }
        };
    } // namespace detail


    // Both graph parameters are already constrained with LiteGraphModel.
    template <LiteGraphModel Graph1, LiteGraphModel Graph2,
        std::invocable<const typename Graph1::node_type&, const typename Graph2::node_type&> NodeSubstFn,
        std::invocable<const typename Graph2::node_type&> NodeInsFn,
        std::invocable<const typename Graph1::node_type&> NodeDelFn,
        std::invocable<const typename Graph1::edge_type&, const typename Graph2::edge_type&> EdgeSubstFn,
        std::invocable<const typename Graph2::edge_type&> EdgeInsFn,
        std::invocable<const typename Graph1::edge_type&> EdgeDelFn>
    double graph_edit_distance(
        const Graph1& g1,
        const Graph2& g2,
        NodeSubstFn&& node_subst_cost,
        NodeInsFn&& node_ins_cost,
        NodeDelFn&& node_del_cost,
        EdgeSubstFn&& edge_subst_cost,
        EdgeInsFn&& edge_ins_cost,
        EdgeDelFn&& edge_del_cost
    ) {
        using SearchNode = detail::GEDSearchNode<Graph1, Graph2>;

        std::priority_queue<SearchNode, std::vector<SearchNode>, std::greater<SearchNode>> open_set;

        auto find_edge = []<typename T0>(const T0& g, NodeId u,
                                         NodeId v) -> std::optional<typename std::decay_t<T0>::edge_type> {
            for (auto eid : g.out_edges(u)) {
                if (g.get_edge(eid).to.value == v.value) {
                    return g.get_edge(eid).data;
                }
            }
            return std::nullopt;
        };

        auto heuristic = [&](const SearchNode& node) -> double {
            double h = 0.0;
            size_t mapped_g1_nodes = 0;
            for (size_t i = 0; i < g1.node_capacity(); ++i) {
                if (node.g1_to_g2_mapping[i]) {
                    mapped_g1_nodes++;
                }
            }
            h += (g1.node_count() - mapped_g1_nodes) * 1.0;
            h += (g2.node_count() - mapped_g1_nodes) * 1.0;
            return h;
        };

        SearchNode start_node;
        start_node.g1_to_g2_mapping.assign(g1.node_capacity(), std::nullopt);
        start_node.g2_is_mapped.assign(g2.node_capacity(), static_cast<std::uint8_t>(0));
        start_node.estimated_total_cost = heuristic(start_node);
        open_set.push(start_node);

        std::vector<std::uint8_t> processed_g1(std::max<std::size_t>(1, g1.edge_capacity()), 0);
        std::vector<std::uint8_t> processed_g2(std::max<std::size_t>(1, g2.edge_capacity()), 0);

        while (!open_set.empty()) {
            SearchNode current = open_set.top();
            open_set.pop();

            std::optional<NodeId> u1_opt;
            for (const auto& [nid, n_obj] : g1.nodes()) {
                if (!current.g1_to_g2_mapping[nid]) {
                    u1_opt = NodeId{nid};
                    break;
                }
            }

            // GOAL CONDITION
            if (!u1_opt) {
                double final_cost = current.cost_so_far;

                // Insert remaining unmapped g2 nodes
                for (const auto& [nid, n_obj] : g2.nodes()) {
                    if (current.g2_is_mapped[nid] == 0) {
                        final_cost += node_ins_cost(n_obj.data);
                    }
                }

                // Build reverse map g2 -> g1 for mapped endpoints
                std::vector<std::optional<NodeId>> g2_to_g1(g2.node_capacity(), std::nullopt);
                for (size_t i = 0; i < current.g1_to_g2_mapping.size(); ++i) {
                    if (current.g1_to_g2_mapping[i] && current.g1_to_g2_mapping[i]->value < g2.node_capacity()) {
                        g2_to_g1[current.g1_to_g2_mapping[i]->value] = NodeId{i};
                    }
                }

                // 1) Reconcile g1 edges among mapped endpoints: substitute or delete (processed once)
                const std::size_t g2_cap = g2.node_capacity();
                std::ranges::fill(processed_g1, static_cast<std::uint8_t>(0));
                for (const auto& [eid_val, e1] : g1.edges()) {
                    if (eid_val < processed_g1.size()) {
                        if (processed_g1[eid_val]) continue;
                        processed_g1[eid_val] = 1;
                    }

                    auto from2_opt = current.g1_to_g2_mapping[e1.from.value];
                    auto to2_opt = current.g1_to_g2_mapping[e1.to.value];

                    if (!from2_opt || !to2_opt) continue;
                    if (from2_opt->value >= g2_cap || to2_opt->value >= g2_cap) continue;

                    NodeId from2 = *from2_opt;
                    NodeId to2 = *to2_opt;

                    auto e2_fwd = find_edge(g2, from2, to2);
                    auto e2_bwd = find_edge(g2, to2, from2);

                    if (e2_fwd) {
                        final_cost += edge_subst_cost(e1.data, *e2_fwd);
                    }
                    else if constexpr (std::is_same_v<typename Graph1::directed_tag, Undirected>) {
                        if (e2_bwd) {
                            final_cost += edge_subst_cost(e1.data, *e2_bwd);
                        }
                        else {
                            final_cost += edge_del_cost(e1.data);
                        }
                    }
                    else {
                        final_cost += edge_del_cost(e1.data);
                    }
                }

                // 2) Reconcile g2 edges among mapped endpoints: insert if missing in g1 (processed once)
                std::ranges::fill(processed_g2, static_cast<std::uint8_t>(0));
                for (const auto& [eid_val, e2] : g2.edges()) {
                    if (eid_val < processed_g2.size()) {
                        if (processed_g2[eid_val]) continue;
                        processed_g2[eid_val] = 1;
                    }

                    auto from1_opt = g2_to_g1[e2.from.value];
                    auto to1_opt = g2_to_g1[e2.to.value];
                    if (!from1_opt || !to1_opt) continue;

                    NodeId from1 = *from1_opt;
                    NodeId to1 = *to1_opt;

                    auto e1_fwd = find_edge(g1, from1, to1);
                    auto e1_bwd = find_edge(g1, to1, from1);

                    if (!e1_fwd) {
                        if constexpr (std::is_same_v<typename Graph2::directed_tag, Undirected>) {
                            if (!e1_bwd) {
                                final_cost += edge_ins_cost(e2.data);
                            }
                        }
                        else {
                            final_cost += edge_ins_cost(e2.data);
                        }
                    }
                }

                // NOTE: Do not add incident edge deletions for sentinel-mapped nodes here.
                // Those edge deletions have already been accounted during successor transitions.

                return final_cost;
            }

            NodeId u1 = *u1_opt;

            // --- Successor: delete node u1 in g1 ---
            {
                SearchNode successor = current;
                successor.g1_to_g2_mapping[u1.value] = NodeId{g2.node_capacity()};

                double cost = node_del_cost(g1.node_data(u1));
                // Add deletion cost for incident edges to already-mapped nodes (real mappings)
                for (size_t v1_idx = 0; v1_idx < g1.node_capacity(); ++v1_idx) {
                    if (successor.g1_to_g2_mapping[v1_idx] &&
                        successor.g1_to_g2_mapping[v1_idx]->value < g2.node_capacity() &&
                        v1_idx != u1.value) {
                        if constexpr (std::is_same_v<typename Graph1::directed_tag, Undirected>) {
                            if (u1.value < v1_idx) {
                                if (auto edge = find_edge(g1, u1, NodeId{v1_idx})) cost += edge_del_cost(*edge);
                            }
                        }
                        else {
                            if (auto edge = find_edge(g1, u1, NodeId{v1_idx})) cost += edge_del_cost(*edge);
                            if (auto edge = find_edge(g1, NodeId{v1_idx}, u1)) cost += edge_del_cost(*edge);
                        }
                    }
                }
                successor.cost_so_far += cost;
                successor.estimated_total_cost = successor.cost_so_far + heuristic(successor);
                open_set.push(successor);
            }

            // --- Successor: substitute u1 with each unmapped node u2 in g2 ---
            for (const auto& [u2_idx, u2_node] : g2.nodes()) {
                if (current.g2_is_mapped[u2_idx] == 0) {
                    NodeId u2{u2_idx};
                    SearchNode successor = current;

                    double step_cost = node_subst_cost(g1.node_data(u1), g2.node_data(u2));

                    // Edge edits against already-mapped nodes
                    // FIX: correct loop bound condition
                    for (size_t v1_idx = 0; v1_idx < g1.node_capacity(); ++v1_idx) {
                        if (auto v2_opt = successor.g1_to_g2_mapping[v1_idx]) {
                            if (v2_opt->value < g2.node_capacity()) {
                                NodeId v1{v1_idx};
                                NodeId v2 = *v2_opt;

                                if constexpr (std::is_same_v<typename Graph1::directed_tag, Undirected>) {
                                    if (u1.value < v1_idx) {
                                        auto e1 = find_edge(g1, u1, v1); // normalized direction
                                        auto e2 = find_edge(g2, u2, v2);
                                        if (e1 && e2) step_cost += edge_subst_cost(*e1, *e2);
                                        else if (e1) step_cost += edge_del_cost(*e1);
                                        else if (e2) step_cost += edge_ins_cost(*e2);
                                    }
                                }
                                else {
                                    auto e1_out = find_edge(g1, u1, v1);
                                    auto e2_out = find_edge(g2, u2, v2);
                                    if (e1_out && e2_out) step_cost += edge_subst_cost(*e1_out, *e2_out);
                                    else if (e1_out) step_cost += edge_del_cost(*e1_out);
                                    else if (e2_out) step_cost += edge_ins_cost(*e2_out);

                                    auto e1_in = find_edge(g1, v1, u1);
                                    auto e2_in = find_edge(g2, v2, u2);
                                    if (e1_in && e2_in) step_cost += edge_subst_cost(*e1_in, *e2_in);
                                    else if (e1_in) step_cost += edge_del_cost(*e1_in);
                                    else if (e2_in) step_cost += edge_ins_cost(*e2_in);
                                }
                            }
                        }
                    }

                    successor.cost_so_far += step_cost;
                    successor.g1_to_g2_mapping[u1.value] = u2;
                    successor.g2_is_mapped[u2.value] = static_cast<std::uint8_t>(1);
                    successor.estimated_total_cost = successor.cost_so_far + heuristic(successor);
                    open_set.push(successor);
                }
            }
        }

        return std::numeric_limits<double>::infinity();
    }

    // ----------- Bidirectional Dijkstra (single-pair shortest path) -----------
    //
    // Grows two simultaneous Dijkstra frontiers — one forward from source, one
    // backward from target.  The search terminates as soon as a node is settled
    // by *both* frontiers, giving O(sqrt(E) log V) settling cost in practice
    // versus O(E log V) for single-direction Dijkstra.
    //
    // Requirements
    // - Directed graph only (backward search needs in-edges).
    // - Non-negative edge weights (same as Dijkstra).
    //
    // Return value
    // - pair{ distance, path } where `path` is the sequence of NodeIds from
    //   source to target.  If no path exists, distance == infinity and path is
    //   empty.

    struct BiDijkstraResult {
        double distance{std::numeric_limits<double>::infinity()};
        std::vector<NodeId> path;
    };

    template <LiteGraphModel GraphT, std::invocable<const typename GraphT::edge_type&> WeightFn>
    BiDijkstraResult bidirectional_dijkstra(
        const GraphT& g,
        NodeId source,
        NodeId target,
        WeightFn&& weight_fn
    ) {
        static_assert(std::is_same_v<typename GraphT::directed_tag, Directed>,
                      "Bidirectional Dijkstra requires a directed graph.");

        if (!g.valid_node(source) || !g.valid_node(target))
            return {};

        if (source.value == target.value)
            return {0.0, {source}};

        const std::size_t cap = g.node_capacity();
        constexpr double INF = std::numeric_limits<double>::infinity();

        std::vector<double> d_fwd(cap, INF), d_bwd(cap, INF);
        std::vector<std::optional<NodeId>> pred_fwd(cap), pred_bwd(cap);
        std::vector<bool> settled_fwd(cap, false), settled_bwd(cap, false);

        using QEntry = std::pair<double, NodeId>;
        auto cmp = [](const QEntry& a, const QEntry& b) { return a.first > b.first; };
        std::priority_queue<QEntry, std::vector<QEntry>, decltype(cmp)> pq_fwd(cmp), pq_bwd(cmp);

        d_fwd[source.value] = 0.0;
        pq_fwd.emplace(0.0, source);
        d_bwd[target.value] = 0.0;
        pq_bwd.emplace(0.0, target);

        double best = INF;
        NodeId meeting_node{};

        // Helper: update `best` whenever a settled-in-both node is found.
        auto check_meeting = [&](const NodeId v) {
            if (settled_fwd[v.value] && settled_bwd[v.value]) {
                if (const double cand = d_fwd[v.value] + d_bwd[v.value]; cand < best) {
                    best = cand;
                    meeting_node = v;
                }
            }
        };

        while (!pq_fwd.empty() || !pq_bwd.empty()) {
            // --- Forward step ---
            if (!pq_fwd.empty()) {
                auto [df, u] = pq_fwd.top();
                pq_fwd.pop();
                if (df > d_fwd[u.value]) goto bwd_step;
                settled_fwd[u.value] = true;
                check_meeting(u);

                // Early termination: if the minimum key exceeds the best path
                // found so far we can stop the forward frontier.
                if (df >= best) goto bwd_step;

                for (EdgeId eid : g.out_edges(u)) {
                    const auto& edge = g.get_edge(eid);
                    NodeId v = edge.to;
                    double w = weight_fn(edge.data);
                    if (d_fwd[u.value] + w < d_fwd[v.value]) {
                        d_fwd[v.value] = d_fwd[u.value] + w;
                        pred_fwd[v.value] = u;
                        pq_fwd.emplace(d_fwd[v.value], v);
                        check_meeting(v);
                    }
                }
            }

        bwd_step:
            // --- Backward step (reverse graph: follow in-edges as if they were
            //     forward edges in the transposed graph) ---
            if (!pq_bwd.empty()) {
                auto [db, u] = pq_bwd.top();
                pq_bwd.pop();
                if (db > d_bwd[u.value]) continue;
                settled_bwd[u.value] = true;
                check_meeting(u);

                if (db >= best) continue;

                for (EdgeId eid : g.in_edges(u)) {
                    const auto& edge = g.get_edge(eid);
                    NodeId v = edge.from; // reverse direction
                    double w = weight_fn(edge.data);
                    if (d_bwd[u.value] + w < d_bwd[v.value]) {
                        d_bwd[v.value] = d_bwd[u.value] + w;
                        pred_bwd[v.value] = u;
                        pq_bwd.emplace(d_bwd[v.value], v);
                        check_meeting(v);
                    }
                }
            }

            // Both frontiers are exhausted or both minimum keys exceed best.
            if (pq_fwd.empty() && pq_bwd.empty()) break;
        }

        if (best == INF) return {};

        // Reconstruct path: source → meeting_node via pred_fwd,
        //                   then meeting_node → target via pred_bwd (reversed).
        std::vector<NodeId> fwd_path, bwd_path;
        NodeId at = meeting_node;
        while (pred_fwd[at.value].has_value()) {
            fwd_path.push_back(at);
            at = *pred_fwd[at.value];
        }
        fwd_path.push_back(at); // source
        std::ranges::reverse(fwd_path);

        at = meeting_node;
        while (pred_bwd[at.value].has_value()) {
            at = *pred_bwd[at.value];
            bwd_path.push_back(at);
        }

        fwd_path.insert(fwd_path.end(), bwd_path.begin(), bwd_path.end());
        return {best, std::move(fwd_path)};
    }

    // Overload with default identity weight for arithmetic edge types.
    template <LiteGraphModel GraphT>
        requires std::is_arithmetic_v<typename GraphT::edge_type>
    BiDijkstraResult bidirectional_dijkstra(const GraphT& g, NodeId source, NodeId target) {
        return bidirectional_dijkstra(g, source, target,
                                      [](const typename GraphT::edge_type& e) { return static_cast<double>(e); });
    }

    namespace parallel {
        // Parallel BFS with execution policy
        template <LiteGraphModel GraphT, typename ExecPolicy, typename Fn>
        void parallel_bfs(ExecPolicy&& policy, const GraphT& g, const NodeId start, Fn&& visit) {
            const size_t node_cap = g.node_capacity();
            std::vector<std::atomic<bool>> visited(node_cap);
            for (size_t i = 0; i < node_cap; ++i) {
                visited[i].store(false, std::memory_order_relaxed);
            }

            std::queue<NodeId> current_level, next_level;
            current_level.push(start);
            visited[start.value].store(true, std::memory_order_relaxed);

            while (!current_level.empty()) {
                // Process current level in parallel
                std::vector<NodeId> level_nodes;
                while (!current_level.empty()) {
                    level_nodes.push_back(current_level.front());
                    current_level.pop();
                }

                // Parallel visit of current level
                std::for_each(policy, level_nodes.begin(), level_nodes.end(),
                              [&](NodeId u) { visit(u, g.node_data(u)); });

                // Collect next level neighbors sequentially to avoid data races
                // on the neighbor_lists structure (pointer arithmetic was fragile)
                std::vector<NodeId> all_neighbors;
                for (const auto& u : level_nodes) {
                    for (auto v : g.neighbors(u)) {
                        all_neighbors.push_back(v);
                    }
                }

                // Add unvisited neighbors to next level (sequential, using atomic visited)
                for (NodeId v : all_neighbors) {
                    if (bool expected = false; visited[v.value].compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                        next_level.push(v);
                    }
                }

                std::swap(current_level, next_level);
            }
        }

        // Parallel shortest path computation using std::expected for error handling
        template <LiteGraphModel GraphT, typename ExecPolicy,
            std::invocable<const typename GraphT::edge_type&> WeightFn>
            requires std::is_execution_policy_v<std::remove_cvref_t<ExecPolicy>>
        std::expected<std::pair<std::vector<double>, std::vector<std::optional<NodeId>>>, GraphError>
        parallel_dijkstra(ExecPolicy&& policy, const GraphT& g, NodeId source, WeightFn&& weight_fn) {
            if (!g.valid_node(source)) {
                return std::unexpected(GraphError::InvalidNode);
            }

            using DistT = double;
            constexpr DistT INF = std::numeric_limits<DistT>::infinity();

            // Arrays are indexed by NodeId::value, so they must be sized by node_capacity()
            // (not node_count()) to correctly handle sparse / lazily-deleted node IDs.
            const std::size_t cap = g.node_capacity();

            std::vector<DistT> dist(cap, INF);
            std::vector<std::optional<NodeId>> pred(cap);
            std::vector<std::uint8_t> processed(cap, static_cast<std::uint8_t>(0));

            dist[source.value] = 0;

            // Parallel relaxation phases — iterate over the full ID space [0, cap) so that
            // nodes whose IDs are >= node_count() (sparse IDs) are not missed.
            for (std::size_t phase = 0; phase < g.node_count(); ++phase) {
                // Find minimum distance unprocessed node
                std::atomic min_dist{INF};
                std::atomic<std::size_t> min_node{cap}; // cap is the "not found" sentinel

                auto node_range = std::views::iota(std::size_t{0}, cap);
                std::for_each(policy, node_range.begin(), node_range.end(),
                              [&](const std::size_t i) {
                                  if (!g.valid_node(NodeId{i})) {
                                      return;
                                  }

                                  if (processed[i] == 0 && dist[i] < min_dist.load()) {
                                      DistT expected = min_dist.load();
                                      while (dist[i] < expected &&
                                          !min_dist.compare_exchange_weak(expected, dist[i])) {}
                                      if (dist[i] == min_dist.load()) {
                                          min_node.store(i);
                                      }
                                  }
                              });

                if (min_node.load() == cap) break; // no reachable unprocessed node remains

                NodeId u{min_node.load()};
                processed[u.value] = static_cast<std::uint8_t>(1);

                // Parallel edge relaxation
                auto out_edges = g.out_edges(u);
                std::for_each(policy, out_edges.begin(), out_edges.end(),
                              [&](EdgeId eid) {
                                  const auto& edge = g.get_edge(eid);
                                  const NodeId v = edge.to;
                                  auto w = weight_fn(edge.data);

                                  const DistT new_dist = dist[u.value] + w;
                                  DistT expected = dist[v.value];

                                  while (new_dist < expected &&
                                      !std::atomic_ref(dist[v.value]).compare_exchange_weak(expected, new_dist)) {
                                      expected = dist[v.value];
                                  }

                                  if (new_dist == dist[v.value]) {
                                      pred[v.value] = u;
                                  }
                              });
            }

            return std::make_pair(std::move(dist), std::move(pred));
        }

        // Overload with default weight function for arithmetic edge types
        template <LiteGraphModel GraphT, typename ExecPolicy>
            requires std::is_execution_policy_v<std::remove_cvref_t<ExecPolicy>>
        auto parallel_dijkstra(ExecPolicy&& policy, const GraphT& g, NodeId source) {
            return parallel_dijkstra(std::forward<ExecPolicy>(policy), g, source,
                                     [](const typename GraphT::edge_type& e) {
                                         return static_cast<double>(e);
                                     });
        }

        // Parallel graph coloring with improved load balancing
        template <LiteGraphModel GraphT, typename ExecPolicy>
            requires std::is_execution_policy_v<std::remove_cvref_t<ExecPolicy>>
        std::vector<std::optional<int>> parallel_greedy_coloring(ExecPolicy&& policy, const GraphT& g) {
            const size_t node_cap = g.node_capacity();
            std::vector<std::optional<int>> colors(node_cap, std::nullopt);
            std::vector<std::atomic<int>> atomic_colors(node_cap);

            // Initialize atomic colors
            std::for_each(policy, atomic_colors.begin(), atomic_colors.end(),
                          [](auto& ac) { ac.store(-1); });

            // Get sorted node list for deterministic coloring
            std::vector<std::size_t> node_ids;
            for (const auto& [nid_val, node_obj] : g.nodes()) {
                node_ids.push_back(nid_val);
            }
            std::sort(policy, node_ids.begin(), node_ids.end());

            // Process nodes in batches to reduce conflicts
            const size_t batch_size = std::max(size_t{1}, node_ids.size() / std::thread::hardware_concurrency());

            for (size_t start = 0; start < node_ids.size(); start += batch_size) {
                const size_t end = std::min(start + batch_size, node_ids.size());
                auto batch_range = std::span(node_ids).subspan(start, end - start);

                std::for_each(policy, batch_range.begin(), batch_range.end(),
                              [&](const std::size_t nid_val) {
                                  NodeId u{nid_val};
                                  std::set<int> neighbor_colors;

                                  // Collect neighbor colors
                                  for (auto v : g.neighbors(u)) {
                                      int color = atomic_colors[v.value].load();
                                      if (color >= 0) {
                                          neighbor_colors.insert(color);
                                      }
                                  }

                                  // Find smallest available color
                                  int current_color = 0;
                                  while (neighbor_colors.contains(current_color)) {
                                      current_color++;
                                  }

                                  atomic_colors[u.value].store(current_color);
                                  colors[u.value] = current_color;
                              });
            }

            return colors;
        }

        // Parallel connected components using Union-Find
        template <LiteGraphModel GraphT, typename ExecPolicy>
            requires std::is_execution_policy_v<std::remove_cvref_t<ExecPolicy>>
        std::vector<std::size_t> parallel_connected_components(ExecPolicy&& policy, const GraphT& g) {
            static_assert(std::is_same_v<typename GraphT::directed_tag, Undirected>,
                          "Connected components are typically computed for undirected graphs.");

            const size_t node_cap = g.node_capacity();
            std::vector<std::atomic<std::size_t>> parent(node_cap);
            std::vector<std::size_t> result(node_cap);

            // Initialize parent pointers
            auto indices = std::views::iota(std::size_t{0}, node_cap);
            std::for_each(policy, indices.begin(), indices.end(),
                          [&](const std::size_t i) { parent[i].store(i); });

            // Parallel edge processing with atomic operations
            auto edges_range = g.edges();
            std::for_each(policy, edges_range.begin(), edges_range.end(),
                          [&](const auto& edge_pair) {
                              const auto& [eid_val, edge] = edge_pair;

                              // Find with path compression (lock-free)
                              auto find = [&](std::size_t x) -> std::size_t {
                                  while (true) {
                                      std::size_t p = parent[x].load();
                                      if (p == x) return x;
                                      const std::size_t gp = parent[p].load();
                                      if (parent[x].compare_exchange_weak(p, gp)) {
                                          x = gp;
                                      }
                                      else {
                                          x = parent[x].load();
                                      }
                                  }
                              };

                              std::size_t root1 = find(edge.from.value);

                              // Union operation
                              if (std::size_t root2 = find(edge.to.value); root1 != root2) {
                                  if (root1 > root2) std::swap(root1, root2);
                                  parent[root2].compare_exchange_strong(root2, root1);
                              }
                          });

            // Final path compression and result collection
            std::for_each(policy, indices.begin(), indices.end(),
                          [&](const std::size_t i) {
                              std::size_t root = i;
                              while (parent[root].load() != root) {
                                  root = parent[root].load();
                              }
                              result[i] = root;
                          });

            return result;
        }
    } // namespace parallel
} // namespace litegraph
