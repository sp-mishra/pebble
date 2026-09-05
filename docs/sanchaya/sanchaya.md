# Sanchaya: Intelligent Persistent Objects and Data Federation for C++23+

**Sanchaya** (`include/sanchaya/`) is Pebble's non-intrusive, object-first persistence, query compilation, and data federation framework engineered for C++23/C++26. It bridges rich C++ in-memory domain models with diverse persistence tiers (in-memory sessions, Petika key-value engines, SQLite transactional stores, and DuckDB columnar analytics engines) with **zero virtual dispatch, zero RTTI, zero macro magic, and zero heap allocation in critical paths**.

---

## 1. Architectural Overview & System Decoupling

Sanchaya enforces **one unified semantic query model** and **one logical IR**. The backends diverge only after placement and federation, during physical planning:

```
                            SANCHAYA SUBSYSTEM ARCHITECTURE

  ┌────────────────────────────────────────────────────────────────────────────────────────┐
  │                            DOMAIN ENTITY LAYER (Plain C++ Structs)                     │
  │  struct Employee { EmployeeId id; std::string name; std::uint32_t age; ... };          │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                   COMPILED PERSISTENCE MODEL (Out-of-Band Descriptors)                 │
  │  describe_row<T>(field<"id", &T::id>(), embedded<"addr">(), relation<"dept">()...)    │
  │  Graph Validation: Tarjan SCC Cycle Detector over LiteGraph                            │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                         QUERY EDSL & LOGICAL RELATIONAL IR                             │
  │  from<T, "alias">().where().through().select().order_by().limit().offset()              │
  │  Two-Stage Equality Saturation (Vākya E-Graph + Sanchaya Logical E-Graph)               │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                       PLACEMENT, FEDERATION & CASCADES MEMO                            │
  │  Multidimensional Cost Model: Latency, Memory, Network, Coordination Risk              │
  │  Exchange Operators: broadcast, repartition_by_key, batch_key_request, row_stream       │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                           PHYSICAL EXECUTION OPERATORS                                 │
  │  - In-Memory Physical IR: memory_sequence_scan, memory_filter_project_fused, SIMD       │
  │  - Relational Physical IR: rel_table_scan, rel_index_range_scan, rel_hash_join (SQL)   │
  │  - Petika Physical IR: petika_point_get, petika_ordered_range_scan, snapshot_scan     │
  └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Core Use Cases

### 2.1 Pure Domain Objects
Domain models remain completely pure C++ with zero framework contamination:
```cpp
struct DepartmentId { std::uint64_t value; };
struct EmployeeId   { std::uint64_t value; };

struct Department {
    DepartmentId id;
    std::string name;
    bool active{true};
};

