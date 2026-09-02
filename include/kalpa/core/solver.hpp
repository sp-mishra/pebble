#pragma once
// ============================================================================
// kalpa/core/solver.hpp — the solve driver, results, stop criteria, line search
// ============================================================================
// Solver<Algorithm, LineSearch, Stop, Telemetry> ties the policies together and
// runs the descent loop. It is concept-bounded and CRTP-free; empty policies
// (NoTelemetry, the stateless line searches) are stored [[no_unique_address]].
//
// The Algorithm policy owns the "which direction next" decision (steepest
// descent, CG, L-BFGS, Newton, …). The Solver owns everything shared: gradient
// evaluation (via the Problem's Derivatives), the line search, domain
// projection, telemetry, stop testing, and result/diagnosis assembly.
//
// Result is returned as std::expected<Result<T>, Diagnosis> — a converged or
// budget-limited run yields a Result; a hard failure (NaN, line-search collapse,
// infeasible start) yields a Diagnosis explaining why and where.
// ============================================================================

#ifndef PEBBLE_KALPA_CORE_SOLVER_HPP
#define PEBBLE_KALPA_CORE_SOLVER_HPP

#include <kalpa/core/concepts.hpp>
#include <kalpa/core/problem.hpp>
#include <containers/matrix/dense.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <string>
#include <utility>

namespace kalpa {
    // =======================================================================
    // Status / Diagnosis
    // =======================================================================
    enum class Status {
        Converged, // stop criterion satisfied
        MaxIterations, // ran out of iteration budget
        Stalled, // step/objective progress below threshold
    };

    enum class Cause {
        Infeasible, // starting point / iterate could not be made feasible
        Unbounded, // objective decreasing without bound
        SingularKKT, // KKT / Newton system singular
        LineSearchFail, // no step satisfied the descent condition
        NaNTrap, // NaN/Inf encountered in f or ∇f
        NumericalError, // catch-all delegated-kernel failure
    };

    struct Diagnosis {
        Cause cause;
        std::string message; // human-readable, names the offending quantity
        std::size_t iteration{0};
    };

    // =======================================================================
    // Result<T>
    // =======================================================================
    template <typename T>
    struct Result {
        ga::Vector<T> x; // best iterate found
        T f{}; // objective there
        T grad_norm{}; // ‖∇f‖ there
        std::size_t iterations{0};
        Status status{Status::MaxIterations};

        // ---- convergence certificate (constrained / least-squares) --------
        // All defaulted → the unconstrained path leaves them at T{} / an empty
        // multiplier vector (no allocation), so they are zero runtime cost when
        // unused. Populated append-only by the constrained and NLS solvers.
        T kkt_stationarity{}; // ‖∇f − Jᵀλ − J_ineqᵀμ‖ (0 when unset)
        T primal_infeasibility{}; // ‖c_eq‖ + ‖max(0, c_ineq)‖
        T complementarity{}; // |μᵀ c_ineq|
        T dual_infeasibility{}; // ‖min(0, μ)‖ (μ ≥ 0 required for cᵢ ≤ 0)
        T residual_norm{}; // ‖r(x)‖ for nonlinear least-squares
        ga::Vector<T> multipliers{}; // λ / μ when computed; empty otherwise
    };

    // =======================================================================
    // Iteration state — what Algorithm / LineSearch / Stop / Telemetry observe
    // =======================================================================
    template <typename T>
    struct IterState {
        ga::Vector<T> x; // current iterate
        ga::Vector<T> g; // current gradient
        ga::Vector<T> dir; // last search direction taken
        T f{}; // current objective
        T grad_norm{};
        T step{}; // last accepted step length ‖x_k − x_{k−1}‖
        T alpha{}; // last line-search step size
        std::size_t iter{0};
    };

    // =======================================================================
    // Stop criteria
    // =======================================================================
    template <typename T>
    struct DefaultStop {
        T grad_tol{static_cast<T>(1e-8)};
        T step_tol{static_cast<T>(1e-12)};
        std::size_t max_iter{2000};
        bool relative_grad_tol{false};
        T grad_scale{T{1}};

