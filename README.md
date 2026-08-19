# Pebble: High-Performance C++23 Systems & Algorithms Library

Pebble is a modern, header-only, policy-based C++23 systems library engineered for extreme performance, minimal latency, and zero runtime waste. It strictly follows the C++ zero-overhead principle: **no virtual functions, no RTTI, no macro anti-patterns, and zero heap allocation in critical paths**.

---

## 📑 Master Subsystem & Library Index

```
                                    ┌──────────────────┐
                                    │      PEBBLE      │
                                    └────────┬─────────┘
        ┌───────────────┬────────────────────┼───────────────────┬─────────────────┐
        ▼               ▼                    ▼                   ▼                 ▼
  ┌───────────┐   ┌───────────┐        ┌───────────┐       ┌───────────┐     ┌───────────┐
  │  Storage  │   │  Memory   │        │Containers │       │Telemetry &│     │Utilities &│
  │ & Engines │   │ & Mapping │        │& Analytics│       │  Policy   │     │Algorithms │
  └─────┬─────┘   └─────┬─────┘        └─────┬─────┘       └─────┬─────┘     └─────┬─────┘
        │               │                    │                   │                 │
  ├── Petika      ├── Smriti           ├── LiteGraph       ├── NADI          ├── UltraCRC
  └── Nitya       └── Setu             ├── Kosha Cache     └── EasyRules     ├── SingleFlight
                                       ├── Lock-Free                         ├── Profiler
                                       ├── Trees & Sets                      └── Logger
                                       └── Meta / AST
```

---

## 1. Storage & Engines

* **Petika** (`include/petika/`) — Unified, engine-agnostic storage framework and platform. Decouples application storage APIs from physical engine architectures.
  * **`petika::Petika`**: Storage hub supporting CRUD, transactions, snapshots, and recovery.
  * **`petika::JournaledSkipEngine`**: Production-ready SkipList engine with $O(\log n)$ point operations, $O(\log n + k)$ range scans, Smriti arena memory, and Nitya log replay.
  * **`kosha::adapter::PetikaAdapter`**: High-performance Kosha cache storage adapter.
  * Documentation: [`docs/petika/petika.md`](docs/petika/petika.md)
* **Nitya** (`include/nitya/`) — Generic Durable Log Engine (DLE). Byte-offset LSN, Reserve $\to$ Publish $\to$ Sync pipeline, Setu memory mapping, streaming recovery, replication streams, and EasyRules retention/archival.
  * Documentation: [`docs/containers/nitya.md`](docs/containers/nitya.md)

---

## 2. Memory & Mapping

* **Smriti** (`include/mem/`) — High-performance memory management and allocation primitives.
  * `LinearArena`, `ScopedArena`, `BumpPool`, `BuddyAllocator`, and thread-safe allocators.
  * Documentation: [`docs/mem/smriti.md`](docs/mem/smriti.md)
* **Setu** (`include/utils/setu.hpp`) — Persistent memory-mapped file abstraction (`setu::mapping`).
  * Type-safe bounds-checked page views, typed memory overlays, anonymous/file mappings, and explicit sync/async flushing.
  * Documentation: [`docs/utils/setu.md`](docs/utils/setu.md)

---

## 3. Containers & Data Structures

Complete module catalog & algorithm mapping available in [`docs/containers/README.md`](docs/containers/README.md).

### Graph & Tree Substrate
* **LiteGraph** (`include/containers/graph/LiteGraph.hpp`) — Flat Structure-of-Arrays (SoA) graph with 30+ graph algorithms (Dijkstra, A*, Tarjan SCC, Brandes betweenness, Kruskal, Prim, PageRank, Edmonds-Karp max-flow, VF2 subgraph isomorphism) and Google Highway SIMD sweeps.
  * Documentation: [`docs/containers/LiteGraph.md`](docs/containers/LiteGraph.md)
* **DominatorTree** (`include/containers/graph/DominatorTree.hpp`) — Lengauer-Tarjan and Cooper-Harvey-Kennedy immediate dominator solvers over `LiteGraphModel`.
* **egraph** (`include/containers/graph/egraph.hpp`) — Equality saturation engine: union-find (path-splitting) + Kosha hash-cons + egg-style rebuild + saturation and best-cost extraction.
  * Documentation: [`docs/containers/egraph.md`](docs/containers/egraph.md)
* **NAryTree** (`include/containers/tree/NAryTree.hpp`) — Owning n-ary tree with SIMD batch traversals.
  * Documentation: [`docs/containers/NAryTree.md`](docs/containers/NAryTree.md)
* **AABBTree** (`include/containers/tree/AABBTree.hpp`) — Bounding-volume hierarchy with SAH-lite sibling-merge insertion and refit rebalancing.
* **DisjointSet** & **union_find** (`include/containers/graph/DisjointSet.hpp`, `include/containers/union_find.hpp`) — Disjoint-set forests with union-by-rank, path compression, and path-splitting.

