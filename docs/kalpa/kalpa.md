# Kalpa (कल्प) — Header-Only C++23/26 Mathematical Optimization Library

**Header**: `#include <kalpa/kalpa.hpp>` (core + EDSL) · algorithm families and telemetry are opt-in
**Namespace**: `kalpa`
**Language standard**: C++23/26, header-only, zero-virtual, zero-macro, zero-RTTI

---

## Table of Contents

1. [Overview & Reuse Surface](#1-overview--reuse-surface)
2. [Problem, Solver & the Policy Model](#2-problem-solver--the-policy-model)
3. [Algorithm Catalog](#3-algorithm-catalog)
4. [EDSL / Fluent Front End](#4-edsl--fluent-front-end)
5. [Introspection & the Diagnosis Catalog](#5-introspection--the-diagnosis-catalog)
6. [Opt-In Header Matrix & Zero-Overhead Guarantees](#6-opt-in-header-matrix--zero-overhead-guarantees)
7. [Performance Notes](#7-performance-notes)

---

## 1. Overview & Reuse Surface

Kalpa is a **thin orchestration layer**. It contributes optimization *algorithms* and a *fluent/EDSL
front end*; every heavy numeric kernel is delegated to an existing pebble library. Kalpa owns the
iteration loop, the policy plumbing, and the diagnosis logic — nothing else.

| Concern | Delegated to | Used for |
|:---|:---|:---|
| Dense/iterative linear algebra, factorizations | **`ga`** (`containers/matrix`) | Newton/KKT systems, projections, LP basis |
| Forward-mode autodiff + Hessian-vector products | **`ga::Dual`**, `ga::grad_vec`, `ga::hessian_vec` | gradients, matrix-free curvature |
| 2D feasible projection (geometric domains) | **`akruti::project`** | 2D convex-domain clamps |
| Data-parallel evaluation | **`pravaha`** | parallel finite-diff, population eval |
| Step telemetry | **`nadi`** (`observability`) | per-iteration pulses, zero-overhead default |
| Curvature-pair / factor caching | **`kosha`** | quasi-Newton history, factor reuse |
| Expression graph for the EDSL | **`vakya`** | objective as an algebraic expression |

**Design tenets.** Concepts gate every policy; `[[no_unique_address]]` keeps empty policies free;
static dispatch throughout (no virtual, no CRTP). An unconstrained `Problem` is byte-identical to its
bare objective, and a `Solver` with `NoTelemetry` is the same size as one with no telemetry field.

---

## 2. Problem, Solver & the Policy Model

### `Problem<Objective, Constraints, Domain>`

An aggregate; all three fields are `[[no_unique_address]]`. Build one with `make_problem`:

```cpp
auto prob = kalpa::make_problem<double>(objective);                 // unconstrained
auto prob = kalpa::make_problem<double>(objective, constraints, domain);
```

An **objective** is any callable written generically over the vector element type, so the *same*
callable serves the value pass (`S = double`) and the Dual gradient pass (`S = ga::Dual<double,1>`):

```cpp
struct Rosenbrock {
    template<typename V> auto operator()(const V& x) const {
        using S = typename V::value_type;
        S a = S{1} - x[0], b = x[1] - x[0]*x[0];
        return a*a + S{100}*b*b;
    }
};
```

> **C++ rule.** A local class cannot carry a member template, so an objective with
> `template<typename V> operator()` must live at namespace/file scope, not inside a function body.

### `Derivatives<Mode, T>` — how gradients are obtained

| Mode | Mechanism | Cost |
|:---|:---|:---|
| `Dual` *(default)* | forward-mode AD via `ga::grad_vec`; curvature via `ga::hessian_vec` | exact gradient, no truncation |
| `FiniteDiff` | central differences | `O(N)` objective calls |
| `Analytic` | forwards the user's own gradient | user-supplied |

```cpp
kalpa::Derivatives<kalpa::Dual, double> d;
d.grad(f, x, out);              // ∇f(x) → out
d.hessian_vec(f, x, v, out);    // ∇²f(x)·v → out (matrix-free)
```

### `Solver<Algorithm, Deriv, LineSrch, Stop, Telem>`

```cpp
template<typename Algorithm,
         typename Deriv    = Derivatives<Dual, double>,
         typename LineSrch = Wolfe<double>,
         typename Stop     = DefaultStop<double>,
         typename Telem    = NoTelemetry>
class Solver;
```

`solve(problem, x0)` runs the loop and returns `std::expected<Result<T>, Diagnosis>`:

```cpp
kalpa::Solver<kalpa::LBFGS<double>> s;
auto r = s.solve(prob, x0);
if (r) {                        // has_value → converged / stopped
    r->x; r->f; r->grad_norm; r->iterations; r->status;
} else {
    kalpa::explain(r.error());  // Diagnosis → formatted string
}
```

| `Result<T>` field | Meaning |
|:---|:---|
| `x` | best iterate (`ga::Vector<T>`) |
| `f`, `grad_norm` | objective and `‖∇f‖` there |
| `iterations` | steps taken |
| `status` | `Converged` / `MaxIterations` / `Stalled` |

**Line searches**: `Armijo<T>` (backtracking), `Wolfe<T>` (strong-Wolfe). **Stop**: `DefaultStop<T>`
composes grad-norm / step / Δf / max-iter predicates. Non-default policies are passed to the ctor:

```cpp
kalpa::Armijo<double> ls; ls.alpha0 = 1.0;
kalpa::Solver<kalpa::TrustRegionNewtonCG<double>, kalpa::Derivatives<kalpa::Dual,double>,
              kalpa::Armijo<double>> s{ {}, {}, ls };
```

---

## 3. Algorithm Catalog

### Unconstrained — `#include <kalpa/algo/unconstrained.hpp>`

| Type | Family | Notes |
|:---|:---|:---|
| `GradientDescent<T>` | first-order | steepest descent |
| `Momentum<T>` | first-order | heavy-ball |
| `Adam<T>` | first-order | adaptive moments |
| `ConjugateGradient<T>` | first-order | nonlinear CG (Polak–Ribière) |
| `LBFGS<T>` | quasi-Newton | two-loop recursion, ring-buffer history (`kosha`) |
| `BFGS<T>` | quasi-Newton | dense inverse-Hessian, rank-2 update |
| `Newton<T>` | second-order | `H p = −g` via `ga` factorization |
| `TrustRegionNewtonCG<T>` | second-order | Steihaug-CG, matrix-free `ga::hessian_vec` operator |

### Constrained — `#include <kalpa/algo/constrained.hpp>`

**Domains / projections** (native n-D): `Box{lo,hi}`, `Ball{center,radius}`, `Polytope{A,b}` — each
exposes `project(x)`; 2D geometric domains route to `akruti::project`.

| Type | Solves | Entry |
|:---|:---|:---|
| `ProjectedGradient` | box/ball/polytope-constrained min | as a `Solver` `Algorithm` |
| `FrankWolfe` | conditional-gradient over a domain | `.solve(prob, x0, deriv, lmo)` |
| `EqualityQP` | `min ½xᵀHx+cᵀx s.t. Ax=b` | `.solve(H,c,A,b) → expected<pair<x,λ>>` |
| `SimplexLP` | dense LP (Bland anti-cycling) | `.solve(A,b,c)` |
| `ExactSimplexLP` | exact-rational LP on native `Fraction` arrays | `.solve(A,b,c) → ExactSimplexResult{x,objective,ok}` |

> Exact-rational LP runs on kalpa's own `Fraction` arrays, **not** `Matrix<exact_rational>` (that
> type has no `numeric_limits`/`sqrt`, so it will not instantiate a `ga` matrix).

### Global / derivative-free — `#include <kalpa/algo/global.hpp>`

| Type | Method | Entry |
|:---|:---|:---|
| `NelderMead` | simplex reflection | `.solve(f, x0)` |
| `DifferentialEvolution<T, Eval>` | population DE | `.solve(f, lo, hi, Rng)` |
| `SimulatedAnnealing` | annealed random walk | `.solve(f, x0, Rng)` |
| `CMAES` | covariance adaptation (uses `ga::eig_sym`) | `.solve(f, x0, Rng)` |

`Rng{seed}` is a seedable policy → **deterministic replay** (same seed ⇒ identical trajectory).
`Eval ∈ {SerialEval, ParallelEval}`; `ParallelEval` batches population evaluation through `pravaha`
and is differentially validated against the serial path.

---

## 4. EDSL / Fluent Front End

`#include <kalpa/kalpa.hpp>` · `using namespace kalpa::edsl;`

Write the objective as an algebraic expression over a `vakya` graph instead of a functor. The graph
evaluates over `double` (value) and over `ga::Dual<double,1>` (gradient) unchanged, so it plugs
straight into `Derivatives<Dual>`.

```cpp
auto x    = vars();                                         // x[i] → coordinate i
auto prob = minimize<double>( sq(x[0] - constant(1.0))      // (x−1)² + (y−2)²
                            + sq(x[1] - constant(2.0)) );
kalpa::Solver<kalpa::LBFGS<double>> s;
auto r = s.solve(prob, x0);                                 // → (1, 2)
```

| Builder | Produces |
|:---|:---|
| `vars()` / `var(i)` | coordinate leaf `x[i]` (a wrapped `vakya` terminal) |
| `constant(v)` | constant leaf |
| `sq(e)` | `e*e` (vakya has no `sq`) |
| `+ − * /` | node operators over leaves/sub-expressions |
| `wrap(expr)` | the graph as a callable objective (value / Dual) |
| `minimize<T>(expr)` | an unconstrained `Problem` |

> `var`/`constant` return **wrapped** expressions (`vakya::as_expr`) deliberately: vakya auto-lifts
> plain terminals only for `+`/`*`, so `x[0] - constant(1.0)` needs both operands wrapped for `−`/`/`
> to resolve through the member operator interface.

---

## 5. Introspection & the Diagnosis Catalog

`#include <kalpa/introspect/telemetry.hpp>`

The `Telem` policy observes an `IterState<T>` (`f`, `grad_norm`, `alpha`, `step`, `iter`) once per
iteration. Sinks:

| Sink | Behavior |
|:---|:---|
| `NoTelemetry` *(default)* | empty type; emit compiles away — zero bytes, zero cost |
| `FullTrace<T>` | records every `IterState` in `rows`; `size()`, `back()` |
| `ProgressBar<T>` | TTY progress (`ProgressBar<double>{stderr}`) |
| `Callback<Fn>` via `on_iteration(fn)` | fires a user monitor each iteration (custom stop) |
| `NadiSink<Backend>` | routes pulses to a `nadi` sink backend |

```cpp
auto cb = kalpa::on_iteration([&](const auto& s){ /* s.f, s.grad_norm, s.iter */ });
kalpa::Solver<kalpa::GradientDescent<double>, kalpa::Derivatives<kalpa::Dual,double>,
              kalpa::Armijo<double>, kalpa::DefaultStop<double>, decltype(cb)>
    s{ {}, {}, {}, {}, cb };
```

### Diagnosis

A failed solve returns `std::expected`'s error — a `Diagnosis{ Cause cause; std::string message;
std::size_t iteration; }`. `explain(d)` formats the message plus a remediation `hint:`;
`remediation(cause)` returns the hint alone.

| `Cause` | Meaning | Remediation direction |
|:---|:---|:---|
| `Infeasible` | no feasible iterate | relax / correct the feasible set |
| `Unbounded` | objective decreasing without bound | add a bound |
| `SingularKKT` | KKT / Newton system singular | regularize / reformulate |
| `LineSearchFail` | no step met the descent condition | check gradient / scaling |
| `NaNTrap` | NaN/Inf in `f` or `∇f` | fix domain (e.g. `log` of negative) / start point |
| `NumericalError` | delegated-kernel failure | inspect the underlying `ga` result |

---

## 6. Opt-In Header Matrix & Zero-Overhead Guarantees

| Include | Brings in |
|:---|:---|
| `<kalpa/kalpa.hpp>` | core (`concepts`, `problem`, `solver`) **+** EDSL |
| `<kalpa/algo/unconstrained.hpp>` | GD / Momentum / Adam / CG / L-BFGS / BFGS / Newton / TR-Newton-CG |
| `<kalpa/algo/constrained.hpp>` | projections, ProjectedGradient, Frank–Wolfe, QP, LP, exact LP |
| `<kalpa/algo/global.hpp>` | Nelder–Mead, DE, SA, CMA-ES, `Rng`, eval policies |
| `<kalpa/introspect/telemetry.hpp>` | `FullTrace`, `ProgressBar`, `Callback`, `NadiSink`, diagnosis helpers |
| `<kalpa/kalpa_all.hpp>` | everything above (prototyping) |

**Guarantees** (asserted in `test_introspect.cpp`): `std::is_empty_v<NoTelemetry>` and
`sizeof(Solver<…, NoTelemetry>) < sizeof(Solver<…, FullTrace<T>>)`. Empty policies cost nothing;
unused `Derivatives` modes and unused algorithm templates are never instantiated.

---

## 7. Performance Notes

- **Matrix-free curvature.** `TrustRegionNewtonCG` and Newton-CG paths consume `ga::hessian_vec`
  (two `grad_vec` calls per product), never forming or storing an `N×N` Hessian.
- **Parallel evaluation.** `Derivatives<FiniteDiff>` coordinate perturbations and
  `DifferentialEvolution<…, ParallelEval>` population evaluation fan out through `pravaha`; both are
  differentially cross-checked against the serial path in the tests.
- **Caching.** Quasi-Newton curvature pairs and reusable factorizations are held in `kosha::LRUCache`.
- **Determinism.** Global methods take an explicit seeded `Rng`, so a fixed seed replays an identical
  trajectory — a property the test suite pins.

### Tests

`src/tests/kalpa/` — `test_unconstrained.cpp`, `test_constrained.cpp`, `test_global.cpp`,
`test_edsl.cpp`, `test_introspect.cpp`, and `bench_kalpa.cpp` (scaling; not a CI gate). Each carries an
analytic-optimum oracle; SIMD/parallel paths are checked against their scalar equivalents. The HVP
addition is covered by appended cases in `src/tests/matrix/test_dual.cpp`.