        // Optional one-time scaling from the starting gradient norm.
        void init(const IterState<T>& s) {
            grad_scale = relative_grad_tol ? std::max(T{1}, s.grad_norm) : T{1};
        }

        [[nodiscard]] T effective_grad_tol() const {
            return grad_tol * grad_scale;
        }

        [[nodiscard]] bool done(const IterState<T>& s) const {
            if (s.grad_norm <= effective_grad_tol()) return true;
            if (s.iter > 0 && s.step <= step_tol) return true;
            if (s.iter >= max_iter) return true;
            return false;
        }

        [[nodiscard]] bool converged(const IterState<T>& s) const {
            return s.grad_norm <= effective_grad_tol() || (s.iter > 0 && s.step <= step_tol);
        }
    };

    // =======================================================================
    // Telemetry — zero-overhead default
    // =======================================================================
    struct NoTelemetry {
        template <typename State>
        void record(const State&) const noexcept {}
    };

    // =======================================================================
    // Line searches
    // =======================================================================
    namespace detail {
        template <typename V>
        typename V::value_type dot(const V& a, const V& b) {
            using T = typename V::value_type;
            T s{};
            for (std::size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
            return s;
        }

        template <typename V>
        typename V::value_type nrm2(const V& a) {
            return std::sqrt(dot(a, a));
        }

        // Jacobian of a residual/constraint set: row i = ∇residuals[i](x).
        // residuals is random-access + Dual-callable (like SQP's `cons`); J is
        // (m × n), filled row-wise. Serial — the parallel variant lives in
        // least_squares.hpp so the base solver header stays free of pravaha.
        template <typename Deriv, typename ConSet, typename T>
        void jacobian(const Deriv& deriv, const ConSet& residuals,
                      const ga::Vector<T>& x, ga::Matrix<T>& J) {
            const std::size_t m = residuals.size();
            const std::size_t n = x.size();
            ga::Vector<T> row(n);
            for (std::size_t i = 0; i < m; ++i) {
                deriv.grad(residuals[i], x, row);
                for (std::size_t j = 0; j < n; ++j) J(i, j) = row[j];
            }
        }
    } // namespace detail

    // Backtracking Armijo (sufficient decrease). Returns accepted α (0 on fail).
    template <typename T>
    struct Armijo {
        T c1{static_cast<T>(1e-4)};
        T shrink{static_cast<T>(0.5)};
        T alpha0{T{1}};
        std::size_t max_backtracks{50};

        template <typename F, typename V>
        [[nodiscard]] T search(const F& f, const V& x, const V& dir,
                               T fx, const V& g) const {
            return search(f, x, dir, fx, g, alpha0);
        }

        template <typename F, typename V>
        [[nodiscard]] T search(const F& f, const V& x, const V& dir,
                               T fx, const V& g, T alpha_init) const {
            const T gTd = detail::dot(g, dir);
            if (gTd >= T{0}) return T{0}; // not a descent direction
            T alpha = (std::isfinite(alpha_init) && alpha_init > T{0}) ? alpha_init : alpha0;
            V trial(x.size());
            for (std::size_t k = 0; k < max_backtracks; ++k) {
                for (std::size_t i = 0; i < x.size(); ++i) trial[i] = x[i] + alpha * dir[i];
                const T ft = f(trial);
                if (ft <= fx + c1 * alpha * gTd) return alpha;
                alpha *= shrink;
            }
            return T{0};
        }
    };

    // Strong-Wolfe line search (bisection zoom). Needs the gradient provider to
    // evaluate ∇f at trial points; hence templated on the Derivatives policy.
    template <typename T>
    struct Wolfe {
        T c1{static_cast<T>(1e-4)};
        T c2{static_cast<T>(0.9)};
        T alpha0{T{1}};
        T alpha_max{static_cast<T>(1e3)};
        std::size_t max_iter{50};

        template <typename F, typename Deriv, typename V>
        [[nodiscard]] T search(const F& f, const Deriv& deriv,
                               const V& x, const V& dir, T fx, const V& g) const {
            return search(f, deriv, x, dir, fx, g, alpha0);
        }

