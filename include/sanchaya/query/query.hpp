#pragma once

// ============================================================================
// sanchaya/query/query.hpp — Alias-Aware Type-Safe Query & Mutation EDSL
// ============================================================================

#include "sanchaya/fwd.hpp"
#include <concepts>
#include <tuple>
#include <utility>

namespace sanchaya {

    // ========================================================================
    // 1. Scope & Alias Bindings
    // ========================================================================
    template <akshara::fixed_string Alias, class Entity>
    struct binding {
        static constexpr auto name = Alias;
        using entity_type = Entity;
    };

    template <class... Bindings>
    struct query_scope {
        template <akshara::fixed_string Alias, auto MemberPtr>
        static constexpr bool is_member_visible() noexcept {
            return (
                (
                    Bindings::name == Alias &&
                    std::same_as<meta::member_owner_t<MemberPtr>, typename Bindings::entity_type>
                ) || ...
            );
        }
    };

    template <class Expr, class Scope>
    concept expression_visible_in_scope = requires {
        requires Scope::template is_member_visible<Expr::alias, Expr::member_pointer>();
    };

    template <class Expr, class Scope, logic_policy Logic = logic_policy::two_valued>
    concept query_predicate =
        vakya::Expression<Expr> ||
        boolean_expression<Expr, Logic>;

    template <akshara::fixed_string Alias, auto MemberPtr>
    [[nodiscard]] constexpr auto member() noexcept {
        return vakya::as_expr(aliased_member_access_descriptor<Alias, MemberPtr>{});
    }

    template <auto MemberPtr>
    [[nodiscard]] constexpr auto member() noexcept {
        return vakya::as_expr(aliased_member_access_descriptor<"root", MemberPtr>{});
    }

    // ========================================================================
    // 2. Logical Plan Placeholders for Query Builder
    // ========================================================================
    template <class Entity>
    struct source_plan {
        using entity_type = Entity;
    };

    template <class Plan, class Predicate>
    struct filter_plan {
        Plan plan;
        Predicate predicate;
    };

    template <class Plan, class... Exprs>
    struct project_plan {
        Plan plan;
        std::tuple<Exprs...> expressions;
    };

    template <class Plan, class... GroupExprs>
    struct group_by_plan {
        Plan plan;
        std::tuple<GroupExprs...> group_expressions;
    };

    template <class Plan>
    struct count_aggregate_plan {
        Plan plan;
    };

    template <class Plan, class OrderExpr>
    struct order_plan {
        Plan plan;
        OrderExpr expr;
        sort_direction direction;
    };

    template <class Plan>
    struct limit_plan {
        Plan plan;
        std::size_t count{0};
    };

    template <class Plan>
    struct offset_plan {
        Plan plan;
        std::size_t count{0};
    };

    // ========================================================================
    // 3. Query Builder Implementation
    // ========================================================================
    template <
        class RootEntity,
        class ActiveScope = query_scope<binding<"root", RootEntity>>,
        class LogicalPlan = source_plan<RootEntity>,
        logic_policy LogicPolicy = logic_policy::two_valued
    >
    class query_builder {
    public:
        using root_entity_type = RootEntity;
        using active_scope_type = ActiveScope;
        using logical_plan_type = LogicalPlan;
        static constexpr logic_policy active_logic_policy = LogicPolicy;

        constexpr query_builder() = default;
        constexpr explicit query_builder(LogicalPlan plan) : plan_(std::move(plan)) {}

        [[nodiscard]] constexpr const LogicalPlan& plan() const noexcept { return plan_; }

        template <class Expr>
            requires query_predicate<Expr, ActiveScope, LogicPolicy>
        [[nodiscard]] constexpr auto where(Expr&& predicate) && {
            using NewPlan = filter_plan<LogicalPlan, std::decay_t<Expr>>;
            return query_builder<RootEntity, ActiveScope, NewPlan, LogicPolicy>{
                NewPlan{std::move(plan_), std::forward<Expr>(predicate)}
            };
        }

