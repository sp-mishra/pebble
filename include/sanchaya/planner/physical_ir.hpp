#pragma once

// ============================================================================
// sanchaya/planner/physical_ir.hpp — Concrete Physical Execution Plan IR
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "sanchaya/planner/cost_model.hpp"
#include <tuple>
#include <type_traits>
#include <utility>

namespace sanchaya::optimizer {

    // ========================================================================
    // 1. Engine Affinity & Execution Strategy Tags
    // ========================================================================
    enum class execution_engine_target : std::uint8_t {
        in_memory_session,
        petika_kv,
        sqlite_transactional,
        duckdb_columnar_vectorized,
        remote_federated
    };

    struct physical_properties {
        execution_engine_target target_engine{execution_engine_target::in_memory_session};
        bool is_streamable{true};
        bool is_vectorized{false};
        bool is_parallelizable{false};
        plan_cost estimated_cost{};
    };

    // ========================================================================
    // 2. Physical Execution Operators
    // ========================================================================

    /// Physical sequential/table scan
    template <class Entity, execution_engine_target Target = execution_engine_target::in_memory_session>
    struct physical_table_scan {
        using entity_type = Entity;
        static constexpr execution_engine_target target_engine = Target;
        physical_properties properties{};

        constexpr physical_table_scan() noexcept {
            properties.target_engine = Target;
            properties.is_streamable = true;
            properties.is_vectorized = (Target == execution_engine_target::duckdb_columnar_vectorized);
        }
    };

    /// Physical index seek (e.g. B-Tree / SkipList primary key point lookup)
    template <class Entity, class KeyExpr, execution_engine_target Target = execution_engine_target::petika_kv>
    struct physical_index_seek {
        using entity_type = Entity;
        using key_expr_type = KeyExpr;
        static constexpr execution_engine_target target_engine = Target;

        [[no_unique_address]] KeyExpr key;
        physical_properties properties{};

        constexpr explicit physical_index_seek(KeyExpr k) noexcept
            : key(std::move(k)) {
            properties.target_engine = Target;
            properties.is_streamable = false;
        }
    };

    /// Physical filter operator
    template <class ChildOp, class Predicate>
    struct physical_filter_op {
        using child_type = ChildOp;
        using predicate_type = Predicate;

        ChildOp child;
        [[no_unique_address]] Predicate predicate;
        physical_properties properties{};

        constexpr physical_filter_op(ChildOp c, Predicate p) noexcept
            : child(std::move(c)), predicate(std::move(p)) {
            properties = child.properties;
        }
    };

    /// Physical projection operator
    template <class ChildOp, class... Exprs>
    struct physical_project_op {
        using child_type = ChildOp;

        ChildOp child;
        std::tuple<Exprs...> expressions;
        physical_properties properties{};

        constexpr physical_project_op(ChildOp c, std::tuple<Exprs...> exprs) noexcept
            : child(std::move(c)), expressions(std::move(exprs)) {
            properties = child.properties;
        }
    };

    /// Physical hash join
    template <class LeftOp, class RightOp, class LeftKey, class RightKey>
    struct physical_hash_join_op {
        using left_type = LeftOp;
        using right_type = RightOp;

        LeftOp left;
        RightOp right;
        [[no_unique_address]] LeftKey left_key;
        [[no_unique_address]] RightKey right_key;
        physical_properties properties{};

        constexpr physical_hash_join_op(LeftOp l, RightOp r, LeftKey lk, RightKey rk) noexcept
            : left(std::move(l)), right(std::move(r)), left_key(std::move(lk)), right_key(std::move(rk)) {
            properties.target_engine = left.properties.target_engine;
            properties.is_streamable = false; // Pipeline breaker on build side
        }
    };

    /// Physical nested loop join
    template <class LeftOp, class RightOp, class Predicate>
    struct physical_nested_loop_join_op {
        using left_type = LeftOp;
        using right_type = RightOp;

        LeftOp left;
        RightOp right;
        [[no_unique_address]] Predicate predicate;
        physical_properties properties{};

        constexpr physical_nested_loop_join_op(LeftOp l, RightOp r, Predicate p) noexcept
            : left(std::move(l)), right(std::move(r)), predicate(std::move(p)) {
            properties.target_engine = left.properties.target_engine;
        }
    };

    /// Physical streaming / hash aggregate operator
    template <class ChildOp, class GroupKeysTuple, class AggregatesTuple>
    struct physical_aggregate_op {
        using child_type = ChildOp;

        ChildOp child;
        GroupKeysTuple group_keys;
        AggregatesTuple aggregates;
        physical_properties properties{};

