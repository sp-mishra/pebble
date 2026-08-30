# Tarka — Zero-Overhead Multi-Solver SMT Substrate

**Status:** Implemented. Header-only, C++23, no virtual, no macros.
**Layer:** Leaf substrate *below* Vākya/Lithe — they consume Tarka; Tarka has zero upward dependency.
**Primary backend:** Z3 (already vendored, `HAS_Z3`); all other backends are compile-time-optional stubs behind
`__has_include`.

Tarka owns a value-semantic term algebra (hash-consed AST) and a solver-agnostic dispatch layer. It reuses the existing
internal libraries (`reference.md`) rather than reinventing them: Smriti arenas, Kosha caches, egraph saturation,
`lithe::features`, lock-free queues, and NADI telemetry.

---

## 1. Architecture

```
Value-Semantic Handles        Term (16B), Sort (16B) — trivially copyable, immutable, non-owning
        │
Hash-consed AST / Context     smriti::BumpPool nodes + kosha::ShardedLRUCache structural interning
        │                     (CSE/DAG; verify-on-hit against hash collisions)
        ├───────────────────────────────┬───────────────────────────────┐
        ▼                               ▼                               ▼
Static Solver Facet            Feature Extraction              Equality Saturation
concept SmtSolverBackend       lithe::features seam →          egraph::e_graph over Tarka
(zero-erasure, no vtable)      32-float feature_vector,        Op ids; generic rule packs;
        │                      feature_store (Kosha) cache     extract_best canonicalize
        │                               │
        │                               ▼
        │                       Multi-Stage Router
        │                       capability-mask filter →
        │                       theory rules → adaptive feedback
        ▼                               │
Dynamic C-ABI Boundary  ◄──────────────┘   (opt-in, deferred)
slot_map + generational_handle
        │
        ▼
Async / Portfolio Engine       std::jthread pool + MPMCQueue dispatch;
std::stop_token cancellation;   first-definitive-wins; HazardRegistry reclaim;
C++20 awaitable SmtTask         no-hang fallback when no solver sets a winner
```

**Zero-overhead invariant.** `RouterEngine<backend::z3>` with one backend and portfolio disabled lowers to direct Z3
calls: no type erasure, no atomics, no thread spawn. Async / portfolio / C-ABI are opt-in headers,
`[[no_unique_address]]`-folded away when unused. `tarka.hpp` pulls only the zero-cost core (`term` + `context` +
`backend` concept + `no_solver`).

---

## 2. Term & Sort — value-semantic handles

`Term` and `Sort` are 16-byte, trivially copyable, **non-owning** handles: `{ const Impl* ptr; uint64_t hash; }`. The
redundant `hash` lets router lookups and egraph interning key without a pointer chase. Lifetime is tied to the owning
`Context` (a `Term` is invalid once its `Context` dies).

- **Sort hierarchy:** base `Bool/Int/Real/String`; parameterized `BitVec(width)`, `Array(index,element)`,
  `Function(domain…,range)`. Constructed via
  `make_sort(SortKind, span<const Sort> sort_params = {}, uint32_t scalar_param = 0)` — width is a scalar param;
  array/function use sort params. Sorts are hash-consed; equality is hash/pointer equality.
- **Op enum:** `enum class Op : uint16_t` — builtin band `[0,1000)`, extension band `>= kOpExtensionBase = 1000`. Op
  metadata (arity, symbol, `is_commutative`, `theory_bits`) lives in an openly-specializable
  `tarka::op_descriptor<Op>` — the single source of truth, mirroring `vakya::emit::tag_descriptor`. Downstream theories
  register custom ops by specializing it; no edit to Tarka's enum.
- **Operator building:** `operator&&/==/+/…` recover their `Context` from the operand node (`ptr->ctx`), so handles stay
  16B/trivial and there is no global state.

`static_assert(std::is_trivially_copyable_v<Term> && sizeof(Term)==16)`.

---

## 3. Context — arena + hash-consing (CSE)

`Context` owns `smriti::LinearArena arena_{arena_bytes}` (default configurable, not hardcoded) and a
`kosha::ShardedCache<uint64_t, Impl*>` interning table. Terms form a strict DAG deduplicated by 64-bit structural hash.

