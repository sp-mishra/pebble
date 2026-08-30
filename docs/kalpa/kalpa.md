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
| Curvature-pair / factor caching | **`kosha`** | quasi-Newton history, interior-point KKT-factor reuse (`LRUCache`) |
| Expression graph for the EDSL | **`vakya`** | objective as an algebraic expression |
| Structural property tagging | **`vakya::property_store`** | caches linearity/convexity/smoothness so `Auto` picks a method |

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
| `kkt_stationarity` | `‖∇f − Jᵀλ − J_ineqᵀμ‖` — certificate (constrained/NLS; `0` when unset) |
| `primal_infeasibility` | `‖c_eq‖ + Σ max(0, c_ineq)` |
| `complementarity` | `\|μᵀc_ineq\|` (interior-point: `xᵀz`) |
| `dual_infeasibility` | `‖min(0, μ)‖` (dual sign feasibility, `μ ≥ 0` for `≤`) |
| `residual_norm` | `‖r(x)‖` for nonlinear least-squares |
| `multipliers` | `λ`/`μ` when computed; empty `Vector` otherwise |

> The six certificate fields are **defaulted** — the unconstrained path leaves them at `T{}` / an
> empty multiplier vector (no allocation), so they are zero runtime cost when unused. They are filled
> append-only by the constrained (`SQP`, `SQP_Ineq`, `InteriorPoint`) and least-squares (`LM`, `GN`)
> solvers; see §5.

**Line searches**: `Armijo<T>` (backtracking), `Wolfe<T>` (strong-Wolfe, midpoint-bisection zoom),
`MoreThuente<T>` (strong-Wolfe with safeguarded cubic/quadratic interpolation — fewer f/∇f evals,
robust on ill-scaled problems; drop-in for `Wolfe`, same 6-arg signature). **Stop**: `DefaultStop<T>`
composes grad-norm / step / Δf / max-iter predicates. `DefaultStop` also supports
`relative_grad_tol=true`, which scales `grad_tol` once from the initial gradient norm
(`effective_grad_tol = grad_tol * max(1, ||g(x0)||)`) for better behavior across objective scaling.
Non-default policies are passed to the ctor:

```cpp
kalpa::Armijo<double> ls; ls.alpha0 = 1.0;
kalpa::DefaultStop<double> stop; stop.relative_grad_tol = true;
 kalpa::Solver<kalpa::TrustRegionNewtonCG<double>, kalpa::Derivatives<kalpa::Dual,double>,
               kalpa::Armijo<double>> s{ {}, {}, ls, stop };
```

---

## 3. Algorithm Catalog

### Unconstrained — `#include <kalpa/algo/unconstrained.hpp>`

| Type | Family | Notes |
|:---|:---|:---|
| `GradientDescent<T>` | first-order | steepest descent |
| `Momentum<T>` | first-order | heavy-ball |
| `Nesterov<T>` | first-order | accelerated gradient (lookahead: gradient sampled at `x+μv`) |
| `Adam<T>` | first-order | adaptive moments |
| `ConjugateGradient<T>` | first-order | nonlinear CG (Polak–Ribière) |
| `LBFGS<T>` | quasi-Newton | two-loop recursion, ring-buffer history (`kosha`) |
| `BFGS<T>` | quasi-Newton | dense inverse-Hessian, rank-2 update |
| `DFP<T>` | quasi-Newton | dense inverse-Hessian, Davidon–Fletcher–Powell rank-2 update (`sᵀy>0`/`yᵀHy>0` guarded) |
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
| `InteriorPoint` | convex QP/LP with `Ax=b, x≥0` (Mehrotra predictor–corrector) | `.solve(H,c,A,b)`; LP is the `H=0` case |
| `SQP` | equality-constrained NLP (sequential QP) | `.solve(f, x0, deriv, cons)` |
| `SQP_Ineq` | inequality+equality NLP (active-set inner QP, SLSQP-grade) | `.solve(f, x0, deriv, c_eq, c_ineq)` |
| `AugmentedLagrangian` | equality-constrained NLP (method of multipliers) | `.solve(f, x0, deriv, cons, inner)` |
| `SimplexLP` | dense LP (Bland anti-cycling) | `.solve(A,b,c)` |
| `ExactSimplexLP` | exact-rational LP on native `Fraction` arrays | `.solve(A,b,c) → ExactSimplexResult{x,objective,ok}` |

