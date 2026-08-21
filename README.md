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
  * **`petika::MvccJournaledSkipEngine`**: Default Anukrama-backed MVCC SkipList engine with stable LSN snapshots and Nitya log replay. `JournaledSkipEngine` remains available through explicit `SingleVersion*` aliases.
  * **`kosha::adapter::PetikaAdapter`**: High-performance Kosha cache storage adapter.
  * Documentation: [`docs/petika/petika.md`](docs/petika/petika.md)
* **Nitya** (`include/nitya/`) — Generic Durable Log Engine (DLE). Byte-offset LSN, Reserve $\to$ Publish $\to$ Sync pipeline, Setu memory mapping, streaming recovery, replication streams, and EasyRules retention/archival.
  * Documentation: [`docs/containers/nitya.md`](docs/containers/nitya.md)
* **Anukrama** (`include/containers/anukrama/`) — Generic static-composition versioned state: immutable MVCC chains, stable snapshots, optimistic validation, and explicit reclamation. Petika can bind durable Nitya LSNs as its commit clock without making Nitya mandatory.
  * Documentation: [`docs/containers/anukrama.md`](docs/containers/anukrama.md)

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
  * Tutorial: [`docs/tutorials/LiteGraph.md`](docs/tutorials/LiteGraph.md)
* **DominatorTree** (`include/containers/graph/DominatorTree.hpp`) — Lengauer-Tarjan and Cooper-Harvey-Kennedy immediate dominator solvers over `LiteGraphModel`.
* **egraph** (`include/containers/graph/egraph.hpp`) — Equality saturation engine: union-find (path-splitting) + Kosha hash-cons + egg-style rebuild + saturation and best-cost extraction.
  * Documentation: [`docs/containers/egraph.md`](docs/containers/egraph.md)
* **NAryTree** (`include/containers/tree/NAryTree.hpp`) — Owning n-ary tree with SIMD batch traversals.
  * Documentation: [`docs/containers/NAryTree.md`](docs/containers/NAryTree.md)
* **AABBTree** (`include/containers/tree/AABBTree.hpp`) — Bounding-volume hierarchy with SAH-lite sibling-merge insertion and refit rebalancing.
* **BPlusTree** (`include/containers/tree/bplus_tree.hpp`) — High-performance, policy-based cache-aligned B+ tree (`BPlusMap`, `BPlusSet`) with Highway SIMD search, Smriti arena compatibility, and $O(\log_B N + K)$ range scanning.
  * Documentation: [`docs/containers/bplus_tree.md`](docs/containers/bplus_tree.md)
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
* **Tensor & Tensor EDSL** (`include/containers/tensor/tensor.hpp`, `include/containers/tensor/tensor_edsl.hpp`) — High-performance policy-based multidimensional tensor engine with lazy expression templates (*deducing this*), C++23 multidimensional indexing, Google Highway SIMD, Apple Silicon MLX GPU acceleration, and Sūtra/Vākya-inspired symbolic EDSL with `_p`/`_t` parameter literals.
  * Documentation: [`docs/containers/tensor.md`](docs/containers/tensor.md)
  * Comprehensive Zero-to-Hero Tutorial: [`docs/tutorials/tensor.md`](docs/tutorials/tensor.md)
* **Math Vectors & Game Graphics Primitives** (`include/containers/numeric/math_vector.hpp`) — Stack-allocated, zero-heap, `constexpr`-enabled linear algebra primitives (`vec2`, `vec3`, `vec4`, `quat`, `mat4`), ray optics (`reflect`, `refract`), camera view (`look_at`), projection (`perspective`), and quaternion slerp built on `static_tensor`.
  * Documentation: [`docs/containers/math_vector.md`](docs/containers/math_vector.md)
* **Akruti** (`include/akruti/`) — Header-only, concept-based 2D shape, geometry, narrowphase, CCD, CSG, and fracture system.
  * `Shape` concept, analytic primitives (`Circle`, `Box`, `Segment`, `Capsule`, `HalfPlane`, `ConvexPoly`), Andrew's monotone chain convex hull (`akruti/hull.hpp`).
  * Queries (`raycast`, `closest_point`, `point_inside`, `winding_number`).
  * GJK boolean intersection, EPA penetration depth/normal, and separation distance (`akruti/gjk.hpp`).
  * Continuous collision detection (conservative advancement TOI and speculative anti-tunneling bounds in `akruti/ccd.hpp`).
  * Constructive Solid Geometry (`akruti/csg.hpp`) with `Union`, `Subtract`, `Intersect`, `SmoothUnion`, `Offset`, `Transform`.
  * Advanced fracture pipeline (**Khanda** in `akruti/khanda.hpp`) with Voronoi partitioning, ear-clipping triangulation, convex decomposition, Poisson-disk sampling with impact densification, and exact polar moment of inertia.
  * Bulk scene orchestrator (`akruti/scene/`) backed by SoA batches, dynamic `AABBTree` BVH, and optional `pravaha` task graph execution.
  * Documentation: [`docs/akruti/akruti.md`](docs/akruti/akruti.md)