struct Employee {
    EmployeeId id;
    std::string name;
    std::uint32_t age{0};
    DepartmentId department_id;
};
```

### 2.2 Local-First Edge & Desktop Workloads
Mutations commit instantaneously to the in-memory session and local Petika/SQLite store with write-behind replication and transactional outbox tracking.

### 2.3 Autonomous Hybrid Transactional & Analytical Processing (HTAP)
Analytical queries automatically lower to DuckDB columnar projections when query cardinality and aggregate complexity exceed cost thresholds, with time-bounded CDC synchronization.

---

## 3. Relational & Optimization Algorithms

### 3.1 Relational Graph Cycle Detection (`validation::validate_model`)
- **Substrate**: Pebble's `containers::LiteGraph` and `containers::strongly_connected_components` (Tarjan SCC).
- **Algorithm**:
  1. Build directed graph $G = (V, E)$ where vertices $V$ represent entities and edges $E$ represent declared relationships (`Embedded` vs `Reference`).
  2. Extract embedded subgraph $G_{\text{embed}} = (V, E_{\text{embed}})$.
  3. Compute Strongly Connected Components via Tarjan SCC.
  4. Any component with $|V_{\text{SCC}}| > 1$ or a self-referencing embedded edge is diagnosed as an invalid infinite-size embedded cycle at compile/validation time.
  5. Reference edges in cycles are permitted and tagged for deferred foreign key resolution.

### 3.2 Two-Stage Equality Saturation Optimization
1. **Stage 1 (Vākya E-Graph)**: Explores scalar algebraic equivalence, constant folding, and identity elimination over Vākya expression DAGs.
2. **Stage 2 (Sanchaya Logical E-Graph)**: Explores relational algebra equivalences (join commutativity/associativity, predicate pushdown through projections/traversals, projection pruning, limit pushdown) using Pebble's `containers/graph/egraph.hpp`.
3. **Extraction**: Bottom-up dynamic-programming extraction (`egraph::extract_best`) selects the Pareto-optimal logical subplans.

### 3.3 Multidimensional Placement Cost Model (`multidimensional_cost_model`)
Normalized scoring across latency, peak memory, and network transfer:
$$\text{Score} = 0.4 \times \frac{\text{Latency}}{\text{Latency}_{\text{ref}}} + 0.3 \times \frac{\text{Memory}}{\text{Memory}_{\text{budget}}} + 0.2 \times \frac{\text{Network}}{\text{Network}_{\text{budget}}} + 0.1 \times \text{CoordinationRisk}$$

### 3.4 Autonomous Storage Tiering Promotion Formula
Promotes operational tables from SQLite/Petika to DuckDB analytical columnar replicas when:
$$\text{ExpectedPromotionBenefit} = (N_{\text{predicted}} \times \Delta\text{Latency}_{\text{avg}}) + \text{RemoteSaving} - \text{BuildCost} - \text{MaintCost} - \text{StorageCost} - \text{SchemaMaintCost} - \text{FreshnessPenalty}$$
Promotion executes when $\text{ExpectedPromotionBenefit} > \text{Threshold} \land \text{Confidence} \ge \text{MinConfidence} \land \text{PaybackPeriod} \le \text{MaxPayback}$.

### 3.5 7-Step Snapshot + CDC Synchronization Protocol
1. Record source checkpoint sequence $S_0$ and timestamp $t_0$.
2. Buffer incoming mutations $> S_0$ in memory.
3. Export snapshot at $S_0$ and bulk-load into DuckDB analytical replica.
4. Replay buffered mutations from $S_0 + 1$ to $S_1$ onto the replica.
5. Drain buffer below latency threshold.
6. Publish checkpoint $\{S_1, t_1, \text{applied\_at}\}$.
7. Mark analytical replica active for queries meeting bounded staleness $\Delta t \le \text{AllowedStaleness}$.

---

## 4. Deep Pebble Library Reuse

| Subsystem / Library | Location | Sanchaya Usage |
| :--- | :--- | :--- |
| **Meta & AST** | `include/meta/meta.hpp` | `member_name`, `member_pointer_traits`, member object/function introspection, multi-step `member_path` traversal, zero-macro compile-time reflection. |
| **Vākya** | `include/vakya/vakya.hpp` | Expression template lifting (`vakya::as_expr`) and type-safe operator composition for query predicates. |
| **Akshara** | `include/meta/akshara.hpp` | Compile-time fixed strings (`akshara::fixed_string`), `make_fixed_string`, identifier tags, and NTTP literal helpers. |
| **E-Graph** | `include/containers/graph/egraph.hpp` | Hashcons deduplication, union-find with path splitting, deferred rebuilding, and bottom-up DP extraction. |
| **LiteGraph & TarjanSCC** | `include/containers/graph/LiteGraph.hpp` | Relational dependency modeling and cycle validation. |
| **SmallVector** | `include/containers/dynamic/SmallVector.hpp` | Zero-heap SBO key sets, projection index lists, and graph edge buffers. |
| **Kosha** | `include/containers/cache/kosha.hpp` | Session caching, query plan memoization, and cost statistics store. |
| **Petika** | `include/petika/petika.hpp` | Embedded high-speed key-value persistence tier (`petika::SkipStore`, `MvccJournaledSkipEngine`, `BTreeEngine`). |
| **Anukrama** | `include/containers/anukrama/anukrama.hpp` | Lock-free / wait-free in-memory MVCC substrate with point-in-time snapshot isolation (`anukrama::store`). |
| **Medha** | `include/medha/medha.hpp` | Multi-store transaction coordination, OCC read/write set staging, conflict validation, and dual-store commit (Anukrama in-memory MVCC + Petika durable storage). |
| **Glaze (BEVE)** | `dependencies/glaze` | High-performance binary encoding (`glz::BEVE`) for type-safe Petika object codec serialization with arithmetic fast paths. |
| **SQLite & DuckDB** | `dependencies/sqlite`, `dependencies/libduckdb` | Transactional OLTP (with RAII `sqlite_statement` prepared bindings) and vectorized columnar analytical engines. |

> [!NOTE]
> Cost-based multi-engine placement currently utilizes cardinality-driven heuristics and balanced composite scores, with advanced hardware-calibrated cache-miss and network metrics under active development.


---

## 5. End-to-End Code Examples

### 5.1 Compile-Time Model Declaration & Progressive Disclosure

Sanchaya embraces **Progressive Disclosure**: the compiler and `meta` reflection infer field names, value types, endpoints, and default stable IDs, requiring declarations only for semantics or exceptions.

#### Level 1: Fully Inferred & Conventional
```cpp
constexpr auto company_model =
    model<"company">(
        entity<Employee>(),
        entity<Department>(),
        many_to_one<"employment", &Employee::department_id, &Department::id>()
    );