> **Delegation.** `InteriorPoint` forms the reduced KKT with a negated `(1,1)` block and solves each
> predictor/corrector step through `ga::schur_solve(…, SymIndefinite)`; the symbolic factor is reused
> across the two solves per iteration and may be cached in a `kosha::LRUCache`. On a pure LP (`H=0`)
> a converged interior iterate sits at the analytic centre of the optimal face; a final **crossover**
> (basis identification, toggle `crossover`) purifies it to a basic optimal vertex — inert for
> strictly-convex QPs. `SQP`'s QP subproblem
> is delegated to `EqualityQP` (KKT via `ga::schur_solve`), with a matrix-free Lagrangian-Hessian
> operator and Jacobian rows from `Derivatives`; steps are accepted by an ℓ₁-merit backtrack.
> `AugmentedLagrangian` wraps `f − Σλᵢcᵢ + (ρ/2)Σcᵢ²` into a `Problem` and hands it to the supplied
> **`inner`** unconstrained `Solver` (e.g. `Solver<LBFGS<double>>`), then updates `λᵢ −= ρ·cᵢ` and
> grows `ρ` while the constraint norm stalls. `SQP`/`AugmentedLagrangian` take the constraint set as a
> random-access container of objective-style functors (e.g. `std::vector<Con>`).

> **`SQP_Ineq`** solves `min f s.t. c_eq(x)=0, c_ineq(x) ≤ 0` (convention `c ≤ 0`, matching the EDSL
> `subject_to` residual). Each major step builds the QP `min ½pᵀWp + gᵀp s.t. J_eq p = −c_eq,
> J_ineq p ≤ −c_ineq` and solves it with an **active-set loop** wrapping `EqualityQP` (`ga::schur_solve`):
> the working set is all equality rows plus the currently-active inequality rows; a row is added when a
> step would violate it (ratio test), dropped when its multiplier goes negative. `W` is a **damped-BFGS**
> approximation of the *Lagrangian* Hessian (Powell damping keeps it SPD → every inner QP is convex).
> Steps are accepted by an ℓ₁-merit backtrack with an inequality term `Σ max(0, c_ineq)`, and the KKT
> certificate (stationarity / primal infeasibility / complementarity / dual infeasibility / multipliers)
> is written to the `Result`.

### Nonlinear least-squares — `#include <kalpa/algo/least_squares.hpp>`

Minimize `½‖r(x)‖²` for a residual vector `r(x)` with components `r_i(x)`. A distinct problem shape
from the `Solver`-Algorithm loop, so it has its own opt-in header and its own drivers. Residuals are a
random-access container of Dual-callable scalar functors `r_i(x)` (the same shape `SQP` takes).

| Type | Solves | Entry |
|:---|:---|:---|
| `LevenbergMarquardt<T, JacEval>` | damped Gauss–Newton (`(JᵀJ + λ·diag(JᵀJ))p = −Jᵀr`) | `.solve(residuals, x0, deriv)` |
| `GaussNewton<T, JacEval>` | undamped least-squares (`J p = −r`) | `.solve(residuals, x0, deriv)` |

