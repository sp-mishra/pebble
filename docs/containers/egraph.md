# `egraph` — Generic Equality Saturation Engine

> **Header:** `include/containers/graph/egraph.hpp`
> **Lithe Adapter (opt-in):** `include/edsl/lithe_egraph.hpp`
>
> **Namespace:** `egraph`
> **Standard required:** C++23 (`-std=c++2b`)
> **Dependencies:** `SmallVector`, `Kosha FlatHashStorage`; optional Google Highway (SIMD children hashing)

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [What is an E-Graph?](#2-what-is-an-e-graph)
3. [Design](#3-design)
4. [Core Types](#4-core-types)
5. [API Reference](#5-api-reference)
6. [Rule Packs](#6-rule-packs)
7. [Saturation](#7-saturation)
8. [Extraction](#8-extraction)
9. [Lithe Adapter](#9-lithe-adapter)
10. [Examples](#10-examples)
11. [Performance Notes](#11-performance-notes)

---

## 1. Introduction

`egraph` is a **zero-overhead, header-only equality saturation engine** for C++23. It provides:

- **Hashcons deduplication** — structurally identical nodes share one e-class
- **Union-find** — path-splitting + union-by-rank for O(α) merge/find
- **Egg-style deferred rebuild** — batch congruence closure; no per-merge allocation
- **Generic rule packs** — commutativity, associativity, distributivity, identity rules as empty types
- **Bottom-up DP extraction** — pick cheapest equivalent form via a pluggable cost model
- **Arena-friendly** — `Alloc` template param threads all vectors through any allocator
- **User class data** — `ClassData` param attaches domain data to e-classes at zero cost
- **Live-class range** — C++23 `filter_view` over root classes; no dead-class scanning in rules
- **SIMD hashing** — optional Highway acceleration for nodes with ≥8 children

No virtual functions, no macros, no RTTI.

---

## 2. What is an E-Graph?

An **e-graph** (equality graph) is a data structure that compactly represents an exponential number of equivalent
expressions. It is the core of the **equality saturation** technique used in:

- Compiler IR optimizers (e.g., LLVM peephole, Cranelift's `egg`)
- Query plan optimizers
- Symbolic math simplifiers
- Tensor expression optimizers

### Key concepts

| Concept        | Description                                                       |
|----------------|-------------------------------------------------------------------|
| **e-node**     | One form of an expression: `{op, children[], payload}`            |
| **e-class**    | A set of equivalent e-nodes                                       |
| **e-class id** | Dense `uint32_t` identifier for an e-class                        |
| **hashcons**   | Deduplication map: `e-node → e-class id`                          |
| **union-find** | Tracks which e-classes have been merged                           |
| **rebuild**    | Re-canonicalizes parent e-nodes after merges (congruence closure) |
| **saturation** | Fixpoint: apply rules until no new merges occur                   |
| **extraction** | Pick the best (lowest-cost) e-node from each e-class              |

### The saturation loop

```
repeat until fixpoint or limit:
    for each rule:
        scan e-graph, call merge() for each match
    rebuild()           ← batch congruence closure
extract_best(root)      ← bottom-up DP
```

---

## 3. Design

### Zero-overhead abstractions

- Rule packs are **empty types** (`[[no_unique_address]]` eligible). No function pointers, no vtables.
- `ClassData = std::monostate` costs nothing — the field is `[[no_unique_address]]`.
- `Alloc = std::allocator<char>` costs nothing when unused.
- SIMD hashing is guarded by `#if __has_include(<hwy/highway.h>)` — zero impact when Highway is absent.

### Egg-style deferred rebuild

Merging two e-classes does **not** immediately repair the hashcons. Instead, the merged class is added to a `dirty_`
worklist. `rebuild()` drains this worklist, re-canonicalizes every parent e-node of each dirty class, and re-inserts
into the hashcons — merging any newly congruent classes recursively. This batches allocation and hash-table work.

### Parent-list size snapshot

During `rebuild()`, the parent list is iterated by index with a **size snapshot** taken before the loop. New parents
appended during inner merges are deferred to the next dirty-flush round. This eliminates `SmallVector` copy allocations
on the hot path.

### Hashcons via Kosha FlatHashStorage

Robin-Hood open-addressing with cache-line-friendly layout. Initial capacity 64; grows as needed. After merge, the loser
class's nodes are re-inserted under the winner's id during `rebuild()`.

---

## 4. Core Types

### `e_class_id`

```cpp
using e_class_id = std::uint32_t;
inline constexpr e_class_id kInvalidClassId = std::numeric_limits<e_class_id>::max();
```

Dense index. Valid range: `[0, class_count())`. After `merge(a, b)`, one of them becomes the canonical root; use
`find(id)` to resolve.

---

### `e_node<OpId, Payload>`

```cpp
template <class OpId = std::size_t, class Payload = std::size_t>
struct e_node {
    OpId     op;
    SmallVector<e_class_id, 4*sizeof(e_class_id)> children; // inline ≤4
    Payload  payload{};

    bool operator==(const e_node&) const noexcept; // std::ranges::equal on children
};
```

- `op` — operation identity (integral or any `std::hash`-able type)
- `children` — ids of child e-classes; inline storage for ≤4 children (one cache line)
- `payload` — leaf value hash; 0 for interior nodes

---

### `e_class<Node, ClassData>`

```cpp
template <class Node, class ClassData = std::monostate>
struct e_class {
    SmallVector<Node, 2*sizeof(Node)> nodes; // equivalent forms; inline ≤2
    e_class_id                        parent;
    std::uint32_t                     rank{0};
    [[no_unique_address]] ClassData   data{};
};
```

`parent == id` iff this class is a root (live, not merged away).

---

### `e_graph<OpId, Payload, Hash, Eq, ClassData, Alloc>`

```cpp
template <
    class OpId      = std::size_t,
    class Payload   = std::size_t,
    class Hash      = default_enode_hash<OpId, Payload>,
    class Eq        = default_enode_eq<OpId, Payload>,
    class ClassData = std::monostate,
    class Alloc     = std::allocator<char>>
class e_graph;
```

| Param       | Default                | Purpose                                          |
|-------------|------------------------|--------------------------------------------------|
| `OpId`      | `size_t`               | Operation identity type                          |
| `Payload`   | `size_t`               | Leaf payload type                                |
| `Hash`      | `default_enode_hash`   | Hashcons hash function                           |
| `Eq`        | `default_enode_eq`     | Hashcons equality                                |
| `ClassData` | `monostate`            | User data per e-class (zero-cost when monostate) |
| `Alloc`     | `std::allocator<char>` | Allocator for all internal vectors               |

**Common aliases:**

```cpp
using MyGraph = egraph::e_graph<uint32_t, uint64_t>;

// With arena allocator (smriti):
using ArenaGraph = egraph::e_graph<
    uint32_t, uint64_t,
    egraph::default_enode_hash<uint32_t, uint64_t>,
    egraph::default_enode_eq<uint32_t, uint64_t>,
    std::monostate,
    smriti::BumpAllocator<smriti::SystemRAMDomain>>;

// With per-class annotation:
struct OptCost { std::size_t value = SIZE_MAX; };
using AnnotatedGraph = egraph::e_graph<size_t, size_t,
    egraph::default_enode_hash<>, egraph::default_enode_eq<>, OptCost>;
```

---

## 5. API Reference

### Construction & capacity

```cpp
e_graph()                       // default construct
explicit e_graph(Alloc alloc)   // arena-backed construct
void reserve(std::size_t n)     // pre-allocate for n e-classes
```

`reserve()` calls `classes_.reserve(n)` and `parent_map_.reserve(n)`. Call before bulk `add()` to amortise growth.

---

### Core operations

```cpp
[[nodiscard]] e_class_id add(node_t n)
```

Hashcons insert. Canonicalizes `n.children` via `find()` first. Returns the canonical root id. If an equal node already
exists, returns its root without inserting.

```cpp
[[nodiscard]] e_class_id find(e_class_id id) const
```

Path-splitting union-find root. O(α) amortised. `const` via mutable storage.

```cpp
[[nodiscard]] bool merge(e_class_id a, e_class_id b)
```

Union-by-rank. Returns `true` if `a` and `b` were in different classes (i.e., a change occurred). Adds the winner to
`dirty_`. Does **not** repair the hashcons — call `rebuild()` after a batch of merges.

```cpp
void rebuild()
```

Egg-style batch congruence closure. Drains `dirty_`, re-canonicalizes parent e-nodes, re-inserts into hashcons. Must be
called before the next round of rule applications.

---

### Queries

```cpp
[[nodiscard]] bool        is_root(e_class_id id) const noexcept
[[nodiscard]] std::size_t class_count() const noexcept       // total (incl. merged-away)
[[nodiscard]] std::size_t class_count_live() const noexcept  // roots only
[[nodiscard]] std::size_t enode_count() const noexcept

[[nodiscard]] const class_t& get_class(e_class_id id) const
[[nodiscard]]       class_t& get_class(e_class_id id)
[[nodiscard]] const auto&    classes() const noexcept        // full vector (for algorithms)
```

```cpp
[[nodiscard]] auto live_class_ids() const noexcept
```

Returns a `std::ranges::filter_view` of `e_class_id` values for root classes only. Use in custom rules to skip dead
classes without a manual `parent != id` guard:

```cpp
for (auto id : g.live_class_ids()) {
    for (const auto& node : g.classes()[id].nodes) { /* ... */ }
}
```

---

### Class data accessors

```cpp
[[nodiscard]] const ClassData& get_class_data(e_class_id id) const
[[nodiscard]]       ClassData& get_class_data(e_class_id id)
void set_class_data(e_class_id id, ClassData d)
```

Zero-overhead when `ClassData = std::monostate`. All accessors call `find(id)` first (always resolves to root).

---

### Concepts

```cpp
template <class G>
concept egraph_model = /* add/find/merge/rebuild/class_count/enode_count/classes */;

template <class R, class G>
concept egraph_rule = requires(R r, G& g) { r.apply(g); };

template <class C, class Node>
concept cost_model = requires(C c, const Node& n) {
    typename C::cost_t;
    { c.cost(n, std::span<const typename C::cost_t>{}) } -> std::same_as<typename C::cost_t>;
};
```

---

## 6. Rule Packs

Built-in rule packs are **empty types** — zero heap, zero virtual, eligible for `[[no_unique_address]]`.

Each pack takes `template <class OpTraits, class G = void>`. The `apply()` method is a function template — it accepts
any graph-like type (the real `e_graph`, a counting wrapper, etc.). `G` is retained for alias compatibility.

### `OpTraits` requirements

| Field                     | Used by                           |
|---------------------------|-----------------------------------|
| `commutative_op`          | `commutativity`                   |
| `associative_op`          | `associativity`                   |
| `add_op`, `mul_op`        | `distributivity`, `identity_zero` |
| `zero_op`, `zero_payload` | `identity_zero` (add identity)    |
| `one_op`, `one_payload`   | `identity_zero` (mul identity)    |

Fields are checked with `if constexpr (requires { ... })` — absent fields disable the corresponding sub-rule at compile
time.

### Available packs

| Pack                  | Rewrite                                                         |
|-----------------------|-----------------------------------------------------------------|
| `commutativity<T,G>`  | `op(a,b) ↔ op(b,a)` where `op == T::commutative_op`             |
| `associativity<T,G>`  | `op(op(a,b),c) ↔ op(a,op(b,c))` where `op == T::associative_op` |
| `distributivity<T,G>` | `mul(a,add(b,c)) ↔ add(mul(a,b),mul(a,c))`                      |
| `identity_zero<T,G>`  | `add(x,zero)→x`, `mul(x,one)→x`                                 |

### Custom rules

Implement the `egraph_rule` concept:

```cpp
struct my_strength_reduction {
    template <class G>
    void apply(G& g) const {
        const std::size_t n = g.class_count();
        for (egraph::e_class_id id = 0; id < n; ++id) {
            if (!g.is_root(id)) continue;
            for (const auto& node : g.classes()[id].nodes) {
                if (node.op == kMul2) {          // x * 2
                    typename G::node_t shifted;
                    shifted.op = kShl;
                    shifted.children.push_back(node.children[0]);
                    shifted.payload = 1;         // shift by 1
                    (void)g.merge(id, g.add(shifted));
                }
            }
        }
    }
};
```

---

## 7. Saturation

```cpp
struct saturation_limits {
    std::size_t max_iters    = 30;
    std::size_t max_enodes   = 100'000;
    std::size_t max_eclasses = 50'000;
};

struct saturation_report {
    std::size_t iters;
    std::size_t enodes;
    std::size_t eclasses;
    std::size_t merges_fired;  // actual merge() calls returning true
    bool        hit_limit;
    bool        saturated;     // true iff fixpoint reached without hitting a limit
};

template <class G, class... Rules>
[[nodiscard]] saturation_report saturate(
    G& graph,
    std::tuple<Rules...> rules,
    const saturation_limits limits = {});
```

`saturate()` wraps the graph in a merge-counting proxy to track `merges_fired` accurately. The fixpoint condition is
`enode_count` and `class_count` both unchanged after a full rule pass + rebuild.

**Usage:**

```cpp
egraph::e_graph g;
// ... build e-graph ...

auto rules = std::make_tuple(
    egraph::commutativity<MyTraits>{},
    egraph::associativity<MyTraits>{},
    my_strength_reduction{}
);

auto report = egraph::saturate(g, rules,
    egraph::saturation_limits{.max_iters = 20, .max_enodes = 50'000});

if (report.saturated)
    // fixpoint reached
if (report.hit_limit)
    // stopped early — result is sound but not complete
```

---

## 8. Extraction

```cpp
template <class CM = node_count_cost, class G>
[[nodiscard]] detail::extraction_result<G, CM>
extract_best(const G& graph, e_class_id root, CM model = {});
```

Bottom-up DP. Visits each reachable e-class once, picks the e-node with minimum cost. Cycle-safe via a `visiting[]`
guard.

`extraction_result<G, CM>` contains:

```cpp
std::vector<std::optional<node_t>> best_nodes; // [class_id] → chosen e_node
std::vector<cost_t>                best_costs; // [class_id] → its cost
```

### Built-in cost model

```cpp
struct node_count_cost {
    using cost_t = std::size_t;
    template <class Node>
    cost_t cost(const Node&, std::span<const cost_t> child_costs) const noexcept;
    // returns 1 + sum(child_costs)
};
```

### Custom cost model

```cpp
struct latency_cost {
    using cost_t = double;

    double cost(const egraph::e_node<>& n,
                std::span<const double> cc) const noexcept {
        double base = (n.op == kMul) ? 3.0 : 1.0; // mul costs 3 cycles
        double child_sum = 0;
        for (auto c : cc) child_sum += c;
        return base + child_sum;
    }
};

auto result = egraph::extract_best<latency_cost>(g, root);
```

---

## 9. Lithe Adapter

> **Header:** `include/edsl/lithe_egraph.hpp`
> **Namespace:** `lithe::egraph`
> Not included by `lithe.hpp` — opt-in.

Bridges the generic `egraph` container with Lithe expression trees.

### Key types

```cpp
using lithe_egraph_t = egraph::e_graph<std::size_t, std::size_t, lithe_node_hash, lithe_node_eq>;
```

### `intern(graph, expr)`

Post-order traversal of a Lithe expression. Maps each sub-expression to an e-node via `tag_descriptor::stable_id` (op) +
`structural_payload_hash` (payload for value-carrying leaves). Returns the root `e_class_id`. No AST contamination.

```cpp
lithe_egraph_t g;
auto root = lithe::egraph::intern(g, my_expr);
```

### Built-in rules

```cpp
lithe::egraph::lithe_default_rules  // commutativity(add,mul) + associativity(add) + identity_zero
```

### Built-in cost models

| Model                  | Strategy                          |
|------------------------|-----------------------------------|
| `ast_size_cost`        | Minimize total e-node count       |
| `cpu_instruction_cost` | Favour lower-latency ops          |
| `gpu_parallel_cost`    | Favour parallelism-friendly forms |
| `tensor_fusion_cost`   | Favour fusion-friendly tensor ops |

### Optimization pass

```cpp
template <class Rules, class CostModel, class Limits>
struct egraph_optimize; // satisfies lithe pass_type_traits
                        // category=optimization, out_stage=optimized, stable_id=1000
```

Apply as a Lithe compiler pass. Emits NADI telemetry when `<observability/nadi.hpp>` is available.

---

## 10. Examples

### Example 1 — Basic hashcons and union-find

```cpp
#include "containers/graph/egraph.hpp"

using G = egraph::e_graph<>;

egraph::e_node<> leaf(std::size_t v) {
    egraph::e_node<> n; n.op = 0; n.payload = v; return n;
}
egraph::e_node<> binop(std::size_t op, egraph::e_class_id a, egraph::e_class_id b) {
    egraph::e_node<> n; n.op = op; n.children.push_back(a); n.children.push_back(b); return n;
}

G g;
auto x  = g.add(leaf(1));
auto y  = g.add(leaf(2));
auto xy = g.add(binop(kAdd, x, y));
auto yx = g.add(binop(kAdd, y, x));

// xy and yx are distinct before saturation
assert(g.find(xy) != g.find(yx));

// Manually assert commutativity
g.merge(xy, yx);
g.rebuild();

assert(g.find(xy) == g.find(yx)); // now equivalent
```

---

### Example 2 — Saturation with commutativity + identity

```cpp
struct MyTraits {
    static constexpr std::size_t commutative_op = kAdd;
    static constexpr std::size_t associative_op = kAdd;
    static constexpr std::size_t add_op         = kAdd;
    static constexpr std::size_t mul_op         = kMul;
    static constexpr std::size_t zero_op        = kLeaf;
    static constexpr std::size_t zero_payload   = 0;
    static constexpr std::size_t one_op         = kLeaf;
    static constexpr std::size_t one_payload    = 1;
};

G g;
auto x    = g.add(leaf(7));
auto zero = g.add(leaf(0));          // leaf with payload 0
auto expr = g.add(binop(kAdd, x, zero)); // x + 0

auto rules = std::make_tuple(
    egraph::identity_zero<MyTraits>{}
);
auto report = egraph::saturate(g, rules);
assert(report.saturated);

// extract: should pick x (cost 1) not x+0 (cost 3)
auto result = egraph::extract_best(g, expr);
auto root   = g.find(expr);
assert(result.best_costs[root] == 1u);
assert(result.best_nodes[root]->children.empty()); // it's a leaf
```

---

### Example 3 — User data on e-classes

```cpp
struct CostAnnotation { double cycles = 1e9; };

egraph::e_graph<size_t, size_t,
    egraph::default_enode_hash<>, egraph::default_enode_eq<>,
    CostAnnotation> g;

auto id = g.add(leaf(42));
g.set_class_data(id, CostAnnotation{3.5});
assert(g.get_class_data(id).cycles == 3.5);
```

---

### Example 4 — Arena-backed e-graph (smriti)

```cpp
#include "mem/smriti.hpp"
#include "containers/graph/egraph.hpp"

smriti::LinearArena arena{1 << 20}; // 1 MiB
using Alloc = smriti::LinearArenaAllocator<char>;

egraph::e_graph<size_t, size_t,
    egraph::default_enode_hash<>, egraph::default_enode_eq<>,
    std::monostate, Alloc> g{Alloc{arena}};

g.reserve(4096); // pre-allocate; no heap fragmentation
// ... build and saturate ...
// arena.reset() frees everything at once
```

---

### Example 5 — Custom rule with `live_class_ids()`

```cpp
// Strength reduction: x * 2 → x << 1
struct mul2_to_shift {
    template <class G>
    void apply(G& g) const {
        for (auto id : g.live_class_ids()) {
            for (const auto& node : g.classes()[id].nodes) {
                if (node.op == kMul && node.children.size() == 2) {
                    auto rhs = g.find(node.children[1]);
                    for (const auto& rn : g.classes()[rhs].nodes) {
                        if (rn.op == kLeaf && rn.payload == 2) {
                            typename G::node_t sh;
                            sh.op = kShl;
                            sh.children.push_back(node.children[0]);
                            sh.payload = 1;
                            (void)g.merge(id, g.add(sh));
                        }
                    }
                }
            }
        }
    }
};
```

---

## 11. Performance Notes

| Concern                   | Design choice                                                                         |
|---------------------------|---------------------------------------------------------------------------------------|
| Children hash (small, ≤4) | Scalar XOR-shift; inline `SmallVector` — no heap                                      |
| Children hash (large, ≥8) | SIMD lane-pair u64 accumulation (Highway, if available)                               |
| Hashcons                  | Kosha `FlatHashStorage`: Robin-Hood open-addressing, cache-line-friendly              |
| Union-find                | Path-splitting + union-by-rank: O(α) per operation                                    |
| Rebuild                   | Batch per-dirty-class; parent list iterated by index (size snapshot, no copy)         |
| Rule iteration            | `live_class_ids()` filter view: skips dead classes without branch                     |
| Allocator                 | Pluggable `Alloc`; all vectors use rebound allocator — arena zeroes fragmentation     |
| Dead classes              | Counted in `class_count()`; excluded from `class_count_live()` and `live_class_ids()` |
| Merge counting            | `merge_counting_wrapper` in `saturate()` — zero overhead outside saturation loop      |

### Sizing guidelines

```
reserve(expected_classes)       // before bulk add — 1 alloc instead of log N
saturation_limits{
    .max_iters    = 20,         // typical: 5–30 for arithmetic rules
    .max_enodes   = 200'000,    // tune per domain
    .max_eclasses = 100'000,
}
```

For query optimizer use cases, `max_enodes = 10'000` and `max_iters = 10` are sufficient for most real queries. For
tensor expression optimization, `max_enodes = 50'000` covers large tensor programs.
