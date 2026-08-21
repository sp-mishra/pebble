#pragma once
// ============================================================================
// pravaha_expr.hpp - User-facing eDSL for the Pravaha heterogeneous overlay.
//
// Hides raw Lithe node construction behind a clean expression API:
//
//   using namespace pravaha::expr;
//
//   var x;                        // input placeholder (call_tag leaf)
//   auto e = x * x + x;           // Lithe arithmetic via operator overloads
//   auto e2 = sqrt(x * x + lit(1.0f));  // math builtins → Metal MSL builtins
//
// Math tags (exp_tag, log_tag, sqrt_tag, sin_tag, cos_tag, abs_tag) are
// defined here and NOT registered as SIMD-capable — they run through the
// scalar fallback path on CPU (Invariant 3 emits a NADI event) and are
// emitted as MSL builtins on the Metal GPU path.
//
// Arithmetic operators (+, -, *, /) come for free from lithe::interface<>.
// ============================================================================

#include "pravaha/pravaha_hetero.hpp"  // defines tag_id, structural_hash, etc.

#include <cmath>

namespace pravaha::expr {
    // Pravaha compute expressions are Vākya trees.  Lithe can consume the
    // same trees through its own opt-in adapter, but is not a dependency here.
    namespace lithe = ::vakya;
    // ============================================================================
    // var / input<N> — named input placeholders.
    //
    // `var x;` is a single-input leaf (slot 0). For multi-input kernels
    // `y = f(x0, x1, …)`, use `input<0> x0; input<1> x1;` — each binds to a
    // distinct source buffer slot (see host_simd_backend / metal dispatch).
    // ============================================================================

    template <std::size_t N>
    struct input : lithe::interface<input<N>> {
        using is_lithe_node = void;
        using tag_type = pravaha::expr::input_tag<N>;
        std::tuple<> children{};
        static constexpr std::size_t slot = N;
    };

    using var = input<0>;

    // ============================================================================
    // lit_node<T> — typed scalar constant leaf.
    // Carries lit_tag (NOT call_tag) so its stored value is honored on every
    // backend path and so distinct constants get distinct kernel-cache keys.
    // ============================================================================

    template <typename T>
    struct lit_node : lithe::interface<lit_node<T>> {
        using is_lithe_node = void;
        using tag_type = pravaha::expr::lit_tag;
        std::tuple<> children{};
        T value{};

        explicit constexpr lit_node(T v) noexcept : value(v) {}
    };

    template <typename T>
    [[nodiscard]] constexpr auto lit(T value) noexcept {
        return lit_node<T>{value};
    }

    // ADL hook for lithe::emit::structural_hash (impl-2). Distinct constants must produce
    // distinct kernel-cache keys — a baked-in MSL literal is part of the compiled kernel.
    // Fold via double bit-pattern to match legacy hash semantics in pravaha_hetero.hpp.
    template <typename T>
    inline std::size_t structural_payload_hash(const lit_node<T>& e) noexcept {
        return std::hash<std::uint64_t>{}(
            std::bit_cast<std::uint64_t>(static_cast<double>(e.value)));
    }

    // ============================================================================
    // Math free functions
    // ============================================================================

    template <lithe::Expression E>
    [[nodiscard]] constexpr auto sqrt(E&& e) {
        return lithe::make_node<sqrt_tag>(std::forward < E > (e));
    }

    template <lithe::Expression E>
    [[nodiscard]] constexpr auto exp(E&& e) {
        return lithe::make_node<exp_tag>(std::forward < E > (e));
    }

    template <lithe::Expression E>
    [[nodiscard]] constexpr auto log(E&& e) {
        return lithe::make_node<log_tag>(std::forward < E > (e));
    }

    template <lithe::Expression E>
    [[nodiscard]] constexpr auto sin(E&& e) {
        return lithe::make_node<sin_tag>(std::forward < E > (e));
    }

    template <lithe::Expression E>
    [[nodiscard]] constexpr auto cos(E&& e) {
        return lithe::make_node<cos_tag>(std::forward < E > (e));
    }

    template <lithe::Expression E>
    [[nodiscard]] constexpr auto abs(E&& e) {
        return lithe::make_node<abs_tag>(std::forward < E > (e));
    }

    // neg: explicit free function (operator-() from lithe::interface<> also works).
    template <lithe::Expression E>
    [[nodiscard]] constexpr auto neg(E&& e) {
        return lithe::make_node<lithe::neg_tag>(std::forward < E > (e));
    }

    // sq: shorthand x*x without naming x twice.
    // Takes the expression by value because it appears in both children.
    template <lithe::Expression E>
    [[nodiscard]] constexpr auto sq(E e) {
        return lithe::make_node<lithe::mul_tag>(e, e);
    }

    // ============================================================================
    // Reductions (Part E) — whole-input fold of an element-wise child → scalar.
    //
    //   var x;
    //   auto r = reduce_sum(x * x);        // Σ x[i]²
    //   float s = exec.reduce(r, src, ctx).value();   // executor dispatches
    //
    // A reduce_node wraps one element-wise child. It is NOT itself an element-wise
    // node — the executor's reduce() path consumes it (see pravaha_hetero.hpp).
    // reduce_child() recovers the child so backends can evaluate it per element.
    // ============================================================================

    template <reduce_op Op, lithe::Expression E>
    [[nodiscard]] constexpr auto reduce_node(E&& e) {
        return lithe::make_node<reduce_tag<Op>>(std::forward < E > (e));
    }

    template <lithe::Expression E>
    [[nodiscard]] constexpr auto reduce_sum(E&& e) {
        return reduce_node<reduce_op::sum>(std::forward < E > (e));
    }

    template <lithe::Expression E>
    [[nodiscard]] constexpr auto reduce_max(E&& e) {
        return reduce_node<reduce_op::max>(std::forward < E > (e));
    }

    template <lithe::Expression E>
    [[nodiscard]] constexpr auto reduce_min(E&& e) {
        return reduce_node<reduce_op::min>(std::forward < E > (e));
    }

    // Recover the element-wise child of a reduce_node.
    template <typename R>
    [[nodiscard]] constexpr const auto& reduce_child(const R& r) noexcept {
        return std::get < 0 > (r.children);
    }

    // Recover the reduce_op of a reduce_node (compile-time).
    template <typename R>
    inline constexpr reduce_op reduce_op_of =
        reduce_tag_op<typename std::decay_t<R>::tag_type>::op;
} // namespace pravaha::expr
