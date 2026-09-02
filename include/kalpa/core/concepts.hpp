#pragma once
// ============================================================================
// kalpa/core/concepts.hpp — the concept vocabulary of the optimizer
// ============================================================================
// Kalpa is a THIN orchestration layer: it contributes optimization *algorithms*
// and a *fluent/EDSL front end*, and delegates every heavy numeric kernel to
// the ga:: linear-algebra library (containers/matrix). These concepts gate the
// policy set so that (a) misuse is a readable compile error, not a deep template
// dump, and (b) empty policies can be stored with [[no_unique_address]] for a
// literal zero-overhead unconstrained solve.
//
// State type throughout kalpa is ga::Vector<T> (native BLAS-backed vector).
// ============================================================================

#ifndef PEBBLE_KALPA_CORE_CONCEPTS_HPP
#define PEBBLE_KALPA_CORE_CONCEPTS_HPP

#include <containers/matrix/dense.hpp>
#include <concepts>
#include <cstddef>
#include <type_traits>

namespace kalpa {
    // -----------------------------------------------------------------------
    // Scalar — an admissible optimization scalar (delegates to ga's floating T)
    // -----------------------------------------------------------------------
    template <typename T>
    concept Scalar = std::is_floating_point_v<T>;

    // -----------------------------------------------------------------------
    // Vec — the state/gradient vector kalpa operates on. Anything providing the
    // ga::Vector surface we actually use: size(), operator[], and construction
    // from a size. (We intentionally do not require the whole ga::Vector API.)
    // -----------------------------------------------------------------------
    template <typename V>
    concept Vec = requires(V v, const V cv, std::size_t i) {
        typename V::value_type;
        { cv.size() } -> std::convertible_to<std::size_t>;
        { v[i] } -> std::convertible_to<typename V::value_type&>;
        { cv[i] } -> std::convertible_to<const typename V::value_type&>;
    };

    // -----------------------------------------------------------------------
    // Objective — f: Vec -> Scalar. The single mandatory ingredient of a
    // Problem. May be a plain lambda/functor, or (via edsl/) a captured
    // vakya expression wrapped to model this same shape.
    // -----------------------------------------------------------------------
    template <typename F, typename V>
    concept Objective = Vec<V> && requires(const F& f, const V& x) {
        { f(x) } -> std::convertible_to<typename V::value_type>;
    };

    // -----------------------------------------------------------------------
    // GradientProvider — supplies ∇f(x) into a caller-owned output vector.
    // Modes (Analytic / Dual / FiniteDiff) all model this; the solver never
    // cares which. grad(f, x, out) fills out with ∇f(x).
    // -----------------------------------------------------------------------
    template <typename D, typename F, typename V>
    concept GradientProvider = Vec<V> && requires(const D& d, const F& f,
                                                  const V& x, V& out) {
        { d.grad(f, x, out) } -> std::same_as<void>;
    };

    // -----------------------------------------------------------------------
    // HessianVecProvider — optional. Supplies ∇²f(x)·v (matrix-free) for
    // Newton-CG / trust-region. Detected; absent → those algorithms fall back.
    // -----------------------------------------------------------------------
    template <typename D, typename F, typename V>
    concept HessianVecProvider = Vec<V> && requires(const D& d, const F& f,
                                                    const V& x, const V& v, V& out) {
        { d.hessian_vec(f, x, v, out) } -> std::same_as<void>;
    };

    // -----------------------------------------------------------------------
    // Constraints — the constraint set attached to a Problem. The empty set
    // (Unconstrained) models this trivially and is an empty type so a
    // [[no_unique_address]] member costs zero bytes.
    // -----------------------------------------------------------------------
    template <typename C, typename V>
    concept Constraints = Vec<V> && requires(const C& c, const V& x) {
        { c.feasible(x) } -> std::convertible_to<bool>; // is x in the set?
        { c.count() } -> std::convertible_to<std::size_t>; // # constraints
    };

    // -----------------------------------------------------------------------
    // Domain — the ambient set the iterate is projected onto (box / polytope /
    // ℝⁿ). project(x, out) writes the nearest feasible point into out.
    // -----------------------------------------------------------------------
    template <typename Dm, typename V>
    concept Domain = Vec<V> && requires(const Dm& dm, const V& x, V& out) {
        { dm.project(x, out) } -> std::same_as<void>;
    };

    // -----------------------------------------------------------------------
    // Stop — convergence predicate. done(state) -> bool. Composable.
    // -----------------------------------------------------------------------
    template <typename S, typename State>
    concept StopCriterion = requires(const S& s, const State& st) {
        { s.done(st) } -> std::convertible_to<bool>;
    };

    // -----------------------------------------------------------------------
    // TelemetrySink — per-iteration observer. record(state) is called each
    // iteration. NoTelemetry (empty, record()==no-op) is the zero-overhead
    // default, mirroring nadi::NoSink.
    // -----------------------------------------------------------------------
    template <typename Tl, typename State>
    concept TelemetrySink = requires(Tl& t, const State& st) {
        { t.record(st) } -> std::same_as<void>;
    };
} // namespace kalpa

#endif // PEBBLE_KALPA_CORE_CONCEPTS_HPP