* **Prakriti** (`include/prakriti/`) — Unified material-state 2D continuum simulator and physics engine.
   * Hybrid particle-field Lagrangian continuum dynamics combining Extended Position-Based Dynamics (XPBD) mechanics, Position-Based Fluids (PBF), graph-Laplacian explicit heat diffusion with latent-heat phase transitions, strain-driven plasticity and fracture with `containers::union_find` island tracking, and `akruti` SDF obstacle contact and XPBD kinematic joints.
   * Features 3 interchangeable `ComputeBackend` execution tiers: `ScalarBackend` (zero-dep reference), `HighwayBackend` (Google Highway SIMD), and `PravahaBackend` (multi-core task graph chunking).
   * Documentation: [`docs/prakriti/prakriti.md`](docs/prakriti/prakriti.md)
* **ECS** (`include/ecs/`) — High-performance, cache-coherent C++23 Entity-Component System.
   * `Entity` generational handle with stale handle detection, `sparseset::SparseSet` dense component storage, $O(\min(A,B))$ multi-component query joins, deferred `CommandBuffer` execution, and Pravaha multi-threaded parallel views (`par_view`).
   * Documentation: [`docs/ecs/ecs.md`](docs/ecs/ecs.md)
* **Gati** (`include/gati/`) — High-performance, header-only C++23/C++26 realtime game runtime and entity orchestration engine.
   * Fixed-step deterministic clock with presentation render interpolation (`alpha`), static `SystemStack` pipeline, Catmull-Rom animation splines & state machines, lock-free `EventBus`, input mapping, and Akruti (geometry/broadphase/narrowphase) + Prakriti (physics/joints) bridges.
   * Directly uses `pebble::math` linear algebra primitives and `pravaha` parallel execution.
   * Documentation: [`docs/gati/gati.md`](docs/gati/gati.md)

---

## 4. Telemetry & Policy Control

* **NADI** (`include/observability/nadi.hpp`) — Zero-overhead compile-time distributed tracing, pulse scopes, and multi-sink profiling.
  * Documentation: [`docs/observability/nadi.md`](docs/observability/nadi.md), [`docs/observability/turbo_twig_telemetry.md`](docs/observability/turbo_twig_telemetry.md)
* **EasyRules** (`include/rules/easy_rules.hpp`) — Declarative C++23 business and policy rule engine, facts registry, and execution pipeline.
  * Documentation: [`docs/rules/easy_rules.md`](docs/rules/easy_rules.md)
* **Medha** (`include/medha/medha.hpp`) — Header-only optimistic and serializable transactions over user-defined resources, with opt-in Anukrama, Smriti, Tarka/Vākya, Pravaha, and metadata-only Lithe adapters.
  * CMake targets: `pebble::medha`, `pebble::medha_smriti`, `pebble::medha_tarka`, `pebble::medha_pravaha`, and `pebble::medha_lithe_metadata`.
  * Documentation: [`docs/medha/medha.md`](docs/medha/medha.md)

---

## 5. Metaprogramming & Type Systems

* **Meta / Concepts** (`include/meta/meta.hpp`) — Comprehensive compile-time type introspection, reflection traits, type lists, and concept constraints.
  * Documentation: [`docs/meta/meta.md`](docs/meta/meta.md)
  * Tutorial: [`docs/tutorials/meta.md`](docs/tutorials/meta.md)
* **Akshara** (`include/meta/akshara.hpp`) — Modern compile-time tokenization, AST generation, and grammar-directed parser combinator engine.
  * Documentation: [`docs/meta/akshara.md`](docs/meta/akshara.md)
* **ExactRational** (`include/meta/exact_rational.hpp`) — Arbitrary precision compile-time rational numbers.
* **Vākya** (`include/vakya/`) — Header-only structural-expression EDSL with pattern matching, properties, type reasoning, and optional Tarka-backed verification. Its core has no Lithe dependency.
  * Documentation: [`docs/vakya/vakya.md`](docs/vakya/vakya.md)
  * Comprehensive Zero-to-Hero Tutorial: [`docs/tutorials/vakya.md`](docs/tutorials/vakya.md)
* **Generic Language + Samasa** (`include/languages/generic/`, `include/languages/samasa/`) — Reusable language substrate (diagnostics, CST, IR, modules, semantics) and compile-time grammar/parser framework.
  * Documentation: [`docs/languages/generic.md`](docs/languages/generic.md), [`docs/languages/samasa/samasa.md`](docs/languages/samasa/samasa.md)
  * Comprehensive Zero-to-Hero Tutorial: [`docs/tutorials/samasa.md`](docs/tutorials/samasa.md)