```

#### Level 2: Explicit Key Declaration (Recommended Standard)
```cpp
constexpr auto company_model =
    model<"company">(
        entity<Employee>(
            key<&Employee::id>()
        ),
        entity<Department>(
            key<&Department::id>()
        ),
        many_to_one<
            "employment",
            &Employee::department_id,
            &Department::id
        >()
    );
```

#### Level 3: Selective Field & Behavior Customization
```cpp
constexpr auto company_model =
    model<"company">(
        entity<Employee>(
            key<&Employee::id>(),
            field<&Employee::name>(stable_id<"emp.display_name">()),
            ignore<&Employee::transient_cache>(),
            embedded<&Employee::address>()
        ),
        entity<Department>(
            key<&Department::id>()
        ),
        many_to_one<
            "employment",
            &Employee::department_id,
            &Department::id
        >(
            on_delete<restrict_delete>()
        )
    );
```

#### Level 4: Full Explicit Control (Escape Hatch for Legacy Schemas)
```cpp
constexpr auto company_model =
    model<"company">()
        .entity(
            describe_row<Employee>(
                field<"id", &Employee::id>(stable_id<"emp.id">()),
                field<"name", &Employee::name>(stable_id<"emp.name">()),
                field<"age", &Employee::age>(stable_id<"emp.age">()),
                field<"department_id", &Employee::department_id>(stable_id<"emp.dept_id">())
            )
        )
        .entity(
            describe_row<Department>(
                field<"id", &Department::id>(stable_id<"dept.id">()),
                field<"name", &Department::name>(stable_id<"dept.name">())
            )
        )
        .relation<"employment">(
            many<Employee>(),
            one<Department>(),
            foreign_key<&Employee::department_id>(),
            principal_key<&Department::id>(),
            reference(),
            on_delete<restrict_delete>()
        )
        .build();
```

### 5.2 Workspace Creation, Slot Map Storage & Entity Lifecycle

```
                     WORKSPACE & SLOT MAP MEMORY LAYOUT

    workspace<Model, ...>
    ┌────────────────────────────────────────────────────────┐
    │  - Model, Planner, Placement, CostModel, Telemetry     │
    │  - epoch_: std::uint64_t = 1                           │
    │  - stores_: std::unique_ptr<entity_stores_t<Model>>    │──────┐ (Pointer stability across moves)
    └────────────────────────────────────────────────────────┘      │
                                                                    ▼
        std::tuple<containers::slot_map<E0, ...>, containers::slot_map<E1, ...>, ...>
        ┌───────────────────────────────────────────────────────────────────────────┐
        │ SmallVector-backed (4 inline slots, zero heap allocation for small sizes) │
        │ Generational slot indexing: handle carries (store*, key, epoch)           │
        └───────────────────────────────────────────────────────────────────────────┘
```

```cpp
// Instantiate zero-overhead policy-composed workspace
auto ws = make_workspace(company_model)
    .local_auto("./company_storage")
    .build();

// Put entity with generational session handle (backed by SmallVector slot_map)
Employee emp{.id = EmployeeId{101}, .name = "Grace Hopper", .age = 45, .department_id = DepartmentId{1}};
auto handle_res = ws.put(emp);

