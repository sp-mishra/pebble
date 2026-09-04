#pragma once

// ============================================================================
// sanchaya/workspace/workspace.hpp — Central Policy-Composed Workspace & Builder
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "sanchaya/schema/model.hpp"
#include "sanchaya/query/query.hpp"
#include "sanchaya/planner/planner.hpp"
#include "sanchaya/session/session.hpp"
#include "containers/cache/kosha.hpp"
#include <memory>
#include <vector>
#include <expected>

namespace sanchaya {

    // Execution Context & Observer Placeholders
    struct execution_context {};

    namespace telemetry {
        struct null_query_observer {
            template <class... Args>
            constexpr void on_event(Args&&...) const noexcept {}
        };
    } // namespace telemetry

    // Cursors & Typed Execution Results
    template <class RowType>
    class typed_cursor {
    public:
        using value_type = RowType;
        typed_cursor() = default;
        explicit typed_cursor(std::vector<RowType> rows) : rows_(std::move(rows)) {}

        [[nodiscard]] std::size_t size() const noexcept { return rows_.size(); }
        [[nodiscard]] bool empty() const noexcept { return rows_.empty(); }
        [[nodiscard]] auto begin() const noexcept { return rows_.begin(); }
        [[nodiscard]] auto end() const noexcept { return rows_.end(); }

    private:
        std::vector<RowType> rows_{};
    };

    template <class Plan, class HandlePolicy>
    struct execution_result;

    template <class Entity, class HandlePolicy>
    struct execution_result<source_plan<Entity>, HandlePolicy> {
        using type = std::vector<Entity>;
    };

    template <class Plan, class Predicate, class HandlePolicy>
    struct execution_result<filter_plan<Plan, Predicate>, HandlePolicy> {
        using type = typename execution_result<Plan, HandlePolicy>::type;
    };

    template <class Plan, class... Exprs, class HandlePolicy>
    struct execution_result<project_plan<Plan, Exprs...>, HandlePolicy> {
        using row_type = std::tuple<typename Exprs::result_type...>;
        using type = typed_cursor<row_type>;
    };

    template <class Plan, class HandlePolicy>
    struct execution_result<count_aggregate_plan<Plan>, HandlePolicy> {
        using type = std::uint64_t;
    };

    template <class Plan, class OrderExpr, class HandlePolicy>
    struct execution_result<order_plan<Plan, OrderExpr>, HandlePolicy> {
        using type = typename execution_result<Plan, HandlePolicy>::type;
    };

    template <class Plan, class HandlePolicy>
    struct execution_result<limit_plan<Plan>, HandlePolicy> {
        using type = typename execution_result<Plan, HandlePolicy>::type;
    };

    template <class Plan, class HandlePolicy>
    struct execution_result<offset_plan<Plan>, HandlePolicy> {
        using type = typename execution_result<Plan, HandlePolicy>::type;
    };

    template <class Query, class HandlePolicy = session_scoped_handle_policy>
    using query_execution_result_t =
        typename execution_result<typename std::decay_t<Query>::logical_plan_type, HandlePolicy>::type;

    // Workspace Storage & Tiering Policies
    struct transactional_outbox_sync {};
    struct autonomous_tiering_engine {};
    struct kosha_statistics_store {};

    template <
        class Model,
        class Planner         = optimizer::adaptive_tiered_planner<>,
        class Placement       = optimizer::multi_candidate_placement_engine,
        class Federation      = optimizer::adaptive_federation_engine,
        class CostModel       = optimizer::multidimensional_cost_model,
        class Statistics      = kosha_statistics_store,
        class StorageSelector = autonomous_tiering_engine,
        class SyncPolicy      = transactional_outbox_sync,
        class HandlePolicy    = session_scoped_handle_policy,
        class Telemetry       = telemetry::null_query_observer
    >
    class workspace {
    public:
        constexpr explicit workspace(
            Model model,
            Planner planner = {},
            Placement placement = {},
            Federation federation = {},
            CostModel cost_model = {},
            Statistics statistics = {},
            StorageSelector storage_selector = {},
            SyncPolicy sync = {},
            HandlePolicy handle_policy = {},
            Telemetry telemetry = {}
        ) : model_(std::move(model)),
            planner_(std::move(planner)),
            placement_(std::move(placement)),
            federation_(std::move(federation)),
            cost_model_(std::move(cost_model)),
            statistics_(std::move(statistics)),
            storage_selector_(std::move(storage_selector)),
            sync_(std::move(sync)),
            handle_policy_(std::move(handle_policy)),
            telemetry_(std::move(telemetry)) {}

        // Entity Storage Put / Get / Erase
        template <class T>
        auto put(const T& entity) -> std::expected<entity_handle<T, HandlePolicy>, sanchaya_error> {
            auto* slot = new entity_slot<T>{.value = entity, .generation = 1};
            return entity_handle<T, HandlePolicy>(slot, 1, epoch_);
        }

        template <class T, class Key>
        auto get(const Key&) -> std::expected<std::optional<entity_handle<T, HandlePolicy>>, sanchaya_error> {
            return std::optional<entity_handle<T, HandlePolicy>>{std::nullopt};
        }

        template <class T, class Key>
        auto erase(const Key&) -> std::expected<bool, sanchaya_error> {
            return true;
        }

        // Query Execution
        template <class Query>
        auto execute(Query&&, execution_context = {})
            -> std::expected<query_execution_result_t<Query, HandlePolicy>, sanchaya_error>
        {
            return query_execution_result_t<Query, HandlePolicy>{};
        }

    private:
        [[no_unique_address]] Model           model_;
        [[no_unique_address]] Planner         planner_;
        [[no_unique_address]] Placement       placement_;
        [[no_unique_address]] Federation      federation_;
        [[no_unique_address]] CostModel       cost_model_;
        [[no_unique_address]] Statistics      statistics_;
        [[no_unique_address]] StorageSelector storage_selector_;
        [[no_unique_address]] SyncPolicy      sync_;
        [[no_unique_address]] HandlePolicy    handle_policy_;
        [[no_unique_address]] Telemetry       telemetry_;
        std::uint64_t epoch_{1};
    };

    // Fluent Workspace Builder
    template <class Model>
    class workspace_builder {
    public:
        explicit constexpr workspace_builder(Model model) : model_(std::move(model)) {}

        template <class P>
        [[nodiscard]] constexpr auto planner(P&&) && { return *this; }

        template <class P>
        [[nodiscard]] constexpr auto placement(P&&) && { return *this; }

        template <class T>
        [[nodiscard]] constexpr auto telemetry(T&&) && { return *this; }

        [[nodiscard]] constexpr auto local_auto(std::string_view) && { return *this; }

        template <class RemoteCfg>
        [[nodiscard]] constexpr auto remote(RemoteCfg&&) && { return *this; }

        template <class WritePolicy>
        [[nodiscard]] constexpr auto writes(WritePolicy&&) && { return *this; }

        template <class ReadPolicy>
        [[nodiscard]] constexpr auto reads(ReadPolicy&&) && { return *this; }

        [[nodiscard]] constexpr auto build() && {
            return workspace<Model>{std::move(model_)};
        }

    private:
        Model model_;
    };

    template <class Model>
    [[nodiscard]] constexpr auto make_workspace(Model model) {
        return workspace_builder<Model>{std::move(model)};
    }

} // namespace sanchaya