### Caching, Stores & Registries
* **Kosha** (`include/containers/cache/kosha.hpp`) — High-performance cache with Robin-Hood flat hashing, multi-policy eviction (LRU, LFU, ARC, FIFO), compile-time thread-safety grading, and external engine adapters (LMDB, RocksDB, Nitya, Petika).
* **content_store** (`include/containers/content_store.hpp`) — SHA-256 content-addressed blob store backed by Setu zero-copy access and atomic filesystem operations.
* **descriptor_registry** & **slot_map** (`include/containers/descriptor_registry.hpp`, `include/containers/associative/slot_map.hpp`) — Generational slot map and handle registry providing stable $O(1)$ memory addresses.
* **SparseSet** (`include/containers/associative/SparseSet.hpp`) — Briggs-Torczon dual-buffer sparse set for $O(1)$ lookup/insert/erase.
  * Documentation: [`docs/containers/SparseSet.md`](docs/containers/SparseSet.md)
* **InternPool** & **SymbolTable** (`include/containers/symbol/InternPool.hpp`, `include/containers/symbol/SymbolTable.hpp`) — String interning with Highway SIMD batch interning and scoped namespace trie symbol resolution.
  * Documentation: [`docs/containers/symbol/InternPool.md`](docs/containers/symbol/InternPool.md), [`docs/containers/symbol/SymbolTable.md`](docs/containers/symbol/SymbolTable.md)

### Lock-Free Concurrency
* **Lock-Free Queues & Primitives** (`include/containers/lockfree/`) — High-throughput synchronization:
  * `MPMCQueue` (Dmitry Vyukov bounded MPMC queue)
  * `MPSCQueue` (Michael-Scott non-blocking MPSC queue)
  * `RingBuffer` (Lock-free SPSC circular buffer)
  * `AtomicStack` (Treiber stack)
  * `HazardRegistry` (Hazard-pointer safe memory reclamation)
  * Documentation: [`docs/containers/lockfree_containers.md`](docs/containers/lockfree_containers.md)

### Dynamic & Fixed Storage
* **SmallVector** (`include/containers/dynamic/SmallVector.hpp`) — Small-Buffer Optimized (SBO) dynamic vector avoiding heap allocations for small sizes.
  * Documentation: [`docs/containers/SmallVector.md`](docs/containers/SmallVector.md)
* **static_vector** (`include/containers/static/static_vector.hpp`) — Fixed-capacity inline vector that never touches the heap.
* **Tensor** (`include/containers/tensor/tensor.hpp`) — Zero-overhead multidimensional strided tensor view and buffer.

---

## 4. Telemetry & Policy Control

* **NADI** (`include/observability/nadi.hpp`) — Zero-overhead compile-time distributed tracing, pulse scopes, and multi-sink profiling.
  * Documentation: [`docs/observability/nadi.md`](docs/observability/nadi.md), [`docs/observability/turbo_twig_telemetry.md`](docs/observability/turbo_twig_telemetry.md)
* **EasyRules** (`include/rules/easy_rules.hpp`) — Declarative C++23 business and policy rule engine, facts registry, and execution pipeline.
  * Documentation: [`docs/rules/easy_rules.md`](docs/rules/easy_rules.md)

---

## 5. Metaprogramming & Type Systems

* **Meta / Concepts** (`include/meta/meta.hpp`) — Comprehensive compile-time type introspection, reflection traits, type lists, and concept constraints.
  * Documentation: [`docs/meta/meta.md`](docs/meta/meta.md)
* **Akshara** (`include/meta/akshara.hpp`) — Modern compile-time tokenization, AST generation, and grammar-directed parser combinator engine.
  * Documentation: [`docs/meta/akshara.md`](docs/meta/akshara.md)
* **ExactRational** (`include/meta/exact_rational.hpp`) — Arbitrary precision compile-time rational numbers.

---

## 6. Utilities & Algorithms

* **SingleFlight** (`include/utils/single_flight.hpp`) — Duplicate function execution suppressor / coalescer for concurrent workloads.
* **Profiler & Logging** (`include/utils/profiler.hpp`, `include/utils/log.hpp`) — Structured, low-latency logging and micro-benchmarking profilers.
  * Documentation: [`docs/utils/profiler.md`](docs/utils/profiler.md), [`docs/utils/log.md`](docs/utils/log.md)

---

## 7. Testing & Quality Assurance

* **Example Registry** (`include/test/example_registry.hpp`) — Self-registering runnable examples and test harness framework.
  * Documentation: [`docs/test/example_registry.md`](docs/test/example_registry.md)

---

## 🛠️ Build & Requirements

* **Compiler**: Modern C++23 compliant compiler (Clang 16+, GCC 13+, Apple Clang 15+).
* **Build System**: CMake 3.25+ (`CMakeLists.txt`).
* **Dependencies**: Google Highway (optional for SIMD acceleration), Catch2 v3 (for unit tests).