if (handle_res) {
    auto handle = *handle_res;
    // Single-pass generation and epoch validation with zero redundant lookups
    if (auto current = handle.get(ws.session_epoch())) {
        std::cout << "Stored: " << current->get().name << " (Age: " << current->get().age << ")\n";
    }
}
```

### 5.3 Type-Safe Query Construction and Execution
```cpp
// Construct expressive query with Vākya expressions
auto query =
    from<Employee>()
        .where(member<&Employee::age>() >= 30)
        .group_by(member<&Employee::department_id>())
        .select(
            group_key(),
            count(),
            average(member<&Employee::age>())
        )
        .order_by(member<&Employee::age>(), sort_direction::descending)
        .limit(20)
        .offset(0);

// Optimize via RBO/CBO pipeline and execute
auto cursor_res = ws.execute(query);
```

### 5.4 Direct Logical and Physical IR Optimization
```cpp
using namespace sanchaya::optimizer;

// 1. Logical Plan
logical_source_node<Employee> scan{};
auto pred = member<&Employee::age>() >= 18;
logical_filter_node filter{scan, pred};
auto proj_exprs = std::make_tuple(member<&Employee::id>(), member<&Employee::name>());
logical_project_node proj{filter, proj_exprs};

// 2. Adaptive Tiered Optimization (RBO Rewriting + CBO Lowering)
adaptive_tiered_planner<> planner{};
auto physical_plan = planner.optimize(proj);
static_assert(physical_plan<decltype(physical_plan)>);
```

### 5.5 Explainability, Observability & Advanced E-Graph Equality Saturation
```cpp
auto explanation = ws.explain(query);
```
Output exposes comprehensive visual execution plan trees, candidate placement matrices, cardinality/selectivity diagnostics, and E-Graph rewrite audit logs:
```
Logical Optimization:
  - Combined 2 filters into single conjunction
  - Advanced E-Graph Rules: Join Commutativity, Join Associativity, Filter-to-IndexSeek, Projection Collapse
  - E-Graph: 42 e-nodes, 18 live e-classes (Fixpoint reached in 2 iterations)

[5.1 Selected In-Memory Physical Execution Plan]:
Top-N Heap [salary DESC, LIMIT 3] (Pipeline Breaker)
  └── Fused Filter-Project (Streamable)
        Predicate: age >= 30 AND salary >= 130000
        Output: name, age, salary
        └── SequenceScan [Employee] (Streamable)

[5.2 Multi-Engine Candidate Cost & Placement Tradeoff Matrix]:
Engine       | Est. Latency   | Peak Memory   | I/O Cost   | Confidence   | Selected?
-------------+----------------+---------------+------------+--------------+-----------
InMemory     | 0.045 ms       | 128 KB        | 0          | 0.95         | ★ SELECTED
Petika       | 0.120 ms       | 256 KB        | 0.5        | 0.90         |   candidate
SQLite       | 0.450 ms       | 512 KB        | 2.1        | 0.88         |   candidate
DuckDB       | 0.280 ms       | 1024 KB       | 1.2        | 0.92         |   candidate

[5.3 Cardinality & Predicate Selectivity Diagnostics]:
Relational Operator Node                               | In Rows   | Selectivity | Out Rows
-------------------------------------------------------+-----------+-------------+----------
logical_source_node<Employee>                          | 1000      | 100.0%      | 1000
logical_filter_node (age >= 30 && salary >= 130000)    | 1000      | 3.0%        | 30
logical_project_node (name, age, salary)               | 30        | 100.0%      | 30
logical_order_node (salary DESC)                       | 30        | 100.0%      | 30
logical_limit_node (LIMIT 3)                           | 30        | 10.0%       | 3
```


### 5.6 4-Engine Live Execution & Conformance Parity
The exact same canonical query is executed live across all 4 underlying physical execution engines:
1. **In-Memory Engine**: `sequence_scan -> fused_filter_project -> top_n`
2. **SQLite Engine**: `index_range_scan(employee_age) -> residual_filter(salary) -> projection -> top_n`
3. **Petika Engine**: `snapshot_scan -> fused_filter_project -> top_n`
4. **DuckDB Engine**: `columnar_scan(name, age, salary) -> vectorized_filter -> top_n`

### 5.7 Running the Standalone Example
A complete end-to-end demo is available in `src/examples/example_sanchaya.hpp` and can be executed via the Pebble CLI:
```bash
./pebble sanchaya
```