> **Delegation.** `LevenbergMarquardt` forms the SPD normal equations with Marquardt diagonal scaling
> (which keeps the matrix SPD so Cholesky never fails) and solves each step through
> `ga::solve(…, SPD)`; a trust-ratio adapts `λ` (shrink on a successful step → Gauss–Newton, grow on a
> rejected one → steepest descent). `GaussNewton` solves the least-squares system directly through
> `ga::qr` / `ga::qr_solve` (the robust, `λ`-free path; requires `m ≥ n`). The Jacobian is built row by
> row from `Derivatives` (forward-mode AD) via `detail::jacobian`; the `Result` reports
> `f = ½‖r‖²`, `grad_norm = ‖Jᵀr‖`, and `residual_norm = ‖r‖`.
>
> **`JacEval ∈ {SerialJacobian, ParallelJacobian}`.** `SerialJacobian` (default) is zero-overhead;
> `ParallelJacobian` fans the row fill out through `pravaha` (the `m` residual gradients are independent,
> so the rows never race). Parallelism is applied **only** to the Jacobian — the normal-equation/QR
> solve, ratio test, and residual evaluation are cheap relative to `m` AD gradient passes — and is
> differentially validated against the serial path in the tests.

### Global / derivative-free — `#include <kalpa/algo/global.hpp>`

| Type | Method | Entry |
|:---|:---|:---|
| `NelderMead` | simplex reflection | `.solve(f, x0)` |
| `DifferentialEvolution<T, Eval>` | population DE | `.solve(f, lo, hi, Rng)` |
| `SimulatedAnnealing` | annealed random walk | `.solve(f, x0, Rng)` |
| `CMAES` | covariance adaptation (uses `ga::eig_sym`) | `.solve(f, x0, Rng)` |
| `BayesianOptimization<T, Eval>` | GP surrogate + acquisition (`Acquisition::{ExpectedImprovement, LowerConfidenceBound}`) | `.solve(f, lo, hi, Rng)` |

`Rng{seed}` is a seedable policy → **deterministic replay** (same seed ⇒ identical trajectory).
`Eval ∈ {SerialEval, ParallelEval}`; `ParallelEval` batches population evaluation through `pravaha`
and is differentially validated against the serial path.

> **`BayesianOptimization`** fits a Gaussian-process surrogate over a box `[lo,hi]`: an RBF-kernel Gram
> matrix `K + noise·I` is factored once per outer step (`α = ga::solve(K, y, SPD)`), the posterior
> `μ,σ²` scores a seeded candidate pool, and the maximal-acquisition candidate is evaluated next. Knobs:
> `init_samples`, `max_iter`, `cand_pool`, `length_scale`, `signal_var`, `noise`, `beta`, `acq`. It is
> sample-efficient rather than high-precision; the returned point is the best *observed* sample.

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

### Constraints — `subject_to`

Comparison operators over EDSL expressions build **signed residual** functors. Each comparison lowers
to a residual `g(x)` with the convention *feasible ⇔ `g(x) ≤ 0`* (equalities: `g(x) = 0`):

| Written | Relation | Residual `g(x)` |
|:---|:---|:---|
| `lhs == rhs` | `Eq` | `lhs − rhs` (zero on the surface) |
| `lhs <= rhs` | `Le` | `lhs − rhs` |
| `lhs >= rhs` | `Ge` | `rhs − lhs` |

```cpp
auto x  = vars();
auto cs = subject_to( (x[0] + x[1]) == constant(2.0),      // ConstraintSet<…>
                      x[0] >= constant(0.0) );
cs.count();               // number of constraints (2)
cs.residual(i, x);        // signed residual of constraint i at x
cs.feasible(x, tol);      // all residuals ≤ tol
```

`subject_to(cs...)` returns a `ConstraintSet<Cs...>` (a tuple of `ConstraintExpr`s); each
`ConstraintExpr` carries its `Relation` statically and reads its two operands from the vakya graph
children. `constrained_problem(expr, cons)` pairs an objective expression with a constraint set.

### `Auto` — property-driven method selection

`Auto<T>` inspects the objective graph, tags it in a `vakya::property_store`, and dispatches to a
concrete solver — the user does not name an algorithm:

```cpp
auto prob = minimize<double>( sq(x[0]-constant(1.0)) + sq(x[1]-constant(2.0)) );
kalpa::edsl::Auto<double> automatic;      // owns a property_store (move-only)
auto r = automatic.solve(prob, x0);       // analyses, selects, runs
```