        template <typename F, typename Deriv, typename V>
        [[nodiscard]] T search(const F& f, const Deriv& deriv,
                               const V& x, const V& dir, T fx, const V& g,
                               T alpha_init) const {
            const std::size_t n = x.size();
            const T phi0 = fx;
            const T dphi0 = detail::dot(g, dir);
            if (dphi0 >= T{0}) return T{0}; // not a descent direction

            auto eval = [&](T a, T& phi, T& dphi) {
                V trial(n), gt(n);
                for (std::size_t i = 0; i < n; ++i) trial[i] = x[i] + a * dir[i];
                phi = f(trial);
                deriv.grad(f, trial, gt);
                dphi = detail::dot(gt, dir);
            };

            T a_prev = T{0}, phi_prev = phi0;
            T a = (std::isfinite(alpha_init) && alpha_init > T{0}) ? alpha_init : alpha0;
            for (std::size_t it = 0; it < max_iter; ++it) {
                T phi, dphi;
                eval(a, phi, dphi);
                if (phi > phi0 + c1 * a * dphi0 || (it > 0 && phi >= phi_prev))
                    return zoom(f, deriv, x, dir, phi0, dphi0, a_prev, a);
                if (std::abs(dphi) <= -c2 * dphi0) return a;
                if (dphi >= T{0}) return zoom(f, deriv, x, dir, phi0, dphi0, a, a_prev);
                a_prev = a;
                phi_prev = phi;
                a = std::min(a * T{2}, alpha_max);
            }
            return a;
        }

    private:
        template <typename F, typename Deriv, typename V>
        [[nodiscard]] T zoom(const F& f, const Deriv& deriv, const V& x, const V& dir,
                             T phi0, T dphi0, T alo, T ahi) const {
            const std::size_t n = x.size();
            auto eval = [&](T a, T& phi, T& dphi) {
                V trial(n), gt(n);
                for (std::size_t i = 0; i < n; ++i) trial[i] = x[i] + a * dir[i];
                phi = f(trial);
                deriv.grad(f, trial, gt);
                dphi = detail::dot(gt, dir);
            };
            for (std::size_t it = 0; it < max_iter; ++it) {
                const T a = T{0.5} * (alo + ahi);
                T phi, dphi;
                eval(a, phi, dphi);
                T philo, dlo;
                eval(alo, philo, dlo);
                if (phi > phi0 + c1 * a * dphi0 || phi >= philo) ahi = a;
                else {
                    if (std::abs(dphi) <= -c2 * dphi0) return a;
                    if (dphi * (ahi - alo) >= T{0}) ahi = alo;
                    alo = a;
                }
                if (std::abs(ahi - alo) < static_cast<T>(1e-14)) return a;
            }
            return T{0.5} * (alo + ahi);
        }
    };

    // Moré–Thuente line search (1994). Same 6-arg signature as Wolfe so the
    // Solver's do_line_search requires-detection picks it up as a drop-in
    // replacement. Enforces the strong-Wolfe conditions but replaces Wolfe's
    // midpoint-bisection zoom with safeguarded cubic/quadratic interpolation of
    // the auxiliary function ψ(α) = φ(α) − φ(0) − c1·α·φ'(0) — fewer f/∇f evals
    // and robust on ill-scaled problems.
    template <typename T>
    struct MoreThuente {
        T c1{static_cast<T>(1e-4)};
        T c2{static_cast<T>(0.9)};
        T alpha0{T{1}};
        T alpha_max{static_cast<T>(1e3)};
        std::size_t max_iter{50};

        template <typename F, typename Deriv, typename V>
        [[nodiscard]] T search(const F& f, const Deriv& deriv,
                               const V& x, const V& dir, T fx, const V& g) const {
            return search(f, deriv, x, dir, fx, g, alpha0);
        }