---

## 6. Utilities & Algorithms

* **Tarka** (`include/tarka/`) — Zero-overhead multi-solver SMT substrate and native DPLL(T) solver.
  * **Theories Supported**: CDCL Propositional SAT, EUF congruence closure (uninterpreted functions), QF_BV bit-blasting (arithmetic/bitwise), Simplex LRA/LIA linear arithmetic, QF_AX arrays with extensionality, and Nelson-Oppen multi-theory combination.
  * **Dual Engine Architecture**: Features high-performance **Native C++23 DPLL(T)** engine alongside an **Optimized Static Z3 Bridge** (`tarka::backend::z3_backend`) for differential validation and complete fallback solving.
  * **Frontend & Formatter**: SMT-LIB2 parser, AST serializer (`smt2_printer.hpp`), and independent certificate/model validator (`model_validator.hpp`).
  * Documentation: [`docs/tarka/tarka.md`](docs/tarka/tarka.md)
  * Comprehensive Zero-to-Hero Tutorial: [`docs/tutorials/tarka.md`](docs/tutorials/tarka.md)
* **SingleFlight** (`include/utils/single_flight.hpp`) — Duplicate function execution suppressor / coalescer for concurrent workloads.
* **Profiler & Logging** (`include/utils/profiler.hpp`, `include/utils/log.hpp`) — Structured, low-latency logging and micro-benchmarking profilers.
  * Documentation: [`docs/utils/profiler.md`](docs/utils/profiler.md), [`docs/utils/log.md`](docs/utils/log.md)

---

## 7. Testing & Quality Assurance

* **Example Registry** (`include/test/example_registry.hpp`) — Self-registering runnable examples and test harness framework.
  * Documentation: [`docs/test/example_registry.md`](docs/test/example_registry.md)

### Test Organization & Layout
Tests in `src/tests/` are organized into modular, dedicated subdirectories mapping directly to subsystems:
```
src/tests/
├── containers/        # Graph (LiteGraph, DominatorTree, egraph), Tree, Cache (Kosha), Associative, Dynamic
├── mem/               # Memory allocators (Smriti arenas, pools, buddy)
├── medha/             # Transactional memory core and opt-in adapters
├── meta/              # Metaprogramming, reflection, Akshara parsing, ExactRational
├── nitya/             # Nitya Durable Log Engine, segments, framing, replication
├── petika/            # Petika storage platform, engines, transactions
├── rules/             # EasyRules business rules, facts, pipeline
├── tarka/             # Tarka SMT solver, theories, CDCL, backends (Native, Z3 differential tests)
├── observability/     # NADI tracing, pulse scopes, telemetry
├── utils/             # Setu mmap, SingleFlight, Profiler, Log, UltraCRC
└── test_harness/      # Example registry and test fixtures
```

### Test Case Format & Tagging Standards
Every test file uses the **Catch2 v3** framework and adheres to a uniform structure:
1. **Naming Convention**: `module: Scenario / Feature description`
   - *Example*: `TEST_CASE("tarka: Propositional logic equivalence", "[tarka][differential][z3]")`
   - *Example*: `TEST_CASE("LiteGraph: Dijkstra shortest path with SIMD", "[LiteGraph][algorithms][shortest_path]")`
   - *Example*: `TEST_CASE("dominates: Entry dominates all reachable nodes", "[DominatorTree][query][dominates]")`
2. **Hierarchical Tagging**:
   - `[<library>]`: Primary module tag (`[tarka]`, `[LiteGraph]`, `[nitya]`, `[meta]`, etc.).
   - `[<subsystem>]`: Specific feature or theory area (`[query]`, `[framing]`, `[bv]`, `[lra]`).
   - `[<property>]`: Test type or dependency gate (`[differential]`, `[z3]`, `[simd]`, `[lockfree]`).
3. **Invariants & Assertions**:
   - Use `STATIC_REQUIRE` for compile-time/consteval validation.
   - Use `REQUIRE` for critical preconditions and `CHECK` for individual invariants.
   - Conditional backend/dependency tests are guarded with feature-test macros (e.g. `HAS_Z3`, `__has_include(<z3++.h>)`).

---

## 📦 Dependencies & Bootstrap Script

Pebble is predominantly header-only with zero mandatory runtime dependencies. Optional acceleration and external integrations are managed cleanly via an automated dependency fetcher script:

### Dependency Matrix