- **Collision safety:** on a hash hit the table performs a **structural equality** check (op + sort + children) before
  returning the interned node — the hash alone is never trusted (same discipline as egraph's Robin-Hood hashcons).
  Verified miss → arena bump-allocates the node.
- **Lifetime:** `Context` is non-copyable, movable; the arena supports `checkpoint`/`rollback` for scoped scratch terms.
  Term construction is single-threaded per Context (arena bump is not thread-safe); interned nodes are immutable, so
  cross-thread *reads* during solve need no lock.

---

## 4. Solver facet — static concept

```
concept SmtSolverBackend<B>:
    typename native_term_t, native_sort_t
    make_sort(Sort)            -> native_sort_t
    lower_term(Term)           -> native_term_t
    assert_formula(Term)       -> void
    check_sat()                -> std::expected<SatResult, SmtError>
    get_value(Term)            -> std::expected<SmtValue, SmtError>
    push(uint32_t)/pop(uint32_t)/reset() -> void

concept CancelableBackend<B> : SmtSolverBackend<B> +
    check_sat_cancelable(Term, std::stop_token) -> std::expected<SatResult, SmtError>
```

`RouterEngine` requires `SmtSolverBackend`; `PortfolioEngine` requires `CancelableBackend` (only the portfolio pays for
cancellation). The static path is fully typed (no erasure). A future dynamic **C-ABI** boundary (plain status enums +
out-params, `slot_map`/`generational_handle` slots) is a separate opt-in header, lifted back into the concept by a
`c_abi_backend` adapter — same static/dynamic split Lithe uses.

`SmtValue = variant<bool, bv_value{uint64_t bits; uint32_t width}, int64/bignum, rational, string_view>` — `bv_value` is
the ≤64-bit fast path, spilling to bignum for wider widths. `make_value` overloads on the C++ scalar and validates
against the target `Sort`.

### Backends

- **`native_backend`** (`tarka/backends/native_backend.hpp`, zero external dependencies, header-only):
  Production-grade native DPLL(T) SMT solver built entirely from first principles on Pebble internal algorithms:
  - **Propositional Core (`cdcl_solver.hpp`)**:
    - **2-Watched Literal Scheme**: Zero-scan Boolean constraint propagation ($O(1)$ backtrack time).
    - **1UIP Conflict Analysis & Non-Chronological Backtracking**: Computes first Unique Implication Points to construct optimal asserting conflict clauses.
    - **VSIDS (Variable State Independent Decaying Sum)**: Exponentially decayed variable activity heuristics for branch ordering.
    - **Luby Sequence Restarts & Phase Saving**: Escapes search dead-ends with optimal restart cadence and polar phase persistence.
  - **Theory of Uninterpreted Functions (`theory_uf.hpp` / `egraph.hpp`)**:
    - Congruence closure using Pebble's union-find engine and structural hash-consing.
    - Incremental merge, explain-trail, and deducing equalities across nested functions ($f(x) = f(y)$ if $x = y$).
  - **Theory of Fixed-Size Bit-Vectors (`theory_bv.hpp`)**:
    - Complete Tseitin bit-blaster into native CNF propositions:
      - Full-adder chains for bitwise additions (`BvAdd`, `BvSub`).
      - Booth/Wallace-tree style bitwise multiplication (`BvMul`), unsigned/signed division/modulo (`BvUDiv`, `BvURem`).
      - Bitwise logic (`BvAnd`, `BvOr`, `BvXor`, `BvNot`, `BvShl`, `BvLShr`, `BvAShr`).
      - Sub-vector extraction (`BvExtract`), concatenation (`BvConcat`), sign/zero extensions.
      - Structural word-level equalities and order comparisons (`BvUlt`, `BvSlt`, `BvUle`, `BvSle`).
  - **Linear Real & Integer Arithmetic (`theory_lra.hpp`, `theory_dl.hpp`)**:
    - **Incremental Simplex Tableau**: Slack-variable augmented matrix with exact rational representation (`ExactRational`).
    - **Bland's Anti-Cycling Rule**: Guaranteed termination on degenerate pivots.
    - **Difference Logic (`theory_dl.hpp`)**: Bellman-Ford / Floyd-Warshall negative cycle detection on constraint graphs for $x - y \le k$ fast-path solving.
  - **Theory of Arrays (`theory_array.hpp`)**:
    - Extensional Array Theory ($QF\_AX$): Enforces McCarthy's read-over-write axioms ($Select(Store(A, i, v), i) = v$ and $i \ne j \implies Select(Store(A, i, v), j) = Select(A, j)$) with weak extensionality lemmas.
  - **Nelson-Oppen Multi-Theory Combination (`theory_combination.hpp`)**:
    - Cooperating DPLL(T) architecture: stably infinite theory arrangement, convex and non-convex interface equality propagation, and back-propagated conflict lemmas.
  - **Simplification & Model Validation**:
    - Pre-solve algebraic AST simplification (`simplifier.hpp`).
    - Independent SAT/SMT model formatting and certificate verification (`model_validator.hpp`).
- **`z3_backend`** (primary external, `#if defined(HAS_Z3) && __has_include(<z3++.h>)`): owns `z3::context`+`z3::solver`; lowers
  `Term`→`z3::expr` via post-order walk cached in `ShardedCache<uint64_t, Z3_ast>` (shared-DAG subterms lower once).
  Cancellation via `z3::context::interrupt()` wired to the `stop_token`. Used for differential testing and external oracle verification.
- **`no_solver_backend`** (zero-cost default, always available): every op returns `deferred`/`Unknown`, mirroring
  `vakya::types::no_smt_backend`. `RouterEngine<>` defaults to it, so Tarka builds and its non-solver tests pass even
  with `BUILD_Z3=OFF`.

---

## 5. Feature extraction & router

**Theory lattice.** A formula's *theory signature* is the join of the theories its operators touch, over
`QF_* ⊑ (quantified / non-linear)`. A backend is eligible iff its capability mask ⊒ the formula signature. Routing is
`argmin` over eligible backends of an adaptive cost estimate.

```
route(term):
    sig  = theory_mask(term)                 // from op_descriptor::theory_bits
    fv   = feature_store.get_or_extract(term.hash, theory_extractor)
    elig = [b for b in backends if b.capabilities() ⊇ sig]   // capability filter
    if elig empty: return Unknown/deferred
    return argmin_{b in elig} adaptive_cost(b, sig, fv)       // static weights ⊕ feedback
```

- **Feature vector:** `tarka::features::theory_extractor` satisfies the existing `lithe::features::feature_extractor`
  concept over `Term`, emitting 9 dims: bv/lra/lia/nra/nia ratios, quantifier depth (raw count), array flag, UF flag,
  and DAG compression ratio (`unique_ptr_count / total_walk_visits`; computed via an inline open-addressing visited
  set — 1.0 = tree, <1.0 = DAG sharing). Cached in the existing `lithe::features::feature_store` keyed by `Term.hash()`.
  The vector, SBO, and cache are **reused**, not reinvented.
- **No hardcoded solver names.** Routing is a capability-mask filter over the compile-time backend set; each backend
  exposes `static constexpr theory_mask capabilities()`. With `RouterEngine<z3_backend>` only Z3 survives — the router
  can never select an absent backend. Theory→preference weights live in a tunable `SparseSet<theory_family, weight>`
  side-table.
- **Adaptive feedback (opt-in):** delegates to `lithe::feedback_store` keyed by
  `(theory_family, backend_id, hardware_signature)`. The base router is stateless (static weights); feedback compiles in
  only when its header is included.

### 5.1 Native CDCL core

The native backend's SAT engine (`native/cdcl_solver.hpp`) is a modern CDCL solver built on reused Pebble containers
(no virtual, no macros):

- **Branching — order-heap VSIDS.** Variable selection uses the generic
  `containers::associative::order_heap<Compare>` (max-heap keyed by an external mutable activity array) instead of the
  previous linear `O(V)` scan. `pick_branch_var` pops until it finds an unassigned variable; `bump_var` calls
  `increase`; backjumping re-inserts freed variables. A linear fallback is kept for very small variable counts so tiny
  formulas keep their exact prior behavior.
- **BCP — 2-watched literals + blocking literals.** Each watch stores a `{ClauseRef, Lit blocker}` pair; propagation
  first tests the cached blocker's truth value and skips visiting the clause body when the blocker is already satisfied,
  cutting cache misses on the hot path.
- **Learning — LBD via `SparseSet`.** `compute_lbd` counts distinct decision levels in a learned clause using a
  `sparseset::SparseSet` scratch instead of a 64-bit mask, so the glue value is exact for clauses spanning more than 64
  levels (the old mask silently saturated).
- **Clause DB — LBD tiers + reduce.** Learned clauses are tiered by LBD (core / mid / local); `reduce_db` keeps
  low-LBD glue clauses and gives recently-used clauses a one-round reprieve, and `compact_db` reclaims arena space.
- **Restarts.** Adaptive LBD-EMA (fast/slow exponential moving averages) with a Luby sequence fallback.

---

## 6. Equality saturation (opt-in)

`egraph_opt.hpp` bridges Tarka terms into the generic `egraph::e_graph<size_t,size_t,…>`
`{op_id, child_class_ids, payload_hash}` model without leaking Tarka types into egraph:

- **`intern_into_egraph(graph, Term[, visited[, sort_map*]])`** — post-order: `op_id = size_t(Op)`,
  `payload_hash = structural_payload_hash(node)`. Three overloads: 2-arg (local visited map), 3-arg (caller-provided
  visited map), 4-arg (+ `sort_map*` for sort-typed reconstruction). Passing `sort_map` enables full term reconstruction
  after saturation.
- Reuses the generic `commutativity` / `associativity` / `identity_zero` rule packs parametrized on a Tarka `OpTraits` (
  And/Or/Add/Mul ids); `is_commutative` sourced from `op_descriptor` (no second table).
- **`reconstruct_from_egraph(graph, root, extraction_result, sort_map, ctx)`** — iterative post-order walk over
  `extraction_result.best_nodes`; rebuilds each class's chosen e_node into a typed Tarka `Term` via `ctx.make_term`.
  Returns `std::nullopt` if any class lacks a best node or sort mapping. Sort map keys are re-compressed after
  saturation merges.
- **`egraph_optimize(Term, saturation_config)`** — intern (with sort tracking) → saturate (commutativity +
  associativity + identity_zero) → `extract_best<node_count_cost>` → reconstruct. Falls back to the original term if no
  node-count improvement or reconstruction fails.
- **`egraph_node_count_dag(Term)`** — counts *distinct hash-consed* nodes by walking children through the inline
  `reinterpret_cast<const Term*>(impl+1)` layout and deduplicating `const TermImpl*` in an `unordered_set`. The former
  tree count double-counted every shared subterm, so `after_count ≥ before_count` always held and `egraph_optimize`
  never accepted a rewrite; the DAG count makes the improvement test fire on genuinely smaller terms.

---

## 7. Async & portfolio (opt-in)

- **`SmtTask`** (`async.hpp`): move-only C++20 awaitable — dtor `destroy()`s the frame, `await_ready/suspend/resume`
  compose, resumption driven on the worker pool, result `std::expected<SatResult,SmtError>`, exceptions captured and
  rethrown on `await_resume`.
- **Worker pool:** persistent `std::jthread` pool backed by `containers::lockfree::MPMCQueue<task,N>` (bounded Vyukov).
  Queries are enqueued, not spawned per call.
- **`PortfolioEngine`** (`portfolio.hpp`, competitive `AnySuccess`): launches eligible backends concurrently; the first
  definitive `Sat`/`Unsat` cancels the rest via `stop_src.request_stop()` (Z3 registers an interrupt callback). *
  *No-hang guarantee:** an atomic outstanding-count ensures that if *no* solver sets a winner (e.g. all error or all
  `Unknown`), the last finishing solver resolves the result to the consensus (`Unknown`) — the future is always
  fulfilled. Cancelled solvers' scratch state is reclaimed via `HazardRegistry`. The engine stays **Pravaha-free**:
  Tarka is a leaf substrate with zero upward dependency. A Pravaha task-graph adapter, if needed, belongs in the
  consumer that composes both libraries — never inside Tarka.

---

## 8. Opt-in header matrix

| Header                          | Pulls threads? | Pulls Z3? | Purpose                                                          |
|---------------------------------|----------------|-----------|------------------------------------------------------------------|
| `tarka/tarka.hpp`               | no             | no        | umbrella: term + context + backend concept + no_solver           |
| `tarka/term.hpp`                | no             | no        | Term/Sort/Op handles + `op_descriptor`                           |
| `tarka/context.hpp`             | no             | no        | arena + hash-consing/CSE                                         |
| `tarka/backend.hpp`             | no             | no        | `SmtSolverBackend`/`CancelableBackend` concepts                  |
| `tarka/backends/native_backend.hpp` | no         | no        | full zero-dependency native SMT solver (CDCL, EUF, BV, LRA, AX)  |
| `tarka/backends/no_solver.hpp`  | no             | no        | zero-cost default backend                                        |
| `tarka/backends/z3_backend.hpp` | no             | yes       | Z3 lowering + solve (guarded `HAS_Z3`)                           |
| `tarka/native/simplifier.hpp`   | no             | no        | pre-encoding algebraic AST simplification pass                   |
| `tarka/frontend/ir.hpp`         | no             | no        | frontend-neutral SMT script IR, spans, names, diagnostics         |
| `tarka/frontend/smt2_lexy.hpp`  | yes            | no        | Lexy SMT-LIB2 syntax frontend → shared IR                         |
| `tarka/frontend/smt2_samasa.hpp`| no            | no        | Samasa SMT-LIB2 scanner/event frontend → shared IR                |
| `tarka/frontend/lower_to_tarka.hpp`| no          | no        | shared IR semantic lowering and RouterEngine execution            |
| `tarka/frontend/smt2_printer.hpp`| no            | no        | SMT-LIB2 term, sort, and benchmark script serializer             |
| `tarka/native/model_validator.hpp`| no           | no        | SAT model formatter and independent assertion validator          |
| `tarka/native/theory_quant.hpp` | no             | no        | quantifier instantiation (E-matching & Skolemization)            |
| `tarka/features.hpp`            | no             | no        | theory extractor + capability-mask router                        |
| `tarka/egraph_opt.hpp`          | no             | no        | intern + reconstruct + `egraph_optimize` (sort-typed round-trip) |
| `containers/associative/order_heap.hpp` | no     | no        | generic mutable-key max-heap; backs native CDCL VSIDS branching  |
| `tarka/async.hpp`               | yes            | no        | `SmtTask` + worker pool                                          |
| `tarka/portfolio.hpp`           | yes            | no        | competitive solving (no-hang)                                    |

---

## 9. Usage

```cpp
#include <tarka/tarka.hpp>
#include <tarka/backends/z3_backend.hpp>
#include <tarka/features.hpp>

using namespace tarka;

Context ctx;                                    // owns arena + interning table

auto bv32 = ctx.make_sort(SortKind::BitVec, {}, 32);   // width is a scalar param
auto x    = ctx.make_symbol("x", bv32);
auto y    = ctx.make_symbol("y", bv32);
auto v100 = ctx.make_value(std::uint32_t{100}, bv32);
auto v10  = ctx.make_value(std::uint32_t{10},  bv32);

Term f = (x + y == v100) && (x > v10);          // operators recover Context from nodes

RouterEngine<backend::z3> solver;               // only Z3 in the set → always eligible
solver.assert_formula(f);
if (auto r = solver.check_sat(); r && *r == SatResult::Sat) {
    auto xv = solver.get_value(x);              // std::expected<SmtValue, SmtError>
}
```

`RouterEngine<backend::z3>` — the backend set lists only present backends, so the router never selects an absent solver.

---

## 10. Linkage — Tarka ↔ Vākya

`vakya/smt.hpp` defines `smt_backend`/`no_smt_backend`/`smt_constraint_solver` in `vakya::types`. **Tarka does not touch
it**, and has no include into `vakya/` or `edsl/`, so the existing Vākya→Lithe re-export linkage is unaffected.

`vakya/smt.hpp` also provides (behind `__has_include(<tarka/tarka.hpp>)`, living on the *Vākya* side):

- **`tarka_smt_backend<TarkaBackend>`** — wraps any `tarka::SmtSolverBackend`, satisfies `smt_backend` concept. The
  opaque `smt_formula` path is a no-op; callers use `assert_tarka(Term)` for the native Tarka path. Also exposes
  `lower_term`, `get_value`, `push/pop/reset`.
- **`tarka_smt_constraint_solver<TarkaBackend>`** — `constraint_solver` specialization; handles `constraint_kind::user`
  and extension kinds (≥ `kConstraintKindExtensionBase`) by reading a `tarka::Term*` (cast from `constraint.payload`).
  Zero overhead for other constraint kinds.

**Construction:** both wrappers are **default-constructed in place** — required for backends owning non-movable state (
e.g. `z3_backend` holds a `z3::context`, which z3++ marks non-copyable/non-movable). The by-value ctor that adopts a
preconfigured backend is constrained on `std::movable<TarkaBackend>`, so it participates only for movable backends and
never breaks non-movable ones.

**Dependency direction preserved:** Vākya consumes Tarka; Tarka has zero include into `vakya/`.

```cpp
#include <tarka/tarka.hpp>
#include <tarka/backends/z3_backend.hpp>
#include <vakya/smt.hpp>

// Wire Tarka Z3 backend into the Vakya constraint solver
vakya::types::tarka_smt_constraint_solver<tarka::backend::z3_backend> smt_solver;

// Build a Tarka term and assert it via the Vakya constraint layer
tarka::Context ctx;
tarka::Term f = /* ... */;

// Direct path — assert Tarka term into the bridge
smt_solver.bridge().assert_tarka(f);
auto status = smt_solver.bridge().check_sat();  // solve_status::solved / unsatisfiable / deferred
```

---

## 11. Master End-to-End SMT Solving Examples

### 11.1 BitVector Arithmetic & Overflow Verification
```cpp
#include "tarka/tarka.hpp"
#include "tarka/backends/z3_backend.hpp"
#include <iostream>

int main() {
    tarka::Context ctx;
    auto bv32 = ctx.make_sort(tarka::SortKind::BitVec, {}, 32);

    // Symbols
    auto a = ctx.make_symbol("a", bv32);
    auto b = ctx.make_symbol("b", bv32);
    auto max_val = ctx.make_value(std::uint32_t{0xFFFFFFFF}, bv32);

    // Formula: a > 0 && b > 0 && (a + b < a) [Checking for unsigned 32-bit addition overflow]
    auto zero = ctx.make_value(std::uint32_t{0}, bv32);
    tarka::Term overflow_condition = (a > zero) && (b > zero) && ((a + b) < a);

    tarka::RouterEngine<tarka::backend::z3_backend> solver;
    solver.assert_formula(overflow_condition);

    auto result = solver.check_sat();
    if (result && *result == tarka::SatResult::Sat) {
        std::cout << "BitVector addition overflow is SATISFIABLE!\n";
        auto model_a = solver.get_value(a);
        auto model_b = solver.get_value(b);
        std::cout << "Witness: a = " << model_a->as_uint64() << ", b = " << model_b->as_uint64() << "\n";
    }
}
```

### 11.2 Asynchronous Portfolio Solver Race
```cpp
#include "tarka/tarka.hpp"
#include "tarka/portfolio.hpp"
#include "tarka/backends/z3_backend.hpp"
#include <iostream>

void solve_async_portfolio(tarka::Term formula) {
    // Spawns parallel solvers with cooperative stop tokens; first definitive result wins
    tarka::PortfolioEngine<tarka::backend::z3_backend> portfolio;
    
    portfolio.assert_formula(formula);
    auto future_result = portfolio.check_sat_async();

    // Do other work while solvers race in background...
    auto final_sat = future_result.get();
    if (final_sat == tarka::SatResult::Sat) {
        std::cout << "Portfolio solved: SAT\n";
    } else if (final_sat == tarka::SatResult::Unsat) {
        std::cout << "Portfolio solved: UNSAT\n";
    }
}
```
