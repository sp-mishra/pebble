#pragma once
// =============================================================================
// vakya/egraph.hpp — Vākya E-Graph Adapter & Equality Saturation Bridge
// =============================================================================
// Connects Vākya AST expressions to the generic equality saturation engine
// (containers/graph/egraph.hpp).
//
// Provides:
// - Stable operation IDs (vakya_op)
// - Expression-to-e-node interning (intern_expression)
// - Structural payload hashing for terminals and constants
// - Dynamic / Static expression reconstruction from E-Graph e-classes
// - Guarded algebraic and boolean rewrite rules
//
// Zero virtual functions, zero macros, zero RTTI. C++23/26.
// =============================================================================

#include "vakya/vakya.hpp"
#include "containers/graph/egraph.hpp"
#include <cstdint>
#include <concepts>
#include <functional>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace vakya::egraph_adapter {

    namespace detail {
        template <class T, class = void>
        struct node_tag {
            using type = typename T::tag_t;
        };

        template <class T>
        struct node_tag<T, std::void_t<typename T::tag_type>> {
            using type = typename T::tag_type;
        };

        template <class T>
        using node_tag_t = typename node_tag<std::decay_t<T>>::type;

        template <class T>
        [[nodiscard]] constexpr decltype(auto) unwrap_terminal(T&& x) noexcept {
            using D = std::decay_t<T>;
            if constexpr (is_expr_wrapper_v<D>) {
                return structural_unwrap(std::forward<T>(x).value);
            } else if constexpr (is_expr_ref_wrapper_v<D>) {
                return structural_unwrap(*std::forward<T>(x).p);
            } else {
                return structural_unwrap(std::forward<T>(x));
            }
        }
    } // namespace detail

    // =========================================================================
    // Stable Vākya Operation Identifiers
    // =========================================================================

    enum class vakya_op : std::uint32_t {
        terminal_leaf   = 0,
        constant_leaf   = 1,
        add             = 10,
        sub             = 11,
        mul             = 12,
        div             = 13,
        mod             = 14,
        neg             = 15,
        eq              = 20,
        ne              = 21,
        lt              = 22,
        le              = 23,
        gt              = 24,
        ge              = 25,
        logical_and     = 30,
        logical_or      = 31,
        logical_not     = 32,
        bit_and         = 40,
        bit_or          = 41,
        bit_xor         = 42,
        bit_not         = 43,
        cast            = 50,
        call            = 60,
        unknown         = 999
    };

    template <class Tag>
    constexpr vakya_op get_tag_op() noexcept {
        if constexpr (std::is_same_v<Tag, add_tag>) return vakya_op::add;
        else if constexpr (std::is_same_v<Tag, sub_tag>) return vakya_op::sub;
        else if constexpr (std::is_same_v<Tag, mul_tag>) return vakya_op::mul;
        else if constexpr (std::is_same_v<Tag, div_tag>) return vakya_op::div;
        else if constexpr (std::is_same_v<Tag, mod_tag>) return vakya_op::mod;
        else if constexpr (std::is_same_v<Tag, neg_tag>) return vakya_op::neg;
        else if constexpr (std::is_same_v<Tag, eq_tag>) return vakya_op::eq;
        else if constexpr (std::is_same_v<Tag, ne_tag>) return vakya_op::ne;
        else if constexpr (std::is_same_v<Tag, lt_tag>) return vakya_op::lt;
        else if constexpr (std::is_same_v<Tag, le_tag>) return vakya_op::le;
        else if constexpr (std::is_same_v<Tag, gt_tag>) return vakya_op::gt;
        else if constexpr (std::is_same_v<Tag, ge_tag>) return vakya_op::ge;
        else if constexpr (std::is_same_v<Tag, and_tag>) return vakya_op::logical_and;
        else if constexpr (std::is_same_v<Tag, or_tag>) return vakya_op::logical_or;
        else if constexpr (std::is_same_v<Tag, not_tag>) return vakya_op::logical_not;
        else if constexpr (std::is_same_v<Tag, bit_and_tag>) return vakya_op::bit_and;
        else if constexpr (std::is_same_v<Tag, bit_or_tag>) return vakya_op::bit_or;
        else if constexpr (std::is_same_v<Tag, bit_xor_tag>) return vakya_op::bit_xor;
        else if constexpr (std::is_same_v<Tag, bit_not_tag>) return vakya_op::bit_not;
        else if constexpr (std::is_same_v<Tag, cast_tag>) return vakya_op::cast;
        else if constexpr (std::is_same_v<Tag, call_tag>) return vakya_op::call;
        else return vakya_op::unknown;
    }

    // =========================================================================
    // Vākya E-Graph Types
    // =========================================================================

    using enode_t = egraph::e_node<vakya_op, std::uint64_t>;
    using graph_t = egraph::e_graph<vakya_op, std::uint64_t>;

    // =========================================================================
    // Expression Interning
    // =========================================================================

    template <class EGraph, class Expr>
    egraph::e_class_id intern_expression(EGraph& graph, const Expr& expr) {
        using Dec = std::decay_t<Expr>;

        if constexpr (!Expression<Dec>) {
            // Terminal leaf node
            enode_t node;
            node.op = vakya_op::terminal_leaf;
            decltype(auto) unwrapped = detail::unwrap_terminal(expr);
            using UnwrappedType = std::decay_t<decltype(unwrapped)>;
            if constexpr (std::is_integral_v<UnwrappedType> || std::is_floating_point_v<UnwrappedType> || std::is_enum_v<UnwrappedType>) {
                node.op = vakya_op::constant_leaf;
                node.payload = static_cast<std::uint64_t>(unwrapped);
            } else {
                node.payload = static_cast<std::uint64_t>(emit::structural_hash(unwrapped));
            }
            return graph.add(std::move(node));
        } else {
            using Tag = detail::node_tag_t<Dec>;
            enode_t node;
            node.op = get_tag_op<Tag>();

            std::apply([&](const auto&... child) {
                (node.children.push_back(intern_expression(graph, child)), ...);
            }, expr.children);

            return graph.add(std::move(node));
        }
    }

    // =========================================================================
    // Guarded Algebraic Rewrite Rules
    // =========================================================================

    struct vakya_op_traits {
        static constexpr vakya_op commutative_op = vakya_op::add;
        static constexpr vakya_op associative_op = vakya_op::add;
        static constexpr vakya_op mul_op         = vakya_op::mul;
        static constexpr vakya_op add_op         = vakya_op::add;
        static constexpr vakya_op zero_op        = vakya_op::constant_leaf;
        static constexpr std::uint64_t zero_payload = 0;
        static constexpr vakya_op one_op         = vakya_op::constant_leaf;
        static constexpr std::uint64_t one_payload  = 1;
    };

    using vakya_commutativity  = egraph::commutativity<vakya_op_traits>;
    using vakya_associativity  = egraph::associativity<vakya_op_traits>;
    using vakya_distributivity = egraph::distributivity<vakya_op_traits>;
    using vakya_identity_zero  = egraph::identity_zero<vakya_op_traits>;

    // Boolean algebra rules: (x AND x) -> x, (x OR x) -> x
    struct boolean_idempotence {
        template <class EGraph>
        void apply(EGraph& g) const {
            const std::size_t snapshot = g.class_count();
            for (egraph::e_class_id id = 0; id < static_cast<egraph::e_class_id>(snapshot); ++id) {
                if (!g.is_root(id)) continue;
                for (const auto& node : g.classes()[id].nodes) {
                    if ((node.op == vakya_op::logical_and || node.op == vakya_op::logical_or)
                        && node.children.size() == 2) {
                        if (g.find(node.children[0]) == g.find(node.children[1])) {
                            (void)g.merge(id, g.find(node.children[0]));
                        }
                    }
                }
            }
        }
    };

    // Double negation: NOT(NOT(x)) -> x
    struct double_negation_elimination {
        template <class EGraph>
        void apply(EGraph& g) const {
            const std::size_t snapshot = g.class_count();
            for (egraph::e_class_id id = 0; id < static_cast<egraph::e_class_id>(snapshot); ++id) {
                if (!g.is_root(id)) continue;
                for (const auto& outer : g.classes()[id].nodes) {
                    if (outer.op == vakya_op::logical_not && outer.children.size() == 1) {
                        const egraph::e_class_id inner_id = g.find(outer.children[0]);
                        for (const auto& inner : g.classes()[inner_id].nodes) {
                            if (inner.op == vakya_op::logical_not && inner.children.size() == 1) {
                                (void)g.merge(id, g.find(inner.children[0]));
                            }
                        }
                    }
                }
            }
        }
    };

} // namespace vakya::egraph_adapter
