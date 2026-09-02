# Vākya — Structural-Construction EDSL

## Table of Contents

- [Introduction](#introduction)
- [Dependency Contract](#dependency-contract)
- [Position in the Stack](#position-in-the-stack)
- [Expression AST](#expression-ast)
- [Terminal Wrappers](#terminal-wrappers)
- [Concepts & Traits](#concepts--traits)
- [Tag Metadata (`vakya::emit`)](#tag-metadata-vakyaemit)
- [Structural Identity](#structural-identity)
- [Algorithms Used](#algorithms-used)
- [The `structural_unwrap` Seam](#the-structural_unwrap-seam)
- [Tree Folds (`vakya::tree`)](#tree-folds-vakyatree)
- [Shared DAG (`vakya::graph`)](#shared-dag-vakyagraph)
- [IRBuilder](#irbuilder)
- [Pattern DSL (`vakya::pattern`)](#pattern-dsl-vakyapattern)
- [Property System (`vakya::property`)](#property-system-vakyaproperty)
- [Rule Registry (`vakya::rule_registry`)](#rule-registry-vakyarule_registry)
- [Type System Stack (opt-in)](#type-system-stack-opt-in)
    - [Layered Architecture](#layered-architecture)
    - [Architecture Deep Dive](#architecture-deep-dive)
    - [End-to-End Integration Flow](#end-to-end-integration-flow)
    - [Header Reference Table](#header-reference-table)
    - [Type Terms (`vakya::types`)](#type-terms-vakyatypes)
    - [Unification (`vakya::types`)](#unification-vakyatypes)
    - [Constraints & Solvers](#constraints--solvers)
    - [Type Checking & Inference](#type-checking--inference)
    - [Guarded Rewriting](#guarded-rewriting)
    - [Validation](#validation)
    - [Diagnostics (`vakya::diag`)](#diagnostics-vakyadiag)
- [End-to-End Pipeline Example](#end-to-end-pipeline-example)
- [Constraint *Reasoning* Layer](#constraint-reasoning-layer)
    - [From Solving to Reasoning](#from-solving-to-reasoning)
    - [Reasoning Layered Architecture](#reasoning-layered-architecture)
    - [Registries & Descriptors](#registries--descriptors)
    - [Descriptor-Routed Constraint Engine](#descriptor-routed-constraint-engine)
    - [Capability & Effect Systems](#capability--effect-systems)
    - [Analysis Store & Semantic Analysis](#analysis-store--semantic-analysis)
    - [Type-Aware Matching & Type-Level Rewriting](#type-aware-matching--type-level-rewriting)
    - [Shape Algebra](#shape-algebra)
    - [Formal Verification (Tarka SMT)](#formal-verification-tarka-smt)
    - [Refinement Types & Semantic Query Engine](#refinement-types--semantic-query-engine)
    - [Reasoning Header Plan](#reasoning-header-plan)
    - [End-to-End Reasoning Flow](#end-to-end-reasoning-flow)
- [Semantic *Optimization* Layer](#semantic-optimization-layer)
    - [From Reasoning to Optimization](#from-reasoning-to-optimization)
    - [Optimization Constraint-Kind Routing](#optimization-constraint-kind-routing)
    - [Consumer-Boundary Discipline](#consumer-boundary-discipline)
- [Relationship to the Generic IR](#relationship-to-the-generic-ir)
- [Consumer Integration](#consumer-integration)

---

## Introduction

**Vākya** is a standalone, header-only, C++26-capable structural-construction EDSL. It constructs, hashes, compares,
traverses, and pattern-matches expression trees without any notion of semantics, passes, code generation, or backends.

| Property       | Detail                                                                          |
|----------------|---------------------------------------------------------------------------------|
| Standard       | C++26 target; core remains C++23-compatible (concepts, `[[no_unique_address]]`) |
| Delivery       | Header-only (`include/vakya/`)                                                  |
| Namespace      | `vakya`                                                                         |
| Entry header   | `vakya/vakya.hpp` (construction surface)                                        |
| Opt-in DSL     | `vakya/pattern.hpp` (structural pattern matching)                               |
| Opt-in types   | `vakya/vakya_types.hpp` (full type-system stack, see below)                     |
| No virtual fns | All dispatch via templates and concepts                                         |
| No macros      | Tag registration is macro-free via NTTP descriptors                             |

Vākya has **no consumer-project dependency**. The `test_vakya_construction.cpp` smoke tests exercise the pure
`vakya::` surface, proving the library stands alone.

---

## Dependency Contract

Vākya is header-only, but it is deliberately layered: include only the surface you use. The core construction and
pattern surfaces have no Pebble subsystem dependency; the semantic/type layers reuse Pebble infrastructure rather than
reimplementing arenas, handles, registries, graphs, caches, or SMT solving.

| Surface                               | Required dependencies                                                                                          | Optional integration                                                                             |
|---------------------------------------|----------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------|
| `vakya/vakya.hpp`                     | C++ standard library only                                                                                      | None                                                                                             |
| `vakya/pattern.hpp`                   | `vakya/vakya.hpp`                                                                                              | None                                                                                             |
| `vakya/property.hpp`                  | Vākya core; `containers/dynamic/SmallVector.hpp`                                                               | None                                                                                             |
| `vakya/rule_registry.hpp`             | Vākya pattern layer                                                                                            | None                                                                                             |
| `vakya/types.hpp`, `unification.hpp`  | Vākya core; Pebble `slot_map`, generational handles, `SmallVector`, `kosha`, `union_find`, and `mem/arena.hpp` | None                                                                                             |
| Constraints, analysis, and registries | Type layer; Pebble `LiteGraph`, descriptor registry, and related container utilities                           | `egraph.hpp` activates equality-saturation paths when available                                  |
| `vakya/smt.hpp`, `verify.hpp`         | Constraint/analysis layers                                                                                     | Tarka bridge only when `tarka/tarka.hpp` is available; otherwise `no_smt_backend` remains usable |
| `vakya/diagnostics.hpp`               | C++ standard library only                                                                                      | `observability/nadi.hpp` enables `nadi_sink` when available                                      |
| `vakya/vakya_types.hpp`               | All Vākya type-system headers and their Pebble dependencies                                                    | Tarka, egraph, and NADI remain opt-in as above                                                   |

There are no direct third-party types in Vākya's public API. Optional capabilities are discovered with
`__has_include`; they do not make the core header or a basic expression consumer depend on a solver, telemetry, or a
downstream compiler project. The `type_ir_module_view` API is Vākya-owned and does not require the Generic Language IR
headers.

For the smallest build and fastest compile path, include `vakya/vakya.hpp` alone. Add `pattern.hpp`,
`property.hpp`, or individual type-system headers only when their corresponding facilities are needed; reserve
`vakya/vakya_types.hpp` for consumers that genuinely need the complete semantic stack.

---

## Position in the Stack

```
 Vākya       structure construction   (this library)
   │
   ▼
 Consumer    optional IR / passes / optimization / code generation / execution
```

Consumers compose Vākya through its public `vakya::` API and may add their own adapters in their own namespaces.

---

## Expression AST

`vakya::node<Tag, Children...>` is the flattened AST node. It exposes `tag_type` and a `children` tuple and satisfies
the `Expression` concept. Nodes are built lazily — no compilation context required.

```cpp
auto e = vakya::as_expr(x) + vakya::as_expr(y) * 2; // add(x, mul(y, 2))
static_assert(std::is_same_v<decltype(e)::tag_type, vakya::add_tag>);
```

`make_node<Tag>(children...)` is the programmatic constructor; the C++ operator surface (`+ - * / …`) is sugar over it.
A strict `capture_t` policy preserves references safely (lvalues captured by ref, rvalues by value).

**Built-in tags** (empty structs): arithmetic (`add_tag`, `sub_tag`, `mul_tag`, `div_tag`, `mod_tag`, `neg_tag`),
comparison, logical, bitwise, control-flow, memory, and advanced tags.

---

## Terminal Wrappers

`as_expr(x)` lifts a value into the expression world:

- lvalue → `expr_ref<T>` (holds `T* p`, non-owning)
- rvalue → `expr<T>` (holds `T value`, owning)

```cpp
int x = 7;
auto lref = vakya::as_expr(x);   // expr_ref<int>
auto rval = vakya::as_expr(42);  // expr<int>
```

Detected with `is_expr_ref_wrapper_v` / `is_expr_wrapper_v`.

---

## Concepts & Traits

```cpp
template <class T> concept Expression;  // has tag_type + children
template <class T> concept Terminal;    // arithmetic, has_vakya_terminal_tag<T>, or is_terminal<T> specialization
template <class T> concept Operand;     // Expression || Terminal
template <class T> concept VariantExpr; // std::variant of alternatives
```

Downstream leaf types opt into `Terminal` by one of three priority-ordered mechanisms (first match wins):

1. **Concept hook**: declare `using vakya_terminal = void;` as a public member — zero specialization required.
2. **Trait specialization**: specialise `vakya::is_terminal<T>` in namespace `vakya`.
3. **Default**: arithmetic types (`std::is_arithmetic_v<T>`) are terminals automatically.

```cpp
struct MyScalar {
    using vakya_terminal = void;  // opt-in — no specialization needed
    float v;
};
static_assert(vakya::Terminal<MyScalar>);
```

---

## Tag Metadata (`vakya::emit`)

`emit::tag_descriptor<Tag>` is the single source of truth for per-tag metadata:

```cpp
using vakya::emit::tag_descriptor;
tag_descriptor<vakya::add_tag>::symbol;        // "+"
tag_descriptor<vakya::add_tag>::arity;         // 2
tag_descriptor<vakya::neg_tag>::arity;         // 1
tag_descriptor<vakya::add_tag>::is_commutative; // true
tag_descriptor<vakya::sub_tag>::is_commutative; // false (default)
vakya::emit::tag_id<vakya::add_tag>::value;    // stable id
```

Downstream EDSLs register custom tags by **specialising** `tag_descriptor` and returning a `stable_id >=
kExtensionIdBase` (`1000u`). `kVariadicArity` (`0xFF`) marks variadic tags. `tag_name` / `tag_id` alias the descriptor.

**`is_commutative`** (default `false`): when `true` the pattern matcher automatically tries the swapped-operand ordering
when the canonical ordering fails to match. Built-in commutative tags: `add_tag`, `mul_tag`. All other built-in tags
default to `false` (primary template). Downstream extensions declare commutative tags by including
`static constexpr bool is_commutative = true;` in their `tag_descriptor` specialization.

> Specialise via the qualified-id form (`struct vakya::emit::tag_descriptor<T> {…}`) or inside a
> `namespace vakya::emit { … }` block. Both resolve against Vākya's primary template.

---

## Structural Identity

Topology-only by default:

```cpp
vakya::structural_equal(a, b);  // same shape + tags
vakya::structural_hash(a);      // stable topology hash (== for structural_equal trees)
vakya::structural_key(a);
```

Value-carrying leaves opt in to payload sensitivity via an ADL hook — define
`structural_payload_hash(const Node&) noexcept` in the leaf type's own namespace. Tags without it pay nothing.

### Equality vs hashing semantics

| Question                                        | Answer                                                                                                                                                                                                                                                                                                                                                            |
|-------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Is equality derived from hash?                  | No. `structural_equal` is a recursive tag + topology check. It does **not** compare hashes.                                                                                                                                                                                                                                                                       |
| Does payload-aware equality exist separately?   | `structural_equal` uses the hash only for terminal leaves that define `structural_payload_hash`. Two terminals are structurally equal iff their `structural_payload_hash` values are equal. If your payload requires exact comparison (not hash equality), define a `structural_payload_equal` ADL hook instead; `structural_equal` checks it first when present. |
| What if two payloads produce identical hashes?  | Hash collision: `structural_hash(a) == structural_hash(b)` does **not** imply `structural_equal(a, b)`. Equality is authoritative; hash is only a fast pre-filter for DAG interning. `property_store` is keyed by hash — store the canonical expression alongside the property if exact identity matters.                                                         |
| When should I define `structural_payload_hash`? | When two leaf values that are `!=` under C++ `operator==` must produce **different** DAG interns (e.g. `lit(1.0f)` vs `lit(2.0f)`). Without the hook, both intern to the same node.                                                                                                                                                                               |

### Hash stability guarantees

`structural_hash` mixes `tag_descriptor::stable_id` values via FNV-1a. The guarantees:

| Stability axis           | Guarantee                                                                                                                                                                                           |
|--------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Within one process run   | **Stable.** Same tree always produces the same hash in the same process.                                                                                                                            |
| Across process runs      | **Stable** for trees whose leaves define no `structural_payload_hash`. For leaves with `structural_payload_hash`, stability depends on the hook's own guarantees.                                   |
| Across compiler versions | **Stable** for built-in tags (`stable_id < 1000`) — their ids are compile-time constants frozen in the header. Extension tags (`stable_id >= 1000`) are caller-assigned; the caller owns stability. |
| Across architectures     | **Stable** — all arithmetic is fixed-width; no pointer-width or endian dependence.                                                                                                                  |
| Across library versions  | **Stable for built-in tags** as long as their `stable_id` values are not changed (breaking change). Extension ids are caller-controlled.                                                            |

**Persistence advice.** If you persist hashes (cache keys, AOT artifact ids, `property_store` snapshots):

- For topology-only trees: hash is safe to persist across runs and machines.
- For payload-carrying trees: only safe if every `structural_payload_hash` hook in the tree is itself stable (e.g.
  hashes a fixed numeric value, not a pointer).
- Never assume a hash uniquely identifies a tree across library version upgrades — always re-hash on load.

---

## Algorithms Used

| Concern              | Algorithm                                                                                                                                       | Where                              |
|----------------------|-------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------|
| Structural hashing   | Tag-`stable_id`-seeded FNV-1a fold over children; ADL `structural_payload_hash` for value leaves; sees through wrappers via `structural_unwrap` | `vakya.hpp` (structural_hash)      |
| Structural equality  | Recursive tag + topology compare (variant visit); optional `structural_payload_equal` hook                                                      | `vakya.hpp` (structural_equal)     |
| DAG interning / CSE  | Hash-cons: `structural_hash` bucket lookup (O(1)) + `structural_equal` confirm, `use_count` refcount                                            | `vakya.hpp` (`graph::dag_builder`) |
| Type interning       | Arena hash-cons for primitive/variable/constructor/callable/quantified type terms                                                               | `types.hpp` (`type_arena`)         |
| Unification          | Robinson most-general-unifier + occurs-check + `subst_delta` (`SmallVector<8>`)                                                                 | `unification.hpp`                  |
| Type inference       | Hindley-Milner (Algorithm-W) with `structural_hash`-keyed kosha LRU cache; let-polymorphism                                                     | `type_inference.hpp`               |
| Pattern matching     | Linear + non-linear (rebind vs. structural_equal) + commutative (both orderings)                                                                | `pattern.hpp`                      |
| Guarded rewriting    | `guarded_rule<Pattern, Rewrite, Guard>` predicate-gated term rewriting                                                                          | `rewrite.hpp`                      |
| Analysis propagation | `analysis_record` (effect_mask / capability_mask / proof_status / trait bits), thread-safe `update_for`                                         | `vakya.hpp` (analysis store)       |

Tree folds and visits (`vakya::tree`) are generic compile-time recursion (`fold`, `all_tags_satisfy`,
`any_tag_satisfies`) — no hand-rolled per-node code.

---

## The `structural_unwrap` Seam

`structural_unwrap(x)` is an ADL customisation point whose Vākya default is identity. A consumer that wraps expressions
in transparent decorators (for example, phase wrappers) registers a
`structural_unwrap` overload for each wrapper; `structural_hash`/`structural_equal` then see straight through them —
**without Vākya ever naming the wrapper types**.

Internally Vākya routes unwrap through an ADL barrier (`emit::unwrap_detail::call`) so the consumer's overloads are
found by argument-dependent lookup. Consumers only need to provide the overload; they never touch Vākya's internals.

### Before/after: adding a phase wrapper

**Before** — raw expression; `structural_hash` sees it directly:

```cpp
int x = 3, y = 4;
auto raw = vakya::as_expr(x) + vakya::as_expr(y);  // node<add_tag, expr_ref<int>, expr_ref<int>>
auto h1  = vakya::structural_hash(raw);
```

**After** — wrap in a consumer-defined phase decorator. Vākya must not name that wrapper type, so the consumer registers
an `structural_unwrap` overload in its own namespace:

```cpp
// consumer/phase_wrapper.hpp  (consumer code, zero Vākya edits)
template <class T>
struct surface_expr {
    T inner;
    explicit surface_expr(T t) : inner(std::move(t)) {}
};

// ADL hook: place in the same namespace as surface_expr
template <class T>
const T& structural_unwrap(const surface_expr<T>& w) noexcept { return w.inner; }

// Now structural_hash / structural_equal see through the wrapper:
auto wrapped = surface_expr{ vakya::as_expr(x) + vakya::as_expr(y) };
auto h2      = vakya::structural_hash(wrapped);   // h2 == h1  ← same topology
assert(h1 == h2);
assert(vakya::structural_equal(raw, wrapped));    // true — wrapper is transparent
```

**Chained wrappers** — multiple phase levels unwrap recursively as long as each defines its own overload:

```cpp
// surface_expr → canonical_expr → optimized_expr:
// structural_hash calls structural_unwrap once per wrapper level via the ADL barrier.
auto opt     = optimized_expr{ canonical_expr{ surface_expr{ raw } } };
auto h3      = vakya::structural_hash(opt);       // h3 == h1
```

**Why not a member function?** The ADL barrier in `emit::unwrap_detail::call` means only free functions in associated
namespaces are found — this prevents accidental satisfaction by unrelated `structural_unwrap` members on arbitrary types
and keeps the seam explicit and reviewable.

---

## Tree Folds (`vakya::tree`)

Compile-time and runtime traversal utilities:

```cpp
vakya::tree::arity(e);   // top-level child count
vakya::tree::size(e);    // total node count
vakya::tree::depth(e);   // tree depth
```

Also: `all_tags_satisfy<E,Pred>` / `any_tag_satisfies<E,Pred>` (consteval predicate traversal), `fold<E>(…)` (consteval
reduction), `for_each_child`, `rebuild_with`, `replace_child`, `map_children`.

---

## Shared DAG (`vakya::graph`)

`build_dag(e)` interns structurally-identical subexpressions once (CSE) into a `dag_view`:

```cpp
auto sub = vakya::as_expr(x) + vakya::as_expr(y);
auto e   = sub + sub;                    // (x+y) + (x+y) — child shared
auto dag = vakya::graph::build_dag(e);
dag.sharing_count();                     // >= 1 after interning
```

Also provides `dag_builder`, `shared_expr`, and `topo_order`.

---

## IRBuilder

Programmatic construction that matches the operator surface structurally:

```cpp
vakya::IRBuilder b;
auto viaBuilder = b.CreateAdd(vakya::as_expr(x),
                              b.CreateMul(vakya::as_expr(y), vakya::as_expr(2)));
auto viaOps     = vakya::as_expr(x) + vakya::as_expr(y) * vakya::as_expr(2);
assert(vakya::structural_equal(viaBuilder, viaOps));
```

### Method coverage

| Operator / operation | C++ operator sugar | `IRBuilder` method             | Tag             |
|----------------------|--------------------|--------------------------------|-----------------|
| Addition             | `a + b`            | `CreateAdd(a, b)`              | `add_tag`       |
| Subtraction          | `a - b`            | `CreateSub(a, b)`              | `sub_tag`       |
| Multiplication       | `a * b`            | `CreateMul(a, b)`              | `mul_tag`       |
| Division             | `a / b`            | `CreateDiv(a, b)`              | `div_tag`       |
| Modulo               | `a % b`            | `CreateMod(a, b)`              | `mod_tag`       |
| Negation (unary)     | `-a`               | `CreateNeg(a)`                 | `neg_tag`       |
| Equal                | `a == b`           | `CreateEq(a, b)`               | `eq_tag`        |
| Not equal            | `a != b`           | `CreateNe(a, b)`               | `ne_tag`        |
| Less than            | `a < b`            | `CreateLt(a, b)`               | `lt_tag`        |
| Less or equal        | `a <= b`           | `CreateLe(a, b)`               | `le_tag`        |
| Greater than         | `a > b`            | `CreateGt(a, b)`               | `gt_tag`        |
| Greater or equal     | `a >= b`           | `CreateGe(a, b)`               | `ge_tag`        |
| Logical AND          | `a && b`           | `CreateAnd(a, b)`              | `and_tag`       |
| Logical OR           | `a \|\| b`         | `CreateOr(a, b)`               | `or_tag`        |
| Logical NOT          | `!a`               | `CreateNot(a)`                 | `not_tag`       |
| Conditional          | —                  | `CreateIf(cond, then_, else_)` | `if_tag`        |
| Sequence             | —                  | `CreateSeq(stmts...)`          | `seq_tag`       |
| Function call        | —                  | `CreateCall(fn, args...)`      | `call_tag`      |
| Subscript            | —                  | `CreateSubscript(base, idx)`   | `subscript_tag` |

All `Create*` methods return `node<Tag, ...>` — same type as the operator surface. `IRBuilder` is a zero-size struct
with no state; all methods are `constexpr` and can be used in constant expressions.

---

## Pattern DSL (`vakya::pattern`)

Opt-in via `#include "vakya/pattern.hpp"`. Compile-time structural pattern matching over Vākya nodes — no virtual, no
macros.

```cpp
namespace pat = vakya::pattern;

auto p = pat::add(pat::pv<0>, pat::lit<0>);      // add(?x, 0)
auto e = vakya::make_node<vakya::add_tag>(vakya::as_expr(x), vakya::as_expr(0));
auto m = pat::match_pattern(p, e);
m->has(std::size_t{0});                           // x is bound
```

- `pv<ID>` — wildcard variable (binds the matched subtree)
- `lit<V>` — exact terminal-value match
- builders: `add`, `sub`, `mul`, `div_`, `neg`
- `match_result` — ID → matched-value bindings
- `rule(lhs, rhs)` / `rule("label", lhs, rhs)` — rewrite rules (named form attaches a diagnostic label)
- `make_rule_set(r…)` → `rule_set` with `apply_first` / `apply_all`
- `rules::arithmetic` presets: `add_zero`, `mul_one`, `mul_zero`, `double_neg`

### Non-Linear Pattern Matching

When the same `pv<ID>` appears more than once in a pattern, all occurrences must bind to **structurally equal**
subtrees. The second (and subsequent) occurrence verifies equality against the first-bound value instead of
unconditionally overwriting it.

```cpp
auto double_pat = pat::add(pat::pv<0>, pat::pv<0>);  // x + x

int a = 5, b = 7;
auto same = vakya::make_node<vakya::add_tag>(vakya::as_expr(a), vakya::as_expr(a));
auto diff = vakya::make_node<vakya::add_tag>(vakya::as_expr(a), vakya::as_expr(b));

pat::match_pattern(double_pat, same);  // matches: both slots are same value
pat::match_pattern(double_pat, diff);  // std::nullopt: a ≠ b
```

### Commutative Pattern Matching

For tags that declare `tag_descriptor<Tag>::is_commutative = true` (built-in: `add_tag`, `mul_tag`), the matcher
automatically retries with the operands swapped when the canonical order fails to match.

```cpp
auto p = pat::add(pat::pv<0>, pat::lit<0>);    // x + 0

// Both orderings match:
auto fwd = make_node<add_tag>(as_expr(x), as_expr(0));  // x + 0 → direct match
auto rev = make_node<add_tag>(as_expr(0), as_expr(x));  // 0 + x → commutative retry
pat::match_pattern(p, fwd);  // matches
pat::match_pattern(p, rev);  // also matches (commutative)
```

`sub_tag`, `div_tag`, and all other built-in tags are **not** commutative; only the two orderings you write fire.

```cpp
auto r  = pat::rule("add_zero", pat::add(pat::pv<0>, pat::lit<0>),
                    [](const pat::match_result& m) -> std::optional<std::any> {
                        return m.get<std::any>(std::size_t{0});
                    });
auto rs = pat::make_rule_set(r);
auto out = rs.apply_first(e);                     // fires
```

---

## Property System (`vakya::property`)

Opt-in via `#include "vakya/property.hpp"`. A lazy, typed, external metadata sidecar. Nodes stay POD; per-node metadata
lives in a `property_store` keyed by structural hash — you pay nothing for nodes that carry no metadata.

```cpp
using LineKey = vakya::property_key<int, "src.line">;

vakya::property_store store;
int x = 3, y = 4;
auto e = vakya::as_expr(x) + vakya::as_expr(y);

store.ensure_for(e).set<LineKey>(99);        // attach metadata keyed by structure
auto* ps = store.find_for(e);                // nullptr if none attached
int line = *ps->get<LineKey>();              // 99
```

- `property_key<T, "name">` — compile-time key; `::id` (FNV-1a of name, name-stable + distinct), `::name`,
  `::value_type`
- `basic_property_set<InlineBytes = 24>` — type-erased small-map: inline SBO for trivial payloads up to `InlineBytes`
  bytes, heap-spill + move/destroy op-table for non-trivial ones. Move-only, no virtual. `set` / `get` / `get_if` /
  `has` / `erase` / `size`
- `property_set` — alias for `basic_property_set<24>` (backward-compatible default); use `basic_property_set<N>` for a
  wider inline budget (e.g., 64 bytes for matrix shapes)
- `property_store` — thread-safe (`std::shared_mutex`): `find`/`contains`/`size` acquire a shared lock; `ensure`/
  `update_for`/`clear`
  acquire an exclusive lock. `find(hash)` (zero-cost nullptr when absent), `ensure(hash)`, `ensure_for(expr)` /
  `find_for(expr)` (key by expression structure).

  **Reference-invalidation hazard**: `ensure()` and `ensure_for()` are `[[deprecated]]` — they return a `property_set&`
  valid only until the next concurrent `ensure()` or `clear()` (rehash can invalidate all map references). For
  multi-threaded mutation use `update_for(key, fn)` which holds the exclusive lock for the entire callback:

  ```cpp
  store.update_for(expr, [](vakya::property_set& s) {
      s.set<LineKey>(42);
  });
  ```

---

## Rule Registry (`vakya::rule_registry`)

Opt-in via `#include "vakya/rule_registry.hpp"`. Metadata + discovery layered on top of `pattern::rule_set` — no change
to `pattern.hpp`. Rewrite packs carry a descriptor (id, category, name, version) so they can be listed, filtered, and
applied by name at runtime.

### Architecture

```
pattern::rule          ← one lhs/rhs rewrite pair (+ optional label)
       │
pattern::rule_set      ← ordered set of rules; apply_first / apply_all
       │
vakya::rule_pack<S>    ← rule_set<S> + rule_descriptor (id, category, name, version)
       │
vakya::rule_registry   ← collection of rule_packs, indexed by id + category
```

Each level adds exactly one concern:

- `rule` — the rewrite itself.
- `rule_set` — ordered application policy (`apply_first` stops on first match; `apply_all` applies every matching rule).
- `rule_pack<S>` — binds a `rule_set` to a stable descriptor so it can be discovered and applied by name.
- `rule_registry` — runtime collection; `register_pack` / `by_category` / `discover` / `apply<S>(name, expr)`.

```cpp
auto r = vakya::make_arithmetic_registry();  // pre-loads the four built-in packs

r.discover();                                // list all entries
r.by_category(vakya::rule_category::arithmetic);

int x = 9;
auto e = vakya::as_expr(x) + 0;              // add(x, 0)
using set_t = std::decay_t<decltype(pattern::rules::arithmetic::add_zero)>;
auto out = r.apply<set_t>("arith.add_zero", e);   // drives the registered pack
```

- `rule_descriptor` — POD: `id`, `category`, `name`, `description`, `version` (`rule_version` semver)
- `rule_category` — `arithmetic` / `boolean` / `algebra` / `tensor` / `physics` / `statistics` / `custom` + `to_string`;
  custom ids start at `kRuleExtensionBase` (`1000`)
- `rule_pack<RuleSet>` — `[[no_unique_address]]` wrapper pairing a `rule_set` with its descriptor; `apply_first`
- `rule_registry` — `register_pack` / `register_rules` / `find(id)` / `find(name)` / `by_category` / `discover` /
  `apply<RuleSet>(name, expr)`
- `rule_packs::arithmetic_{add_zero, mul_one, mul_zero, double_neg}` — the built-in packs; `make_arithmetic_registry()`
  loads all four

---

## Type System Stack (opt-in)

All headers below are opt-in and **never pulled by `vakya.hpp`**. They extend the construction surface with
Hindley-Milner type inference, constraint solving, guarded rewriting, validation, and diagnostics. Pull the whole stack
at once with `#include "vakya/vakya_types.hpp"`, or include individual headers as needed.

### Layered Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                        Layer 4: Validation & Rewriting                        │
│  vakya/validation.hpp  (validator<Checks...>)                                │
│  vakya/rewrite.hpp     (guarded_rule<Pattern, Rewrite, Guard>)               │
└──────────────────────────────────────┬───────────────────────────────────────┘
                                       │
┌──────────────────────────────────────▼───────────────────────────────────────┐
│                       Layer 3: Type System & Inference                        │
│  vakya/type_inference.hpp  (Algorithm-W + kosha LRU cache)                   │
│  vakya/type_checking.hpp   (typing_rule<Tag>, type_check, kMaxWalkDepth)     │
│  vakya/unification.hpp     (Robinson MGU, substitution, occurs-check)        │
│  vakya/constraints.hpp     (composite_solver, constraint_kind)               │
│  vakya/types.hpp           (type_arena, type_ref, type_node)                 │
│  vakya/constraint_solvers.hpp (rule/graph/egraph backends)                   │
│  vakya/smt.hpp             (smt_backend concept, no_smt_backend stub,        │
│                             tarka_smt_backend<B> Tarka bridge [opt-in])      │
└──────────────────────────────────────┬───────────────────────────────────────┘
                                       │
┌──────────────────────────────────────▼───────────────────────────────────────┐
│                       Layer 2: Metadata & Pattern Matching                    │
│  vakya/property.hpp       (property_store, SBO property_set, update_for)     │
│  vakya/pattern.hpp        (pv<>, non-linear + commutative matching)          │
│  vakya/rule_registry.hpp  (rule_pack, rule_registry, rule_category)          │
│  vakya/diagnostics.hpp    (collecting_sink, null_sink, nadi_sink)            │
└──────────────────────────────────────┬───────────────────────────────────────┘
                                       │
┌──────────────────────────────────────▼───────────────────────────────────────┐
│                       Layer 1: Core AST Substrate (vakya.hpp)                 │
│  node<Tag, Children...>       interface<Derived> (deducing-this operators)   │
│  Terminal concept & trait     graph::shared_expr & dag_builder (CSE)         │
│  structural_hash/equal/key    tree::* folds    IRBuilder                     │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Header dependency order** (strict DAG, no cycles):

```
vakya/types.hpp
  └─ vakya/unification.hpp
       └─ vakya/constraints.hpp
            └─ vakya/constraint_solvers.hpp
            └─ vakya/type_checking.hpp  ──┐
                 └─ vakya/type_inference.hpp
                 └─ vakya/rewrite.hpp    ──┤ (also pulls pattern.hpp, rule_registry.hpp)
                 └─ vakya/validation.hpp ──┘
                                          ▼
                              vakya/vakya_types.hpp  (umbrella)
```

### Architecture Deep Dive

#### Layer 1 — Core AST Substrate (`vakya.hpp`)

- **Explicit object operators**: `interface<Derived>` uses C++23 deducing-this (`this Self&& self`) to provide all
  arithmetic, comparison, bitwise, and subscript operators without CRTP macro boilerplate.
- **Terminal opt-in**: three priority-ordered mechanisms — `using vakya_terminal = void;` member, `is_terminal<T>`
  specialization, or automatic arithmetic detection.
- **Commutativity**: `tag_descriptor<Tag>::is_commutative` declares commutative tags at compile time; the pattern
  matcher and structural operations read this flag with zero overhead for non-commutative tags.
- **Shared DAG**: `graph::dag_builder` hash-consing turns expression trees into `graph::shared_expr` with `use_count`
  and `topo_order`; structural equality is O (1) handle comparison after interning.

#### Layer 2 — Metadata & Pattern Matching

- **Thread-safe sidecar**: `property_store` uses `std::shared_mutex` — shared lock for reads, exclusive lock for writes.
  `update_for(key, fn)` holds the exclusive lock for the entire closure, preventing reference invalidation from
  `unordered_map` rehashes.
- **SBO property set**: `basic_property_set<InlineBytes>` stores trivial payloads inline (default 24 bytes) with
  type-erased move/destroy thunks for heap-spilled values — no vtable.
- **Deprecated reference returns**: `ensure()` and `ensure_for()` are `[[deprecated]]`; all internal mutation (including
  `type_check_impl`) uses `update_for(key, fn)` which is the only thread-safe mutation path.
- **Non-linear matching**: repeated `pv<ID>` occurrences verify `structural_equal` against the first-bound subtree
  instead of overwriting.
- **Commutative fallback**: when direct match fails on a binary tag with `is_commutative = true`, `do_match_impl`
  retries with operands swapped.

#### Layer 3 — Type System & Inference

- **Handle-based type arena**: types (`τ ::= κ | α | C(τ…) | (τ…→τ) | ∀ᾱ.τ`) are hash-consed in a `type_arena` backed by
  `slot_map`; `type_ref` is a generational handle making structural type equality an O (1) integer comparison.
- **Cached type hash**: `type_node` carries a `mutable std::uint64_t cached_hash = 0` member; `type_hash()` computes
  FNV-1a on first call and caches the result — zero recomputation cost on repeated intern lookups in deep trees.
- **Robinson MGU**: `unify()` with union-find variable chains, occurs-check (prevents infinite types), and full
  let-polymorphism via `generalize` / `instantiate`. `instantiate` uses a C++23 `auto&&` recursive lambda — no
  `std::function` heap allocation. `subst_delta` vectors pre-reserve `children.size() * 2` entries before unification
  loops to eliminate dynamic reallocation for typical type signatures.
- **Algorithm-W**: `infer()` is bottom-up HM, LRU-memoized via kosha cache on `structural_hash` keys.
- **Arity-adaptive typing rules**: `detail::invoke_emit<Tag>` detects at compile time whether a `typing_rule<Tag>::emit`
  specialization accepts 4 or 5 parameters (`substitution&`), keeping older downstream specializations
  binary-compatible.
- **N-ary constraint generation**: `arithmetic_typing_rule::emit_binary` emits `same_type(child[0], child[i])` for every
  `i ≥ 1`, fully constraining variadic arithmetic operations.
- **Modular solver routing**: `composite_solver<Solvers...>` — zero erasure variadic fold. Each constraint routed to the
  first solver returning `handles(kind) = true`.
    - `unification_solver` — `same_type`, `convertible`, `subtype` via `unify()`.
    - `rule_constraint_solver` — semi-naïve fixpoint closure for `implements` / `requires_cap` trait implications.
    - `graph_constraint_solver` — LiteGraph Tarjan SCC for `same_rank` / `broadcastable` / `compatible`; cycle
      diagnostics carry the originating `constraint_ref` from `constraint.source`.
    - `smt_constraint_solver<Backend>` — wraps any `smt_backend`; `no_smt_backend` is a zero-cost deferred stub.
    - `tarka_smt_backend<TarkaBackend>` (opt-in, behind `__has_include(<tarka/tarka.hpp>)`) — bridges any
      `tarka::SmtSolverBackend` into the Vakya `smt_backend` concept. Satisfies `smt_backend` (opaque `smt_formula` path
      is no-op); adds `assert_tarka(Term)` for the native Tarka Term path, plus `lower_term`, `get_value`,
      `push/pop/reset`.
    - `tarka_smt_constraint_solver<TarkaBackend>` — `constraint_solver` specialization that handles
      `constraint_kind::user` and extension kinds (≥ `kConstraintKindExtensionBase`). Reads a `tarka::Term*` from
      `constraint.payload` (caller contract: cast `tarka::Term*` → `uint64_t`). Zero overhead when no such constraints
      are submitted.

#### Layer 4 — Validation & Rewriting

- **Guarded rewriting**: `guarded_rule<Pattern, Rewrite, Guard>` matches structurally then queries the guard (which may
  inspect `type_environment` and a solver) before executing the rewrite. `try_apply(expr)` bypasses the guard for
  backward compatibility.
- **Validation**: `validator<Checks...>` folds a tuple of independent `ValidationCheck` functors; results merged into
  `validation_report`. Zero vtable.

---

```
1. Construct AST Expression (vakya.hpp)
              │
2. Type Inference (type_inference.hpp)  ──or──  Type Check (type_checking.hpp)
              │
     ┌────────┴────────┐
     ▼                 ▼
Post-Order          Generate Constraints
Rule Walk           (same_type / implements / SMT refinement)
(typing_rule<Tag>)
     └────────┬────────┘
              ▼
3. Composite Constraint Solving (constraints.hpp)
   • unification_solver       → unify() / MGU
   • rule_constraint_solver   → trait forward-chaining fixpoint
   • graph_constraint_solver  → Tarjan SCC cycle detection + constraint_ref attribution
   • smt_constraint_solver    → refinement / SAT (no_smt_backend = zero-cost stub)
              │
4. Store Type Results in property_store (property.hpp)
   (Key: TypeResultKey, Val: type_ref)
   • Thread-safe mutation via update_for(expr, fn)
              │
5. Guarded Pattern Rewriting (rewrite.hpp)
   • Structural match (pattern.hpp)
   • Guard: query property_store & type_environment
   • Execute transformation
              │
6. Validation (validation.hpp)
   • validator<Checks...> folds independent checks
   • validation_report::ok()
```

### Header Reference Table

| Header                         | Namespace        | Core Role & Key Structures                                                                                                                                                                                            | Primary Dependencies                                    |
|--------------------------------|------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------|
| `vakya/vakya.hpp`              | `vakya::`        | Core AST (`node<>`), deducing-this operators (`interface<>`), `Terminal` concept, shared DAG (`graph::shared_expr`)                                                                                                   | None (root)                                             |
| `vakya/property.hpp`           | `vakya::`        | Thread-safe metadata sidecar (`property_store`), `update_for`, configurable SBO (`basic_property_set`), `property_key`                                                                                                | `vakya.hpp`                                             |
| `vakya/pattern.hpp`            | `vakya::pattern` | Non-linear matching (`pv<>`), commutative fallback, `rule_set`                                                                                                                                                        | `vakya.hpp`                                             |
| `vakya/rule_registry.hpp`      | `vakya::`        | Runtime rule discovery (`rule_registry`), semver metadata (`rule_descriptor`), domain packs (`rule_pack`)                                                                                                             | `pattern.hpp`                                           |
| `vakya/diagnostics.hpp`        | `vakya::diag`    | `diagnostic`, `diagnostic_sink` concept, `null_sink`, `collecting_sink`, `nadi_sink`                                                                                                                                  | None                                                    |
| `vakya/types.hpp`              | `vakya::types`   | Type terms (`τ`), handle-based interning arena (`type_arena`), generational handles (`type_ref`), canonicalization                                                                                                    | `vakya.hpp`                                             |
| `vakya/unification.hpp`        | `vakya::types`   | Robinson MGU (`unify`), `substitution` (path-splitting union-find), occurs-check, `generalize` / `instantiate`                                                                                                        | `types.hpp`                                             |
| `vakya/constraints.hpp`        | `vakya::types`   | Constraint algebra (`constraint`, `constraint_ref source`), `constraint_solver` concept, `composite_solver`, `unification_solver`                                                                                     | `unification.hpp`                                       |
| `vakya/constraint_solvers.hpp` | `vakya::types`   | Trait forward-chaining (`rule_constraint_solver`), Tarjan SCC with `constraint_ref` attribution (`graph_constraint_solver`), e-graph solver                                                                           | `constraints.hpp`                                       |
| `vakya/smt.hpp`                | `vakya::types`   | `smt_backend` concept, zero-cost stub (`no_smt_backend`), `smt_constraint_solver<Backend>`; opt-in `tarka_smt_backend<B>` Tarka bridge + `tarka_smt_constraint_solver<B>` (behind `__has_include(<tarka/tarka.hpp>)`) | `constraints.hpp`                                       |
| `vakya/type_checking.hpp`      | `vakya::types`   | Scoped name environment (`type_environment`), per-tag rules (`typing_rule<Tag>`), arity-adaptive `detail::invoke_emit`, N-ary constraint generation, depth-protected `type_check()`                                   | `constraints.hpp`, `property.hpp`                       |
| `vakya/type_inference.hpp`     | `vakya::types`   | Algorithm-W HM bottom-up inference (`infer()`), kosha LRU memoization                                                                                                                                                 | `type_checking.hpp`                                     |
| `vakya/rewrite.hpp`            | `vakya::types`   | Semantic-aware guarded rewriting (`guarded_rule`), `always_true_guard` back-compat                                                                                                                                    | `type_checking.hpp`, `pattern.hpp`, `rule_registry.hpp` |
| `vakya/validation.hpp`         | `vakya::types`   | Composable static checks (`validator<Checks...>`), `ValidationCheck` concept, `validation_report`                                                                                                                     | `type_checking.hpp`                                     |
| `vakya/vakya_types.hpp`        | `vakya::types`   | Umbrella header: pulls the complete type, inference, constraint, SMT, and diagnostic stack                                                                                                                            | All type-system headers                                 |

Namespace: `vakya::types` (type system), `vakya::diag` (diagnostics). Zero change to `vakya.hpp`, `pattern.hpp`,
`property.hpp`, or `rule_registry.hpp`.

---

### Type Terms (`vakya::types`)

`#include "vakya/types.hpp"` — type-term representation, interning, and arena.

```cpp
using namespace vakya::types;

type_arena        arena;
type_var_generator gen;

type_ref int_ref  = arena.intern_primitive<integer_type_tag>();
type_ref bool_ref = arena.intern_primitive<bool_type_tag>();
type_ref var      = arena.intern_variable(gen.fresh());

// Constructor type: List<T>
type_ref children[1] = {var};
type_ref list_t = arena.intern_constructor<list_type_tag>(
    std::span<const type_ref>(children, 1));

// Callable: (Int) -> Bool
type_ref params[1] = {int_ref};
type_ref fn = arena.intern_callable(std::span<const type_ref>(params, 1), bool_ref);
```

- `type_ref` — `containers::generational_handle<type_tag, uint32_t>`; `.index` is a **data member** (not a method).
- `type_kind` — `primitive | variable | constructor | callable | quantified | alias`
- `type_descriptor<Ctor>` — per-type tag metadata (`stable_id`, `arity`, `symbol`); extension band
  `>= kTypeKindExtensionBase` (1000).
- `type_arena` — hash-consed DAG; `intern()`, `intern_primitive<Ctor>()`, `intern_variable()`,
  `intern_constructor<Ctor>()`, `intern_callable()`, `intern_quantified()`, `intern_alias()`, `get()`, `canonicalize()`.
- `type_var_generator` — issues fresh `type_var_id` values via `gen.fresh()`.

Generic disjoint-set forest extracted to `include/containers/union_find.hpp` (namespace `containers`):

```cpp
containers::union_find<> uf;
auto a = uf.make_set(), b = uf.make_set();
uf.unite(a, b);
assert(uf.connected(a, b));
```

---

### Unification (`vakya::types`)

`#include "vakya/unification.hpp"` — Robinson mgu with occurs-check.

```cpp
substitution subst;
type_var_id  tid = gen.fresh();  subst.make_var();
type_ref     t   = arena.intern_variable(tid);

// unify(T, Int) → binds T → Int
auto r = unify(t, int_ref, subst, arena);
REQUIRE(r.has_value());
type_ref pruned = prune(t, subst, arena);  // → int_ref
```

- `substitution` — wraps `mutable union_find` (path-splitting is logically const) +
  `unordered_map<type_var_id, type_ref>`; `make_var()`, `bind()`, `walk()`. **Thread isolation**: each compilation
  thread must own its own `substitution` — the type is not thread-safe.
- `unify(a, b, subst, arena) → std::expected<subst_delta, unify_error>` — returns `unify_error_kind::infinite_type`
  on occurs-check failure; `constructor_clash` / `arity_mismatch` / `kind_mismatch` otherwise.
- `subst_delta` — `SmallVector<binding_record, 8>` (no heap for common unary/binary unifications).
- `prune(t, subst, arena)` — follows variable chains; `apply(t, subst, arena)` — deep walk + re-intern.
- `free_vars(t, subst, arena) → unordered_set<type_var_id>`.
- `generalize(t, env_free, subst, arena) → type_ref` — wraps unconstrained vars in `∀`.
- `instantiate(scheme, subst, arena, gen) → type_ref` — freshens quantified vars at each use site. Uses a recursive
  `auto` lambda (no `std::function` overhead).

---

### Constraints & Solvers

`#include "vakya/constraints.hpp"` and `#include "vakya/constraint_solvers.hpp"`.

```cpp
// Constraint algebra
constraint c;
c.kind = constraint_kind::same_type;
c.operands.push_back(t_ref);
c.operands.push_back(int_ref);

// Solver concept + composite routing
composite_solver cs{unification_solver{}, rule_constraint_solver{}, graph_constraint_solver{}};
solve_context ctx{&arena, &subst};
auto r = cs.solve(std::span<const constraint>(&c, 1), ctx);
REQUIRE(r.status == solve_status::solved);
```

- `constraint_kind` — open enum with extension band `>= 1000`: `same_type`, `convertible`, `subtype`, `implements`,
  `same_rank`, `broadcastable`, `compatible`, `requires_cap`, `user`.
- `constraint` — `{kind, operands, trait_name_hash, payload, source}`. `source` is an optional `constraint_ref` back to
  the originating entry in a `constraint_store`; defaults to `constraint_ref{}` when no store is in use. Equality
  comparison is semantic: `source` is intentionally excluded from `operator==`.
- `constraint_solver<S>` concept — requires `s.solve(batch, ctx) → solve_result` and `s.handles(kind) → bool`.
- `composite_solver<Solvers...>` — variadic `[[no_unique_address]]` fold; routes each constraint to the first solver
  that `handles()` it. Zero erasure on the hot path.
- `unification_solver` — wraps `unify()`; handles `same_type`, `convertible`, `subtype`.
- `rule_constraint_solver` — forward-chaining trait closure (semi-naïve); handles `implements`, `requires_cap`, `user`.
- `graph_constraint_solver` — Tarjan SCC via `litegraph::strongly_connected_components(g)`; reports cycles as
  `unsatisfiable`; handles `same_rank`, `broadcastable`, `compatible`. Cycle diagnostics carry the `constraint_ref`
  of the first originating constraint found in the SCC **and** the full cycle path `α_idx1 → α_idx2 → … → α_idx1`
  in the message, enabling debugging even when source locations are absent.
- `egraph_constraint_solver` — guarded by `__has_include("containers/graph/egraph.hpp")`; currently `deferred`.
- `any_solver` — type-erased boundary for tooling/plugin paths only (not the hot path).

---

### Type Checking & Inference

`#include "vakya/type_checking.hpp"` and `#include "vakya/type_inference.hpp"`.

```cpp
// type_check: post-order walk + constraint solving
type_environment env;
property_store   store;
unification_solver solver;
auto vr = type_check(expr, env, solver, arena, gen, subst, store);
REQUIRE(vr.ok());  // validation_status::success

// infer: Algorithm-W HM, kosha-cached
infer_cache_t cache{4096};
auto r = infer(expr, env, subst, arena, gen, cache);
REQUIRE(r.has_value());  // std::expected<type_ref, infer_error>
```

- `type_environment` — scoped name (`uint64_t` hash) → `type_scheme` bindings; `push_scope()` / `pop_scope()`;
  `free_type_vars(subst, arena)`.
- `typing_rule<Tag>` seam — default: fresh var for leaves, first child type for interior nodes. Specialise to add custom
  per-tag constraint generation. Built-in arithmetic tags (`add_tag`, `sub_tag`, …) emit `same_type(child[0],
  child[i])` for every `i >= 1`, fully constraining N-ary or variadic arithmetic tags.
  `emit` may be declared with either a 4-arg signature `(child_types, env, arena, gen)` or the extended 5-arg form
  `(child_types, env, arena, gen, subst)`. Both are supported via `detail::invoke_emit<Tag>` — the call site detects the
  available overload at compile time so older specializations remain binary-compatible.
- `type_check(expr, env, solver, arena, gen, subst, store, max_depth = kMaxWalkDepth) → validation_result` — post-order
  walk, accumulates constraints, solves in one batch. `max_depth` (default `kMaxWalkDepth = 1024`) guards against stack
  overflow on deeply nested auto-generated trees; at depth 0 a fresh unresolved variable is returned.
- `infer(expr, env, subst, arena, gen, cache, max_depth = kMaxWalkDepth) → std::expected<type_ref, infer_error>` —
  incremental Algorithm-W;
  `same_type` constraints resolved per-node; kosha-cached on `structural_hash`. Passes `max_depth - 1` into recursive
  calls.
- Results stored in `property_store` under `TypeResultKey = vakya::property_key<type_ref, "vakya.type_result">`. All
  writes use `update_for(key, fn)` — `ensure()` is deprecated.

`kMaxWalkDepth` is a `constexpr std::size_t` defined in `type_checking.hpp`. Override by passing an explicit
`max_depth`.

---

### Guarded Rewriting

`#include "vakya/rewrite.hpp"`.

```cpp
// Guard: passes only when type environment proves a condition
auto type_guard = [](const match_result& m, type_environment& env, auto& solver) -> bool {
    // inspect m, env, solver...
    return true;
};

// Named guarded rule
auto rule = guarded("a_div_a_to_one",
    pat::div(pat::pv<0>, pat::pv<0>),     // a / a
    [](match_result) { return make_node<one_tag>(); },
    type_guard);

// try_apply with env+solver (guard fires)
auto result = rule.try_apply(expr, env, solver);

// try_apply without env (guard bypassed — backward compat)
auto result2 = rule.try_apply(expr);
```

- `GuardFn<G, Solver>` concept — `guard(match_result, type_environment&, Solver&) → bool`.
- `always_true_guard` — default guard; `guarded_rule` with this guard behaves identically to `pattern::rewrite_rule`.
- `guarded_rule<Pattern, Rewrite, Guard>` — `try_apply(expr, env, solver)` (guard runs) and `try_apply(expr)`
  (guard bypassed, plain structural-match only).
- Factories: `guarded(pat, rhs)`, `guarded(pat, rhs, guard)`, `guarded(name, pat, rhs, guard)`.

---

### Validation

`#include "vakya/validation.hpp"`.

```cpp
// Compose independent check functors
auto v = make_validator(check_well_typed{}, check_no_infinite_loop{});
validation_report report = v.validate(expr);
REQUIRE(report.ok());
```

- `ValidationCheck<C, Expr>` concept — `c(expr) → validation_result`.
- `validator<Checks...>` — variadic `std::tuple` fold; each check runs independently; results merged into
  `validation_report`. Zero vtable.
- `validation_report` — `{overall_status, vector<solver_diagnostic>}`; `merge()`, `ok()`.

---

### Diagnostics (`vakya::diag`)

`#include "vakya/diagnostics.hpp"` — standalone, with no consumer-project dependency.

```cpp
using namespace vakya::diag;

collecting_sink sink;
sink.on_diagnostic(make_error("vakya.unify.clash", "Int cannot unify with Bool"));
REQUIRE(sink.has_errors());
```

- `severity` — `note | info | warning | error | fatal`.
- `diagnostic` — `{level, code, message, optional<source_span>}`.
- `diagnostic_sink<S>` concept — `s.on_diagnostic(const diagnostic&) → void`.
- `null_sink` — zero-cost drop; `collecting_sink` — accumulates to `vector<diagnostic>`;
  `nadi_sink` — guarded by `__has_include("observability/nadi.hpp")`.

---

---

## End-to-End Pipeline Example

Complete walkthrough combining all opt-in subsystems: AST construction, type checking, constraint solving, property
attachment, guarded rewriting.

```cpp
#include "vakya/vakya.hpp"
#include "vakya/property.hpp"
#include "vakya/vakya_types.hpp"

using namespace vakya;
using namespace vakya::types;

void pipeline_example() {
    // ── Step 1: AST construction (vakya.hpp) ──────────────────────────────
    float x_val = 3.14f;
    auto  x     = as_expr(x_val);
    auto  zero  = as_expr(0.0f);
    auto  expr  = x + zero;  // node<add_tag, expr_ref<float>, expr<float>>

    // ── Step 2: Type system init (types.hpp, unification.hpp) ─────────────
    type_arena        arena;
    type_var_generator gen;
    substitution      subst;
    type_environment  env;
    property_store    pstore;

    type_ref float_t = arena.intern_primitive<float_type_tag>();

    // ── Step 3: Composite solver (constraints.hpp, constraint_solvers.hpp) ─
    unification_solver              unifier;
    rule_constraint_solver          trait_solver;
    graph_constraint_solver         graph_solver;
    smt_constraint_solver<no_smt_backend> smt_solver;
    composite_solver solver(unifier, trait_solver, graph_solver, smt_solver);

    // ── Step 4: Type check (type_checking.hpp) ────────────────────────────
    // Generates constraints, solves them, stores inferred type_ref in pstore.
    validation_result vres = type_check(expr, env, solver, arena, gen, subst, pstore);
    assert(vres.ok());

    // Retrieve inferred type thread-safely.
    std::optional<type_ref> inferred;
    pstore.update_for(expr, [&](property_set& ps) {
        inferred = ps.get<TypeResultKey>();
    });
    assert(inferred.has_value());

    // ── Step 5: Guarded pattern rewriting (rewrite.hpp, pattern.hpp) ──────
    namespace pat = vakya::pattern;

    auto float_guard = [&](const pat::match_result& /*m*/,
                            type_environment& /*te*/,
                            auto& /*ts*/) -> bool {
        // Inspect pstore / type_environment for semantic conditions.
        return true;
    };

    auto add_zero_rule = guarded(
        "float_add_zero",
        pat::add(pat::pv<0>, pat::lit<0.0f>),
        [](const pat::match_result& m) { return m.get<std::any>(std::size_t{0}); },
        float_guard);

    auto result = add_zero_rule.try_apply(expr, env, solver);
    assert(result.has_value());
}
```

**Key observations**:

- `composite_solver` accepts any mix of solver backends; routing is zero-cost template dispatch.
- `pstore.update_for` is the safe multi-threaded mutation path; `ensure_for` is single-threaded only.
- `try_apply(expr)` skips the guard (pure structural match); `try_apply(expr, env, solver)` runs the full guard.
- `no_smt_backend` defers all refinement queries — replace with `tarka_smt_backend<tarka::backend::z3_backend>` for
  numeric / refinement types when `<tarka/tarka.hpp>` is available.

---

## Constraint *Reasoning* Layer

> **Status:** implemented. Additive on top of the core stack above — `vakya.hpp`, `pattern.hpp`, `property.hpp`,
> and the existing type-system headers are unchanged; every reasoning layer is an opt-in header that folds to zero size
> when unused.

### From Solving to Reasoning

With **Tarka** (the zero-overhead multi-solver SMT substrate, see [tarka.md](../tarka/tarka.md)) now sitting *below*
Vākya, the type-system evolution shifts from **constraint *solving*** to **constraint *reasoning***: Tarka becomes the
formal reasoning engine underneath, joining the existing unification / rule / graph / egraph backends. The bridge
already exists — `tarka_smt_backend<B>` / `tarka_smt_constraint_solver<B>` in `vakya/smt.hpp`, behind
`__has_include(<tarka/tarka.hpp>)`. This layer does **not** write a new SMT solver; it wires that bridge into a
*registry-routed* engine and adds the surrounding registries, capability/effect systems, analysis store, and reasoning
layers.

Reuse-first (as `union_find` was extracted to `containers/`): the only new *generic* extraction is
`containers/descriptor_registry.hpp`. Everything else composes existing internal libraries — **EasyRules** (traits),
**LiteGraph** (shape/dependency SCC), **egraph** (equivalence), **Tarka** (proof), **Smriti/Kosha** (arena/cache),
application-defined feature routing, **NADI** (telemetry).

### Reasoning Layered Architecture

```
 Structural Representation        vakya.hpp                       (unchanged)
          │
     Type Registry                types/type_registry.hpp         (Phase 1)
          │
      Type System                 types.hpp / unification.hpp     (existing)
          │
   Constraint Registry            constraint_registry.hpp         (Phase 1)
          │
     Constraint Engine            constraints.hpp (extended)      ← descriptor-routed
   ┌────────┬────────┬────────┬──────────┐
   ▼        ▼        ▼        ▼          ▼
 Unify    Rules    Graphs   EGraph    SMT(Tarka)
(exists) (exists) (exists)  (activate)  (bridge exists)
          │
   Semantic Analysis             analysis.hpp                    (Phase 2)
          │
     Analysis Store              analysis_store.hpp              (Phase 2)
          │
  Type-Aware Matching            typed_pattern.hpp               (Phase 2)
          │
    Rewrite System               rewrite.hpp + type_rewrite.hpp  (Phase 2/3)
          │
      Validation                 validation.hpp                  (existing)
          │
  Formal Verification            verify.hpp                      (Phase 4)
          │
  Semantic Query Engine          query.hpp                       (Phase 5)
```

### Registries & Descriptors

The type registry and the constraint registry share one shape — a compile-time descriptor plus a runtime discovery/index
table (exactly the proven `rule_registry` pattern). Extract the core once:

- **`containers::descriptor_registry<Desc>`** — `RegistrableDescriptor<D>` requires `D::stable_id` (builtin `< 1000`,
  ext `>= 1000`), `D::name` (fixed_string NTTP), `D::category`. Store = `containers::slot_map<Desc>` + two
  `SparseSet` indices (`id → handle`, `category → handles`). API: `register / find(id) / find(name_hash) /
  by_category / discover`. An empty registry allocates nothing.
- **`type_registry`** (`types/type_registry.hpp`) — runtime metadata layer over the compile-time
  `type_descriptor<Ctor>` seam so types can be enumerated/named/looked-up (needed by the query + LSP layers). Entry =
  descriptor snapshot + `type_kind` + optional `capability_mask` + optional `effect_mask`. Categories:
  `primitive | tensor | effect | capability | language`. Arena interning stays the hot path; the registry is metadata
  only, keyed by `type_descriptor::stable_id` (no back-pointer).
- **`constraint_registry`** (`constraint_registry.hpp`) — the *one major missing component*. Replaces the linear
  `handles(kind)` scan with declarative routing:

  ```
  Constraint → constraint_descriptor → constraint_registry → solver routing
  ```

  `constraint_descriptor` = `{ constraint_kind kind; fixed_string name; solver_class target; theory_mask theory;
  uint8_t cost_hint; }`, `solver_class ∈ {unify, rule, graph, egraph, smt}`. Builtin seeding:

  | constraint_kind | solver_class | backing library |
      |---|---|---|
  | `same_type`, `convertible`, `subtype` | `unify` | unification (Robinson MGU) |
  | `implements`, `requires_cap` | `rule` | EasyRules forward-chaining |
  | `same_rank`, `broadcastable`, `compatible` | `graph` | LiteGraph Tarjan SCC |
  | `equivalent` / `canonical` (new) | `egraph` | egraph saturation |
  | `user` + ext (`forall`, `refine`, `arith`) | `smt` | Tarka bridge |

  Downstream registers a new ext-band `constraint_kind` + `solver_class` and the engine routes it with **zero code
  change** — plug-and-play.

### Descriptor-Routed Constraint Engine

`composite_solver<Solvers...>` stays the zero-erasure executor; the reasoning layer adds a routing front-end that
partitions a batch by `solver_class` (O (1) `SparseSet` lookup) and runs a cross-class fixpoint:

```
solve_batch(constraints, ctx, registry, solver):
    buckets = group_by(c → registry.find(c.kind).target)
    repeat:
        changed = false
        for class in [unify, rule, graph, egraph, smt]:      # cheap → expensive
            r = solver.solve_class(class, buckets[class], ctx)
            changed |= r.produced_new_facts
            if r.status == unsatisfiable: return unsatisfiable(r.origin)
    until not changed
    return solved
```

- **Why cross-class fixpoint:** a unify binding can unlock a trait rule, which asserts a refinement the SMT solver must
  discharge. Ordering `unify → rule → graph → egraph → smt` runs cheap solvers first; Tarka runs last, only on residual
  obligations (cost-directed via `cost_hint`).
- **Zero-overhead when a class is empty:** empty buckets are skipped; a `composite_solver` without an SMT backend never
  enters the `smt` branch — `no_smt_backend` keeps the build SMT-free.
- **Origin tracking:** each bucket carries `constraint.source` (`constraint_ref`) so an unsat verdict names the
  originating expression — generalizes the existing `graph_constraint_solver` cycle-attribution mechanism.

### Capability & Effect Systems

Capabilities and effects — once plain metadata — become **first-class constraints** in the reasoning layer.

- **`types/capability.hpp`** — `capability_descriptor` (`Read | Write | Network | Execute | Allocate` + ext band).
  `requires_capability(T, Cap)` lowers to the existing `requires_cap` kind: routed to the **rule** solver for simple
  membership, or **Tarka** when path-sensitive (`forall path: has(Write)`). `capability_mask` =
  `SparseSet<capability id>` on a type-registry entry (e.g. `NetworkSocket → {Network}`).
- **`types/effect.hpp`** — `effect_descriptor` (`FileSystem | Memory | IO | …`). A function type carries an
  `effect_mask`; calling it emits an effect *obligation*. Effects aggregate up the call tree via LiteGraph reachability;
  a policy like "every writing path requires `Write`" becomes a Tarka obligation
  `forall p ∈ paths: writes(p) ⇒ has_cap(Write)`. Simple checks stay on EasyRules; quantified ones go to Tarka.

### Analysis Store & Semantic Analysis

- **`analysis_store.hpp`** — a schema'd sidecar (vs. loose `property_store` keys) so downstream reads a schema. Keyed by
  `structural_hash`; value is a fixed `analysis_record`:
  `{ type_ref type; shape_ref shape; effect_mask effects; capability_mask caps; proof_status proofs;
  trait_set traits; feature_vector features; }`. Backing reuses `property_store`'s `shared_mutex` + `update_for`
  discipline verbatim (value type = `analysis_record`, SBO-sized). Unpopulated fields cost nothing.
- **`analysis.hpp`** — drives the `typing_rule` walk, emits type/effect/capability/shape obligations into the constraint
  batch, then persists the solved `analysis_record`:

  ```
  AST → Analysis (typing_rule walk + constraint solve) → analysis_record → analysis_store
  ```

### Type-Aware Matching & Type-Level Rewriting

- **`typed_pattern.hpp`** — additive combinators over `pattern.hpp` (no edit to `pattern.hpp`, same discipline as
  `rule_registry`):
    - `typed<TypeCtor>(inner)` — matches iff `inner` matches **and** the analysis-store type of the bound subtree
      unifies with `TypeCtor`.
    - `trait<Trait>(inner)` — matches iff the bound subtree's type `implements(Trait)`.
    - `typed<Integer>(pv<0>) + lit<0>` is distinct from `typed<Tensor>(pv<0>) + lit<0>` — type-directed rewriting.

  The typed wrapper holds the inner matcher `[[no_unique_address]]` + a compile-time type key; it runs the structural
  match first, then consults the `analysis_store` (or on-the-fly `infer`). Untyped patterns never instantiate the
  wrapper → zero cost.
- **`type_rewrite.hpp`** — rewrites over **types**, proven convergent by egraph. Alias expansion
  (`String ↝ Array<Character>`), tensor normalization (`Tensor<Float,[1,N]> ↝ Vector<Float,N>`), generic simplification
  (`Optional<Optional<T>> ↝ Optional<T>`). Backing = the **exact egraph round-trip Tarka already uses for terms**
  (`intern_into_egraph → saturate → reconstruct_from_egraph`), reused on `type_ref` with sort tracking. The dormant
  `egraph_constraint_solver` is activated for a new `constraint_kind::equivalent`
  (`solver_class = egraph`, still `__has_include`-guarded).

### Shape Algebra

**`types/shape.hpp`** promotes `same_rank`/`compatible`/`broadcastable` into a small formal language:

- `shape<Dims...>` interned in `type_arena` (`shape_type_tag` constructor kind).
- Algebraic rules: `shape<N,M> × shape<M,K> ↝ shape<N,K>`; broadcasting via LiteGraph compatibility edges.
- **Side conditions delegated to Tarka:** dimension proofs `N>0 ∧ M>0 ∧ K>0 ∧ inner==inner` are arithmetic — beyond
  `same_type` — so they become ext-kind SMT obligations routed to the Tarka bridge.

### Formal Verification (Tarka SMT)

Obligations lower to Tarka `Term`s and discharge via the existing `tarka_smt_constraint_solver` — no new solver.

- New ext `constraint_kind`s: `refine` (refinement predicate), `prove` (proof obligation), `arith` (arithmetic). Each
  carries a `tarka::Term*` in `constraint.payload` (existing bridge contract: cast `tarka::Term*` →
  `uint64_t`); routed to `solver_class = smt`.
- Examples: `prove(index < tensor_size)` before an unchecked subscript rewrite; `divide(a,b) ↝ a/b` **requires**
  `prove(b != 0)`; `forall path: writes(path) ⇒ has_cap(Write)`.
- **`verify.hpp`** — `verify(expr, analysis_store, smt_solver) → verification_report` collects `prove/refine/arith`
  obligations from the analysis record, batches them into Tarka, returns `{ proven | refuted(model) | unknown }`
  per obligation (reuses `validation_report` merge; refuted attaches the Tarka counter-model as `SmtValue`).
- **Zero-cost path:** with `no_smt_backend` every obligation resolves to `deferred` — verification degrades to core
  best-effort, build stays SMT-free.

### Refinement Types & Semantic Query Engine

- **Refinement types** — base type + `refine` predicate `Term` (`{ v: Int | v > 0 }`); refinement subtyping = SMT
  implication (`p ⇒ q`) via the bridge. Dependent constraints (value-indexed types) become `arith`/`prove`
  obligations over interned dim/value vars.
- **`query.hpp`** — lazy fluent query over AST + analysis store (for LSP / refactor / static analysis):

  ```
  query(ast)
    .where(type<Integer>())
    .where(effect<FileSystem>())
    .where(capability<Network>())
    .where(proven())
  ```

  Each `.where(pred)` is a compile-time-composed predicate over an `analysis_record`; the pipeline is a lazy range
  adaptor (no allocation, no virtual) folding predicates via `[[no_unique_address]]`. `proven()` reads
  `proof_status`. Its fluent selector pattern is independent of any consumer project.

### Reasoning Header Plan

Additive; the existing strict include DAG (`types → unification → constraints → constraint_solvers →
type_checking → …`) is unchanged — new headers hang off leaves.

```
containers/descriptor_registry.hpp   (generic; usable by any registry)
vakya/types/type_registry.hpp        → types.hpp
vakya/types/capability.hpp           → type_registry.hpp
vakya/types/effect.hpp               → type_registry.hpp
vakya/types/shape.hpp                → types.hpp, constraints.hpp
vakya/constraint_registry.hpp        → constraints.hpp
vakya/analysis_store.hpp             → types/capability.hpp, types/effect.hpp
vakya/analysis.hpp                   → analysis_store.hpp, type_checking.hpp, constraint_registry.hpp
vakya/typed_pattern.hpp              → pattern.hpp, analysis_store.hpp   (does NOT edit pattern.hpp)
vakya/type_rewrite.hpp               → egraph solver (opt-in), types.hpp
vakya/verify.hpp                     → smt.hpp, analysis_store.hpp
vakya/query.hpp                      → analysis_store.hpp
```

`vakya_types.hpp` pulls all reasoning headers; each remains independently includable and zero-cost when unused.

#### Reasoning Header Reference

| Header                               | Namespace              | Core Role                                                                                                                                             |
|--------------------------------------|------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------|
| `containers/descriptor_registry.hpp` | `containers::`         | Generic `descriptor_registry<Desc>` + `RegistrableDescriptor` concept + `desc_name_hash`                                                              |
| `vakya/types/type_registry.hpp`      | `vakya::types`         | Runtime `type_registry_entry` + `type_registry` + `make_builtin_type_registry()`                                                                      |
| `vakya/types/capability.hpp`         | `vakya::types`         | `capability_descriptor`, `capability_mask`, `make_builtin_capability_registry()`                                                                      |
| `vakya/types/effect.hpp`             | `vakya::types`         | `effect_descriptor`, `effect_mask`, `make_builtin_effect_registry()`                                                                                  |
| `vakya/types/shape.hpp`              | `vakya::types`         | `shape_type_tag`, `intern_shape`, `make_matmul_constraints`, `make_broadcastable_constraint`                                                          |
| `vakya/constraint_registry.hpp`      | `vakya::types`         | `constraint_descriptor`, `solver_class`, `constraint_registry`, `solve_batch`, `make_builtin_constraint_registry()`                                   |
| `vakya/analysis_store.hpp`           | `vakya::types`         | `analysis_record`, `proof_status`, `analysis_store` (thread-safe, `update`/`find`/`discover_impl`)                                                    |
| `vakya/analysis.hpp`                 | `vakya::types`         | `analyze()` (mirrors all sub-expr types into `analysis_store`), `analyze_with_registry()` (cross-class fixpoint via `solve_batch`), `analyze_options` |
| `vakya/typed_pattern.hpp`            | `vakya::typed_pattern` | `typed<TypeCtor>(inner)`, `with_trait<TraitId>(inner)` — ext-band traits (≥64) checked via `caps` mask                                                |
| `vakya/type_rewrite.hpp`             | `vakya::types`         | `type_rewrite_rule`, `type_rewrite_engine`, `kEquivalentKind`; opt-in `egraph_type_canonicalize`                                                      |
| `vakya/verify.hpp`                   | `vakya::types`         | `verification_report`, `verify()`, `kRefineKind`/`kProveKind`/`kArithKind`; uses real `tarka::Term*` when `<tarka/tarka.hpp>` available               |
| `vakya/query.hpp`                    | `vakya::query`         | `make_query()`, `type_pred`/`effect_pred`/`capability_pred`/`proven_pred`/`trait_pred`, `query_builder`                                               |

### End-to-End Reasoning Flow

```
1. Construct AST                         vakya.hpp
2. Register types/constraints            type_registry + constraint_registry   (once, opt-in)
3. Semantic analysis                     analysis.hpp
     ├─ typing_rule walk → constraints
     ├─ effect/capability obligations
     └─ shape obligations
4. Constraint ENGINE (registry-routed fixpoint)
     unify → rule(EasyRules) → graph(LiteGraph) → egraph → smt(Tarka)
5. Persist                               analysis_store  (type/shape/effects/caps/proofs/traits)
6. Type-aware matching + rewriting       typed_pattern + type_rewrite (egraph-convergent)
7. Validation                            validation.hpp
8. Formal verification                   verify.hpp  (Tarka discharges prove/refine/arith)
9. Query                                 query.hpp   (LSP / refactor / static analysis)
```

Cheap solvers run first; Tarka runs last, only on residual obligations. Every stage after 1 is opt-in — dropping Tarka
reduces stages 4/8 to `deferred` and the pipeline still type-checks and rewrites as with the core stack.

Domain/ecosystem concerns (domain registry, interactions, conversions, differentiation, algebras, packages, execution
affinity) live in the **Sutra Domain Framework** (`sutra::domain`), a Sutra-owned layer *above* Vākya — see
[docs/sutra/sutra.md](../sutra/sutra.md). Vākya remains pure: AST, patterns, types, constraints, capabilities, effects,
analysis, verification. The layer touches Vākya with exactly one additive field (`analysis_record.domain`, a bare
`uint32` that defaults to the scalar domain) and otherwise references Vākya engines only through non-owning handles.

---

## Semantic *Optimization* Layer

> **Status:** implemented. Additive on top of the core + reasoning stacks — no existing header changes except a
> bounded, trivially-copyable widening of `analysis_record` and additive constraint-registry seeding. Every
> optimization header is opt-in and folds to zero size/cost when unused.

### From Reasoning to Optimization

The reasoning layer moved effects, capabilities, shapes, and refinements into the *reasoning* phase. This layer moves
the remaining **optimization / verification / scheduling facts** into that same phase, so Crank / Pravaha / Medha
inherit *proven* facts instead of re-deriving them. The invariant: **Vākya proves a fact once, at the type level; every
consumer reads it.** Vākya gains **no** downstream dependency — the consumer-side adapters (scheduler pools, IR
mutation) live in the consumer trees; Vākya emits only neutral facts.

Every optimization obligation routes through an *existing* solver: an ext-band `constraint_kind` (`>= 1000`) already
falls to the Tarka SMT bridge with **zero new solver code**, and the registry seeds the cheaper `graph` / `rule` /
`unify` /
`egraph` fast paths where a decision is possible without SMT. With `no_smt_backend`, every optimization proof degrades
to
`deferred` (never a spurious failure) and the build stays SMT-free.

### Widened `analysis_record`

The record stays a trivially-copyable POD (`static_assert(std::is_trivially_copyable_v<analysis_record>)`); every
optimization field is a handle / enum / integer and defaults to null / unknown / 0 (zero-cost when unused). Payloads
live in per-phase side-arenas keyed by these handles.

| field        | type                 | meaning                                  |
|--------------|----------------------|------------------------------------------|
| `region`     | `region_ref`         | aliasing region of this node's value     |
| `effect_row` | `effect_row_ref`     | polymorphic effect row `{concrete \| ρ}` |
| `rw`         | `rw_summary_ref`     | read/write region summary                |
| `state`      | `typestate_id`       | affine typestate protocol state          |
| `simd_width` | `uint16`             | synthesized SIMD lane count (0 = none)   |
| `tile_hint`  | `uint16`             | synthesized loop-tile size (0 = none)    |
| `affinity`   | `execution_affinity` | scheduling hint                          |
| `cost`       | `cost_class`         | compile-time cost lattice band           |
| `cert_id`    | `uint32`             | rewrite_certificate index (0 = none)     |

The handle tags + enums are declared in a minimal `vakya/types/opt_handles.hpp` that `analysis_store.hpp` includes,
keeping the record's dependency surface tiny; full logic lives in the per-phase headers.

### Header Reference Table

| header                        | provides                                                                                                                                         |
|-------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------|
| `vakya/types/opt_handles.hpp` | fwd handle tags (`region_ref`/`effect_row_ref`/`rw_summary_ref`) + enums (`execution_affinity`, `cost_class`, `typestate_id`)                    |
| `vakya/types/region.hpp`      | `region_arena` (root/field/index projection tree, alias-class `union_find`), `regions_syntactically_disjoint`                                    |
| `vakya/alias.hpp`             | `kDisjointKind`, `make_disjoint_constraint`, `may_alias`, `disjoint_solver`                                                                      |
| `vakya/types/effect_row.hpp`  | `effect_row_arena` (`{concrete \| tail}`), `subsumes` (Rémy/Leijen row rule), `kEffectSubKind`, `effect_row_solver`                              |
| `vakya/types/value_param.hpp` | const-generic value params (`value_param_type_tag`), `unify_value`, `kValueEqKind`, `synthesize_simd_width` / `synthesize_tile` (`width_policy`) |
| `vakya/types/typestate.hpp`   | `protocol_descriptor` / `transition` / `check_transition`, `affine_scope` RAII, `kTransitionKind`                                                |
| `vakya/types/rw_summary.hpp`  | `rw_summary_arena`, `predict_conflict`, `kNoConflictKind`, `no_conflict_solver`                                                                  |
| `vakya/exec_affinity.hpp`     | `synthesize_affinity` (folds effect row / rw / cost → hint), `affinity_policy`                                                                   |
| `vakya/cost.hpp`              | `cost_join` (⊔ max monoid), `synthesize_cost` / `synthesize_shape_cost`, `cost_policy`                                                           |
| `vakya/types/refine.hpp`      | `refinement_type_tag` / `intern_refinement`, `syntactic_subtype`, `refine_subtype_obligation` (`kRefineSubKind`), elision bits                   |
| `vakya/proof_carrying.hpp`    | `rewrite_certificate` / `certificate_arena`, `certify_rewrite`, `verified_rewrite_engine`, `kEquivCertKind`                                      |

### Optimization Constraint-Kind Routing

The reasoning layer consumed the ext band offsets +0 (`equivalent`), +1/+2/+3 (`refine`/`prove`/`arith`), +10/+11/+12
(shape dim eq/pos/matmul). The optimization layer claims +20..+26 to avoid all collisions. Each kind is seeded in
`make_builtin_constraint_registry` for a cheap fast-path class; anything the fast path can't decide falls to the SMT
band (Tarka bridge, zero extra code). Kind constants are **defined in their owning headers** (single source of truth);
the registry only routes them.

| offset | kind             | solver_class | fast path / residual                                             |
|--------|------------------|--------------|------------------------------------------------------------------|
| +20    | `disjoint`       | `graph`      | `union_find` root + syntactic disjointness; symbolic index → SMT |
| +21    | `effect_subsume` | `rule`       | bitmask subset + tail rule; distinct symbolic tails → SMT        |
| +22    | `value_eq`       | `unify`      | literal equality / MGU bind; symbolic value → SMT                |
| +23    | `transition`     | `rule`       | `check_transition` table lookup; dynamic state → SMT             |
| +24    | `no_conflict`    | `graph`      | pairwise region disjointness; symbolic pair → SMT                |
| +25    | `refine_sub`     | `smt`        | implication `P ⇒ Q` (Tarka); `no_smt_backend` → deferred         |
| +26    | `equiv_cert`     | `egraph`     | e-class congruence witness; else → SMT                           |

Value-level *type* tags use a separate namespace (`type_descriptor::stable_id` ext band): `value_param_type_tag`
= `kTypeKindExtensionBase + 50`, `refinement_type_tag` = `+51` — no `type_kind` enum edit (users don't pay for a wider
enum they don't use).

### Consumer-Boundary Discipline

The optimization layer writes **neutral facts**; it ships **no** scheduler, no IR mutator, no hardware assumption:

- `synthesize_affinity` emits `io_bound` / `cpu_bound` / `pure` / `sequential`; the adapter mapping `io_bound` to an
  actual async pool is **consumer-side** (Pravaha / Lithe), documented, not shipped.
- `width_policy` / `cost_policy` carry the SIMD width, tile budget, and cost thresholds as **`analyze_options`
  inputs** — no ISA width or magnitude band is hardcoded in the logic; the defaults are portable, documented starting
  points.
- `verified_rewrite_engine` gates application on a `rewrite_policy`: an e-graph witness is proven-by-construction; an
  SMT verdict is proven/refuted/deferred; a `deferred` rewrite is applied only under `allow_deferred` and always flagged
  via `cert_id` so a consumer can re-verify. Refuted rewrites are never applied.
- `refine.hpp` elision bits (`kElisionBoundsCheck` / `kElisionNullCheck` / `kElisionOverflow`) are a bit convention over
  the free `analysis_record::features` vector — Vākya *sets* the bit once a guard is discharged; the consumer *reads* it
  to drop the runtime check.

Vākya stays pure: it keeps no downward dependency and touches no consumer type.

---

## Relationship to the Generic IR

Vākya's `type_arena` is the **HandleStore flavor** of `lang::ir_module` (see `docs/languages/generic.md`).

### Handle alignment

| Concept       | vakya                                                | generic IR                                          |
|---------------|------------------------------------------------------|-----------------------------------------------------|
| Node id       | `type_ref = generational_handle<type_tag, uint32_t>` | `ir_module<…, handle_store<type_tag>>::node_handle` |
| Storage       | `slot_map<type_node, type_ref>`                      | `slot_map` inside `handle_store` specialization     |
| Interning     | `kosha::core::Cache` keyed on `type_hash` (FNV-1a)   | `kosha_dedup_adapter` satisfies generic Dedup seam  |
| Arena backing | `smriti::pools::LinearArena`                         | caller-supplied via policy (retained unchanged)     |

`type_ref` and `ir_module<…, handle_store<type_tag>>::node_handle` are the **same type** — no conversion needed.

### View adapters (non-invasive)

`type_arena` exposes two zero-copy view methods:

```cpp
type_ir_module_view view = arena.as_ir_module_view();
// find(type_ref) — const type_node* (nullptr if stale)
// size()         — live node count
// adj(type_ref)  — SmallVector<type_ref,8> of child handles
// as_egraph_view() / as_adjacency() — return self (already adjacency-capable)

type_ir_module_view egraph_view = arena.as_egraph_view();  // same view
```

The existing `LinearArena + slot_map + kosha` hot path is fully retained — the view holds a `const*` to the arena's
private `store_` and never copies nodes.

### kosha Dedup adapter

`kosha_dedup_adapter` wraps `type_intern_cache_t` so the same LRU cache that drives `type_arena::intern()` satisfies the
generic `Dedup` policy seam:

```cpp
vakya::types::type_intern_cache_t cache{256};
vakya::types::kosha_dedup_adapter adapter{&cache};

// dedup(hash, ref) → first ref seen for this hash; inserts if new
type_ref canonical = adapter.dedup(type_hash(*n), ref);
```

This means frontends that compose with `type_arena` get structural hash-consing for free through the same kosha cache —
no second interner introduced.

### When to use the view vs. the native API

| Need                                              | Recommendation                                                |
|---------------------------------------------------|---------------------------------------------------------------|
| Type inference / unification                      | Use `type_arena` native API (`intern`, `get`, `canonicalize`) |
| Generic IR tooling (egraph, LiteGraph, dominance) | Use `as_ir_module_view()` / `as_egraph_view()`                |
| Structural hash-consing for a new frontend        | Use `kosha_dedup_adapter` over `type_arena`'s cache           |

## Consumer Integration

Pebble owns Vākya independently. A downstream project includes the Vākya headers it needs and may define ADL
`structural_unwrap` overloads for its own transparent wrapper types. Vākya neither includes nor names consumer types.

---

## Master End-to-End Vākya Pipeline Examples

### 1. Structural AST Construction, Hash Deduplication & Folds

```cpp
#include "vakya/vakya.hpp"
#include <iostream>

using namespace vakya;

int main() {
    // 1. Build immutable structural AST nodes
    auto x = var("x");
    auto two = lit(2);
    auto expr1 = add(mul(two, x), lit(5)); // 2 * x + 5
    auto expr2 = add(mul(lit(2), var("x")), lit(5)); // Identical AST

    // 2. Exact Structural Hash Equivalence (O(1) comparison without pointer chasing)
    std::cout << "Expr 1 Hash: " << structural_hash(expr1) << "\n";
    std::cout << "Expr 2 Hash: " << structural_hash(expr2) << "\n";
    assert(structural_hash(expr1) == structural_hash(expr2));

    // 3. Tree Depth Fold
    size_t depth = tree::fold(expr1, [](auto tag, auto&&... child_depths) -> size_t {
        size_t max_c = 0;
        ((max_c = std::max(max_c, child_depths)), ...);
        return 1 + max_c;
    });
    std::cout << "Expression Depth: " << depth << "\n";
}
```

### 2. Declarative Pattern Matching & Algebraic Simplification

```cpp
#include "vakya/vakya.hpp"
#include "vakya/pattern.hpp"
#include "vakya/rewrite.hpp"
#include <iostream>

using namespace vakya;
using namespace vakya::pattern;

int main() {
    // Rule: x * 0 -> 0
    auto x_pat = capture("x");
    auto zero_mul_rule = make_rewrite_rule(
        mul(x_pat, lit(0)),
        [](const MatchContext& ctx) { return lit(0); }
    );

    // Expression: (y + 10) * 0
    auto target = mul(add(var("y"), lit(10)), lit(0));

    // Apply rewrite rule
    auto simplified = apply_rewrite(target, zero_mul_rule);
    std::cout << "Simplified AST: " << to_string(simplified) << "\n"; // Output: "0"
}
```

### 3. Hindley-Milner Type Inference with Tarka SMT Verification

```cpp
#include "vakya/vakya_types.hpp"
#include "vakya/smt.hpp"
#include <iostream>

int main() {
    vakya::types::type_arena arena;
    vakya::types::type_environment env;

    // Register primitive types
    auto t_int  = arena.make_primitive(vakya::types::primitive_kind::i32);
    auto t_bool = arena.make_primitive(vakya::types::primitive_kind::boolean);

    // Create type variables for polymorphic function: fn(x: ?T) -> ?T
    auto var_t = arena.make_variable("T");
    auto fn_type = arena.make_function({var_t}, var_t);

    // Unification of ?T with i32
    vakya::types::unification_solver unifier(arena);
    auto result = unifier.unify(var_t, t_int);
    if (result.has_value()) {
        std::cout << "Unified type variable T to: " << arena.to_string(unifier.resolve(var_t)) << "\n";
    }
}
```