        template <typename F, typename Deriv, typename V>
        [[nodiscard]] T search(const F& f, const Deriv& deriv,
                               const V& x, const V& dir, T fx, const V& g,
                               T alpha_init) const {
            const std::size_t n = x.size();
            const T phi0 = fx;
            const T dphi0 = detail::dot(g, dir);
            if (dphi0 >= T{0}) return T{0}; // not a descent direction

            auto eval = [&](T a, T& phi, T& dphi) {
                V trial(n), gt(n);
                for (std::size_t i = 0; i < n; ++i) trial[i] = x[i] + a * dir[i];
                phi = f(trial);
                deriv.grad(f, trial, gt);
                dphi = detail::dot(gt, dir);
            };

            // Bracket endpoints, tracked with their φ/φ' values. `bracketed`
            // flips once a minimizer is trapped between al and au.
            T al = T{0}, phial = phi0, dphial = dphi0;
            T au = T{0}, phiau = phi0, dphiau = dphi0;
            T a = (std::isfinite(alpha_init) && alpha_init > T{0}) ? alpha_init : alpha0;
            bool bracketed = false;
            const T c2abs = -c2 * dphi0; // strong-curvature RHS

            for (std::size_t it = 0; it < max_iter; ++it) {
                T phi, dphi;
                eval(a, phi, dphi);

                // Strong-Wolfe acceptance.
                if (phi <= phi0 + c1 * a * dphi0 && std::abs(dphi) <= c2abs)
                    return a;

                if (phi > phi0 + c1 * a * dphi0 || (bracketed && phi >= phial)) {
                    // Higher than the sufficient-decrease line (or above the
                    // low endpoint): the minimizer lies below a → au = a.
                    au = a;
                    phiau = phi;
                    dphiau = dphi;
                    bracketed = true;
                }
                else if (dphi * (au - al) >= T{0} && bracketed) {
                    // Derivative points the wrong way inside a live bracket:
                    // reflect the upper endpoint to the current low one.
                    au = al;
                    phiau = phial;
                    dphiau = dphial;
                    al = a;
                    phial = phi;
                    dphial = dphi;
                }
                else {
                    // Sufficient decrease holds but curvature not yet met:
                    // advance the low endpoint.
                    al = a;
                    phial = phi;
                    dphial = dphi;
                    if (!bracketed) {
                        // Still expanding: grow the trial toward alpha_max.
                        a = std::min(a * T{2}, alpha_max);
                        if (a >= alpha_max) {
                            T pe, de;
                            eval(alpha_max, pe, de);
                            return alpha_max;
                        }
                        continue;
                    }
                }

                // ---- safeguarded interpolation of the next trial ------------
                a = interpolate(al, phial, dphial, au, phiau, dphiau);
                // Keep the trial strictly inside the bracket (Moré–Thuente
                // safeguard: at least 1/9 of the way in from either endpoint).
                const T lo = std::min(al, au), hi = std::max(al, au);
                const T guard = (hi - lo) / T{9};
                if (a <= lo + guard || a >= hi - guard) a = T{0.5} * (lo + hi);
                if (std::abs(hi - lo) < static_cast<T>(1e-14)) return a;
            }
            return a;
        }

    private:
        // Cubic (Hermite) minimizer of the segment [a1,a2] from endpoint values
        // and slopes; falls back to the quadratic minimizer if the cubic is
        // degenerate. Standard Moré–Thuente / Nocedal–Wright interpolation.
        [[nodiscard]] T interpolate(T a1, T f1, T d1, T a2, T f2, T d2) const {
            const T da = a2 - a1;
            if (std::abs(da) < static_cast<T>(1e-30)) return a1;
            const T d3 = d1 + d2 - T{3} * (f1 - f2) / da;
            const T disc = d3 * d3 - d1 * d2;
            if (disc >= T{0}) {
                const T sq = std::sqrt(disc);
                const T denom = (d2 - d1) + T{2} * sq;
                if (std::abs(denom) > static_cast<T>(1e-30)) {
                    const T q = (d2 + sq - d3) / denom;
                    return a2 - q * da;
                }
            }
            // Quadratic fallback: minimizer from f1, d1, f2.
            const T qden = T{2} * (f2 - f1 - d1 * da);
            if (std::abs(qden) > static_cast<T>(1e-30))
                return a1 - d1 * da * da / qden;
            return T{0.5} * (a1 + a2);
        }
    };

    template <typename Algorithm,
              typename Deriv = Derivatives<Dual, double>,
              typename LineSrch = Wolfe<double>,
              typename Stop = DefaultStop<double>,
              typename Telem = NoTelemetry>
    class Solver {
    public:
        using T = typename Deriv::Scalar_t;

