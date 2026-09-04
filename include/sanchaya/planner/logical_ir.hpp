#pragma once

// ============================================================================
// sanchaya/planner/logical_ir.hpp — Clean Relational Logical IR Operators
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "sanchaya/planner/cost_model.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include <tuple>
#include <type_traits>
#include <utility>

namespace sanchaya::optimizer {

    // ========================================================================
    // 1. Relational Operator Logical IR Nodes
    // ========================================================================

    /// Logical base scan operator for an entity
    template <class Entity>
    struct logical_source_node {
        using entity_type = Entity;
        logical_properties properties{};

        constexpr logical_source_node() noexcept {
            properties.estimated_cardinality = 1000; // Baseline estimate
        }
    };

    /// Logical point lookup on an entity with a specific key expression
    template <class Entity, class KeyExpr>
    struct logical_key_lookup_node {
        using entity_type = Entity;
        using key_expr_type = KeyExpr;
        [[no_unique_address]] KeyExpr key;
        logical_properties properties{};

        constexpr explicit logical_key_lookup_node(KeyExpr k) noexcept
            : key(std::move(k)) {
            properties.estimated_cardinality = 1;
        }
    };

    /// Logical filter / selection operator ($\sigma$)
    template <class Input, class Predicate>
    struct logical_filter_node {
        using input_type = Input;
        using predicate_type = Predicate;
        Input input;
        [[no_unique_address]] Predicate predicate;
        logical_properties properties{};

        constexpr logical_filter_node(Input in, Predicate pred) noexcept
            : input(std::move(in)), predicate(std::move(pred)) {
            // Selectivity estimation: standard default 10% selectivity
            properties = input.properties;
            properties.estimated_cardinality = (input.properties.estimated_cardinality + 9) / 10;
        }
    };

    /// Logical projection operator ($\pi$)
    template <class Input, class... Exprs>
    struct logical_project_node {
        using input_type = Input;
        Input input;
        std::tuple<Exprs...> expressions;
        logical_properties properties{};

        constexpr logical_project_node(Input in, std::tuple<Exprs...> exprs) noexcept
            : input(std::move(in)), expressions(std::move(exprs)) {
            properties = input.properties;
        }
    };

    /// Logical traverse / relationship expansion operator
    template <class Input, akshara::fixed_string RelationName, akshara::fixed_string Alias>
    struct logical_traverse_node {
        using input_type = Input;
        static constexpr auto relation_name = RelationName;
        static constexpr auto alias_name = Alias;
        Input input;
        logical_properties properties{};

        constexpr explicit logical_traverse_node(Input in) noexcept
            : input(std::move(in)) {
            properties = input.properties;
        }
    };

    /// Logical join operator ($\bowtie$)
    template <class Left, class Right, class Predicate, join_kind Kind = join_kind::inner>
    struct logical_join_node {
        using left_type = Left;
        using right_type = Right;
        using predicate_type = Predicate;
        static constexpr join_kind kind = Kind;

        Left left;
        Right right;
        [[no_unique_address]] Predicate predicate;
        logical_properties properties{};

        constexpr logical_join_node(Left l, Right r, Predicate pred) noexcept
            : left(std::move(l)), right(std::move(r)), predicate(std::move(pred)) {
            properties.estimated_cardinality =
                std::max(left.properties.estimated_cardinality, right.properties.estimated_cardinality);
        }
    };

    /// Logical grouping operator ($\gamma$)
    template <class Input, class... KeyExprs>
    struct logical_group_node {
        using input_type = Input;
        Input input;
        std::tuple<KeyExprs...> group_keys;
        logical_properties properties{};

        constexpr logical_group_node(Input in, std::tuple<KeyExprs...> keys) noexcept
            : input(std::move(in)), group_keys(std::move(keys)) {
            properties = input.properties;
            properties.estimated_cardinality = std::max<std::size_t>(1, input.properties.estimated_cardinality / 10);
        }
    };

    /// Logical aggregation operator
    template <class Input, class... Functions>
    struct logical_aggregate_node {
        using input_type = Input;
        Input input;
        std::tuple<Functions...> aggregates;
        logical_properties properties{};

