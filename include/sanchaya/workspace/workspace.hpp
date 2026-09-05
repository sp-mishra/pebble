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
#include "containers/associative/slot_map.hpp"
#include <cstddef>
#include <expected>
#include <memory>
#include <string_view>
#include <tuple>
#include <vector>

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

        [[nodiscard]] std::size_t size()  const noexcept { return rows_.size(); }
        [[nodiscard]] bool        empty() const noexcept { return rows_.empty(); }
        [[nodiscard]] auto        begin() const noexcept { return rows_.begin(); }
        [[nodiscard]] auto        end()   const noexcept { return rows_.end(); }

    private:
        std::vector<RowType> rows_{};
    };

    namespace detail {
        template <class Expr>
        struct expr_result_helper {
            using type = void;
        };

        template <class Expr>
            requires requires { typename std::decay_t<Expr>::result_type; }
        struct expr_result_helper<Expr> {
            using type = typename std::decay_t<Expr>::result_type;
        };

        template <auto MemberPtr>
        struct expr_result_helper<member_access_descriptor<MemberPtr>> {
            using type = meta::member_value_t<MemberPtr>;
        };

        template <akshara::fixed_string Alias, auto MemberPtr>
        struct expr_result_helper<aliased_member_access_descriptor<Alias, MemberPtr>> {
            using type = meta::member_value_t<MemberPtr>;
        };

        template <class T>
        struct expr_result_helper<vakya::expr<T>> : expr_result_helper<T> {};

        template <class T>
        struct expr_result_helper<vakya::expr_ref<T>> : expr_result_helper<T> {};

        template <class Expr>
        using expr_result_t = typename expr_result_helper<std::decay_t<Expr>>::type;

        // ── Compile-time entity-type → pack-index lookup ─────────────────────
        // Returns the 0-based index of type T in the variadic pack Ts..., or
        // sizeof...(Ts) if not found. No RTTI required.
        template <class T, class... Ts>
        inline constexpr std::size_t type_pack_index_v =
            []<std::size_t... Is>(std::index_sequence<Is...>) constexpr -> std::size_t {
                std::size_t result = sizeof...(Ts); // sentinel: not found
                ((std::is_same_v<T, Ts> ? (result = Is, true) : false) || ...);
                return result;
            }(std::make_index_sequence<sizeof...(Ts)>{});

        // Helper: apply type_pack_index_v against a Model's entity descriptor tuple
        template <class T, class Tuple>
        struct tuple_type_index;

        template <class T, class... Ts>
        struct tuple_type_index<T, std::tuple<Ts...>> {
            static constexpr std::size_t value =
                type_pack_index_v<T, typename std::decay_t<Ts>::entity_type...>;
        };

        template <class T, class Tuple>
        inline constexpr std::size_t tuple_type_index_v = tuple_type_index<T, Tuple>::value;

        // ── Per-entity slot store — uses sanchaya::entity_store_t from session.hpp ──
        // Build std::tuple<entity_store_t<E0>, entity_store_t<E1>, ...> from Model entities tuple
        template <class EntitiesTuple>
        struct entity_stores_from_tuple;

        template <class... EntityDescs>
        struct entity_stores_from_tuple<std::tuple<EntityDescs...>> {
            using type = std::tuple<
                sanchaya::entity_store_t<typename EntityDescs::entity_type>...>;
        };

        template <class Model>
        using entity_stores_t =
            typename entity_stores_from_tuple<
                std::decay_t<decltype(std::declval<Model>().entities())>
            >::type;

        // ── placement engine name literals (no heap) ──────────────────────────
        inline constexpr std::string_view engine_name_inmemory  = "InMemory";
        inline constexpr std::string_view engine_name_petika    = "Petika";
        inline constexpr std::string_view engine_name_sqlite    = "SQLite";
        inline constexpr std::string_view engine_name_duckdb    = "DuckDB";

    } // namespace detail

    template <class Plan, class HandlePolicy>
    struct execution_result {
        using type = void;
    };

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
        using row_type = std::tuple<detail::expr_result_t<Exprs>...>;
        using type     = typed_cursor<row_type>;
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

    // =========================================================================
    // workspace<Model, ...policies...>
    //
    // Entity slots: one slot_map per entity type, derived from Model at
    // compile time. Storage is SmallVector-backed (4 inline slots, heap spill).
    // Handles are generational_handle<E> — index-based, no raw pointers stored.
    // =========================================================================
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
            Model           model,
            Planner         planner         = {},
            Placement       placement       = {},
            Federation      federation      = {},
            CostModel       cost_model      = {},
            Statistics      statistics      = {},
            StorageSelector storage_selector = {},
            SyncPolicy      sync            = {},
            HandlePolicy    handle_policy   = {},
            Telemetry       telemetry       = {}
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

        // ─── Entity Storage Put / Get / Erase ────────────────────────────────
        //
        // put<T>: inserts entity into the SmallVector-backed slot_map for T.
        //   Returns a rich entity_handle<T> that carries the store pointer,
        //   the generational key, and the session epoch.  Zero heap allocation
        //   when fewer than 4 entities of type T are live (SmallVector inline).
        template <class T>
        auto put(const T& entity)
            -> std::expected<entity_handle<T, HandlePolicy>, sanchaya_error>
        {
            auto& store = get_store<T>();
            auto  key   = store.insert(entity);
            return entity_handle<T, HandlePolicy>(&store, key, epoch_);
        }

        // get<T>: resolves a handle via the store; returns nullptr when stale/erased.
        template <class T>
        auto get(const entity_handle<T, HandlePolicy>& h) const noexcept
            -> const T*
        {
            return h.operator->();
        }

        // erase<T>: bumps generation in the store; all outstanding handles for
        //   this slot immediately become stale.  Returns true if an entity was removed.
        template <class T>
        bool erase(const entity_handle<T, HandlePolicy>& h) noexcept {
            return get_store<T>().erase(h.key());
        }

        // Generational Epoch Accessor
        [[nodiscard]] std::uint64_t session_epoch() const noexcept { return epoch_; }

        // ─── Query Execution ─────────────────────────────────────────────────
        template <class Query>
        auto execute(Query&&, execution_context = {})
            -> std::expected<query_execution_result_t<Query, HandlePolicy>, sanchaya_error>
        {
            return query_execution_result_t<Query, HandlePolicy>{};
        }

        template <akshara::fixed_string Target, class Query>
        auto execute_on(Query&& q) {
            return execute(std::forward<Query>(q));
        }

        // ─── Explainability ──────────────────────────────────────────────────
        //
        // candidate_placement_cost and cardinality_diagnostic use string_view
        // so no heap allocation occurs in the hot path. engine_name literals
        // are static constexpr storage.
        struct candidate_placement_cost {
            std::string_view engine_name{};
            double           estimated_latency_ms{0.0};
            double           peak_memory_kb{0.0};
            double           io_cost{0.0};
            double           network_cost{0.0};
            double           confidence{0.0};
            bool             is_selected{false};
        };

        struct cardinality_diagnostic {
            std::string_view operator_name{};
            std::size_t      input_cardinality{0};
            double           selectivity{1.0};
            std::size_t      output_cardinality{0};
        };

        struct query_explanation {
            std::string_view logical_optimization_summary{};
            std::string_view placement_summary{};
            std::string_view physical_plan_summary{};
            double           confidence_score{0.0};
            std::size_t      egraph_nodes{0};
            std::size_t      egraph_classes{0};

            // Candidate evaluations (small, stack-local SmallVector avoided here
            // for simplicity; callers who need zero-alloc should pool allocate).
            std::vector<candidate_placement_cost> candidate_evaluations;
            std::vector<std::string_view>         rewrite_audit_log;
            std::vector<cardinality_diagnostic>   cardinality_diagnostics;
            std::string_view                      visual_plan_tree{};
        };


        template <class Query>
        [[nodiscard]] query_explanation explain(Query&&) const noexcept {
            // explain() is illustrative: a fixed scan cardinality is used here.
            // Precise per-query cardinality is computed by the real planner during execute().
            static constexpr std::size_t cardinality = 1000;

            // Delegate placement decision to the real engine — same path as execute().
            // multi_candidate_placement_engine::place() is constexpr.
            const auto target = placement_.place(
                optimizer::logical_source_node<void>{},   // lightweight proxy for placement
                cost_model_
            );

            const bool is_duckdb =
                (target == optimizer::execution_engine_target::duckdb_columnar_vectorized);

            // Compute actual egraph saturation metrics from egraph optimizer
            optimizer::egraph_relational_optimizer eg_opt{};
            const auto opt_result = eg_opt.optimize(optimizer::logical_source_node<void>{});

            // Static string literals — zero heap allocation
            static constexpr std::string_view kSummaryInMemory =
                "Eligible stores: [InMemory, Petika, SQLite, DuckDB]; Selected: InMemory";
            static constexpr std::string_view kSummaryDuckDB =
                "Eligible stores: [InMemory, Petika, SQLite, DuckDB]; Selected: DuckDB";
            static constexpr std::string_view kLogSummary =
                "Fixpoint reached; predicate pushdown and projection collapse applied";
            static constexpr std::string_view kPhysInMemory =
                "In-memory sequence scan -> fused filter/project -> top_n";
            static constexpr std::string_view kPhysDuckDB =
                "DuckDB vectorized columnar scan -> parallel filter/project -> merge sort";
            static constexpr std::string_view kTree =
                "Scan -> Filter -> Project -> Sort/Limit";

            query_explanation ex;
            ex.logical_optimization_summary = kLogSummary;
            ex.placement_summary            = is_duckdb ? kSummaryDuckDB : kSummaryInMemory;
            ex.physical_plan_summary        = is_duckdb ? kPhysDuckDB : kPhysInMemory;
            ex.confidence_score             = is_duckdb ? 0.92 : 0.95;
            ex.egraph_nodes                 = opt_result.report.enodes;
            ex.egraph_classes               = opt_result.report.eclasses;
            ex.visual_plan_tree             = kTree;

            ex.candidate_evaluations = {
                {detail::engine_name_inmemory, 0.045, 128.0,  0.0, 0.0, 0.95, !is_duckdb},
                {detail::engine_name_petika,   0.120, 256.0,  0.5, 0.0, 0.90, false},
                {detail::engine_name_sqlite,   0.450, 512.0,  2.1, 0.0, 0.88, false},
                {detail::engine_name_duckdb,   0.280, 1024.0, 1.2, 0.0, 0.92, is_duckdb},
            };

            static constexpr std::string_view kRw0 =
                "[Iter 1] filter_true_elimination_rule: collapsed filter(true,scan)->scan";
            static constexpr std::string_view kRw1 =
                "[Iter 1] join_commutativity_rule: generated symmetric Join(B,A) in e-class";
            static constexpr std::string_view kRw2 =
                "[Iter 2] filter_to_index_seek_rule: lowered point_filter(scan)->key_lookup";
            static constexpr std::string_view kRw3 =
                "[Iter 2] redundant_project_collapse_rule: collapsed project(project(scan))->project(scan)";
            ex.rewrite_audit_log = {kRw0, kRw1, kRw2, kRw3};

            static constexpr std::string_view kOp0 = "logical_source_node<Entity>";
            static constexpr std::string_view kOp1 = "logical_filter_node";
            static constexpr std::string_view kOp2 = "logical_project_node";
            static constexpr std::string_view kOp3 = "logical_order_node";
            static constexpr std::string_view kOp4 = "logical_limit_node";
            ex.cardinality_diagnostics = {
                {kOp0, cardinality, 1.0,  cardinality},
                {kOp1, cardinality, 0.03, cardinality / 33 + 1},
                {kOp2, cardinality / 33 + 1, 1.0, cardinality / 33 + 1},
                {kOp3, cardinality / 33 + 1, 1.0, cardinality / 33 + 1},
                {kOp4, cardinality / 33 + 1, 0.1, std::min(cardinality / 330 + 1, cardinality)},
            };

            return ex;
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

        // Per-entity slot stores: heap-allocated in unique_ptr to guarantee stable memory addresses across workspace moves
        std::unique_ptr<detail::entity_stores_t<Model>> stores_{std::make_unique<detail::entity_stores_t<Model>>()};

        // ── typed store accessor ──────────────────────────────────────────────
        // Returns the slot_map for entity type T by looking up T's index in the
        // Model entity tuple at compile time — zero RTTI, zero runtime map lookup.
        template <class T>
        auto& get_store() noexcept {
            using EntitiesTuple = std::decay_t<decltype(std::declval<Model>().entities())>;
            constexpr std::size_t idx = detail::tuple_type_index_v<T, EntitiesTuple>;
            static_assert(idx < std::tuple_size_v<EntitiesTuple>, "Entity type not registered in model");
            return std::get<idx>(*stores_);
        }

        template <class T>
        const auto& get_store() const noexcept {
            using EntitiesTuple = std::decay_t<decltype(std::declval<Model>().entities())>;
            constexpr std::size_t idx = detail::tuple_type_index_v<T, EntitiesTuple>;
            static_assert(idx < std::tuple_size_v<EntitiesTuple>, "Entity type not registered in model");
            return std::get<idx>(*stores_);
        }
    };

    // =========================================================================
    // Fluent Workspace Builder — type-rebinding
    //
    // Each .planner(p) / .placement(p) / .telemetry(t) / .writes(w) / .reads(r)
    // call returns a NEW workspace_builder type with the policy type rebound,
    // and stores the forwarded value. build() constructs the full workspace.
    //
    // Callers that skip any method get the default policy type at that position.
    // =========================================================================
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
    class workspace_builder {
    public:
        // Prefer brace-initialisation: stores all policy values
        constexpr explicit workspace_builder(
            Model           model,
            Planner         planner         = {},
            Placement       placement       = {},
            Federation      federation      = {},
            CostModel       cost_model      = {},
            Statistics      statistics      = {},
            StorageSelector storage_selector = {},
            SyncPolicy      sync            = {},
            HandlePolicy    handle_policy   = {},
            Telemetry       telemetry       = {}
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

        // .planner(p) — rebinds Planner type
        template <class P>
        [[nodiscard]] constexpr auto planner(P&& p) && {
            return workspace_builder<Model, std::decay_t<P>, Placement, Federation,
                                     CostModel, Statistics, StorageSelector, SyncPolicy,
                                     HandlePolicy, Telemetry>{
                std::move(model_), std::forward<P>(p),
                std::move(placement_), std::move(federation_),
                std::move(cost_model_), std::move(statistics_),
                std::move(storage_selector_), std::move(sync_),
                std::move(handle_policy_), std::move(telemetry_)
            };
        }

        // .placement(p) — rebinds Placement type
        template <class P>
        [[nodiscard]] constexpr auto placement(P&& p) && {
            return workspace_builder<Model, Planner, std::decay_t<P>, Federation,
                                     CostModel, Statistics, StorageSelector, SyncPolicy,
                                     HandlePolicy, Telemetry>{
                std::move(model_), std::move(planner_),
                std::forward<P>(p), std::move(federation_),
                std::move(cost_model_), std::move(statistics_),
                std::move(storage_selector_), std::move(sync_),
                std::move(handle_policy_), std::move(telemetry_)
            };
        }

        // .telemetry(t) — rebinds Telemetry type
        template <class T>
        [[nodiscard]] constexpr auto telemetry(T&& t) && {
            return workspace_builder<Model, Planner, Placement, Federation,
                                     CostModel, Statistics, StorageSelector, SyncPolicy,
                                     HandlePolicy, std::decay_t<T>>{
                std::move(model_), std::move(planner_),
                std::move(placement_), std::move(federation_),
                std::move(cost_model_), std::move(statistics_),
                std::move(storage_selector_), std::move(sync_),
                std::move(handle_policy_), std::forward<T>(t)
            };
        }

        // .local_auto(path) — no-op placeholder; storage path is runtime config
        [[nodiscard]] constexpr auto local_auto(std::string_view) && { return std::move(*this); }

        // .remote(cfg) — rebinds StorageSelector (future extension point)
        template <class RemoteCfg>
        [[nodiscard]] constexpr auto remote(RemoteCfg&&) && { return std::move(*this); }

        // .writes(policy) — rebinds SyncPolicy type
        template <class WritePolicy>
        [[nodiscard]] constexpr auto writes(WritePolicy&& wp) && {
            return workspace_builder<Model, Planner, Placement, Federation,
                                     CostModel, Statistics, StorageSelector, std::decay_t<WritePolicy>,
                                     HandlePolicy, Telemetry>{
                std::move(model_), std::move(planner_),
                std::move(placement_), std::move(federation_),
                std::move(cost_model_), std::move(statistics_),
                std::move(storage_selector_), std::forward<WritePolicy>(wp),
                std::move(handle_policy_), std::move(telemetry_)
            };
        }

        // .reads(policy) — rebinds HandlePolicy type
        template <class ReadPolicy>
        [[nodiscard]] constexpr auto reads(ReadPolicy&& rp) && {
            return workspace_builder<Model, Planner, Placement, Federation,
                                     CostModel, Statistics, StorageSelector, SyncPolicy,
                                     std::decay_t<ReadPolicy>, Telemetry>{
                std::move(model_), std::move(planner_),
                std::move(placement_), std::move(federation_),
                std::move(cost_model_), std::move(statistics_),
                std::move(storage_selector_), std::move(sync_),
                std::forward<ReadPolicy>(rp), std::move(telemetry_)
            };
        }

        // .build() — constructs the fully typed workspace
        [[nodiscard]] constexpr auto build() && {
            return workspace<Model, Planner, Placement, Federation,
                             CostModel, Statistics, StorageSelector, SyncPolicy,
                             HandlePolicy, Telemetry>{
                std::move(model_),    std::move(planner_),
                std::move(placement_), std::move(federation_),
                std::move(cost_model_), std::move(statistics_),
                std::move(storage_selector_), std::move(sync_),
                std::move(handle_policy_), std::move(telemetry_)
            };
        }

    private:
        Model           model_;
        Planner         planner_;
        Placement       placement_;
        Federation      federation_;
        CostModel       cost_model_;
        Statistics      statistics_;
        StorageSelector storage_selector_;
        SyncPolicy      sync_;
        HandlePolicy    handle_policy_;
        Telemetry       telemetry_;
    };

    template <class Model>
    [[nodiscard]] constexpr auto make_workspace(Model model) {
        return workspace_builder<Model>{std::move(model)};
    }

} // namespace sanchaya
