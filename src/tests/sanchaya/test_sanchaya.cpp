// ============================================================================
// test_sanchaya.cpp — Comprehensive Unit Tests for Sanchaya Subsystem
// ============================================================================

#include "catch_amalgamated.hpp"
#include "sanchaya/sanchaya.hpp"
#include <string>

namespace {

struct DepartmentId {
    std::uint64_t value{0};
    constexpr auto operator<=>(const DepartmentId&) const noexcept = default;
};

struct EmployeeId {
    std::uint64_t value{0};
    constexpr auto operator<=>(const EmployeeId&) const noexcept = default;
};

struct Address {
    std::string street;
    std::string city;
};

struct Employee {
    EmployeeId id;
    std::string name;
    int age{0};
    DepartmentId department_id;
    Address address;
};

struct Department {
    DepartmentId id;
    std::string name;
};

struct employee_id_allocator {
    EmployeeId allocate() { return EmployeeId{++counter_}; }
private:
    std::uint64_t counter_{0};
};

struct department_id_allocator {
    DepartmentId allocate() { return DepartmentId{++counter_}; }
private:
    std::uint64_t counter_{0};
};

static_assert(sanchaya::concepts::identity_allocator_for<employee_id_allocator, EmployeeId>);
static_assert(sanchaya::concepts::identity_allocator_for<department_id_allocator, DepartmentId>);

} // namespace

TEST_CASE("sanchaya: compile-time schema model creation", "[sanchaya][schema]") {
    constexpr auto company_model =
        sanchaya::model<"company">()
            .entity(
                sanchaya::describe_row<Employee>(
                    sanchaya::field<"id", &Employee::id>(
                        sanchaya::stable_id<"emp.id">(),
                        sanchaya::allocator<employee_id_allocator>()
                    ),
                    sanchaya::field<"name", &Employee::name>(sanchaya::stable_id<"emp.name">()),
                    sanchaya::field<"age", &Employee::age>(sanchaya::stable_id<"emp.age">()),
                    sanchaya::field<"department_id", &Employee::department_id>(sanchaya::stable_id<"emp.dept_id">()),
                    sanchaya::embedded<"address", &Employee::address>(sanchaya::stable_id<"emp.address">())
                )
            )
            .entity(
                sanchaya::describe_row<Department>(
                    sanchaya::field<"id", &Department::id>(
                        sanchaya::stable_id<"dept.id">(),
                        sanchaya::allocator<department_id_allocator>()
                    ),
                    sanchaya::field<"name", &Department::name>(sanchaya::stable_id<"dept.name">())
                )
            )
            .relation<"employment">(
                sanchaya::many<Employee>(),
                sanchaya::one<Department>(),
                sanchaya::foreign_key<&Employee::department_id>(),
                sanchaya::principal_key<&Department::id>(),
                sanchaya::reference(),
                sanchaya::on_delete<sanchaya::restrict_delete>()
            )
            .build();

    STATIC_REQUIRE(company_model.name == akshara::fixed_string{"company"});
    STATIC_REQUIRE(std::tuple_size_v<std::decay_t<decltype(company_model.entities())>> == 2);
    STATIC_REQUIRE(std::tuple_size_v<std::decay_t<decltype(company_model.relations())>> == 1);
}

TEST_CASE("sanchaya: query builder EDSL construction & typing", "[sanchaya][query]") {
    using namespace sanchaya;

    auto q = from<Employee>()
        .where(member<&Employee::age>() >= 30)
        .group_by(member<&Employee::department_id>())
        .select(
            group_key(),
            count(),
            average(member<&Employee::age>())
        )
        .order_by(member<&Employee::age>(), sort_direction::descending)
        .limit(10)
        .offset(5);

    STATIC_REQUIRE(std::same_as<decltype(q)::root_entity_type, Employee>);
    STATIC_REQUIRE(decltype(q)::active_logic_policy == logic_policy::two_valued);
}

TEST_CASE("sanchaya: three-valued logic & schema identity", "[sanchaya][logic]") {
    using namespace sanchaya;

    STATIC_REQUIRE(std::same_as<logic_traits<logic_policy::two_valued>::result_type, bool>);
    STATIC_REQUIRE(std::same_as<logic_traits<logic_policy::sql_three_valued>::result_type, sql_bool>);
    STATIC_REQUIRE(std::same_as<logic_traits<logic_policy::optional_propagating>::result_type, std::optional<bool>>);

    constexpr sql_bool b_unknown{};
    constexpr sql_bool b_true{sql_bool::state::is_true};
    constexpr sql_bool b_false{sql_bool::state::is_false};

    STATIC_REQUIRE(b_unknown.is_unknown());
    STATIC_REQUIRE(b_true.is_true());
    STATIC_REQUIRE(b_false.is_false());

    STATIC_REQUIRE(!where_accepts(b_unknown));
    STATIC_REQUIRE(where_accepts(b_true));
    STATIC_REQUIRE(!where_accepts(b_false));

    using Schema = named_schema_identity<"hr", "employee">;
    STATIC_REQUIRE(Schema::value.algorithm_version == 1);
    STATIC_REQUIRE(Schema::canonical_name == akshara::fixed_string{"hr.employee"});
}