        constexpr logical_aggregate_node(Input in, std::tuple<Functions...> aggs) noexcept
            : input(std::move(in)), aggregates(std::move(aggs)) {
            properties = input.properties;
            properties.estimated_cardinality = 1; // Scalar aggregation
        }
    };

    /// Logical ordering / sort operator ($\tau$)
    template <class Input, class OrderExpr>
    struct logical_order_node {
        using input_type = Input;
        using order_expr_type = OrderExpr;
        Input input;
        [[no_unique_address]] OrderExpr expr;
        sort_direction direction{sort_direction::ascending};
        logical_properties properties{};

        constexpr logical_order_node(Input in, OrderExpr ex, sort_direction dir) noexcept
            : input(std::move(in)), expr(std::move(ex)), direction(dir) {
            properties = input.properties;
        }
    };

    /// Logical limit / offset slicing operator
    template <class Input>
    struct logical_limit_offset_node {
        using input_type = Input;
        Input input;
        std::size_t limit_count{0};
        std::size_t offset_count{0};
        logical_properties properties{};

        constexpr logical_limit_offset_node(Input in, std::size_t lim, std::size_t off) noexcept
            : input(std::move(in)), limit_count(lim), offset_count(off) {
            properties = input.properties;
            if (limit_count > 0 && limit_count < properties.estimated_cardinality) {
                properties.estimated_cardinality = limit_count;
            }
        }
    };

    /// Logical cross-backend exchange boundary
    template <class Input, class DistributionPolicy>
    struct logical_exchange_node {
        using input_type = Input;
        Input input;
        [[no_unique_address]] DistributionPolicy distribution;
        logical_properties properties{};

        constexpr logical_exchange_node(Input in, DistributionPolicy dist) noexcept
            : input(std::move(in)), distribution(std::move(dist)) {
            properties = input.properties;
        }
    };

    /// Logical materialization boundary
    template <class Input, class StorageTarget>
    struct logical_materialize_node {
        using input_type = Input;
        Input input;
        [[no_unique_address]] StorageTarget target;
        logical_properties properties{};

        constexpr logical_materialize_node(Input in, StorageTarget tgt) noexcept
            : input(std::move(in)), target(std::move(tgt)) {
            properties = input.properties;
        }
    };

    // ========================================================================
    // 2. Logical Node Concepts & Traits
    // ========================================================================
    template <class T>
    struct is_logical_plan_node : std::false_type {};

    template <class E>
    struct is_logical_plan_node<logical_source_node<E>> : std::true_type {};

    template <class E, class K>
    struct is_logical_plan_node<logical_key_lookup_node<E, K>> : std::true_type {};

    template <class I, class P>
    struct is_logical_plan_node<logical_filter_node<I, P>> : std::true_type {};

    template <class I, class... X>
    struct is_logical_plan_node<logical_project_node<I, X...>> : std::true_type {};

    template <class I, akshara::fixed_string R, akshara::fixed_string A>
    struct is_logical_plan_node<logical_traverse_node<I, R, A>> : std::true_type {};

    template <class L, class R, class P, join_kind K>
    struct is_logical_plan_node<logical_join_node<L, R, P, K>> : std::true_type {};

    template <class I, class... K>
    struct is_logical_plan_node<logical_group_node<I, K...>> : std::true_type {};

    template <class I, class... F>
    struct is_logical_plan_node<logical_aggregate_node<I, F...>> : std::true_type {};

    template <class I, class O>
    struct is_logical_plan_node<logical_order_node<I, O>> : std::true_type {};

    template <class I>
    struct is_logical_plan_node<logical_limit_offset_node<I>> : std::true_type {};

    template <class I, class D>
    struct is_logical_plan_node<logical_exchange_node<I, D>> : std::true_type {};

    template <class I, class S>
    struct is_logical_plan_node<logical_materialize_node<I, S>> : std::true_type {};

    template <class T>
    inline constexpr bool is_logical_plan_node_v = is_logical_plan_node<std::decay_t<T>>::value;

    template <class T>
    concept logical_plan = is_logical_plan_node_v<T>;

} // namespace sanchaya::optimizer
