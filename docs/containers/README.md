# Containers — Module Index

Header-only C++23 container library (`include/containers/`). No virtual, no macros; concept-based
static polymorphism. This index catalogs every container in the module, its category, the concrete
algorithm(s) it implements, and links to the detailed per-container docs where they exist.

## Table of Contents

- [Architecture](#architecture)
- [Algorithms Used](#algorithms-used)
- [Container Catalog](#container-catalog)
    - [Graph](#graph)
    - [Trees](#trees)
    - [Associative & Handles](#associative--handles)
    - [Dynamic / Inline Storage](#dynamic--inline-storage)
    - [Lock-Free](#lock-free)
    - [Symbol / Interning](#symbol--interning)
    - [Content & Registry](#content--registry)
    - [Compile-Time](#compile-time)
- [Detailed Docs](#detailed-docs)

---

## Architecture

The module is organized by concern, not by a single class hierarchy. Every container is an independent,
header-only unit; higher-level facilities (registries, caches) compose the lower-level ones by inclusion.

```
Compile-time            ct_parser (ct_trie)                         — pure constexpr
      │
Inline / small storage  static_vector · SmallVector (SBO)           — no heap in the common case
      │
Associative & handles   generational_handle · slot_map · SparseSet  — stable IDs, O(1) access
      │  (composed by)
Registries / stores     descriptor_registry · content_store · Kosha cache · InternPool · SymbolTable
      │
Graph / tree substrate  LiteGraph · NAryTree · AABBTree · DisjointSet · union_find · DominatorTree · egraph
      │
Concurrency             RingBuffer · MPMCQueue · MPSCQueue · AtomicStack · HazardRegistry
      │
Serialization           canonical_codec · conversion_graph
```

Dependency direction is downward: e.g. `descriptor_registry` and `slot_map` use `generational_handle`;
`DominatorTree` uses `LiteGraphAlgorithms`; `egraph` uses union-find + the Kosha hash-cons cache.

---

## Algorithms Used

Concrete named algorithms per container, with the header they live in.

| Container           | Algorithm                                                                                                                                                                                          | Where                            |
|---------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------|
| LiteGraph           | 30+ graph algorithms: BFS, DFS, Dijkstra, Bellman-Ford, A*, Floyd-Warshall, Tarjan SCC, VF2 subgraph iso, Edmonds-Karp max-flow, Brandes betweenness, Kruskal, Prim, PageRank, graph-edit-distance | `graph/LiteGraphAlgorithms.hpp`  |
| LiteGraphHighway    | SIMD (Google Highway) batch node/edge sweeps                                                                                                                                                       | `graph/LiteGraphHighway.hpp`     |
| DominatorTree       | Lengauer-Tarjan dominator tree + Cooper-Harvey-Kennedy iterative immediate-dominator solver                                                                                                        | `graph/DominatorTree.hpp`        |
| DisjointSet         | Union-by-rank + full path compression (halving variant) — **canonical disjoint-set interface**                                                                                                     | `graph/DisjointSet.hpp`          |
| union_find          | ⚠️ **Deprecated** — use `DisjointSet`. Disjoint-set forest: union-by-rank + path-splitting (Tarjan / van Leeuwen). Kept for egraph internal use only.                                              | `union_find.hpp`                 |
| egraph              | Equality saturation: union-find (path-splitting) + Kosha hash-cons (Robin-Hood) + egg-style rebuild + saturation + best-cost extraction                                                            | `graph/egraph.hpp`               |
| NAryTree            | Owning `unique_ptr` n-ary tree + `tree_simd` Highway batch ops                                                                                                                                     | `tree/NAryTree.hpp`              |
| AABBTree            | Bounding-volume hierarchy: SAH-lite sibling-merge insertion (minimize merged surface area) + AABB refit rebalance                                                                                  | `tree/AABBTree.hpp`              |
| SparseSet           | Briggs-Torczon dual-buffer sparse set, O(1) insert/erase/contains                                                                                                                                  | `associative/SparseSet.hpp`      |
| slot_map            | Generational slot map, stable-address O(1) insert/erase/lookup keyed by `generational_handle`                                                                                                      | `associative/slot_map.hpp`       |
| generational_handle | Stale-safe phantom-typed (index, generation) pair                                                                                                                                                  | `handle/generational_handle.hpp` |
| SmallVector         | Small-buffer optimization (SBO): inline storage until capacity exceeded, then heap                                                                                                                 | `dynamic/SmallVector.hpp`        |
| static_vector       | Fixed-capacity inline vector, never allocates                                                                                                                                                      | `static/static_vector.hpp`       |
| RingBuffer          | Lock-free SPSC ring buffer                                                                                                                                                                         | `lockfree/RingBuffer.hpp`        |
| MPMCQueue           | Lock-free MPMC bounded queue (Vyukov)                                                                                                                                                              | `lockfree/MPMCQueue.hpp`         |
| MPSCQueue           | Lock-free MPSC queue (Michael-Scott)                                                                                                                                                               | `lockfree/MPSCQueue.hpp`         |
| AtomicStack         | Lock-free stack (Treiber)                                                                                                                                                                          | `lockfree/AtomicStack.hpp`       |
| HazardRegistry      | Hazard-pointer safe-memory-reclamation registry                                                                                                                                                    | `lockfree/HazardRegistry.hpp`    |
| InternPool          | String interning: `shared_mutex` (concurrent read / exclusive write) + Highway batch intern                                                                                                        | `symbol/InternPool.hpp`          |
| SymbolTable         | `unordered_map` symbol store + `NamespaceIndex` trie; `shared_mutex` read/write                                                                                                                    | `symbol/SymbolTable.hpp`         |
| Kosha               | Cache: Robin-Hood open-addressing flat storage + compile-time `ThreadSafeCache` `shared_mutex` wrapper                                                                                             | `cache/kosha.hpp`                |
| descriptor_registry | Stable generational handles via slot_map; FNV-1a name hashing                                                                                                                                      | `descriptor_registry.hpp`        |
| content_store       | Content-addressed blob store, 32-byte SHA-256 digest keys; sharded atomic-rename filesystem backend + Setu zero-copy get                                                                           | `content_store.hpp`              |
| conversion_graph    | Weighted least-cost directed path via self-contained Dijkstra (non-negative uint32 costs, binary heap)                                                                                             | `conversion_graph.hpp`           |
| canonical_codec     | Deterministic serialization: lexicographically-sorted string table + finalize                                                                                                                      | `canonical_codec.hpp`            |
| ct_parser           | `ct_trie` — O(depth) compile-time keyword lookup (each trie node is a template parameter)                                                                                                          | `ct_parser.hpp`                  |

---

## Container Catalog

### Graph

- **LiteGraph** — flat SoA graph + 30+ algorithm library (+ Highway SIMD). See [LiteGraph.md](LiteGraph.md).
- **DominatorTree** — Lengauer-Tarjan + iterative idom over a `LiteGraphModel`.
- **DisjointSet** — canonical disjoint-set forest (union-by-rank + path-compression halving). Prefer this for all new code.
- **union_find** — ⚠️ **Deprecated**: prefer `DisjointSet`. Uses union-by-rank + path-splitting. Currently used internally by `egraph`; do not use in new code.
- **egraph** — generic equality-saturation engine. See [egraph.md](egraph.md).

### Trees

- **NAryTree** — owning n-ary tree with Highway batch ops. See [NAryTree.md](NAryTree.md).
- **AABBTree** — SAH-lite bounding-volume hierarchy with refit rebalance.

### Associative & Handles

- **SparseSet** — Briggs-Torczon dual-buffer sparse set. See [SparseSet.md](SparseSet.md).
- **slot_map** — generational slot map with stable addresses.
- **generational_handle** — stale-safe phantom-typed handle (foundation for slot_map / registries).

### Spatial & Dynamic Storage

- **SpatialHashGrid** — $O(N)$ zero-allocation broadphase grid with SplitMix64 coordinate hashing and Morton Z-order cache locality.
- **SoAVector** — policy-driven Structure-of-Arrays vector (`StaticStoragePolicy`, `SmallVectorStoragePolicy`, `DynamicStoragePolicy`) with SIMD unrolled vectorization.
- **BarnesHutTree** — $O(N \log N)$ hierarchical multipole gravity tree with unrolled fast reciprocal square root evaluation.

### Dynamic / Inline Storage

- **SmallVector** — SBO dynamic array. See [SmallVector.md](SmallVector.md).
- **static_vector** — fixed-capacity, never-allocating inline vector.

### Lock-Free

- **RingBuffer** (SPSC), **MPMCQueue** (Vyukov), **MPSCQueue** (Michael-Scott), **AtomicStack** (Treiber), **HazardRegistry** (hazard pointers). See [lockfree_containers.md](lockfree_containers.md).

### Symbol / Interning

- **InternPool** — concurrent string interning (shared_mutex + Highway batch).
  See [symbol/InternPool.md](symbol/InternPool.md).
- **SymbolTable** — symbol store + namespace trie (shared_mutex). See [symbol/SymbolTable.md](symbol/SymbolTable.md).

### Content & Registry

- **descriptor_registry** — generational-handle registry keyed by FNV-1a name hash.
- **content_store** — SHA-256 content-addressed blob store (filesystem + Setu backends).
- **Kosha** — Robin-Hood open-addressing cache with ARC, LFU, LRU, FIFO, TTL, and cluster skeleton. See [kosha.md](kosha.md).
- **canonical_codec** — deterministic serialization with sorted string table.
- **conversion_graph** — Dijkstra least-cost conversion-path finder.

### Compile-Time

- **ct_parser** — `ct_trie` constexpr keyword dispatch.

### Reactive

- **Signal** / **Computed** / **Callback** — observable value cell, lazily-memoized derived value, and SBO move-only `void()` callback. See [reactive.md](reactive.md).

---

## Detailed Docs

| Container / Subsystem | Dedicated Documentation Guide |
|:---|:---|
| **Spatial Acceleration Structures** | [spatial.md](spatial.md) (`BarnesHutTree`, `SpatialHashGrid`, `AABBTree`) |
| **Structure-of-Arrays & Static Vector** | [soa_vector.md](soa_vector.md) (`SoAVector` SIMD Kinematics, `static_vector`) |
| **Handles, SlotMaps & Registries** | [handle_and_registry.md](handle_and_registry.md) (`generational_handle`, `slot_map`, `descriptor_registry`, `content_store`) |
| **High-Performance Cache** | [kosha.md](kosha.md) (`kosha::core`, LRU/LFU/FIFO/ARC, `FlatHashStorage`, Sharding, TTL) |
| **Multidimensional Tensor Engine** | [tensor.md](tensor.md) (`ts::tensor`, `ts::edsl`, MLX GPU backend, SmallTensor) |
| **Versioned State & MVCC Substrate** | [anukrama.md](anukrama.md) (Immutable version chains, Snapshot isolation, `atomic_clock`) |
| **B+ Tree Block Storage** | [bplus_tree.md](bplus_tree.md) (SoA node layouts, Highway SIMD search, Intrusive freelist recycling) |
| **Lock-Free Concurrency Primitives** | [lockfree_containers.md](lockfree_containers.md) (Vyukov `MPMCQueue`, `RingBuffer`, `HazardRegistry`) |
| **Math Vectors & Graphics Primitives**| [math_vector.md](math_vector.md) (`vec2/3/4`, `mat2/3/4`, `quat`, Look-At view & Perspective projections) |
| **Small-Buffer Dynamic Array** | [SmallVector.md](SmallVector.md) (Inline SBO byte budgeting, allocator traits) |
| **Sparse Set Indexing** | [SparseSet.md](SparseSet.md) (Briggs-Torczon dual-buffer sparse set) |
| **LiteGraph Network Analysis** | [LiteGraph.md](LiteGraph.md) · [tutorial](../tutorials/LiteGraph.md) (30+ Graph algorithms, SIMD sweeps) |
| **E-Graph Equality Saturation** | [egraph.md](egraph.md) (Congruence closure, AST rewriting) |
| **N-Ary Tree Hierarchy** | [NAryTree.md](NAryTree.md) (First-child next-sibling pointer trees) |
| **String Interning Pool** | [symbol/InternPool.md](symbol/InternPool.md) (Concurrent atomic string interning) |
| **Symbol Table & Namespace Trie** | [symbol/SymbolTable.md](symbol/SymbolTable.md) (Scoped symbol resolution) |
| **Reactive Value Primitives** | [reactive.md](reactive.md) (`Signal`, `Computed`, `Callback` — observable cells + memoized derivations) |