TEST_CASE("sanchaya: workspace lifecycle & session handles", "[sanchaya][workspace]") {
    using namespace sanchaya;

    constexpr auto company_model =
        model<"company">()
            .entity(
                describe_row<Employee>(
                    field<"id", &Employee::id>(),
                    field<"name", &Employee::name>(),
                    field<"age", &Employee::age>()
                )
            )
            .build();

    auto ws = make_workspace(company_model)
        .local_auto("./test_storage")
        .build();

    Employee emp{.id = EmployeeId{101}, .name = "Ada", .age = 36};
    auto put_res = ws.put(emp);
    REQUIRE(put_res.has_value());

    auto handle = *put_res;
    REQUIRE(handle.is_valid(1));

    auto get_res = handle.get(1);
    REQUIRE(get_res.has_value());
    CHECK(get_res->get().name == "Ada");
    CHECK(get_res->get().age == 36);

    // Evicted epoch check
    CHECK(!handle.is_valid(2));
    auto stale_res = handle.get(2);
    CHECK(!stale_res.has_value());
    CHECK(stale_res.error().domain == error_domain::binding);
}

TEST_CASE("sanchaya: relational graph cycle analysis with LiteGraph", "[sanchaya][graph][validation]") {
    using namespace sanchaya;

    // A valid model with acyclic embedded and reference relations
    constexpr auto valid_model =
        model<"valid_domain">()
            .entity(describe_row<Department>(field<"id", &Department::id>()))
            .entity(describe_row<Employee>(field<"id", &Employee::id>(), field<"department_id", &Employee::department_id>()))
            .relation<"dept_employees">(
                many<Employee>(), one<Department>(),
                foreign_key<&Employee::department_id>(), principal_key<&Department::id>(),
                reference()
            )
            .build();

    auto analysis = validation::validate_model(valid_model);
    CHECK(analysis.status == validation::validation_status::valid);
    CHECK(!analysis.has_reference_cycles);
    CHECK(analysis.embedded_cycle_count == 0);
}

TEST_CASE("sanchaya: autonomous storage promotion formula", "[sanchaya][sync][promotion]") {
    using namespace sanchaya::sync;

    promotion_metrics high_benefit{
        .predicted_queries = 1000.0,
        .avg_latency_delta_ms = 5.0,
        .remote_savings = 50.0,
        .build_cost = 10.0,
        .maint_cost = 5.0,
        .storage_cost = 5.0,
        .schema_maint_cost = 2.0,
        .freshness_penalty = 1.0,
        .confidence = 0.95,
        .payback_period = 120.0
    };

    double benefit = autonomous_tiering_evaluator::compute_expected_benefit(high_benefit);
    CHECK(benefit > 4000.0);
    CHECK(autonomous_tiering_evaluator::should_promote(high_benefit, 50.0, 0.7, 3600.0));

    promotion_metrics low_benefit = high_benefit;
    low_benefit.predicted_queries = 2.0;
    low_benefit.build_cost = 500.0;
    CHECK(!autonomous_tiering_evaluator::should_promote(low_benefit, 50.0, 0.7, 3600.0));
}

TEST_CASE("sanchaya: compile-time tagged service registry", "[sanchaya][integration]") {
    using namespace sanchaya::integration;

    struct custom_optimizer_service {
        int optimize_flag{42};
    };

    struct custom_telemetry_service {
        std::string sink_name{"nadi_pulse"};
    };

    auto reg = make_service_registry(
        service_instance<"optimizer", custom_optimizer_service>{custom_optimizer_service{100}},
        service_instance<"telemetry", custom_telemetry_service>{custom_telemetry_service{"custom_sink"}}
    );

    CHECK(reg.get<"optimizer">().optimize_flag == 100);
    CHECK(reg.get<"telemetry">().sink_name == "custom_sink");
}

TEST_CASE("sanchaya: logical IR and physical IR lowering with RBO passes", "[sanchaya][planner][ir]") {
    using namespace sanchaya;
    using namespace sanchaya::optimizer;

    // 1. Logical Plan Construction
    logical_source_node<Employee> scan{};
    STATIC_REQUIRE(logical_plan<decltype(scan)>);

    auto pred = member<&Employee::age>() >= 30;
    logical_filter_node filter{scan, pred};
    STATIC_REQUIRE(logical_plan<decltype(filter)>);

    auto proj_exprs = std::make_tuple(member<&Employee::id>(), member<&Employee::name>());
    logical_project_node proj{filter, proj_exprs};
    STATIC_REQUIRE(logical_plan<decltype(proj)>);

    // 2. Logical RBO Rewriting (Predicate Pushdown)
    // Filter on top of Project: \sigma(\pi(Scan))
    logical_project_node inner_proj{scan, proj_exprs};
    logical_filter_node outer_filter{inner_proj, pred};

    predicate_pushdown_rule pushdown_rule{};
    auto pushed = pushdown_rule.apply(outer_filter);
    // Should transform to \pi(\sigma(Scan))
    STATIC_REQUIRE(std::same_as<decltype(pushed.input), logical_filter_node<logical_source_node<Employee>, decltype(pred)>>);

    // 3. Physical IR Lowering
    default_physical_planner physical_planner{};
    auto phys_plan = physical_planner.lower(pushed, execution_engine_target::in_memory_session);
    STATIC_REQUIRE(physical_plan<decltype(phys_plan)>);

    // 4. Adaptive Tiered Planner Integration
    adaptive_tiered_planner<> planner{};
    auto optimized_physical = planner.optimize(outer_filter);
    STATIC_REQUIRE(physical_plan<decltype(optimized_physical)>);
}


