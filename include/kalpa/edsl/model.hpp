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
#include <kalpa/algo/unconstrained.hpp>
#include <kalpa/algo/constrained.hpp>
#include <kalpa/algo/global.hpp>
#include <kalpa/core/solver.hpp>
#include <containers/matrix/dense.hpp>
#include <containers/matrix/dual.hpp>
#include <vakya/vakya.hpp>
#include <vakya/property.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <tuple>
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

    // =======================================================================
    // Constraints — a comparison expression (lhs ⋈ rhs) becomes a signed
    // residual functor. vakya already emits eq_tag/le_tag/ge_tag for ==/<=/>=;
    // we peel that top tag and reuse the objective's eval_child on both sides.
    // Residual convention (all "feasible ⇔ residual sense holds"):
    //     Eq :  r = lhs − rhs   (feasible ⇔ r == 0)
    //     Le :  r = lhs − rhs   (feasible ⇔ r ≤ 0)
    //     Ge :  r = rhs − lhs   (feasible ⇔ r ≤ 0)
    // Ge is stored as its Le-equivalent so a solver only ever sees ≤/==. The
    // objective evaluation path (Expr::operator(), eval_node's static_assert) is
    // untouched — this is a separate, additive residual walk.
    // =======================================================================
    enum class Relation { Eq, Le, Ge };

    namespace detail {
        // Signed residual of a comparison node whose two children are `lhs`,`rhs`.
        template<typename S, typename L, typename R, typename Coord>
        [[nodiscard]] S eval_constraint(Relation rel, const L& lhs, const R& rhs, const Coord& coord) {
            const S l = eval_child<S>(lhs, coord);
            const S r = eval_child<S>(rhs, coord);
            switch (rel) {
                case Relation::Ge: return r - l;   // rhs − lhs ≤ 0
                case Relation::Eq:
                case Relation::Le:
                default:           return l - r;   // lhs − rhs (== 0 or ≤ 0)
            }
        }

        // map a vakya comparison tag → Relation
        template<typename Tag>
        [[nodiscard]] constexpr Relation relation_of() {
            if constexpr (std::is_same_v<Tag, ::vakya::eq_tag>) return Relation::Eq;
            else if constexpr (std::is_same_v<Tag, ::vakya::le_tag>) return Relation::Le;
            else if constexpr (std::is_same_v<Tag, ::vakya::ge_tag>) return Relation::Ge;
            else { static_assert(std::is_same_v<Tag, ::vakya::eq_tag>,
                     "kalpa::edsl::subject_to: argument is not a ==, <= or >= comparison"); return Relation::Eq; }
        }
    } // namespace detail

    // ConstraintExpr — wraps a comparison graph; callable like an objective,
    // returning the signed residual. Its top node's children are evaluated with
    // the SAME dual/value coordinate accessor used for objectives, so a
    // Derivatives<Dual> pass yields the constraint gradient (row of the Jacobian).
    template<typename Graph>
    struct ConstraintExpr {
        Graph graph;
        static constexpr Relation rel =
            detail::relation_of<typename Graph::tag_type>();

        template<typename V>
        [[nodiscard]] auto operator()(const V& x) const {
            using S = typename V::value_type;
            auto coord = [&x](std::size_t i) -> S { return x[i]; };
            const auto& ch = graph.children;
            return detail::eval_constraint<S>(rel, std::get<0>(ch), std::get<1>(ch), coord);
        }
    };

    // ConstraintSet — a tuple of ConstraintExpr, modeling kalpa::Constraints
    // and offering random-access to the constraints as functors for SQP/ALM.
    // count()/feasible() satisfy the Constraints concept; operator[] is a
    // type-erased functor view (all residuals share signature V→scalar) so the
    // algorithms can index constraints at runtime.
    template<typename... Cs>
    struct ConstraintSet {
        std::tuple<Cs...> items;

        [[nodiscard]] static constexpr std::size_t count() noexcept { return sizeof...(Cs); }
        [[nodiscard]] constexpr std::size_t size() const noexcept { return sizeof...(Cs); }

        template<typename V>
        [[nodiscard]] bool feasible(const V& x, typename V::value_type tol =
                                    static_cast<typename V::value_type>(1e-8)) const {
            bool ok = true;
            std::apply([&](const auto&... c) {
                (( ok = ok && (c.rel == Relation::Eq ? std::abs(c(x)) <= tol
                                                      : c(x) <= tol) ), ...);
            }, items);
            return ok;
        }

        // Runtime-indexed residual: needed by SQP/ALM which loop i = 0..m−1.
        template<typename V>
        [[nodiscard]] typename V::value_type residual(std::size_t i, const V& x) const {
            using T = typename V::value_type;
            T out{};
            std::size_t k = 0;
            std::apply([&](const auto&... c) {
                (( (k++ == i) ? (void)(out = c(x)) : (void)0 ), ...);
            }, items);
            return out;
        }
    };

    // subject_to(c1, c2, ...) — each ci is a comparison expression (x[0]+x[1]==1
    // etc.). Wrap each in ConstraintExpr, pack into a ConstraintSet.
    template<typename... Cs>
    [[nodiscard]] auto subject_to(Cs&&... cs) {
        return ConstraintSet<ConstraintExpr<std::decay_t<Cs>>...>{
            std::tuple{ ConstraintExpr<std::decay_t<Cs>>{std::forward<Cs>(cs)}... }
        };
    }

    // constrained_problem(objective, subject_to(...)) → a Problem whose
    // constraints slot is the ConstraintSet. Domain stays FullSpace (the
    // constrained algorithms enforce feasibility themselves).
    template<typename T = double, typename E, typename Cons>
    [[nodiscard]] auto constrained_problem(E&& expr, Cons&& cons) {
        auto obj = wrap(std::forward<E>(expr));
        using V = ga::Vector<T>;
        return Problem<decltype(obj), std::decay_t<Cons>, FullSpace<V>>{
            std::move(obj), std::forward<Cons>(cons), {} };
    }

    // =======================================================================
    // Auto — analyze the objective graph and dispatch to a solver whose
    // assumptions the objective satisfies. Structural properties are cached in
    // a vakya::property_store keyed by structural_key(expr); a second call on
    // the same expression reuses the tags. The solver set is fixed at compile
    // time; the choice among them is a runtime branch, and every branch
    // normalizes to expected<Result<T>, Diagnosis>.
    // =======================================================================
    namespace prop {
        using linear_key = ::vakya::property_key<bool, "kalpa.linear">;
        using convex_key = ::vakya::property_key<bool, "kalpa.convex">;
        using smooth_key = ::vakya::property_key<bool, "kalpa.smooth">;
        using dim_key    = ::vakya::property_key<std::size_t, "kalpa.dim">;
    }

    struct Analysis { bool linear{true}, convex{true}, smooth{true}; std::size_t dim{0}; };

    enum class MethodChoice { LBFGS, Newton, CMAES };

    namespace detail {
        // structural walk collecting linear/convex/smooth + max Var index.
        template<typename Node>
        void analyze_walk(const Node& node, Analysis& a);

        template<typename Child>
        void analyze_child(const Child& c, Analysis& a) {
            using C = std::decay_t<Child>;
            if constexpr (::vakya::Expression<Child>) analyze_walk(c, a);
            else if constexpr (std::is_same_v<C, Var>)
                a.dim = std::max(a.dim, c.idx + 1);
            else if constexpr (requires { c.value; })
                analyze_child(c.value, a);   // peel expr<Var>/expr<Const> (rvalue wrappers)
            else if constexpr (requires { *c.p; })
                analyze_child(*c.p, a);      // peel expr_ref<Var>/expr_ref<Const> (lvalue wrappers)
            // bare arithmetic literals / Const contribute nothing to dim.
        }

        // wrapped-terminal peeling handled inline in analyze_child above.

        template<typename Node>
        void analyze_walk(const Node& node, Analysis& a) {
            using Tag = typename Node::tag_type;
            const auto& ch = node.children;
            if constexpr (std::is_same_v<Tag, ::vakya::add_tag> ||
                          std::is_same_v<Tag, ::vakya::sub_tag>) {
                // affine-closed: linear/convex/smooth all preserved
                analyze_child(std::get<0>(ch), a);
                analyze_child(std::get<1>(ch), a);
            } else if constexpr (std::is_same_v<Tag, ::vakya::neg_tag>) {
                analyze_child(std::get<0>(ch), a);   // convexity flips but − of convex handled by callers; keep smooth/linear
            } else if constexpr (std::is_same_v<Tag, ::vakya::mul_tag>) {
                // x*x (and general products of non-constants) are nonlinear;
                // convexity only guaranteed for a squared affine term, which is
                // the dominant EDSL idiom (sq()). Mark nonlinear, keep smooth,
                // leave convex true (sq is convex) — refined below for div.
                a.linear = false;
                analyze_child(std::get<0>(ch), a);
                analyze_child(std::get<1>(ch), a);
            } else if constexpr (std::is_same_v<Tag, ::vakya::div_tag>) {
                a.linear = false; a.convex = false;   // general 1/g is nonconvex
                analyze_child(std::get<0>(ch), a);
                analyze_child(std::get<1>(ch), a);
            } else {
                // unknown / comparison tags shouldn't appear in an objective;
                // be conservative.
                a.linear = false; a.convex = false; a.smooth = false;
            }
        }
    } // namespace detail

    // analyze(expr) → Analysis, also stashed into the store under expr's key.
    template<typename E>
    [[nodiscard]] Analysis analyze(const E& expr, ::vakya::property_store& store) {
        Analysis a;
        if constexpr (::vakya::Expression<E>) detail::analyze_walk(expr, a);
        else detail::analyze_child(expr, a);   // bare terminal objective
        store.update_for(expr, [&](::vakya::property_set& ps) {
            ps.set<prop::linear_key>(a.linear);
            ps.set<prop::convex_key>(a.convex);
            ps.set<prop::smooth_key>(a.smooth);
            ps.set<prop::dim_key>(a.dim);
        });
        return a;
    }

    // choose — pure selection logic, testable in isolation.
    struct Choice { MethodChoice algo; Analysis analysis; };
    template<typename E>
    [[nodiscard]] Choice choose(const E& expr, ::vakya::property_store& store) {
        const Analysis a = analyze(expr, store);
        MethodChoice m;
        if (!a.smooth)                 m = MethodChoice::CMAES;   // nonsmooth ⇒ derivative-free
        else if (a.convex && a.dim <= 20) m = MethodChoice::Newton; // small convex ⇒ 2nd order
        else if (a.convex)             m = MethodChoice::LBFGS;   // convex, larger ⇒ quasi-Newton
        else                           m = MethodChoice::LBFGS;   // smooth nonconvex ⇒ LBFGS
        return Choice{m, a};
    }

    // Auto — owns the store (move-only: property_store holds a shared_mutex).
    template<typename T = double>
    struct Auto {
        ::vakya::property_store store;

        Auto() = default;
        Auto(const Auto&) = delete;
        Auto& operator=(const Auto&) = delete;
        Auto(Auto&&) = default;
        Auto& operator=(Auto&&) = default;

        // solve an unconstrained EDSL problem, dispatching by structural analysis.
        template<typename Prob>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const Prob& prob, const ga::Vector<T>& x0) {
            const auto& obj = prob.objective;      // Expr<Graph>
            const Choice ch = choose(obj.graph, store);
            switch (ch.algo) {
                case MethodChoice::Newton: {
                    Solver<Newton<T>, Derivatives<Dual, T>> s;
                    return s.solve(prob, x0);
                }
                case MethodChoice::CMAES: {
                    // derivative-free over the raw objective from x0.
                    CMAES<T> alg;
                    return alg.solve(obj, x0);
                }
                case MethodChoice::LBFGS:
                default: {
                    Solver<LBFGS<T>, Derivatives<Dual, T>> s;
                    return s.solve(prob, x0);
                }
            }
        }
    };

} // namespace kalpa::edsl

#endif // PEBBLE_KALPA_EDSL_MODEL_HPP