        Solver() = default;

        explicit Solver(Algorithm alg, Deriv d = {}, LineSrch ls = {},
                        Stop stop = {}, Telem tl = {})
            : alg_(std::move(alg)), deriv_(std::move(d)),
              line_(std::move(ls)), stop_(std::move(stop)), telem_(std::move(tl)) {}

        // solve — run the descent loop from x0.
        template <typename Prob>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const Prob& prob, const ga::Vector<T>& x0) {
            const auto& f = prob.objective;
            const std::size_t n = x0.size();
            alpha_hint_ = T{1};

            IterState<T> s;
            s.x = x0;
            s.g = ga::Vector<T>(n);
            s.dir = ga::Vector<T>(n);
            // project the start onto the domain
            {
                ga::Vector<T> px(n);
                prob.domain.project(s.x, px);
                s.x = px;
            }

            s.f = f(s.x);
            if (!std::isfinite(s.f))
                return std::unexpected(Diagnosis{Cause::NaNTrap, "objective is NaN/Inf at x0", 0});
            deriv_.grad(f, s.x, s.g);
            s.grad_norm = detail::nrm2(s.g);

            if constexpr (requires { stop_.init(s); }) stop_.init(s);

            alg_.reset(n);

            for (s.iter = 0; ; ++s.iter) {
                telem_.record(s);
                if (stop_.done(s)) break;

                // direction from the algorithm policy
                alg_.direction(deriv_, f, s, /*out*/ s.dir);

                // line search
                const T alpha = do_line_search(f, s);
                if (alpha <= T{0})
                    return std::unexpected(Diagnosis{
                        Cause::LineSearchFail,
                        "line search failed to find a descent step", s.iter
                    });
                s.alpha = alpha;
                alpha_hint_ = alpha;

                // step + project onto domain
                ga::Vector<T> xn(n), xp(n);
                for (std::size_t i = 0; i < n; ++i) xn[i] = s.x[i] + alpha * s.dir[i];
                prob.domain.project(xn, xp);

                T step_norm{};
                for (std::size_t i = 0; i < n; ++i) {
                    const T d = xp[i] - s.x[i];
                    step_norm += d * d;
                }
                s.step = std::sqrt(step_norm);

                ga::Vector<T> gn(n);
                const T fn = f(xp);
                if (!std::isfinite(fn))
                    return std::unexpected(Diagnosis{Cause::NaNTrap, "objective became NaN/Inf", s.iter});
                deriv_.grad(f, xp, gn);

                // let the algorithm update its curvature memory
                alg_.update(s.x, s.g, xp, gn);

                s.x = std::move(xp);
                s.g = std::move(gn);
                s.f = fn;
                s.grad_norm = detail::nrm2(s.g);
            }

            Result<T> r;
            r.x = s.x;
            r.f = s.f;
            r.grad_norm = s.grad_norm;
            r.iterations = s.iter;
            r.status = stop_.converged(s) ? Status::Converged : Status::MaxIterations;
            return r;
        }

    private:
        template <typename F>
        T do_line_search(const F& f, const IterState<T>& s) {
            if constexpr (requires { line_.search(f, deriv_, s.x, s.dir, s.f, s.g, alpha_hint_); })
                return line_.search(f, deriv_, s.x, s.dir, s.f, s.g, alpha_hint_);
            else if constexpr (requires { line_.search(f, deriv_, s.x, s.dir, s.f, s.g); })
                return line_.search(f, deriv_, s.x, s.dir, s.f, s.g);
            else if constexpr (requires { line_.search(f, s.x, s.dir, s.f, s.g, alpha_hint_); })
                return line_.search(f, s.x, s.dir, s.f, s.g, alpha_hint_);
            else
                return line_.search(f, s.x, s.dir, s.f, s.g);
        }

        [[no_unique_address]] Algorithm alg_;
        [[no_unique_address]] Deriv deriv_;
        [[no_unique_address]] LineSrch line_;
        [[no_unique_address]] Stop stop_;
        [[no_unique_address]] Telem telem_;
        T alpha_hint_{T{1}};
    };
} // namespace kalpa

#endif // PEBBLE_KALPA_CORE_SOLVER_HPP