| Dependency | Required By | Optional? | Purpose |
|:---|:---|:---:|:---|
| **Catch2 v3** | `pebble_tests` | Yes (Test-only) | Unit test framework and test runner. |
| **Z3 SMT Solver** | `tarka` | Yes | Static, optimized SMT solver engine backend for differential testing and complete theory fallback. |
| **Google Highway** | `LiteGraph`, `SymbolTable`, `NAryTree` | Yes | Portable SIMD intrinsics for vector-accelerated graph/tree sweeps and batch string interning. |
| **crc32c** | `nitya`, `petika`, `UltraCRC` | Yes | Hardware-accelerated (SSE4.2/ARMv8) CRC32C checksums for WAL framing and block validation. |
| **spdlog** | `pebble` / `pebble_tests` | Yes | High-performance asynchronous and structured logging. |
| **liblmdb** | `kosha` | Yes | Embedded transactional key-value backing store adapter. |
| **glaze** | `meta` / `tarka` | Yes | Zero-allocation compile-time JSON/binary serialization. |

### Using `scripts/fetch_deps.sh`

Pebble provides [`scripts/fetch_deps.sh`](scripts/fetch_deps.sh) to bootstrap all third-party dependencies from verified release archives without requiring system-level package manager modifications:

```bash
# Download all missing dependencies automatically
./scripts/fetch_deps.sh

# Download specific dependency groups
./scripts/fetch_deps.sh --group pebble   # all Pebble dependencies
./scripts/fetch_deps.sh --group smt      # Z3 and Catch2 for SMT development
./scripts/fetch_deps.sh --group core     # core systems dependencies

# Download individual libraries
./scripts/fetch_deps.sh z3 highway

# Force re-download / refresh existing archives
./scripts/fetch_deps.sh --force z3

# Clean up dependencies directory
./scripts/fetch_deps.sh --clean
```

CMake automatically detects missing dependencies during configuration and seamlessly invokes `scripts/fetch_deps.sh`.

---

## 🛠️ Build & Requirements

* **Compiler**: Modern C++23 compliant compiler (Clang 16+, GCC 13+, Apple Clang 15+).
* **Build System**: CMake 3.25+ (`CMakeLists.txt`) — builds all test files recursively (`GLOB_RECURSE`).
* **Configuration Options**:
  - `-DBUILD_TESTS=ON/OFF`: Build unit test runner (`pebble_tests`). Default `ON` for top-level build.
  - `-DBUILD_Z3=ON/OFF`: Enable static optimized Z3 backend compilation for Tarka. Default `ON`.
  - `-DDEPS_VERBOSE=ON/OFF`: Show verbose dependency fetch output during CMake configuration.

---

## 🤝 Contribution Guidelines

We welcome high-quality contributions, bug fixes, and algorithmic optimizations! Please ensure all contributions adhere to Pebble's architectural standards:

1. **Zero-Overhead Principle**: No virtual functions, no dynamic memory allocations on hot paths, and no unnecessary runtime overhead.
2. **Modern C++ Standard**: All code must conform to C++23 / C++26 standard idioms and compile cleanly without warnings on macOS (Apple Clang) and Linux (GCC/Clang).
3. **Header-Only Design**: Core subsystems must remain lightweight, header-only, and modular.
4. **Test Coverage**: Every new feature, data structure, or algorithm must include comprehensive unit tests under `src/tests/<subsystem>/` adhering to Catch2 tagging guidelines.
5. **No Circular Dependencies**: Subsystems must maintain a strict top-down dependency hierarchy (`mem/` $\to$ `containers/`, `tarka/`, `petika/`).

---

## 🤖 AI Usage Guidelines & Human Oversight

Artificial Intelligence (AI) coding tools and LLM pair programmers may be used in the development and enhancement of Pebble, subject to the following mandatory guidelines:

- **Mandatory Human Review**: All AI-generated or AI-assisted code, documentation, and tests must undergo thorough human review for architectural compliance, correctness, zero-overhead guarantees, and safety before being integrated.
- **Verification & Testing**: AI-generated contributions must compile without warnings and pass all unit tests, differential validation suites, and static analyzers.
- **Design Alignment**: AI tools must strictly adhere to Pebble's zero-virtual, header-only, and memory-policy architecture. Blind boilerplate generation is prohibited.

---

## ⚖️ License & Legal Disclaimer

### License
Pebble is distributed under the terms of the project's open-source license. See `LICENSE` for full details.

### Disclaimer
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE, AND NON-INFRINGEMENT. IN NO EVENT SHALL THE AUTHORS, CONTRIBUTORS, OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT, OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

THIRD-PARTY LIBRARIES (SUCH AS Z3, HIGHWAY, CATCH2, CRC32C, SPDLOG, GLAZE, AND LMDB) ARE THE PROPERTY OF THEIR RESPECTIVE OWNERS AND ARE SUBJECT TO THEIR OWN LICENSING TERMS. USERS AND CONTRIBUTORS ARE RESPONSIBLE FOR ENSURING COMPLIANCE WITH ALL APPLICABLE THIRD-PARTY LICENSES.