A single structural walk records `linear`, `convex`, `smooth`, and `dim` into the store (keyed by the
expression's structural hash, so repeat analyses of the same shape are cache hits). `choose(expr,
store)` maps the analysis to a `MethodChoice`:

| Analysis | Choice |
|:---|:---|
| non-smooth | `CMAES` |
| convex **and** `dim ≤ 20` | `Newton` |
| otherwise (incl. non-convex / higher-dim smooth) | `LBFGS` |

Tagging rules: `+`/`−` preserve child properties; `*` clears `linear`; `/` clears both `linear` and
`convex`; a `Var` leaf raises `dim`.

> The EDSL grammar currently exposes only `+ − * /` (no `abs`/`max`/nonsmooth op), so every graph is
> smooth and the `CMAES` branch is **presently unreachable** from `subject_to`/`minimize` — it is a
> documented forward path for when a nonsmooth node is added to the grammar.

---

## 5. Introspection & the Diagnosis Catalog

`#include <kalpa/introspect/telemetry.hpp>`

The `Telem` policy observes an `IterState<T>` (`f`, `grad_norm`, `alpha`, `step`, `iter`) once per
iteration. Sinks:

| Sink | Behavior |
|:---|:---|
| `NoTelemetry` *(default)* | empty type; emit compiles away — zero bytes, zero cost |
| `FullTrace<T>` | records every `IterState` in `rows`; `size()`, `back()` |
| `SparseTrace<T>` | configurable sparse recording (`stride`, `min_rel_drop`) for long runs |
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
| `<kalpa/algo/constrained.hpp>` | projections, ProjectedGradient, Frank–Wolfe, QP, LP, exact LP, `SQP`, `SQP_Ineq`, ALM, InteriorPoint |
| `<kalpa/algo/least_squares.hpp>` | `LevenbergMarquardt`, `GaussNewton`, `SerialJacobian` / `ParallelJacobian` (also in `kalpa_all.hpp`) |
| `<kalpa/algo/global.hpp>` | Nelder–Mead, DE, SA, CMA-ES, `Rng`, eval policies |
| `<kalpa/introspect/telemetry.hpp>` | `FullTrace`, `SparseTrace`, `ProgressBar`, `Callback`, `NadiSink`, diagnosis helpers |
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
- **Parallel Jacobian (least-squares, opt-in).** `LevenbergMarquardt` / `GaussNewton` take a
  `JacEval` policy (`SerialJacobian` default → zero-overhead; `ParallelJacobian` fans the `m`
  independent residual-gradient rows out through `pravaha`). Parallelism is applied **only** to the
  Jacobian fill — the normal-equation / QR solve, ratio test, and residual sweep are cheap next to `m`
  AD passes — and the parallel path is validated against the serial one in `test_least_squares.cpp`.
- **Caching.** Quasi-Newton curvature pairs and reusable factorizations are held in `kosha::LRUCache`.
- **Line-search warm-start.** Solver reuses the last accepted step length as the next line-search
  initial trial (when the line-search policy exposes the warm-start overload), typically reducing
  backtracking/zoom evaluations on smooth local phases.
 - **Determinism.** Global methods take an explicit seeded `Rng`, so a fixed seed replays an identical
   trajectory — a property the test suite pins.

### Tests

`src/tests/kalpa/` — `test_unconstrained.cpp`, `test_constrained.cpp`, `test_least_squares.cpp`,
`test_global.cpp`, `test_edsl.cpp`, `test_introspect.cpp`, and `bench_kalpa.cpp` (scaling; not a CI
gate — now including LM and `SQP_Ineq` timings). Each carries an analytic-optimum oracle; SIMD/parallel
paths (including the opt-in parallel Jacobian) are checked against their scalar equivalents. The HVP
addition is covered by appended cases in `src/tests/matrix/test_dual.cpp`.
