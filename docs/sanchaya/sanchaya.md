# Sanchaya: Intelligent Persistent Objects and Data Federation

**Sanchaya** (`include/sanchaya/`) is Pebble's non-intrusive, object-first persistence, query compilation, and data federation framework engineered for C++23/C++26. It bridges rich C++ in-memory domain models with diverse persistence tiers (in-memory sessions, Petika key-value engines, SQLite transactional stores, and DuckDB columnar analytics engines) with **zero virtual dispatch, zero RTTI, zero macro magic, and zero heap allocation in critical paths**.

---

## 1. Architectural Overview & System Decoupling

Sanchaya strictly separates concerns into four distinct layers:

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
  │  from<T>().where().group_by().select().order_by().limit().offset()                      │
  │  Logical IR: logical_source_node, logical_filter_node, logical_project_node, etc.      │
  │  RBO Rewriter: Predicate Pushdown, Projection Pruning, Limit Pushdown                  │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                         PHYSICAL PLAN IR & PLACEMENT ENGINE                            │
  │  Cost-Model-Guided Placement: In-Memory / Petika KV / SQLite OLTP / DuckDB OLAP        │
  │  Physical IR: physical_table_scan, physical_index_seek, physical_filter_op, etc.        │
  │  Execution: Typed cursors, generational session handles, CDC synchronization            │
  └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Core Use Cases

### 2.1 Non-Intrusive In-Memory Domain Persistence
Retain pure domain structs with no inheritance, virtual tables, or intrusive base classes:
```cpp
struct DepartmentId { std::uint64_t value; };
struct EmployeeId   { std::uint64_t value; };

struct Department {
    DepartmentId id;
    std::string name;
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

### 3.2 Rule-Based Optimization (RBO) Passes
- **Predicate Pushdown (`predicate_pushdown_rule`)**:
  $$\sigma_{P}(\pi_{A}(R)) \implies \pi_{A}(\sigma_{P}(R))$$
  Pushes filter selection operations down through projection operators to reduce intermediate tuple volume early.
- **Projection Pruning (`projection_pruning_rule`)**:
  $$\pi_{A}(\pi_{B}(R)) \implies \pi_{A}(R)$$
  Collapses redundant nested projections into a single projection operator.
- **Limit/Offset Pushdown (`limit_pushdown_rule`)**:
  Propagates pagination constraints toward leaf scans to truncate scan volume.

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
| **Meta & AST** | `include/meta/meta.hpp` | `member_pointer_traits`, member object/function introspection, multi-step `member_path` traversal. |
| **Vākya** | `include/vakya/vakya.hpp` | Expression template lifting (`vakya::as_expr`) and type-safe operator composition for query predicates. |
| **Akshara** | `include/meta/akshara.hpp` | Compile-time fixed strings (`akshara::fixed_string`) for field, table, alias, and service tags. |
| **LiteGraph & TarjanSCC** | `include/containers/graph/LiteGraph.hpp` | Relational dependency modeling and cycle validation. |
| **SmallVector** | `include/containers/dynamic/SmallVector.hpp` | Zero-heap SBO key sets, projection index lists, and graph edge buffers. |
| **Kosha** | `include/containers/cache/kosha.hpp` | Session caching, query plan memoization, and cost statistics store. |
| **Petika** | `include/petika/petika.hpp` | Embedded high-speed key-value persistence tier (`petika::SkipStore`). |
| **SQLite & DuckDB** | `dependencies/sqlite`, `dependencies/libduckdb` | Transactional OLTP and vectorized columnar analytical engines. |

---

## 5. End-to-End Code Examples

### 5.1 Compile-Time Model Declaration
```cpp
#include "sanchaya/sanchaya.hpp"

using namespace sanchaya;

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

### 5.2 Workspace Creation and Entity Lifecycle
```cpp
// Instantiate zero-overhead policy-composed workspace
auto ws = make_workspace(company_model)
    .local_auto("./company_storage")
    .build();

// Put entity with generational session handle
Employee emp{.id = EmployeeId{101}, .name = "Grace Hopper", .age = 45, .department_id = DepartmentId{1}};
auto handle_res = ws.put(emp);

if (handle_res) {
    auto handle = *handle_res;
    if (auto current = handle.get(1)) {
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
adaptive_tiered_planner planner{};
auto physical_plan = planner.optimize(proj);
static_assert(physical_plan<decltype(physical_plan)>);
```
