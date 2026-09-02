# `LiteGraph` — Lightweight, High-Performance Graph Container

> **Tutorial:** [`docs/tutorials/LiteGraph.md`](../tutorials/LiteGraph.md) — island map, then Grand Regatta stress-test,
> parallel algorithms, and profiler races.
>
> **Headers:**
> - `include/containers/graph/LiteGraph.hpp` — Core graph data structure
> - `include/containers/graph/LiteGraphAlgorithms.hpp` — Algorithm library (BFS, DFS, Dijkstra, etc.)
> - `include/containers/graph/LiteGraphHighway.hpp` — SIMD-accelerated operations (optional add-on)
> - `include/containers/graph/LiteGraphPravaha.hpp` — Multi-core parallel graph algorithms (optional add-on)
>
> **Namespace:** `litegraph` (and `litegraph::pravaha` for parallel add-ons)  
> **Standard required:** C++23 (`-std=c++2b`)  
> **Dependencies:** Standard library only (core); optional external Google Highway (SIMD) or Pravaha (Task-Parallel)

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Philosophy](#2-philosophy)
3. [Core Components](#3-core-components)
4. [Graph Types & Templates](#4-graph-types--templates)
5. [Node and Edge Management](#5-node-and-edge-management)
6. [Graph Navigation](#6-graph-navigation)
7. [Memory & Performance Management](#7-memory--performance-management)
8. [Algorithms](#8-algorithms)
9. [SIMD Support (Highway)](#9-simd-support-highway)
10. [Error Handling](#10-error-handling)
11. [Examples](#11-examples)

---

## 1. Introduction

`LiteGraph` is a **modern, production-ready graph library** for C++23 that combines:

- **Zero-copy navigation** — efficient lazy ranges and reference returns
- **Strong type safety** — `NodeId` and `EdgeId` prevent accidental type confusion
- **Lazy evaluation** — views over graph structure without materialization
- **Flexible data payloads** — attach arbitrary types to nodes and edges
- **Rich algorithm suite** — 30+ graph algorithms (traversal, shortest path, matching, centrality, etc.)
- **SIMD acceleration** — optional Google Highway integration for bulk operations
- **Safe deletion** — lazy removal with optional compaction and ID remapping

All operations are zero-overhead abstractions. The generated machine code is comparable to hand-written imperative
loops.

---

## 2. Philosophy

### Two-level graph model

LiteGraph distinguishes between **logical** and **physical** storage:

- **Logical Graph:** Active nodes and edges visible to the user.
- **Physical Storage:** Vector of nodes and edges; some entries may be marked inactive/removed.

Lazy removal (mark as inactive) provides O (1) deletion with eventual compaction. This prevents O (N) rewiring costs but
trades memory fragmentation for speed.

### No dependency on RTTI or virtual dispatch

All algorithms are template-based and compile to direct code. The optional Highway headers provide SIMD variants without
affecting the core API.

### Strong identity types

`NodeId` and `EdgeId` are distinct types wrapping `std::size_t`. This prevents `add_edge(n0, n0, data)`
misinterpretations and makes code self-documenting:

```cpp
NodeId src = g.add_node(data);
NodeId dst = g.add_node(data);
EdgeId e = g.add_edge(src, dst, weight);  // Clear; cannot confuse src/dst types
```

### Directedness as a template parameter

Directed vs. undirected graphs are compile-time distinctions:

```cpp
Graph<NodeT, EdgeT, Directed>    // directed: u -> v
Graph<NodeT, EdgeT, Undirected>  // undirected: u -- v
```

Compiler checks for directedness-specific operations (e.g., `in_edges()`, `topological_sort()`).

---

## 3. Core Components

### 3.1 Error Types

```cpp
enum class GraphError {
    InvalidNode,
    InvalidEdge,
    NodeNotFound,
    EdgeNotFound,
    InvalidOperation
};

enum class AlgorithmError {
    CycleDetected,
    NotBipartite,
    IncompatibleGraphs,
    NoPath
};
```

### 3.2 Strong ID Types

```cpp
struct NodeId {
    std::size_t value;
    
    constexpr bool is_valid() const noexcept;
    explicit constexpr operator std::size_t() const noexcept;
    constexpr auto operator<=> (const NodeId &) const noexcept = default;
};

struct EdgeId {
    // Same interface as NodeId
    std::size_t value;
    constexpr bool is_valid() const noexcept;
    // ...
};

// Global invalid ID constants
inline constexpr NodeId INVALID_NODE_ID{};
inline constexpr EdgeId INVALID_EDGE_ID{};
```

### 3.3 Directedness Tags

```cpp
struct Directed {
    static constexpr bool is_directed = true;
};

struct Undirected {
    static constexpr bool is_directed = false;
};

template<typename T>
concept DirectednessTag = std::same_as<T, Directed> || std::same_as<T, Undirected>;
```

### 3.4 Concepts

```cpp
template<typename T>
concept Hashable = requires(T t) {
    std::hash<T>{}(t);
} && std::equality_comparable<T>;

template<typename T>
concept Serializable = requires(T t) {
    { std::format("{}", t) } -> std::convertible_to<std::string>;
};

template<typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;

// The LiteGraphModel concept: any graph-like object must satisfy this
template<typename G>
concept LiteGraphModel = requires(G g, NodeId nid, EdgeId eid, ...) {
    typename G::node_type;
    typename G::edge_type;
    typename G::directed_tag;
    { g.add_node(std::declval<typename G::node_type>()) } -> std::same_as<NodeId>;
    { g.add_edge(nid, nid, std::declval<typename G::edge_type>()) } -> std::same_as<EdgeId>;
    { g.remove_node(nid) } -> std::same_as<void>;
    { g.remove_edge(eid) } -> std::same_as<void>;
    { g.node_data(nid) } -> std::same_as<typename G::node_type &>;
    { g.edge_data(eid) } -> std::same_as<typename G::edge_type &>;
    { g.valid_node(nid) } -> std::same_as<bool>;
    { g.valid_edge(eid) } -> std::same_as<bool>;
    { g.neighbors(nid) };
    { g.out_edges(nid) };
    { g.degree(nid) } -> std::same_as<std::size_t>;
    { g.nodes() };
    { g.edges() };
};
```

---

## 4. Graph Types & Templates

### 4.1 Main Graph Class

```cpp
template<
    Hashable NodeT = std::monostate,
    Hashable EdgeT = std::monostate,
    DirectednessTag Directedness = Directed
>
    requires (std::move_constructible<NodeT> && std::move_constructible<EdgeT>)
class Graph {
    using node_type = NodeT;
    using edge_type = EdgeT;
    using directed_tag = Directedness;
    
    // Special members: explicitly defaulted for correct copy/move semantics
    Graph() = default;
    ~Graph() = default;
    Graph(const Graph &) = default;
    Graph(Graph &&) noexcept = default;
    Graph &operator=(const Graph &) = default;
    Graph &operator=(Graph &&) noexcept = default;
};
```

### 4.2 Pre-Defined Type Aliases

```cpp
using SimpleGraph = Graph<>;                                  // nodes and edges are std::monostate
using WeightedGraph = Graph<std::monostate, double>;         // edges have weight
using LabeledGraph = Graph<std::string, std::string>;        // nodes and edges have labels
using UndirectedGraph = Graph<std::monostate, std::monostate, Undirected>;
using WeightedUndirectedGraph = Graph<std::monostate, double, Undirected>;
```

### 4.3 Factory Functions

```cpp
template<typename NodeT = std::monostate, typename EdgeT = std::monostate>
auto make_directed_graph() -> Graph<NodeT, EdgeT, Directed>;

template<typename NodeT = std::monostate, typename EdgeT = std::monostate>
auto make_undirected_graph() -> Graph<NodeT, EdgeT, Undirected>;
```

---

## 5. Node and Edge Management

### 5.1 Adding Nodes

```cpp
// Lvalue reference overload
NodeId add_node(const NodeT &data = NodeT{});

// Rvalue overload (move optimization)
NodeId add_node(NodeT &&data);

// Batch add
template<std::ranges::input_range Range>
void batch_add_nodes(Range &&node_data_range)
    requires std::convertible_to<std::ranges::range_value_t<Range>, NodeT>;
```

**Example:**

```cpp
litegraph::Graph<int, double> g;
auto n0 = g.add_node(42);       // NodeId{0}
auto n1 = g.add_node(100);      // NodeId{1}
```

### 5.2 Adding Edges

```cpp
// Lvalue reference overload
EdgeId add_edge(NodeId from, NodeId to, const EdgeT &data = EdgeT{});

// Rvalue overload (move optimization)
EdgeId add_edge(NodeId from, NodeId to, EdgeT &&data);
```

**Example:**

```cpp
auto e0 = g.add_edge(n0, n1, 3.14);  // EdgeId{0}
```

Throws `std::out_of_range` if `from` or `to` is invalid.

### 5.3 Lazy Node Removal

```cpp
void remove_node(NodeId nid);
```

Marks the node and all incident edges as inactive. Does **not** reallocate storage. Throws `std::out_of_range` if `nid`
is invalid or already inactive.

**Important:** After removal, the node's storage slot remains but is logically deleted.

### 5.4 Lazy Edge Removal

```cpp
void remove_edge(EdgeId eid);
```

Marks edge as inactive. Throws `std::out_of_range` if `eid` is invalid.

### 5.5 Compaction and ID Remapping

```cpp
std::pair<IdMap, IdMap> compact();
```

**Purpose:** Physically eliminate inactive nodes and edges, returning ID remapping tables.

```cpp
IdMap node_id_map;
IdMap edge_id_map;
std::tie(node_id_map, edge_id_map) = g.compact();

// node_id_map[old_id] = new_id (or std::nullopt if the node was inactive)
if (node_id_map[old_nid.value]) {
    new_nid = NodeId{*node_id_map[old_nid.value]};
}
```

**Caution:** After `compact()`, all old `NodeId`/`EdgeId` handles are invalidated.

> **Warning:** ID invalidation is a hard break — any stored `NodeId` or `EdgeId` from before `compact()` is silently
> stale and will produce undefined behavior if used. Always use the returned `IdMap` tables to translate every stored
> handle before continuing. Do not cache IDs across a `compact()` call unless you immediately remap them.

---

## 6. Graph Navigation

### 6.1 Node and Edge Data Access

```cpp
// Mutable access (non-const graph)
NodeT &node_data(NodeId nid);
EdgeT &edge_data(EdgeId eid);

// Const access
const NodeT &node_data(NodeId nid) const;
const EdgeT &edge_data(EdgeId eid) const;

// Safe access with std::expected
std::expected<std::reference_wrapper<NodeT>, GraphError> try_node_data(NodeId nid) noexcept;
std::expected<std::reference_wrapper<const NodeT>, GraphError> try_node_data(NodeId nid) const noexcept;
std::expected<std::reference_wrapper<EdgeT>, GraphError> try_edge_data(EdgeId eid) noexcept;
```

Throws `std::out_of_range` if the node/edge is invalid.

**Example:**

```cpp
auto val = g.node_data(n0);       // Throws if invalid
val = 999;                        // Mutate in-place

auto result = g.try_node_data(n0);
if (result) {
    result->get() = 999;
} else {
    std::cerr << "Node invalid\n";
}
```

### 6.2 Validity Checks

```cpp
[[nodiscard]] bool valid_node(NodeId nid) const noexcept;
[[nodiscard]] bool valid_edge(EdgeId eid) const noexcept;
```

### 6.3 Lazy Edge Views (Non-Owning Ranges)

```cpp
// Outgoing edges (or all edges for undirected)
auto out_edges(NodeId nid) const;

// Incoming edges (directed graphs only)
auto in_edges(NodeId nid) const;
```

These return lazy ranges (filtered `std::views::iota`). **Invalidation hazard:** Do not mutate the graph while
iterating.

**Example:**

```cpp
for (auto eid : g.out_edges(n0)) {
    const auto &edge = g.get_edge(eid);
    std::cout << "Edge to node " << edge.to.value << "\n";
}
```

### 6.4 Materialized Edge Vectors (Safe Across Mutations)

```cpp
[[nodiscard]] std::vector<EdgeId> out_edge_ids(NodeId nid) const;
[[nodiscard]] std::vector<EdgeId> in_edge_ids(NodeId nid) const;  // Directed only
```

Returns an independent copy of active edge IDs, safe to iterate while mutating the graph.

### 6.5 Neighbors (Reachable Nodes)

```cpp
auto neighbors(NodeId nid) const;
```

Lazy range of active neighboring `NodeId`s via outgoing edges.

### 6.6 Degree

```cpp
[[nodiscard]] std::size_t out_degree(NodeId nid) const;
[[nodiscard]] std::size_t in_degree(NodeId nid) const;  // Directed only
[[nodiscard]] std::size_t degree(NodeId nid) const;     // Total degree
```

### 6.7 Hot-Path Callbacks

For performance-critical code, callback-based traversal:

```cpp
template<typename Fn>
void for_each_out_edge(NodeId nid, Fn &&fn);

template<typename Fn>
void for_each_in_edge(NodeId nid, Fn &&fn);  // Directed only

template<typename Fn>
void for_each_neighbor(NodeId nid, Fn &&fn);
```

**Callback signature:** `fn(EdgeId eid, NodeId source, NodeId target, EdgeDataRef data)`

**Example:**

```cpp
g.for_each_out_edge(n0, [](EdgeId eid, NodeId src, NodeId dst, double &w) {
    std::cout << "Edge " << src.value << " -> " << dst.value << " (weight=" << w << ")\n";
});
```

### 6.8 Node and Edge Iteration

```cpp
// Lazy filtered range: pair<index, const Node&>
auto nodes() const;

// Lazy filtered range: pair<index, const Edge&>
auto edges() const;

// Active NodeId ranges
auto active_node_ids() const;
auto active_edge_ids() const;

// Ranges for external algorithms (prefer over nodes/edges for compatibility)
auto node_range() const;   // pair<NodeId, const NodeT&>
auto edge_range() const;   // pair<EdgeId, const Edge&>
```

**Example:**

```cpp
for (auto [nid, node_data] : g.nodes()) {
    std::cout << "Node " << nid << ": " << node_data << "\n";
}
```

---

## 7. Memory & Performance Management

### 7.1 Capacity and Size

```cpp
[[nodiscard]] std::size_t node_count() const noexcept;     // Active nodes
[[nodiscard]] std::size_t edge_count() const noexcept;     // Active edges
[[nodiscard]] std::size_t node_capacity() const noexcept;  // Allocated slots
[[nodiscard]] std::size_t edge_capacity() const noexcept;  // Allocated slots
[[nodiscard]] bool empty() const noexcept;
```

### 7.2 Reservation

```cpp
void reserve_nodes(std::size_t capacity);
void reserve_edges(std::size_t capacity);
```

Pre-allocate storage to avoid reallocation during bulk additions.

### 7.3 Clearing

```cpp
void clear();
```

Erases all nodes and edges; resets capacity to zero.

### 7.4 Performance Monitoring

```cpp
struct GraphStats {
    std::size_t total_nodes;      // Allocated slots
    std::size_t active_nodes;     // Logical nodes
    std::size_t total_edges;      // Allocated slots
    std::size_t active_edges;     // Logical edges
    double load_factor;           // active_nodes / total_nodes
    std::size_t memory_usage;     // Approximate bytes
};

[[nodiscard]] GraphStats get_stats() const noexcept;
```

**Example:**

```cpp
auto stats = g.get_stats();
std::cout << "Memory: " << stats.memory_usage << " bytes\n";
std::cout << "Load factor: " << stats.load_factor << "\n";
```

### 7.5 Parallel Node Processing

```cpp
template<typename Exec, typename UnaryPred>
auto parallel_count_nodes_if(Exec &&policy, UnaryPred pred) const
    requires std::is_execution_policy_v<std::remove_cvref_t<Exec>>;
```

Uses C++17 execution policies for parallel iteration.

### 7.6 Removed: `as_node_matrix`

`as_node_matrix` was removed because it returned a raw pointer into internal storage, creating undefined behavior
whenever nodes were added, removed, or `compact()` was called while the pointer was live. The internal node pool is a
`std::vector<std::optional<Node>>` that can reallocate; any dangling pointer into it would silently corrupt memory.

**Alternative:** iterate via `node_range()` or copy node data into a caller-owned contiguous buffer as needed.

---

## 8. Algorithms

All algorithms live in the `litegraph` namespace and accept `LiteGraphModel` types.

### Algorithm Selection Guide

With 30+ algorithms available, use this table to pick the right one:

| Goal                             | Directed         | Undirected          | Notes                                       |
|----------------------------------|------------------|---------------------|---------------------------------------------|
| Visit all reachable nodes        | BFS / DFS        | BFS / DFS           | BFS = shortest hop count; DFS = stack-based |
| Shortest path (weighted)         | Dijkstra         | Dijkstra            | Non-negative weights only                   |
| Shortest path (negative weights) | Bellman-Ford     | —                   | Detects negative cycles                     |
| All-pairs shortest paths         | Floyd-Warshall   | Floyd-Warshall      | O(V³), dense graphs                         |
| Minimum spanning tree            | —                | Prim / Kruskal      | Prim = dense; Kruskal = sparse              |
| Detect cycles                    | DFS (back-edge)  | DFS (back-edge)     |                                             |
| Topological order                | Topological sort | —                   | DAG only                                    |
| Strongly connected components    | Tarjan's SCC     | —                   | Directed only                               |
| Cut vertices / bridges           | —                | Articulation points | Undirected only                             |
| Pattern matching                 | VF2 subgraph iso | VF2 subgraph iso    | Expensive for large graphs                  |
| Node influence / centrality      | PageRank         | —                   | Directed only                               |
| Community structure              | —                | Graph coloring      | Undirected only                             |
| Flow problems                    | Max flow         | —                   | Use for capacity networks                   |

**Rule of thumb:** unweighted reachability → BFS; weighted single-source → Dijkstra; global structure → SCC/topological
sort; connectivity → articulation points.

### 8.1 Traversal Algorithms

#### BFS (Breadth-First Search)

```cpp
template<LiteGraphModel GraphT, typename Fn>
void bfs(const GraphT &g, NodeId start, Fn &&visit);
```

Visits nodes in order of distance from `start`. Callback: `visit(NodeId, const NodeTRef&)`.

```cpp
litegraph::bfs(g, n0, [](NodeId nid, const int &data) {
    std::cout << "Visiting node " << nid.value << " (data=" << data << ")\n";
});
```

#### DFS (Depth-First Search)

```cpp
template<LiteGraphModel GraphT, typename Fn>
void dfs(const GraphT &g, NodeId start, Fn &&visit);
```

Visits nodes via depth-first order.

### 8.2 Shortest Path Algorithms

#### Dijkstra's Algorithm

```cpp
template<LiteGraphModel GraphT>
auto dijkstra(
    const GraphT &g, 
    NodeId source,
    std::function<double(const typename GraphT::edge_type &)> weight_fn = 
        [](const auto &e) { return static_cast<double>(e); }
) -> std::pair<std::vector<double>, std::vector<std::optional<NodeId>>>;
```

Returns: `(distances, predecessors)`. Distance infinity means unreachable.

```cpp
auto [dist, pred] = litegraph::dijkstra(g, n0);
std::cout << "Distance to n1: " << dist[n1.value] << "\n";

// Reconstruct path
auto path = litegraph::reconstruct_path(n1, pred);
for (auto nid : path) {
    std::cout << nid.value << " ";
}
std::cout << "\n";
```

#### A* Search

```cpp
template<LiteGraphModel GraphT>
auto a_star_search(
    const GraphT &g,
    NodeId source,
    NodeId target,
    std::function<double(const typename GraphT::edge_type &)> weight_fn,
    std::function<double(NodeId)> heuristic_fn  // Admissible heuristic
) -> std::pair<std::vector<double>, std::vector<std::optional<NodeId>>>;
```

**Heuristic requirement:** Never overestimate true cost (admissible).

```cpp
auto heuristic = [&](NodeId nid) -> double {
    // Euclidean distance to target (pseudo-code)
    return estimate_distance(nid, target);
};

auto [cost, pred] = litegraph::a_star_search(g, n0, n5, weight_fn, heuristic);
```

#### Bellman-Ford Algorithm

```cpp
template<LiteGraphModel GraphT>
auto bellman_ford(
    const GraphT &g,
    NodeId source,
    std::function<double(const typename GraphT::edge_type &)> weight_fn
) -> std::tuple<std::vector<double>, std::vector<std::optional<NodeId>>, bool>;
```

Returns: `(distances, predecessors, has_negative_cycle)`.

Handles negative-weight edges and detects negative cycles reachable from source.

#### Floyd-Warshall Algorithm

```cpp
template<LiteGraphModel GraphT>
auto floyd_warshall(
    const GraphT &g,
    std::function<double(const typename GraphT::edge_type &)> weight_fn
) -> std::pair<std::vector<std::vector<double>>, std::vector<std::vector<std::optional<NodeId>>>>;
```

Returns: `(all-pairs distance matrix, all-pairs next-node matrix)`.

All-pairs shortest paths; O (V³) time complexity.

```cpp
auto [dist, next] = litegraph::floyd_warshall(g, weight_fn);
auto path = litegraph::reconstruct_path(n0, n5, next);
```

### 8.3 Cycle Detection

```cpp
template<LiteGraphModel GraphT>
bool has_cycle(const GraphT &g);
```

Returns `true` if the graph contains any cycle.

### 8.4 Topological Sort

```cpp
template<LiteGraphModel GraphT>
std::vector<NodeId> topological_sort(const GraphT &g);
```

Only for directed graphs. Returns linearly ordered nodes such that all edges go forward.

Requires: DAG (directed acyclic graph).

```cpp
auto order = litegraph::topological_sort(dag);
for (auto nid : order) {
    std::cout << nid.value << " ";  // Topologically ordered
}
```

### 8.5 Strongly Connected Components (Tarjan's Algorithm)

```cpp
template<LiteGraphModel GraphT>
std::vector<std::vector<NodeId>> strongly_connected_components(const GraphT &g);
```

Only for directed graphs. Returns groups of mutually reachable nodes.

```cpp
auto sccs = litegraph::strongly_connected_components(directed_g);
for (const auto &component : sccs) {
    std::cout << "SCC: ";
    for (auto nid : component) std::cout << nid.value << " ";
    std::cout << "\n";
}
```

### 8.6 Subgraph Isomorphism (VF2 Algorithm)

```cpp
template<LiteGraphModel PatternGraph, LiteGraphModel TargetGraph>
auto vf2_subgraph_isomorphism(
    const PatternGraph &pattern,
    const TargetGraph &target,
    typename detail::VF2State<PatternGraph, TargetGraph>::NodeComparator node_comp = 
        [](const auto &, const auto &) { return true; },
    typename detail::VF2State<PatternGraph, TargetGraph>::EdgeComparator edge_comp = 
        [](const auto &, const auto &) { return true; }
) -> std::vector<std::unordered_map<std::size_t, std::size_t>>;
```

Finds all occurrences of a pattern graph in a target graph.

Returns: Vector of mappings (pattern node ID → target node ID).

```cpp
auto matches = litegraph::vf2_subgraph_isomorphism(pattern, target);
std::cout << "Found " << matches.size() << " matches\n";
for (const auto &mapping : matches) {
    std::cout << "Pattern node 0 -> Target node " << mapping.at(0) << "\n";
}
```

### 8.7 Network Flow Algorithms

#### Edmonds-Karp Max Flow

```cpp
template<LiteGraphModel GraphT>
double edmonds_karp_max_flow(
    const GraphT &g,
    NodeId source,
    NodeId sink,
    std::function<double(const typename GraphT::edge_type &)> capacity_fn
);
```

Directed graphs only. Returns maximum flow from source to sink.

```cpp
double max_flow = litegraph::edmonds_karp_max_flow(
    g, source, sink,
    [](const double &cap) { return cap; }
);
```

### 8.8 Graph Coloring

#### Greedy Graph Coloring

```cpp
template<LiteGraphModel GraphT>
std::vector<std::optional<int>> greedy_graph_coloring(const GraphT &g);
```

Assigns smallest-available colors to nodes; treats edges as undirected.

Returns: Vector where `colors[i]` is the color (0, 1, 2, …) of node `i`.

Not optimal (NP-hard problem) but fast and reasonable.

### 8.9 Bipartite Matching

#### Maximum Bipartite Matching

```cpp
template<LiteGraphModel GraphT>
std::vector<EdgeId> max_bipartite_matching(const GraphT &g);
```

Undirected graphs only. First verifies bipartiteness; returns set of non-adjacent edges.

```cpp
auto matching = litegraph::max_bipartite_matching(bipartite_g);
std::cout << "Matching size: " << matching.size() << "\n";
```

### 8.10 Centrality Measures

#### Degree Centrality

```cpp
template<LiteGraphModel GraphT>
std::vector<double> degree_centrality(const GraphT &g);
```

Normalized: `degree / (N - 1)`.

#### Closeness Centrality

```cpp
template<LiteGraphModel GraphT>
std::vector<double> closeness_centrality(const GraphT &g);
```

For each node, reciprocal of average distance to reachable nodes.

#### Betweenness Centrality (Brandes' Algorithm)

```cpp
template<LiteGraphModel GraphT>
std::vector<double> betweenness_centrality(const GraphT &g);
```

Fraction of all-pairs shortest paths passing through a node. Normalized.

### 8.11 Minimum Spanning Tree (MST)

#### Kruskal's Algorithm

```cpp
template<LiteGraphModel GraphT>
std::vector<EdgeId> kruskal_mst(
    const GraphT &g,
    std::function<double(const typename GraphT::edge_type &)> weight_fn
);
```

Undirected graphs only.

#### Prim's Algorithm

```cpp
template<LiteGraphModel GraphT>
std::vector<EdgeId> prim_mst(
    const GraphT &g,
    std::function<double(const typename GraphT::edge_type &)> weight_fn,
    const std::optional<NodeId> start_node = std::nullopt
);
```

Undirected graphs only. Can specify a starting node.

### 8.12 Graph Edit Distance

```cpp
template<LiteGraphModel Graph1, LiteGraphModel Graph2>
double graph_edit_distance(
    const Graph1 &g1,
    const Graph2 &g2,
    std::function<double(const typename Graph1::node_type &, const typename Graph2::node_type &)> node_subst_cost,
    std::function<double(const typename Graph2::node_type &)> node_ins_cost,
    std::function<double(const typename Graph1::node_type &)> node_del_cost,
    std::function<double(const typename Graph1::edge_type &, const typename Graph2::edge_type &)> edge_subst_cost,
    std::function<double(const typename Graph2::edge_type &)> edge_ins_cost,
    std::function<double(const typename Graph1::edge_type &)> edge_del_cost
);
```

Computes minimum cost sequence of node/edge edits to transform g1 into g2. Uses A* search.

### 8.13 PageRank (CSR)

See Section 8.14 below.

### 8.14 Frozen CSR Graph and Algorithms

For performance-critical bulk algorithms, convert to **Compressed Sparse Row (CSR)** format:

```cpp
template<Hashable NodeT, Hashable EdgeT, DirectednessTag Directedness>
CsrGraph<EdgeT, Directedness> freeze_to_csr(const Graph<NodeT, EdgeT, Directedness> &g);
```

The CSR graph is **immutable** and **contiguous**:

```cpp
struct CsrGraph {
    [[nodiscard]] std::size_t node_count() const noexcept;
    [[nodiscard]] std::size_t edge_count() const noexcept;
    
    [[nodiscard]] std::span<const NodeId> out_neighbors(std::size_t compact_node_index) const;
    [[nodiscard]] std::span<const EdgeT> out_edge_data(std::size_t compact_node_index) const;
    [[nodiscard]] bool has_edge_weights() const noexcept;
    [[nodiscard]] std::span<const double> edge_weights() const;
    
    [[nodiscard]] const std::vector<std::optional<std::size_t>> &original_to_compact() const noexcept;
    [[nodiscard]] const std::vector<NodeId> &compact_to_original() const noexcept;
};
```

**PageRank on CSR:**

```cpp
struct CsrPageRankOptions {
    double damping_factor = 0.85;
    std::size_t max_iterations = 100;
    double tolerance = 1e-9;
};

struct CsrPageRankResult {
    std::vector<double> ranks;
    std::size_t iterations;
    bool converged;
};

template<typename EdgeT, DirectednessTag Directedness>
CsrPageRankResult pagerank(
    const CsrGraph<EdgeT, Directedness> &g,
    const CsrPageRankOptions &options = {}
);
```

**BFS on CSR:**

```cpp
struct CsrBfsResult {
    std::vector<std::size_t> distances;
    std::vector<std::optional<std::size_t>> predecessors;
};

template<typename EdgeT, DirectednessTag Directedness>
CsrBfsResult bfs(const CsrGraph<EdgeT, Directedness> &g, std::size_t start_compact_index);
```

**Example:**

```cpp
auto csr = litegraph::freeze_to_csr(g);
auto pr = litegraph::pagerank(csr);

std::cout << "PageRank scores:\n";
for (std::size_t i = 0; i < csr.node_count(); ++i) {
    auto original_nid = csr.original_node_id(i);
    std::cout << "Node " << original_nid.value << ": " << pr.ranks[i] << "\n";
}
```

### 8.15 Display Functions

#### DOT Format Export

```cpp
template<typename NodeT, typename EdgeT, typename Directedness>
void to_dot(const Graph<NodeT, EdgeT, Directedness> &g, std::ostream &os);
```

Outputs Graphviz DOT format. Pipe to `dot -Tsvg` for visualization.

```cpp
std::ofstream out("graph.dot");
litegraph::to_dot(g, out);
```

#### ASCII Adjacency List

```cpp
template<typename NodeT, typename EdgeT, typename Directedness>
void to_ascii(
    const Graph<NodeT, EdgeT, Directedness> &g,
    std::ostream &os,
    NodeFormatter &&node_formatter,
    EdgeFormatter &&edge_formatter
);

template<typename NodeT, typename EdgeT, typename Directedness>
void to_ascii(const Graph<NodeT, EdgeT, Directedness> &g, std::ostream &os);
```

Pretty-prints the graph as an ASCII adjacency list.

```cpp
litegraph::to_ascii(g, std::cout,
    [](const int &data) { return "data=" + std::to_string(data); },
    [](const double &w) { return "w=" + std::to_string(w); }
);
```

---

## 9. SIMD Support (Highway)

The `LiteGraphHighway.hpp` header provides optional SIMD-accelerated operations using Google Highway.

### Enable SIMD

Define the macro before inclusion:

```cpp
#define LITEGRAPH_ENABLE_HIGHWAY
#include "containers/graph/LiteGraphHighway.hpp"
```

Or compile with `-DLITEGRAPH_ENABLE_HIGHWAY` if Highway is available.

### SIMD Functions

```cpp
namespace litegraph::highway {
    [[nodiscard]] constexpr bool enabled() noexcept;
    [[nodiscard]] inline std::int64_t supported_targets_mask() noexcept;
    
    // SIMD PageRank (faster than scalar version)
    template<typename EdgeT, DirectednessTag Directedness>
    CsrPageRankResult pagerank(
        const CsrGraph<EdgeT, Directedness> &g,
        const CsrPageRankOptions &options = {}
    );
    
    namespace experimental {
        // Block-wise relaxation of edges using SIMD
        template<typename EdgeT, DirectednessTag Directedness>
        void relax_weighted_edges_block(
            const CsrGraph<EdgeT, Directedness> &g,
            const std::size_t edge_begin,
            const std::size_t edge_end,
            const double source_distance,
            std::vector<double> &distances,
            const std::optional<std::size_t> source_compact = std::nullopt,
            std::vector<std::optional<std::size_t>> *predecessors = nullptr
        );
    }
}
```

### Example

```cpp
auto csr = litegraph::freeze_to_csr(g);

// Check if SIMD is available
if (litegraph::highway::enabled()) {
    auto pr = litegraph::highway::pagerank(csr);
    std::cout << "SIMD PageRank converged: " << pr.converged << "\n";
} else {
    auto pr = litegraph::pagerank(csr);
    std::cout << "Scalar PageRank converged: " << pr.converged << "\n";
}
```

---

## 10. Error Handling

### Exception-Based (Throwing)

Most operations throw `std::out_of_range` on invalid input:

```cpp
try {
    g.node_data(invalid_node_id);
} catch (const std::out_of_range &e) {
    std::cerr << "Node not found\n";
}
```

### Expected-Based (Non-Throwing)

Safe versions return `std::expected`:

```cpp
auto result = g.try_node_data(maybe_invalid_nid);
if (result) {
    std::cout << "Data: " << result->get() << "\n";
} else {
    std::cout << "Error: " << static_cast<int>(result.error()) << "\n";
}
```

---

## 11. Examples

### Example 1: Simple Directed Graph

```cpp
#include "containers/graph/LiteGraph.hpp"
#include <iostream>

int main() {
    litegraph::Graph<std::string, int, litegraph::Directed> g;
    
    auto n0 = g.add_node("A");
    auto n1 = g.add_node("B");
    auto n2 = g.add_node("C");
    
    g.add_edge(n0, n1, 10);
    g.add_edge(n1, n2, 20);
    g.add_edge(n2, n0, 5);
    
    std::cout << "Nodes: " << g.node_count() << "\n";
    std::cout << "Edges: " << g.edge_count() << "\n";
    
    litegraph::to_ascii(g, std::cout);
    
    return 0;
}
```

### Example 2: Shortest Path (Dijkstra)

```cpp
#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"
#include <iostream>

int main() {
    litegraph::WeightedGraph g;  // nodes are monostate, edges are double
    
    auto n0 = g.add_node();
    auto n1 = g.add_node();
    auto n2 = g.add_node();
    auto n3 = g.add_node();
    
    g.add_edge(n0, n1, 1.0);
    g.add_edge(n0, n2, 4.0);
    g.add_edge(n1, n2, 2.0);
    g.add_edge(n1, n3, 5.0);
    g.add_edge(n2, n3, 1.0);
    
    auto [dist, pred] = litegraph::dijkstra(g, n0);
    
    std::cout << "Shortest path from 0 to 3:\n";
    auto path = litegraph::reconstruct_path(n3, pred);
    for (auto nid : path) {
        std::cout << nid.value << " ";
    }
    std::cout << "\nDistance: " << dist[n3.value] << "\n";
    
    return 0;
}
```

### Example 3: Centrality Analysis

```cpp
#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"
#include <iostream>

int main() {
    litegraph::SimpleGraph g;
    
    // Create a graph
    std::vector<litegraph::NodeId> nodes;
    for (int i = 0; i < 5; ++i) {
        nodes.push_back(g.add_node());
    }
    
    // Add edges (make it somewhat connected)
    g.add_edge(nodes[0], nodes[1]);
    g.add_edge(nodes[0], nodes[2]);
    g.add_edge(nodes[1], nodes[2]);
    g.add_edge(nodes[1], nodes[3]);
    g.add_edge(nodes[2], nodes[3]);
    g.add_edge(nodes[3], nodes[4]);
    
    // Compute centrality measures
    auto degree_cent = litegraph::degree_centrality(g);
    auto between_cent = litegraph::betweenness_centrality(g);
    
    std::cout << "Degree Centrality:\n";
    for (size_t i = 0; i < nodes.size(); ++i) {
        std::cout << "  Node " << i << ": " << degree_cent[nodes[i].value] << "\n";
    }
    
    std::cout << "Betweenness Centrality:\n";
    for (size_t i = 0; i < nodes.size(); ++i) {
        std::cout << "  Node " << i << ": " << between_cent[nodes[i].value] << "\n";
    }
    
    return 0;
}
```

### Example 4: Mutable Graph with Compaction

```cpp
#include "containers/graph/LiteGraph.hpp"
#include <iostream>

int main() {
    litegraph::LabeledGraph g;  // string nodes, string edges
    
    auto n0 = g.add_node("Alice");
    auto n1 = g.add_node("Bob");
    auto n2 = g.add_node("Charlie");
    
    auto e0 = g.add_edge(n0, n1, "friend");
    auto e1 = g.add_edge(n1, n2, "colleague");
    
    std::cout << "Before removal: " << g.node_count() << " nodes, " 
              << g.edge_count() << " edges\n";
    
    g.remove_node(n1);
    
    std::cout << "After removal (logical): " << g.node_count() << " nodes, " 
              << g.edge_count() << " edges\n";
    std::cout << "(But capacity still: " << g.node_capacity() << " nodes)\n";
    
    auto [node_map, edge_map] = g.compact();
    
    std::cout << "After compaction: " << g.node_count() << " nodes, " 
              << g.edge_count() << " edges\n";
    
    // Old n0 maps to new index 0
    if (node_map[n0.value]) {
        std::cout << "Old node 0 is now at index " << *node_map[n0.value] << "\n";
    }
    
    return 0;
}
```

### Example 5: CSR and PageRank

```cpp
#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"
#include <iostream>

int main() {
    litegraph::WeightedGraph g;
    
    // Build a small graph
    auto n0 = g.add_node();
    auto n1 = g.add_node();
    auto n2 = g.add_node();
    auto n3 = g.add_node();
    
    g.add_edge(n0, n1, 1.0);
    g.add_edge(n0, n2, 1.0);
    g.add_edge(n1, n2, 1.0);
    g.add_edge(n2, n3, 1.0);
    g.add_edge(n3, n1, 1.0);
    
    // Convert to CSR for efficient PageRank
    auto csr = litegraph::freeze_to_csr(g);
    
    litegraph::CsrPageRankOptions options{
        .damping_factor = 0.85,
        .max_iterations = 100,
        .tolerance = 1e-9
    };
    
    auto result = litegraph::pagerank(csr, options);
    
    std::cout << "PageRank (converged: " << result.converged 
              << ", iterations: " << result.iterations << "):\n";
    for (size_t i = 0; i < csr.node_count(); ++i) {
        auto orig_nid = csr.original_node_id(i);
        std::cout << "  Node " << orig_nid.value << ": " << result.ranks[i] << "\n";
    }
    
    return 0;
}
```

---

## 10. Multi-Core Task Parallelism (Pravaha Add-on)

When including `LiteGraphPravaha.hpp`, callers can execute heavy graph workloads across multi-core CPUs via
`pravaha::Runner` / `JThreadBackend`:

```cpp
#include <containers/graph/LiteGraph.hpp>
#include <containers/graph/LiteGraphPravaha.hpp>

litegraph::Graph<std::string, double, litegraph::Directed> g;
// ... populate graph ...

// 1. Parallel Level-Synchronous BFS
litegraph::pravaha::parallel_bfs(g, start_node, [](litegraph::NodeId u, const std::string& data) {
    // Thread-safe visitor execution across frontier chunks
});

// 2. Parallel CSR PageRank
auto csr = litegraph::freeze_to_csr(g);
auto pr_res = litegraph::pravaha::parallel_pagerank(csr, {.damping_factor = 0.85});

// 3. Parallel Multi-Source Dijkstra
std::vector<litegraph::NodeId> sources = {n0, n1, n2, n3};
auto all_paths = litegraph::pravaha::parallel_multi_source_dijkstra(g, sources);

// 4. Parallel Betweenness Centrality (Parallel Brandes Algorithm)
auto centrality = litegraph::pravaha::parallel_betweenness_centrality(g);
```

---

## Appendix: Quick Reference

| Feature                     | Function                                   | Signature                                                                         |
|-----------------------------|--------------------------------------------|-----------------------------------------------------------------------------------|
| Add node                    | `add_node`                                 | `NodeId add_node(const NodeT &data)`                                              |
| Add edge                    | `add_edge`                                 | `EdgeId add_edge(NodeId from, NodeId to, const EdgeT &data)`                      |
| Remove node                 | `remove_node`                              | `void remove_node(NodeId nid)`                                                    |
| Remove edge                 | `remove_edge`                              | `void remove_edge(EdgeId eid)`                                                    |
| Node data                   | `node_data`                                | `NodeT &node_data(NodeId nid)`                                                    |
| Edge data                   | `edge_data`                                | `EdgeT &edge_data(EdgeId eid)`                                                    |
| Valid node                  | `valid_node`                               | `bool valid_node(NodeId nid) const`                                               |
| Out edges                   | `out_edges`                                | `auto out_edges(NodeId nid) const`                                                |
| In edges                    | `in_edges`                                 | `auto in_edges(NodeId nid) const` (directed)                                      |
| Neighbors                   | `neighbors`                                | `auto neighbors(NodeId nid) const`                                                |
| Degree                      | `degree`                                   | `std::size_t degree(NodeId nid) const`                                            |
| Compaction                  | `compact`                                  | `std::pair<IdMap, IdMap> compact()`                                               |
| **BFS**                     | `bfs`                                      | `void bfs(const GraphT &g, NodeId start, Fn &&visit)`                             |
| **DFS**                     | `dfs`                                      | `void dfs(const GraphT &g, NodeId start, Fn &&visit)`                             |
| **Dijkstra**                | `dijkstra`                                 | `auto dijkstra(const GraphT &g, NodeId source, weight_fn)`                        |
| **A\***                     | `a_star_search`                            | `auto a_star_search(const GraphT &g, src, dst, weight_fn, heuristic)`             |
| **Bellman-Ford**            | `bellman_ford`                             | `auto bellman_ford(const GraphT &g, source, weight_fn)`                           |
| **Floyd-Warshall**          | `floyd_warshall`                           | `auto floyd_warshall(const GraphT &g, weight_fn)`                                 |
| **Topological sort**        | `topological_sort`                         | `std::vector<NodeId> topological_sort(const GraphT &g)`                           |
| **SCC**                     | `strongly_connected_components`            | `std::vector<std::vector<NodeId>> strongly_connected_components(const GraphT &g)` |
| **Cycle detect**            | `has_cycle`                                | `bool has_cycle(const GraphT &g)`                                                 |
| **PageRank**                | `pagerank`                                 | `CsrPageRankResult pagerank(const CsrGraph<...> &g, options)`                     |
| **Parallel PageRank**       | `pravaha::parallel_pagerank`               | `CsrPageRankResult parallel_pagerank(const CsrGraph<...> &g, options)`            |
| **Parallel BFS**            | `pravaha::parallel_bfs`                    | `void parallel_bfs(const GraphT &g, NodeId start, Fn &&visit)`                    |
| **Parallel Multi Dijkstra** | `pravaha::parallel_multi_source_dijkstra`  | `vector<pair<vector<double>, vector<opt<NodeId>>>> (g, sources)`                  |
| **Parallel Betweenness**    | `pravaha::parallel_betweenness_centrality` | `vector<double> parallel_betweenness_centrality(const GraphT &g)`                 |
| **VF2**                     | `vf2_subgraph_isomorphism`                 | `auto vf2_subgraph_isomorphism(pattern, target, node_comp, edge_comp)`            |
| **Max flow**                | `edmonds_karp_max_flow`                    | `double edmonds_karp_max_flow(g, source, sink, capacity_fn)`                      |
| **Graph coloring**          | `greedy_graph_coloring`                    | `std::vector<std::optional<int>> greedy_graph_coloring(const GraphT &g)`          |
| **Bipartite matching**      | `max_bipartite_matching`                   | `std::vector<EdgeId> max_bipartite_matching(const GraphT &g)`                     |
| **Degree centrality**       | `degree_centrality`                        | `std::vector<double> degree_centrality(const GraphT &g)`                          |
| **Closeness centrality**    | `closeness_centrality`                     | `std::vector<double> closeness_centrality(const GraphT &g)`                       |
| **Betweenness centrality**  | `betweenness_centrality`                   | `std::vector<double> betweenness_centrality(const GraphT &g)`                     |
| **Kruskal MST**             | `kruskal_mst`                              | `std::vector<EdgeId> kruskal_mst(const GraphT &g, weight_fn)`                     |
| **Prim MST**                | `prim_mst`                                 | `std::vector<EdgeId> prim_mst(const GraphT &g, weight_fn, start)`                 |
| **GED**                     | `graph_edit_distance`                      | `double graph_edit_distance(g1, g2, cost_fns...)`                                 |
| **Freeze to CSR**           | `freeze_to_csr`                            | `CsrGraph<...> freeze_to_csr(const Graph<...> &g)`                                |
| **Display (DOT)**           | `to_dot`                                   | `void to_dot(const Graph<...> &g, std::ostream &os)`                              |
| **Display (ASCII)**         | `to_ascii`                                 | `void to_ascii(const Graph<...> &g, std::ostream &os, formatters...)`             |


