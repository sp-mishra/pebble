#pragma once
// ============================================================================
// kalpa/core/problem.hpp — Problem aggregate + derivative policies
// ============================================================================
// A Problem bundles an objective with (optionally) a constraint set and a
// domain. All three are held with [[no_unique_address]] so that an
// unconstrained, full-space problem is byte-identical to the bare objective.
//
// Derivatives<Mode> is the gradient/curvature policy:
//   Analytic   — user supplies ∇f (and optionally ∇²f·v) directly.
//   Dual       — forward-mode AD via ga::Dual<T,1>, coordinate-seeded. The
//                objective must be callable on ga::Vector<ga::Dual<T,1>>.
//   FiniteDiff — central differences; the per-coordinate evaluations are
//                embarrassingly parallel and fan out via pravaha when a
//                parallel policy is selected (see finite_diff_parallel.hpp).
// ============================================================================

#ifndef PEBBLE_KALPA_CORE_PROBLEM_HPP
#define PEBBLE_KALPA_CORE_PROBLEM_HPP

#include <kalpa/core/concepts.hpp>
#include <containers/matrix/dense.hpp>
#include <containers/matrix/dual.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace kalpa {

    // =======================================================================
    // Empty constraint set / full-space domain — zero-byte policies
    // =======================================================================
    template<typename V>
    struct Unconstrained {
        [[nodiscard]] constexpr bool feasible(const V&) const noexcept { return true; }
        [[nodiscard]] constexpr std::size_t count() const noexcept { return 0; }
    };

    template<typename V>
    struct FullSpace {
        // projection onto ℝⁿ is the identity
        void project(const V& x, V& out) const {
            for (std::size_t i = 0; i < x.size(); ++i) out[i] = x[i];
        }
    };

    // =======================================================================
    // Problem<Objective, Constraints, Domain>
    // =======================================================================
    template<typename Obj, typename Cons, typename Dom>
    struct Problem {
        using objective_type   = Obj;
        using constraints_type = Cons;
        using domain_type      = Dom;

        [[no_unique_address]] Obj  objective;
        [[no_unique_address]] Cons constraints;
        [[no_unique_address]] Dom  domain;
    };

    // Factory: unconstrained problem over ga::Vector<T>. Deduces objective type.
    template<typename T = double, typename Obj>
    [[nodiscard]] auto make_problem(Obj&& f) {
        using V = ga::Vector<T>;
        return Problem<std::decay_t<Obj>, Unconstrained<V>, FullSpace<V>>{
            std::forward<Obj>(f), {}, {}
        };
    }

    // Factory with an explicit constraint set + domain.
    template<typename Obj, typename Cons, typename Dom>
    [[nodiscard]] auto make_problem(Obj&& f, Cons&& c, Dom&& d) {
        return Problem<std::decay_t<Obj>, std::decay_t<Cons>, std::decay_t<Dom>>{
            std::forward<Obj>(f), std::forward<Cons>(c), std::forward<Dom>(d)
        };
    }

    // =======================================================================
    // Derivative modes
    // =======================================================================
    struct Analytic {};   // tag: user gradient functor is the objective's .grad
    struct Dual {};       // tag: forward-mode AD
    struct FiniteDiff {}; // tag: central differences (serial)

    // -----------------------------------------------------------------------
    // Derivatives<Mode, T> — gradient provider policy.
    // Empty for stateless modes; costs zero bytes as a solver member.
    // -----------------------------------------------------------------------
    template<typename Mode, typename T = double>
    struct Derivatives;

    // ---- Dual: forward-mode AD, coordinate-seeded --------------------------
    // Objective F must be callable on ga::Vector<ga::Dual<T,1>> and return a
    // ga::Dual<T,1>. One forward pass per coordinate (N passes) yields ∇f.
    // Also provides matrix-free hessian_vec via central difference on this
    // exact gradient (mirrors ga::hessian_vec, adapted to runtime-sized Vec).
    template<typename T>
    struct Derivatives<Dual, T> {
        using Scalar_t = T;
        using DualT    = ga::Dual<T,1>;

        template<typename F, typename V>
        void grad(const F& f, const V& x, V& out) const {
            const std::size_t n = x.size();
            ga::Vector<DualT> dx(n);
            for (std::size_t i = 0; i < n; ++i) dx[i] = DualT(x[i]);
            for (std::size_t i = 0; i < n; ++i) {
                dx[i].d[0] = T{1};                // seed coordinate i
                auto r = f(dx);
                out[i] = r.d[0];
                dx[i].d[0] = T{0};                // unseed
            }
        }

        template<typename F, typename V>
        void hessian_vec(const F& f, const V& x, const V& v, V& out) const {
            const std::size_t n = x.size();
            const T eps = std::cbrt(std::numeric_limits<T>::epsilon());
            V xp(n), xm(n), gp(n), gm(n);
            for (std::size_t i = 0; i < n; ++i) {
                xp[i] = x[i] + eps * v[i];
                xm[i] = x[i] - eps * v[i];
            }
            grad(f, xp, gp);
            grad(f, xm, gm);
            const T inv2e = T{1} / (T{2} * eps);
            for (std::size_t i = 0; i < n; ++i) out[i] = (gp[i] - gm[i]) * inv2e;
        }
    };

    // ---- FiniteDiff: central differences (serial) -------------------------
    // Objective F callable on plain ga::Vector<T> -> T. 2N evaluations.
    template<typename T>
    struct Derivatives<FiniteDiff, T> {
        T step{std::sqrt(std::numeric_limits<T>::epsilon())};

        template<typename F, typename V>
        void grad(const F& f, const V& x, V& out) const {
            const std::size_t n = x.size();
            V xp = x, xm = x;
            for (std::size_t i = 0; i < n; ++i) {
                const T h = step * (std::abs(x[i]) > T{1} ? std::abs(x[i]) : T{1});
                xp[i] = x[i] + h; xm[i] = x[i] - h;
                out[i] = (f(xp) - f(xm)) / (T{2} * h);
                xp[i] = x[i]; xm[i] = x[i];
            }
        }

        template<typename F, typename V>
        void hessian_vec(const F& f, const V& x, const V& v, V& out) const {
            const std::size_t n = x.size();
            const T h = std::cbrt(std::numeric_limits<T>::epsilon());
            V xp = x, xm = x, gp(n), gm(n);
            for (std::size_t i = 0; i < n; ++i) { xp[i] = x[i] + h*v[i]; xm[i] = x[i] - h*v[i]; }
            grad(f, xp, gp);
            grad(f, xm, gm);
            const T inv2h = T{1} / (T{2} * h);
            for (std::size_t i = 0; i < n; ++i) out[i] = (gp[i] - gm[i]) * inv2h;
        }
    };

    // ---- Analytic: the objective carries its own gradient -----------------
    // F must expose f.value(x)->T and f.grad(x,out). Optionally f.hessian_vec.
    template<typename T>
    struct Derivatives<Analytic, T> {
        template<typename F, typename V>
        void grad(const F& f, const V& x, V& out) const { f.grad(x, out); }

        template<typename F, typename V>
        void hessian_vec(const F& f, const V& x, const V& v, V& out) const {
            f.hessian_vec(x, v, out);
        }
    };

} // namespace kalpa

#endif // PEBBLE_KALPA_CORE_PROBLEM_HPP
