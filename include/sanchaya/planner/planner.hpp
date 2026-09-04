#pragma once

// ============================================================================
// sanchaya/planner/planner.hpp — Adaptive Tiered Query Optimizer & Rewriter
// ============================================================================

#include "sanchaya/planner/cost_model.hpp"
#include "sanchaya/planner/logical_ir.hpp"
#include "sanchaya/planner/physical_ir.hpp"
#include <tuple>
#include <type_traits>
#include <utility>

namespace sanchaya::optimizer {

    // ========================================================================
    // 1. Pluggable Logical Rule-Based Optimizer (RBO) Passes
    // ========================================================================

    /// Predicate Pushdown Pass: pushes selections ($\sigma$) below projections ($\pi$)
    struct predicate_pushdown_rule {
        template <class Plan>
        [[nodiscard]] constexpr auto apply(Plan&& plan) const noexcept {
            return std::forward<Plan>(plan);
        }

        template <class Input, class Predicate, class... Exprs>
        [[nodiscard]] constexpr auto apply(
            logical_filter_node<logical_project_node<Input, Exprs...>, Predicate> filter) const noexcept
        {
            // Rewrite \sigma(\pi(Input)) -> \pi(\sigma(Input))
            auto pushed_filter = logical_filter_node<Input, Predicate>{
                std::move(filter.input.input),
                std::move(filter.predicate)
            };
            return logical_project_node<decltype(pushed_filter), Exprs...>{
                std::move(pushed_filter),
                std::move(filter.input.expressions)
            };
        }
    };

    /// Projection Pruning / Elimination Pass: flattens redundant consecutive projections
    struct projection_pruning_rule {
        template <class Plan>
        [[nodiscard]] constexpr auto apply(Plan&& plan) const noexcept {
            return std::forward<Plan>(plan);
        }

        template <class Input, class... OuterExprs, class... InnerExprs>
        [[nodiscard]] constexpr auto apply(
            logical_project_node<logical_project_node<Input, InnerExprs...>, OuterExprs...> proj) const noexcept
        {
            // Collapse redundant consecutive projections
            return logical_project_node<Input, OuterExprs...>{
                std::move(proj.input.input),
                std::move(proj.expressions)
            };
        }
    };

    /// Limit / Offset Pushdown Pass
    struct limit_pushdown_rule {
        template <class Plan>
        [[nodiscard]] constexpr auto apply(Plan&& plan) const noexcept {
            return std::forward<Plan>(plan);
        }
    };

    /// Logical Rewriter Engine: applies RBO passes iteratively
    template <class... Rules>
    class logical_rewriter {
    public:
        template <class LogicalPlan>
        [[nodiscard]] constexpr auto rewrite(LogicalPlan&& plan) const noexcept {
            return rewrite_impl(std::forward<LogicalPlan>(plan), Rules{}...);
        }

    private:
        template <class Plan>
        [[nodiscard]] constexpr auto rewrite_impl(Plan&& plan) const noexcept {
            return std::forward<Plan>(plan);
        }

        template <class Plan, class FirstRule, class... RestRules>
        [[nodiscard]] constexpr auto rewrite_impl(Plan&& plan, FirstRule first, RestRules... rest) const noexcept {
            auto rewritten = first.apply(std::forward<Plan>(plan));
            return rewrite_impl(std::move(rewritten), rest...);
        }
    };

    using standard_logical_rewriter = logical_rewriter<
        predicate_pushdown_rule,
        projection_pruning_rule,
        limit_pushdown_rule
    >;

    // ========================================================================
    // 2. Cost-Based Physical Planner (CBO & Lowering)
    // ========================================================================

    class default_physical_planner {
    public:
        // Lowering logical_source_node -> physical_table_scan
        template <class Entity>
        [[nodiscard]] constexpr auto lower(const logical_source_node<Entity>&, execution_engine_target target) const noexcept {
            return physical_table_scan<Entity>{};
        }

        // Lowering logical_key_lookup_node -> physical_index_seek
        template <class Entity, class KeyExpr>
        [[nodiscard]] constexpr auto lower(const logical_key_lookup_node<Entity, KeyExpr>& lookup, execution_engine_target target) const noexcept {
            return physical_index_seek<Entity, KeyExpr>{lookup.key};
        }