        constexpr physical_aggregate_op(ChildOp c, GroupKeysTuple keys, AggregatesTuple aggs) noexcept
            : child(std::move(c)), group_keys(std::move(keys)), aggregates(std::move(aggs)) {
            properties = child.properties;
            properties.is_streamable = false;
        }
    };

    /// Physical sort operator
    template <class ChildOp, class OrderExpr>
    struct physical_sort_op {
        using child_type = ChildOp;

        ChildOp child;
        [[no_unique_address]] OrderExpr expr;
        sort_direction direction{sort_direction::ascending};
        physical_properties properties{};

        constexpr physical_sort_op(ChildOp c, OrderExpr ex, sort_direction dir) noexcept
            : child(std::move(c)), expr(std::move(ex)), direction(dir) {
            properties = child.properties;
            properties.is_streamable = false; // Pipeline breaker
        }
    };

    /// Physical limit / offset operator
    template <class ChildOp>
    struct physical_limit_offset_op {
        using child_type = ChildOp;

        ChildOp child;
        std::size_t limit_count{0};
        std::size_t offset_count{0};
        physical_properties properties{};

        constexpr physical_limit_offset_op(ChildOp c, std::size_t lim, std::size_t off) noexcept
            : child(std::move(c)), limit_count(lim), offset_count(off) {
            properties = child.properties;
        }
    };

    /// Direct engine-delegated federated scan for SQLite
    template <class Entity, class PredicateTuple, class ProjectTuple>
    struct physical_sqlite_federated_op {
        using entity_type = Entity;
        PredicateTuple predicates;
        ProjectTuple projections;
        physical_properties properties{};

        constexpr physical_sqlite_federated_op(PredicateTuple preds, ProjectTuple projs) noexcept
            : predicates(std::move(preds)), projections(std::move(projs)) {
            properties.target_engine = execution_engine_target::sqlite_transactional;
            properties.is_streamable = true;
        }
    };

    /// Direct engine-delegated vectorized scan for DuckDB
    template <class Entity, class PredicateTuple, class ProjectTuple, class AggTuple>
    struct physical_duckdb_vectorized_op {
        using entity_type = Entity;
        PredicateTuple predicates;
        ProjectTuple projections;
        AggTuple aggregates;
        physical_properties properties{};

        constexpr physical_duckdb_vectorized_op(PredicateTuple preds, ProjectTuple projs, AggTuple aggs) noexcept
            : predicates(std::move(preds)), projections(std::move(projs)), aggregates(std::move(aggs)) {
            properties.target_engine = execution_engine_target::duckdb_columnar_vectorized;
            properties.is_vectorized = true;
        }
    };

    // ========================================================================
    // 3. Physical Plan Concept & Traits
    // ========================================================================
    template <class T>
    struct is_physical_plan_node : std::false_type {};

    template <class E, execution_engine_target T>
    struct is_physical_plan_node<physical_table_scan<E, T>> : std::true_type {};

    template <class E, class K, execution_engine_target T>
    struct is_physical_plan_node<physical_index_seek<E, K, T>> : std::true_type {};

    template <class C, class P>
    struct is_physical_plan_node<physical_filter_op<C, P>> : std::true_type {};

    template <class C, class... X>
    struct is_physical_plan_node<physical_project_op<C, X...>> : std::true_type {};

    template <class L, class R, class LK, class RK>
    struct is_physical_plan_node<physical_hash_join_op<L, R, LK, RK>> : std::true_type {};

    template <class L, class R, class P>
    struct is_physical_plan_node<physical_nested_loop_join_op<L, R, P>> : std::true_type {};

    template <class C, class G, class A>
    struct is_physical_plan_node<physical_aggregate_op<C, G, A>> : std::true_type {};

    template <class C, class O>
    struct is_physical_plan_node<physical_sort_op<C, O>> : std::true_type {};

    template <class C>
    struct is_physical_plan_node<physical_limit_offset_op<C>> : std::true_type {};

    template <class E, class PR, class PJ>
    struct is_physical_plan_node<physical_sqlite_federated_op<E, PR, PJ>> : std::true_type {};

    template <class E, class PR, class PJ, class AG>
    struct is_physical_plan_node<physical_duckdb_vectorized_op<E, PR, PJ, AG>> : std::true_type {};

    template <class T>
    inline constexpr bool is_physical_plan_node_v = is_physical_plan_node<std::decay_t<T>>::value;

    template <class T>
    concept physical_plan = is_physical_plan_node_v<T>;

} // namespace sanchaya::optimizer