        template <class... Exprs>
        [[nodiscard]] constexpr auto select(Exprs&&... exprs) && {
            using NewPlan = project_plan<LogicalPlan, std::decay_t<Exprs>...>;
            return query_builder<RootEntity, ActiveScope, NewPlan, LogicPolicy>{
                NewPlan{std::move(plan_), std::make_tuple(std::forward<Exprs>(exprs)...)}
            };
        }

        template <class... GroupExprs>
        [[nodiscard]] constexpr auto group_by(GroupExprs&&... exprs) && {
            using NewPlan = group_by_plan<LogicalPlan, std::decay_t<GroupExprs>...>;
            return query_builder<RootEntity, ActiveScope, NewPlan, LogicPolicy>{
                NewPlan{std::move(plan_), std::make_tuple(std::forward<GroupExprs>(exprs)...)}
            };
        }

        template <class Expr>
        [[nodiscard]] constexpr auto order_by(Expr&& expr, sort_direction dir = sort_direction::ascending) && {
            using NewPlan = order_plan<LogicalPlan, std::decay_t<Expr>>;
            return query_builder<RootEntity, ActiveScope, NewPlan, LogicPolicy>{
                NewPlan{std::move(plan_), std::forward<Expr>(expr), dir}
            };
        }

        [[nodiscard]] constexpr auto limit(std::size_t count) && {
            using NewPlan = limit_plan<LogicalPlan>;
            return query_builder<RootEntity, ActiveScope, NewPlan, LogicPolicy>{
                NewPlan{std::move(plan_), count}
            };
        }

        [[nodiscard]] constexpr auto offset(std::size_t count) && {
            using NewPlan = offset_plan<LogicalPlan>;
            return query_builder<RootEntity, ActiveScope, NewPlan, LogicPolicy>{
                NewPlan{std::move(plan_), count}
            };
        }

        [[nodiscard]] constexpr auto consistency(consistency_requirement req) && {
            consistency_ = req;
            return *this;
        }

        [[nodiscard]] constexpr auto consistency(bounded_staleness b) && {
            consistency_ = consistency_requirement::bounded_staleness;
            staleness_ = b.max_staleness;
            return *this;
        }

    private:
        LogicalPlan plan_{};
        consistency_requirement consistency_{consistency_requirement::eventual};
        std::chrono::nanoseconds staleness_{0};
    };

    template <class Entity>
    [[nodiscard]] constexpr auto from() noexcept {
        return query_builder<Entity>{};
    }

    // Common Aggregate Helpers
    struct count_expr { using result_type = std::uint64_t; };
    struct group_key_expr { using result_type = void; };

    template <class Expr>
    struct average_expr {
        using result_type = double;
        Expr expr;
    };

    [[nodiscard]] constexpr auto count() noexcept { return count_expr{}; }
    [[nodiscard]] constexpr auto group_key() noexcept { return group_key_expr{}; }

    template <class Expr>
    [[nodiscard]] constexpr auto average(Expr&& expr) noexcept {
        return average_expr<std::decay_t<Expr>>{std::forward<Expr>(expr)};
    }

    // ========================================================================
    // 4. Mutation Builder Implementation
    // ========================================================================
    template <class Target, class Source>
    concept assignment_compatible = std::is_assignable_v<Target&, Source> || std::is_convertible_v<Source, Target>;

    template <class Entity, logic_policy LogicPolicy = logic_policy::two_valued>
    class update_builder {
    public:
        using Scope = query_scope<binding<"root", Entity>>;

        template <class Expr>
            requires query_predicate<Expr, Scope, LogicPolicy>
        constexpr auto where(Expr&&) && {
            return *this;
        }

        template <auto MemberPtr, class Value>
            requires meta::member_of<Entity, MemberPtr>
        constexpr auto set(Value&&) && {
            return *this;
        }
    };

    template <class Entity>
    [[nodiscard]] constexpr auto update() noexcept {
        return update_builder<Entity>{};
    }

} // namespace sanchaya
