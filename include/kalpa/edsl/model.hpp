#pragma once
// ============================================================================
// kalpa/edsl/model.hpp — fluent / EDSL front end over vakya
// ============================================================================
// Lets an objective be written as an algebraic expression instead of a lambda:
//
//     auto x = kalpa::vars(2);
//     auto prob = kalpa::minimize( sq(x[0]-1) + sq(x[1]-2) );
//     Solver<LBFGS<double>> s; s.solve(prob, x0);
//
// The expression is a vakya graph (kalpa contributes a `Var` leaf that binds to
// a coordinate of the state vector). kalpa walks the graph twice:
//   • forward   — evaluate f(x)                    (objective callable)
//   • Dual pass — evaluate over ga::Dual<T,1> for a gradient with no user code
// so an EDSL Problem plugs straight into the existing Derivatives<Dual>/<Analytic>.
//
// Canonicalization (constant fold / x+0 / x*1) reuses vakya::pattern::rule_set;
// property tagging for Auto method selection reuses vakya::property_store. vakya
// has no `sq`, so kalpa provides `sq(e) = e*e`.
// ============================================================================

#ifndef PEBBLE_KALPA_EDSL_MODEL_HPP
#define PEBBLE_KALPA_EDSL_MODEL_HPP

#include <kalpa/core/problem.hpp>
#include <containers/matrix/dense.hpp>
#include <containers/matrix/dual.hpp>
#include <vakya/vakya.hpp>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace kalpa::edsl {

    // =======================================================================
    // Var — a vakya terminal that binds to state coordinate `idx`. Marked
    // `vakya_terminal` so vakya's +,-,*,/ operators lift it into a node graph.
    // =======================================================================
    struct Var {
        using vakya_terminal = void;      // opt into vakya terminal detection
        std::size_t idx{0};
        constexpr explicit Var(std::size_t i) noexcept : idx(i) {}
    };

    // Constant terminal (carries its own value; distinct from Var).
    template<typename T>
    struct Const {
        using vakya_terminal = void;
        T value{};
        constexpr explicit Const(T v) noexcept : value(v) {}
    };

    // vars(n) → a small handle giving x[i] as a wrapped Var expression.
    // Wrapping via vakya::as_expr is what lets the FULL operator set (−, / as
    // well as +, *) apply: vakya only auto-lifts plain terminals for + and *,
    // so a bare Var would fail on `x[0] - c`. Returning an rvalue-wrapped
    // expr<Var> (by value — no dangling) gives every operator via the member
    // interface. constant() wraps a Const the same way.
    struct VarPack {
        [[nodiscard]] auto operator[](std::size_t i) const {
            return ::vakya::as_expr(Var{i});          // expr<Var> (by value)
        }
    };
    [[nodiscard]] inline VarPack vars(std::size_t = 0) noexcept { return VarPack{}; }
    [[nodiscard]] inline auto var(std::size_t i) noexcept { return ::vakya::as_expr(Var{i}); }
    template<typename T = double>
    [[nodiscard]] auto constant(T v) noexcept { return ::vakya::as_expr(Const<T>{v}); }

    // sq(e) = e*e  (vakya has no sq).
    template<typename E>
    [[nodiscard]] constexpr auto sq(E&& e) {
        return std::forward<E>(e) * std::forward<E>(e);
    }

    // =======================================================================
    // Graph evaluation — walk a vakya expression, resolving Var against a
    // caller-supplied coordinate accessor. Scalar type S is deduced from the
    // accessor (T for a value pass, ga::Dual<T,1> for a gradient pass), so the
    // SAME graph yields both f(x) and ∇f(x) with no duplicated user code.
    // =======================================================================
    namespace detail {

        // coordinate accessor concept: coord(i) -> S
        template<typename S, typename Node, typename Coord>
        [[nodiscard]] S eval_node(const Node& node, const Coord& coord);

        // Terminals -------------------------------------------------------
        template<typename S, typename Coord>
        [[nodiscard]] S eval_term(const Var& v, const Coord& coord) { return coord(v.idx); }

        template<typename S, typename T, typename Coord>
        [[nodiscard]] S eval_term(const Const<T>& c, const Coord&) { return S(c.value); }

        // vakya wrappers around a kalpa terminal — unwrap and recurse. as_expr
        // lifts Var/Const into expr<…> (rvalue) or expr_ref<…> (lvalue); the
        // graph therefore stores wrapped kalpa terminals, which we peel here.
        template<typename S, typename Coord>
        [[nodiscard]] S eval_term(const ::vakya::expr<Var>& e, const Coord& coord) { return eval_term<S>(e.value, coord); }
        template<typename S, typename T, typename Coord>
        [[nodiscard]] S eval_term(const ::vakya::expr<Const<T>>& e, const Coord& coord) { return eval_term<S>(e.value, coord); }
        template<typename S, typename Coord>
        [[nodiscard]] S eval_term(const ::vakya::expr_ref<Var>& e, const Coord& coord) { return eval_term<S>(*e.p, coord); }
        template<typename S, typename T, typename Coord>
        [[nodiscard]] S eval_term(const ::vakya::expr_ref<Const<T>>& e, const Coord& coord) { return eval_term<S>(*e.p, coord); }

        // vakya value wrappers around a raw arithmetic literal + raw arithmetic.
        template<typename S, typename T, typename Coord>
            requires std::is_arithmetic_v<T>
        [[nodiscard]] S eval_term(const ::vakya::expr<T>& e, const Coord&) { return S(e.value); }
        template<typename S, typename T, typename Coord>
            requires std::is_arithmetic_v<T>
        [[nodiscard]] S eval_term(const ::vakya::expr_ref<T>& e, const Coord&) { return S(*e.p); }
        template<typename S, typename T, typename Coord>
            requires std::is_arithmetic_v<T>
        [[nodiscard]] S eval_term(const T& x, const Coord&) { return S(x); }

        // Dispatch a child that may be a node or a terminal.
        template<typename S, typename Child, typename Coord>
        [[nodiscard]] S eval_child(const Child& c, const Coord& coord) {
            if constexpr (::vakya::Expression<Child>) return eval_node<S>(c, coord);
            else                                       return eval_term<S>(c, coord);
        }

        // Node by tag ------------------------------------------------------
        template<typename S, typename Node, typename Coord>
        [[nodiscard]] S eval_node(const Node& node, const Coord& coord) {
            using Tag = typename Node::tag_type;
            const auto& ch = node.children;
            if constexpr (std::is_same_v<Tag, ::vakya::add_tag>)
                return eval_child<S>(std::get<0>(ch), coord) + eval_child<S>(std::get<1>(ch), coord);
            else if constexpr (std::is_same_v<Tag, ::vakya::sub_tag>)
                return eval_child<S>(std::get<0>(ch), coord) - eval_child<S>(std::get<1>(ch), coord);
            else if constexpr (std::is_same_v<Tag, ::vakya::mul_tag>)
                return eval_child<S>(std::get<0>(ch), coord) * eval_child<S>(std::get<1>(ch), coord);
            else if constexpr (std::is_same_v<Tag, ::vakya::div_tag>)
                return eval_child<S>(std::get<0>(ch), coord) / eval_child<S>(std::get<1>(ch), coord);
            else if constexpr (std::is_same_v<Tag, ::vakya::neg_tag>)
                return -eval_child<S>(std::get<0>(ch), coord);
            else {
                static_assert(std::is_same_v<Tag, ::vakya::add_tag>,
                              "kalpa::edsl: unsupported node tag in objective expression");
                return S{};
            }
        }
    } // namespace detail

    // =======================================================================
    // Expr — wraps a captured vakya graph as a kalpa Objective. Callable on
    // ga::Vector<T> (value pass) and on ga::Vector<ga::Dual<T,1>> (Dual pass),
    // so Derivatives<Dual,T> differentiates it with zero extra user code.
    // =======================================================================
    template<typename Graph>
    struct Expr {
        Graph graph;

        // value / Dual pass — deduces scalar from the vector element type.
        template<typename V>
        [[nodiscard]] auto operator()(const V& x) const {
            using S = typename V::value_type;
            auto coord = [&x](std::size_t i) -> S { return x[i]; };
            if constexpr (::vakya::Expression<Graph>)
                return detail::eval_node<S>(graph, coord);
            else
                return detail::eval_term<S>(graph, coord);
        }
    };

    template<typename Graph>
    [[nodiscard]] auto wrap(Graph&& g) {
        return Expr<std::decay_t<Graph>>{std::forward<Graph>(g)};
    }

    // =======================================================================
    // minimize(expr) → an unconstrained Problem whose objective is the graph.
    // Pair with any Solver; Derivatives<Dual> is the natural default since the
    // graph evaluates over ga::Dual<T,1> unchanged.
    // =======================================================================
    template<typename T = double, typename E>
    [[nodiscard]] auto minimize(E&& expr) {
        auto obj = wrap(std::forward<E>(expr));
        using V = ga::Vector<T>;
        return Problem<decltype(obj), Unconstrained<V>, FullSpace<V>>{ std::move(obj), {}, {} };
    }

} // namespace kalpa::edsl

#endif // PEBBLE_KALPA_EDSL_MODEL_HPP