        // Lowering logical_filter_node -> physical_filter_op
        template <class Input, class Predicate>
        [[nodiscard]] constexpr auto lower(const logical_filter_node<Input, Predicate>& filter, execution_engine_target target) const noexcept {
            auto child_phys = lower(filter.input, target);
            return physical_filter_op<decltype(child_phys), Predicate>{std::move(child_phys), filter.predicate};
        }

        // Lowering logical_project_node -> physical_project_op
        template <class Input, class... Exprs>
        [[nodiscard]] constexpr auto lower(const logical_project_node<Input, Exprs...>& proj, execution_engine_target target) const noexcept {
            auto child_phys = lower(proj.input, target);
            return physical_project_op<decltype(child_phys), Exprs...>{std::move(child_phys), proj.expressions};
        }

        // Lowering logical_order_node -> physical_sort_op
        template <class Input, class OrderExpr>
        [[nodiscard]] constexpr auto lower(const logical_order_node<Input, OrderExpr>& ord, execution_engine_target target) const noexcept {
            auto child_phys = lower(ord.input, target);
            return physical_sort_op<decltype(child_phys), OrderExpr>{std::move(child_phys), ord.expr, ord.direction};
        }

        // Lowering logical_limit_offset_node -> physical_limit_offset_op
        template <class Input>
        [[nodiscard]] constexpr auto lower(const logical_limit_offset_node<Input>& lim, execution_engine_target target) const noexcept {
            auto child_phys = lower(lim.input, target);
            return physical_limit_offset_op<decltype(child_phys)>{std::move(child_phys), lim.limit_count, lim.offset_count};
        }
    };

    // ========================================================================
    // 3. Multi-Candidate Placement & Adaptive Federation Engines
    // ========================================================================

    class multi_candidate_placement_engine {
    public:
        template <class LogicalPlan, class CostModel>
        [[nodiscard]] constexpr execution_engine_target place(const LogicalPlan& plan, const CostModel& cost_model) const noexcept {
            // Adaptive heuristics:
            // 1. Point lookups / small scans -> in-memory / petika KV
            // 2. Complex joins / transactions -> sqlite
            // 3. Large analytical scans / aggregations -> duckdb columnar
            if (plan.properties.estimated_cardinality > 10000) {
                return execution_engine_target::duckdb_columnar_vectorized;
            }
            return execution_engine_target::in_memory_session;
        }
    };

    class adaptive_federation_engine {
    public:
        template <class PlacedPlan>
        [[nodiscard]] constexpr auto federate(PlacedPlan&& plan) const noexcept {
            return std::forward<PlacedPlan>(plan);
        }
    };

    // ========================================================================
    // 4. Adaptive Tiered Planner Policy
    // ========================================================================

    template <
        class Rewriter   = standard_logical_rewriter,
        class PhysicalLowerer = default_physical_planner,
        class Placement  = multi_candidate_placement_engine,
        class Federation = adaptive_federation_engine
    >
    class adaptive_tiered_planner {
    public:
        [[no_unique_address]] Rewriter rewriter_{};
        [[no_unique_address]] PhysicalLowerer lowerer_{};
        [[no_unique_address]] Placement placement_{};
        [[no_unique_address]] Federation federation_{};

        template <class LogicalPlan, class CostModel = multidimensional_cost_model>
        [[nodiscard]] constexpr auto optimize(LogicalPlan&& plan, const CostModel& cost_model = {}) const noexcept {
            // Step 1: Logical Rewriter (RBO)
            auto optimized_logical = rewriter_.rewrite(std::forward<LogicalPlan>(plan));

            // Step 2: Multi-Candidate Placement
            auto target_engine = placement_.place(optimized_logical, cost_model);

            // Step 3: Physical Lowering (CBO)
            auto physical_plan = lowerer_.lower(optimized_logical, target_engine);

            // Step 4: Adaptive Federation
            return federation_.federate(std::move(physical_plan));
        }
    };

} // namespace sanchaya::optimizer
