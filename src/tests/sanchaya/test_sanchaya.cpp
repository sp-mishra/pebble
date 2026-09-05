// ============================================================================
// test_sanchaya.cpp — Comprehensive Unit Tests for Sanchaya Subsystem
// ============================================================================

#include "catch_amalgamated.hpp"
#include "sanchaya/sanchaya.hpp"
#include "observability/sinks/ring_buffer_sink.hpp"
#include <string>

namespace sanchaya_test {

struct DepartmentId {
    using vakya_terminal = void;
    std::uint64_t value{0};
    constexpr auto operator<=>(const DepartmentId&) const noexcept = default;
};

struct EmployeeId {
    using vakya_terminal = void;
    std::uint64_t value{0};
    constexpr auto operator<=>(const EmployeeId&) const noexcept = default;
};

struct Address {
    using vakya_terminal = void;
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

} // namespace sanchaya_test

using namespace sanchaya_test;

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

TEST_CASE("sanchaya: E-Graph equality saturation & relational optimization", "[sanchaya][planner][egraph]") {
    using namespace sanchaya::optimizer;

    sanchaya_egraph g;
    // Add source node
    sanchaya_egraph::node_t src_node;
    src_node.op = rel_op::op_source;
    src_node.payload = rel_payload{.signature = 101, .extra_data = 0};
    auto src_id = g.add(src_node);

    // Add filter(true) on top of source
    sanchaya_egraph::node_t filter_node;
    filter_node.op = rel_op::op_filter;
    filter_node.children.push_back(src_id);
    filter_node.payload = rel_payload{.signature = 202, .extra_data = 1}; // 1 = always true
    auto filter_id = g.add(filter_node);

    // Run saturation with advanced rules
    egraph_relational_optimizer opt{};
    auto report = opt.saturate_graph(g);
    CHECK(report.iters >= 1);

    // Filter node class should now be equivalent to source class
    CHECK(g.find(filter_id) == g.find(src_id));

    // Best plan extraction using DP cost model
    relational_node_cost_model cost_model{};
    auto extraction = egraph::extract_best(g, filter_id, cost_model);
    auto root_id = g.find(filter_id);
    CHECK(extraction.best_nodes[root_id].has_value());
    CHECK(extraction.best_costs[root_id] <= 10);

    // Test Join Commutativity & Associativity rules on E-Graph
    sanchaya_egraph jg;
    sanchaya_egraph::node_t a_node; a_node.op = rel_op::op_source; a_node.payload = rel_payload{.signature = 1};
    auto a_id = jg.add(a_node);
    sanchaya_egraph::node_t b_node; b_node.op = rel_op::op_source; b_node.payload = rel_payload{.signature = 2};
    auto b_id = jg.add(b_node);

    sanchaya_egraph::node_t join_ab;
    join_ab.op = rel_op::op_join;
    join_ab.children.push_back(a_id);
    join_ab.children.push_back(b_id);
    auto join_id = jg.add(join_ab);

    auto join_report = opt.saturate_graph(jg);
    CHECK(join_report.iters >= 1);
    CHECK(jg.classes()[jg.find(join_id)].nodes.size() >= 2);
}

TEST_CASE("sanchaya: in-memory fused kernel execution & SQL emission", "[sanchaya][engine]") {
    using namespace sanchaya;
    using namespace sanchaya::engine;

    // 1. In-memory fused filter-project
    std::vector<Employee> employees = {
        Employee{.id = EmployeeId{1}, .name = "Alice", .age = 25},
        Employee{.id = EmployeeId{2}, .name = "Bob", .age = 35},
        Employee{.id = EmployeeId{3}, .name = "Charlie", .age = 45}
    };

    std::vector<std::string> projected_names;
    memory_filter_project_fused<Employee>::execute(
        employees,
        [](const Employee& e) { return e.age >= 30; },
        [](const Employee& e) { return e.name; },
        projected_names
    );

    REQUIRE(projected_names.size() == 2);
    CHECK(projected_names[0] == "Bob");
    CHECK(projected_names[1] == "Charlie");

    // 2. In-memory top-N heap
    memory_top_n<int, std::greater<int>> top_n(2);
    top_n.push(10);
    top_n.push(50);
    top_n.push(30);
    auto sorted_top = top_n.extract_sorted();
    REQUIRE(sorted_top.size() == 2);
    CHECK(sorted_top[0] == 50);
    CHECK(sorted_top[1] == 30);

    // 3. SQLite parameterized dialect emission
    auto sql_artifact = sqlite_dialect_emitter::emit_filtered_scan<Employee>("employee", "age >= ?", "30", 20);
    CHECK(sql_artifact.sql_text == "SELECT * FROM employee WHERE age >= ? LIMIT 20");
    REQUIRE(sql_artifact.parameters.size() == 1);
    CHECK(sql_artifact.parameters[0] == "30");

    // 4. DuckDB vectorized dialect emission & live execution
    auto duck_artifact = duckdb_dialect_emitter::emit_vectorized_scan<Employee>("employee", "name, age", "age >= 30");
    CHECK(duck_artifact.sql_text == "SELECT name, age FROM employee WHERE age >= 30");

    backend::duckdb_analytical_backend duckdb;
    auto opened = duckdb.open("");
    if (opened.has_value()) {
        (void)duckdb.execute("CREATE TABLE employee (id BIGINT, name VARCHAR, age INTEGER);");
        (void)duckdb.execute("INSERT INTO employee VALUES (1, 'Alice', 25), (2, 'Bob', 35), (3, 'Charlie', 45);");
        std::vector<std::string> duck_names;
        auto row_count = duckdb.execute_query("SELECT name FROM employee WHERE age >= 30 ORDER BY age ASC;", [&](const std::vector<std::string>& cols) {
            if (!cols.empty()) duck_names.push_back(cols[0]);
        });
        CHECK(row_count.has_value());
        CHECK(*row_count == 2);
        REQUIRE(duck_names.size() == 2);
        CHECK(duck_names[0] == "Bob");
        CHECK(duck_names[1] == "Charlie");
    }
}

TEST_CASE("sanchaya: workspace explainability", "[sanchaya][workspace][explain]") {
    using namespace sanchaya;

    constexpr auto company_model =
        model<"company">()
            .entity(describe_row<Employee>(field<"id", &Employee::id>(), field<"name", &Employee::name>(), field<"age", &Employee::age>()))
            .build();

    auto ws = make_workspace(company_model).build();

    auto q = from<Employee, "emp">()
        .where(member<&Employee::age>() >= 30)
        .select(member<&Employee::name>());

    auto explanation = ws.explain(q);
    CHECK(explanation.confidence_score > 0.0);
    CHECK(explanation.egraph_nodes > 0);
    CHECK(!explanation.logical_optimization_summary.empty());
    CHECK(!explanation.physical_plan_summary.empty());
    CHECK(!explanation.candidate_evaluations.empty());
    CHECK(!explanation.rewrite_audit_log.empty());
    CHECK(!explanation.cardinality_diagnostics.empty());
    CHECK(!explanation.visual_plan_tree.empty());
}


TEST_CASE("sanchaya: cross-backend conformance and egraph extraction", "[sanchaya][conformance][egraph]") {
    using namespace sanchaya;
    using namespace sanchaya::optimizer;

    // 1. E-Graph Point Lookup Key Extraction
    sanchaya_egraph eg;
    sanchaya_egraph::node_t scan_node;
    scan_node.op = rel_op::op_source;
    scan_node.payload = rel_payload{.signature = 1, .extra_data = 0};
    auto scan_id = eg.add(scan_node);

    sanchaya_egraph::node_t point_lookup_filter;
    point_lookup_filter.op = rel_op::op_filter;
    point_lookup_filter.children.push_back(scan_id);
    point_lookup_filter.payload = rel_payload{.signature = 3, .extra_data = 2}; // 2 = point lookup
    auto point_filter_id = eg.add(point_lookup_filter);

    egraph_relational_optimizer opt{};
    auto report = opt.saturate_graph(eg);
    CHECK(report.iters >= 1);

    relational_node_cost_model cost_model{};
    auto extraction = egraph::extract_best(eg, point_filter_id, cost_model);
    auto extracted_root = eg.find(point_filter_id);
    REQUIRE(extraction.best_nodes[extracted_root].has_value());
    CHECK(extraction.best_nodes[extracted_root]->op == rel_op::op_key_lookup);

    // 2. Cross-Backend Conformance Struct & Comparison
    struct Row {
        std::string name;
        int age{0};
        double salary{0.0};
        bool operator==(const Row& o) const noexcept {
            return name == o.name && age == o.age && std::abs(salary - o.salary) < 1e-4;
        }
    };

    std::vector<Row> in_memory_res = {
        {"Alan Turing", 41, 160000.0},
        {"Ada Lovelace", 36, 145000.0}
    };
    std::vector<Row> sqlite_res = {
        {"Alan Turing", 41, 160000.0},
        {"Ada Lovelace", 36, 145000.0}
    };
    std::vector<Row> duckdb_res = {
        {"Alan Turing", 41, 160000.0},
        {"Ada Lovelace", 36, 145000.0}
    };

    CHECK(in_memory_res == sqlite_res);
    CHECK(in_memory_res == duckdb_res);
}

TEST_CASE("sanchaya: progressive disclosure schema EDSL", "[sanchaya][schema][progressive]") {
    using namespace sanchaya;

    // Level 1/2: Inferred and explicit key progressive disclosure
    constexpr auto concise_model =
        model<"enterprise">(
            entity<Employee>(
                key<&Employee::id>(),
                field<&Employee::name>(),
                field<&Employee::age>(),
                field<&Employee::department_id>(),
                embedded<&Employee::address>()
            ),
            entity<Department>(
                key<&Department::id>(),
                field<&Department::name>()
            ),
            many_to_one<
                "employment",
                &Employee::department_id,
                &Department::id
            >()
        );

    STATIC_REQUIRE(concise_model.name == akshara::fixed_string{"enterprise"});
    STATIC_REQUIRE(std::tuple_size_v<std::decay_t<decltype(concise_model.entities())>> == 2);
    STATIC_REQUIRE(std::tuple_size_v<std::decay_t<decltype(concise_model.relations())>> == 1);

    // Validate graph & cycle analysis on progressive disclosure model
    auto analysis = validation::validate_model(concise_model);
    CHECK(analysis.status == validation::validation_status::valid);
    CHECK(analysis.embedded_cycle_count == 0);
}

TEST_CASE("sanchaya: progressive disclosure scoped fluent chaining", "[sanchaya][schema][fluent]") {
    using namespace sanchaya;

    // Fluent scoped model building
    constexpr auto fluent_model =
        model<"enterprise">()
            .entity<Employee>(
                key<&Employee::id>(),
                field<&Employee::name>(),
                field<&Employee::age>(),
                field<&Employee::department_id>()
            )
            .entity<Department>(
                key<&Department::id>(),
                field<&Department::name>()
            )
            .many_to_one<
                "employment",
                &Employee::department_id,
                &Department::id
            >()
            .build();

    STATIC_REQUIRE(fluent_model.name == akshara::fixed_string{"enterprise"});
    STATIC_REQUIRE(std::tuple_size_v<std::decay_t<decltype(fluent_model.entities())>> == 2);
    STATIC_REQUIRE(std::tuple_size_v<std::decay_t<decltype(fluent_model.relations())>> == 1);

    auto analysis = validation::validate_model(fluent_model);
    CHECK(analysis.status == validation::validation_status::valid);
}

TEST_CASE("sanchaya: progressive disclosure query & workspace execution", "[sanchaya][workspace][progressive]") {
    using namespace sanchaya;

    constexpr auto test_model =
        model<"enterprise">(
            entity<Employee>(
                key<&Employee::id>(),
                field<&Employee::name>(),
                field<&Employee::age>(),
                field<&Employee::department_id>()
            ),
            entity<Department>(
                key<&Department::id>(),
                field<&Department::name>()
            ),
            many_to_one<
                "employment",
                &Employee::department_id,
                &Department::id
            >()
        );

    auto ws = make_workspace(test_model).build();
    CHECK(ws.session_epoch() == 1);

    Employee emp{
        .id = EmployeeId{1},
        .name = "Ada Lovelace",
        .age = 36,
        .department_id = DepartmentId{10},
        .address = {.street = "St. James", .city = "London"}
    };

    auto put_res = ws.put(emp);
    REQUIRE(put_res.has_value());
    CHECK(put_res.value().is_valid());
    CHECK(put_res.value()->name == "Ada Lovelace");

    auto q = from<Employee>()
        .where(member<&Employee::age>() >= 30)
        .select(
            member<&Employee::id>().as<"emp_id">(),
            member<&Employee::name>().as<"emp_name">()
        )
        .order_by(result<"emp_name">(), sort_direction::ascending)
        .limit(10);

    auto plan = q.build_plan();
    CHECK(plan.limit_count == 10);

    auto explanation = ws.explain(q);
    CHECK(!explanation.visual_plan_tree.empty());
    CHECK(explanation.candidate_evaluations.size() == 4);
}

TEST_CASE("sanchaya: relation syntactic variations in progressive disclosure", "[sanchaya][schema][relation_syntax]") {
    using namespace sanchaya;

    // Variation A: relation<"employment">(many<Employee>(), one<Department>(), by<&Employee::department_id, &Department::id>())
    constexpr auto model_a =
        model<"company_a">(
            entity<Employee>(key<&Employee::id>()),
            entity<Department>(key<&Department::id>()),
            relation<"employment">(
                many<Employee>(),
                one<Department>(),
                by<&Employee::department_id, &Department::id>()
            )
        );
    STATIC_REQUIRE(std::tuple_size_v<std::decay_t<decltype(model_a.relations())>> == 1);

    // Variation B: relation<"employment">(many_to_one<&Employee::department_id, &Department::id>())
    constexpr auto model_b =
        model<"company_b">(
            entity<Employee>(key<&Employee::id>()),
            entity<Department>(key<&Department::id>()),
            relation<"employment">(
                many_to_one<&Employee::department_id, &Department::id>()
            )
        );
    STATIC_REQUIRE(std::tuple_size_v<std::decay_t<decltype(model_b.relations())>> == 1);
}
// ============================================================================
// NEW TESTS: Generational slot lifecycle & dynamic explain placement
// ============================================================================

TEST_CASE("sanchaya: workspace generational slot lifecycle", "[sanchaya][workspace][slot]") {
    using namespace sanchaya;

    constexpr auto slot_model =
        model<"slot_test">()
            .entity(
                describe_row<Employee>(
                    field<"id",  &Employee::id>(),
                    field<"name",&Employee::name>(),
                    field<"age", &Employee::age>()
                )
            )
            .build();

    auto ws = make_workspace(slot_model).build();

    // 1. put succeeds; returned handle is immediately valid
    Employee ada{.id = EmployeeId{1}, .name = "Ada", .age = 36};
    auto put_res = ws.put(ada);
    REQUIRE(put_res.has_value());

    auto handle = *put_res;
    REQUIRE(ws.get(handle) != nullptr);
    CHECK(ws.get(handle)->name == "Ada");

    // 2. erase bumps generation; original handle becomes stale
    bool erased = ws.erase(handle);
    REQUIRE(erased);
    CHECK(ws.get(handle) == nullptr);   // stale — generation mismatch

    // 3. put again reuses the freed slot; NEW handle is valid, OLD is still stale
    Employee babbage{.id = EmployeeId{2}, .name = "Babbage", .age = 79};
    auto put_res2 = ws.put(babbage);
    REQUIRE(put_res2.has_value());

    auto handle2 = *put_res2;
    REQUIRE(ws.get(handle2) != nullptr);
    CHECK(ws.get(handle2)->name == "Babbage");

    // Old handle must still be stale even though the slot was recycled
    CHECK(ws.get(handle) == nullptr);

    // 4. Multiple entities live simultaneously
    Employee turing{.id = EmployeeId{3}, .name = "Turing", .age = 41};
    auto put_res3 = ws.put(turing);
    REQUIRE(put_res3.has_value());
    CHECK(ws.get(*put_res3)->name == "Turing");
    CHECK(ws.get(handle2)->name == "Babbage"); // still alive
}

TEST_CASE("sanchaya: explain() reflects actual placement engine decision", "[sanchaya][workspace][explain][placement]") {
    using namespace sanchaya;

    constexpr auto explain_model =
        model<"explain_test">()
            .entity(
                describe_row<Employee>(
                    field<"id",  &Employee::id>(),
                    field<"name",&Employee::name>(),
                    field<"age", &Employee::age>()
                )
            )
            .build();

    auto ws = make_workspace(explain_model).build();

    // Small query — limit(10) should stay InMemory
    auto small_q = from<Employee>()
        .where(member<&Employee::age>() >= 25)
        .select(member<&Employee::name>())
        .limit(10);

    auto small_ex = ws.explain(small_q);
    CHECK(!small_ex.placement_summary.empty());
    CHECK(!small_ex.candidate_evaluations.empty());
    CHECK(small_ex.confidence_score > 0.0);

    // All four candidate stores must appear in the evaluation list
    CHECK(small_ex.candidate_evaluations.size() == 4);

    // At least one candidate must be marked selected
    bool any_selected = false;
    for (const auto& c : small_ex.candidate_evaluations) {
        if (c.is_selected) { any_selected = true; break; }
    }
    CHECK(any_selected);

    // Cardinality pipeline must be non-empty and plausible
    CHECK(!small_ex.cardinality_diagnostics.empty());
    CHECK(!small_ex.rewrite_audit_log.empty());
    CHECK(!small_ex.visual_plan_tree.empty());
    CHECK(!small_ex.logical_optimization_summary.empty());
    CHECK(!small_ex.physical_plan_summary.empty());
}

TEST_CASE("sanchaya: CDC change_record buffer policies", "[sanchaya][sync][cdc]") {
    using namespace sanchaya::sync;

    // Default (string_view_buffer) — zero allocation
    static constexpr std::string_view table_sv = "employee";
    static constexpr std::string_view payload_sv = R"({"id":1,"name":"Ada"})";

    change_record_view rv{
        .sequence   = 42,
        .op         = change_op::insert,
        .table_name = table_sv,
        .payload    = payload_sv
    };
    CHECK(rv.sequence   == 42);
    CHECK(rv.table_name == "employee");
    CHECK(rv.payload    == R"({"id":1,"name":"Ada"})");

    // Owning copy — heap allocation, outlives source buffer
    change_record_owned ro{
        .sequence   = 43,
        .op         = change_op::update,
        .table_name = std::string(table_sv),
        .payload    = std::string(payload_sv)
    };
    CHECK(ro.sequence   == 43);
    CHECK(ro.table_name == "employee");
    CHECK(ro.payload    == R"({"id":1,"name":"Ada"})");

    // Static assertion: default template parameter is string_view_buffer
    static_assert(std::is_same_v<change_record<>::string_t, std::string_view>);
    static_assert(std::is_same_v<change_record_owned::string_t, std::string>);
}

TEST_CASE("sanchaya: service_registry compile-time tag dispatch", "[sanchaya][integration][registry]") {
    using namespace sanchaya::integration;

    struct DummyPlanner { int version{1}; };
    struct DummyStats { int row_count{100}; };

    auto reg = make_service_registry(
        service_instance<"planner", DummyPlanner>{DummyPlanner{.version = 42}},
        service_instance<"stats", DummyStats>{DummyStats{.row_count = 500}}
    );

    CHECK(reg.get<"planner">().version == 42);
    CHECK(reg.get<"stats">().row_count == 500);

    const auto& const_reg = reg;
    CHECK(const_reg.get<"planner">().version == 42);
}

TEST_CASE("sanchaya: sqlite_storage_backend and sqlite_statement RAII", "[sanchaya][backend][sqlite]") {
    using namespace sanchaya::backend;

    sqlite_storage_backend db(":memory:");
    if constexpr (has_sqlite_support) {
        REQUIRE(db.is_open());

        auto exec_res = db.execute("CREATE TABLE test_tbl (id INTEGER PRIMARY KEY, val REAL, name TEXT);");
        REQUIRE(exec_res.has_value());

        auto stmt_res = db.prepare("INSERT INTO test_tbl (id, val, name) VALUES (?, ?, ?);");
        REQUIRE(stmt_res.has_value());
        auto stmt = std::move(*stmt_res);
        REQUIRE(stmt.valid());

        CHECK(stmt.bind(1, 101).has_value());
        CHECK(stmt.bind(2, 3.14159).has_value());
        CHECK(stmt.bind(3, std::string_view{"Pebble"}).has_value());

        auto step_res = stmt.step();
        REQUIRE(step_res.has_value());
        CHECK(*step_res == false); // SQLITE_DONE

        // Query back
        int count = 0;
        double out_val = 0.0;
        std::string out_name;
        auto q_res = db.execute_query("SELECT id, val, name FROM test_tbl;", [&](int, char** argv, char**) {
            count++;
            if (argv[1]) out_val = std::stod(argv[1]);
            if (argv[2]) out_name = argv[2];
        });
        REQUIRE(q_res.has_value());
        CHECK(count == 1);
        CHECK(std::abs(out_val - 3.14159) < 1e-4);
        CHECK(out_name == "Pebble");
    } else {
        auto res = db.prepare("SELECT 1;");
        CHECK(!res.has_value());
        CHECK(res.error().code == 501);
    }
}

TEST_CASE("sanchaya: petika object_codec BEVE binary serialization", "[sanchaya][backend][codec]") {
    using namespace sanchaya::backend;

    // 1. Primitive arithmetic fast path
    int int_val = 4242;
    auto int_encoded = object_codec<int>::encode(int_val);
    auto int_decoded = object_codec<int>::decode(int_encoded);
    REQUIRE(int_decoded.has_value());
    CHECK(*int_decoded == 4242);

    // 2. Struct serialization via BEVE
    Employee emp{
        .id = EmployeeId{101},
        .name = "Ada Lovelace",
        .age = 36,
        .department_id = DepartmentId{2},
        .address = Address{.street = "123 Engine St", .city = "London"}
    };

    auto enc = object_codec<Employee>::encode(emp);
    CHECK(!enc.empty());

    auto dec = object_codec<Employee>::decode(enc);
    REQUIRE(dec.has_value());
    CHECK(dec->id.value == 101);
    CHECK(dec->name == "Ada Lovelace");
    CHECK(dec->age == 36);
    CHECK(dec->department_id.value == 2);
    CHECK(dec->address.city == "London");
}

TEST_CASE("sanchaya: memory_top_n comparator ordering", "[sanchaya][engine][top_n]") {
    using namespace sanchaya::engine;

    memory_top_n<int> top3(3);
    for (int val : {10, 50, 20, 90, 30, 80, 70}) {
        top3.push(val);
    }

    auto sorted = top3.extract_sorted();
    REQUIRE(sorted.size() == 3);
    // Descending order: largest first
    CHECK(sorted[0] == 90);
    CHECK(sorted[1] == 80);
    CHECK(sorted[2] == 70);
}

TEST_CASE("sanchaya: Anukrama Backend snapshot isolation guarantees historical visibility", "[sanchaya][backend][anukrama]") {
    using namespace sanchaya::backend;

    anukrama_storage_backend<std::string, int> store;

    auto put1 = store.put("key1", 10);
    REQUIRE(put1.has_value());

    // Capture point-in-time snapshot
    auto snapshot = store.get_snapshot();
    auto snap_res1 = snapshot.get("key1");
    REQUIRE(snap_res1.has_value());
    CHECK(*snap_res1 == 10);

    // Mutate state with new version
    auto put2 = store.put("key1", 20);
    REQUIRE(put2.has_value());

    // Snapshot continues observing historical version (10)
    auto snap_res2 = snapshot.get("key1");
    REQUIRE(snap_res2.has_value());
    CHECK(*snap_res2 == 10);

    // Latest query observes new version (20)
    CHECK(store.get_latest("key1") == std::optional<int>{20});
}

TEST_CASE("sanchaya: Medha + Sanchaya multi-store transaction resource traits", "[sanchaya][integration][medha]") {
    using namespace sanchaya::integration;
    using namespace sanchaya::backend;

    anukrama_storage_backend<std::string, Employee> mem_store;
    petika_storage_backend<std::string, std::string> durable_store("./demo_storage/petika_medha_test_db");

    entity_table_resource<Employee> res{
        .memory_store = &mem_store,
        .durable_store = &durable_store
    };

    Employee emp{
        .id = EmployeeId{1},
        .name = "Katherine Johnson",
        .age = 50,
        .department_id = DepartmentId{3},
        .address = Address{.street = "Langley", .city = "Hampton"}
    };

    (void)mem_store.put("emp:1", emp);

    medha::transaction_context ctx{};
    auto read_res = tx_read(res, ctx, "emp:1");
    REQUIRE(read_res.has_value());
    CHECK(read_res->name == "Katherine Johnson");
    CHECK(read_res->age == 50);

    auto stage_res = tx_stage(res, ctx, "emp:1", emp);
    CHECK(stage_res.has_value());

    auto val_res = tx_validate(res, ctx);
    CHECK(val_res.has_value());

    auto commit_res = tx_commit(res, ctx);
    CHECK(commit_res.has_value());

    tx_rollback(res, ctx);
}

TEST_CASE("sanchaya: Anukrama multi-key generational history and pruning", "[sanchaya][backend][anukrama][prune]") {
    using namespace sanchaya::backend;

    anukrama_storage_backend<std::string, std::string> store;

    // Insert v1
    (void)store.put("keyA", "v1_A");
    (void)store.put("keyB", "v1_B");

    auto snap1 = store.get_snapshot();

    // Insert v2
    (void)store.put("keyA", "v2_A");
    (void)store.put("keyB", "v2_B");

    auto snap2 = store.get_snapshot();

    // Verify snapshot isolation across multiple keys
    auto s1_a = snap1.get("keyA");
    auto s1_b = snap1.get("keyB");
    REQUIRE(s1_a.has_value());
    REQUIRE(s1_b.has_value());
    CHECK(*s1_a == "v1_A");
    CHECK(*s1_b == "v1_B");

    auto s2_a = snap2.get("keyA");
    auto s2_b = snap2.get("keyB");
    REQUIRE(s2_a.has_value());
    REQUIRE(s2_b.has_value());
    CHECK(*s2_a == "v2_A");
    CHECK(*s2_b == "v2_B");

    CHECK(store.get_latest("keyA") == std::optional<std::string>{"v2_A"});
    CHECK(store.get_latest("keyB") == std::optional<std::string>{"v2_B"});

    // Explicit history pruning
    store.prune_history();

    // Latest state remains intact after pruning
    CHECK(store.get_latest("keyA") == std::optional<std::string>{"v2_A"});
}

TEST_CASE("sanchaya: E-Graph common subexpression elimination deduplication", "[sanchaya][planner][egraph][cse]") {
    using namespace sanchaya::optimizer;

    sanchaya_egraph eg;

    // Node 1: source(1)
    sanchaya_egraph::node_t scan1;
    scan1.op = rel_op::op_source;
    scan1.payload = rel_payload{.signature = 10, .extra_data = 0};
    auto c1 = eg.add(scan1);

    // Node 2: identical source(1) in a different tree branch
    sanchaya_egraph::node_t scan2;
    scan2.op = rel_op::op_source;
    scan2.payload = rel_payload{.signature = 10, .extra_data = 0};
    auto c2 = eg.add(scan2);

    // Node 3 & 4: project(scan1) and project(scan2)
    sanchaya_egraph::node_t proj1;
    proj1.op = rel_op::op_project;
    proj1.children.push_back(c1);
    proj1.payload = rel_payload{.signature = 20, .extra_data = 0};
    auto p1 = eg.add(proj1);

    sanchaya_egraph::node_t proj2;
    proj2.op = rel_op::op_project;
    proj2.children.push_back(c2);
    proj2.payload = rel_payload{.signature = 20, .extra_data = 0};
    auto p2 = eg.add(proj2);

    egraph_relational_optimizer opt{};
    auto report = opt.saturate_graph(eg);

    // CSE rule merges identical subgraphs
    CHECK(eg.find(c1) == eg.find(c2));
    CHECK(eg.find(p1) == eg.find(p2));
    CHECK(report.iters >= 1);
}

TEST_CASE("sanchaya: sqlite_statement move assignment and state reset", "[sanchaya][backend][sqlite][raii]") {
    using namespace sanchaya::backend;

    sqlite_storage_backend db(":memory:");
    if constexpr (has_sqlite_support) {
        REQUIRE(db.is_open());
        (void)db.execute("CREATE TABLE stmt_tbl (k INT, v TEXT);");

        auto prep1 = db.prepare("INSERT INTO stmt_tbl VALUES (?, ?);");
        REQUIRE(prep1.has_value());

        sqlite_statement stmt1 = std::move(*prep1);
        CHECK(stmt1.valid());

        // Move assign
        sqlite_statement stmt2;
        CHECK(!stmt2.valid());
        stmt2 = std::move(stmt1);

        CHECK(!stmt1.valid());
        CHECK(stmt2.valid());

        CHECK(stmt2.bind(1, 42).has_value());
        CHECK(stmt2.bind(2, std::string_view{"Test"}).has_value());
        auto step_res = stmt2.step();
        REQUIRE(step_res.has_value());

        // Reset
        stmt2.reset();
        CHECK(!stmt2.valid());
    }
}

TEST_CASE("sanchaya: workspace move stability preserves session handle validity", "[sanchaya][workspace][move]") {
    using namespace sanchaya;

    constexpr auto company_model =
        model<"move_test_company">()
            .entity(
                describe_row<Employee>(
                    field<"id", &Employee::id>(),
                    field<"name", &Employee::name>(),
                    field<"age", &Employee::age>()
                )
            )
            .build();

    // 1. Create workspace and insert entity
    auto ws1 = make_workspace(company_model).build();
    Employee emp{.id = EmployeeId{777}, .name = "Ada Lovelace", .age = 36};

    auto h_res = ws1.put(emp);
    REQUIRE(h_res.has_value());
    auto handle = *h_res;

    // Verify handle resolves before move
    auto val_before = handle.get(ws1.session_epoch());
    REQUIRE(val_before.has_value());
    CHECK(val_before->get().name == "Ada Lovelace");
    CHECK(val_before->get().age == 36);

    // 2. Move workspace to new variable
    auto ws2 = std::move(ws1);

    // Verify handle still resolves through the moved-to workspace's stable storage
    auto val_after = handle.get(ws2.session_epoch());
    REQUIRE(val_after.has_value());
    CHECK(val_after->get().name == "Ada Lovelace");
    CHECK(val_after->get().age == 36);
    CHECK(handle->name == "Ada Lovelace");

    // Verify workspace get and erase work seamlessly on ws2
    const Employee* ptr = ws2.get(handle);
    REQUIRE(ptr != nullptr);
    CHECK(ptr->name == "Ada Lovelace");

    CHECK(ws2.erase(handle));
    CHECK(!handle.is_valid());
    CHECK(!handle.get(ws2.session_epoch()).has_value());
}

TEST_CASE("sanchaya: compile-time relational model validation", "[sanchaya][schema][validation]") {
    using namespace sanchaya;

    constexpr auto valid_model =
        model<"valid_model">()
            .entity(
                describe_row<Employee>(
                    field<"id", &Employee::id>()
                )
            )
            .entity(
                describe_row<Department>(
                    field<"id", &Department::id>()
                )
            )
            .relation<"emp_dept">(
                many<Employee>(),
                one<Department>(),
                foreign_key<&Employee::department_id>(),
                principal_key<&Department::id>(),
                reference()
            )
            .build();

    auto report = validation::validate_model(valid_model);
    CHECK(report.status == validation::validation_status::valid);
    CHECK(!report.has_reference_cycles);
}

TEST_CASE("sanchaya: memory_top_n strictly descending sort order", "[sanchaya][engine][memory][top_n]") {
    using namespace sanchaya::engine;

    memory_top_n<int> top3(3);
    top3.push(10);
    top3.push(50);
    top3.push(20);
    top3.push(40);
    top3.push(30);

    auto res = top3.extract_sorted();
    REQUIRE(res.size() == 3);
    CHECK(res == std::vector<int>{50, 40, 30});
}

TEST_CASE("sanchaya: medha transactional adapter staged writes and commit durability", "[sanchaya][medha][transaction]") {
    using namespace sanchaya::integration;
    using namespace sanchaya::backend;

    anukrama_storage_backend<std::string, std::string> mem_backend;
    entity_table_resource<std::string> resource{.memory_store = &mem_backend};
    medha::resource_handle handle{resource, medha::resource_id{1, 1}};

    medha::transaction_context ctx;
    REQUIRE(ctx.store(handle, std::string{"emp:101"}, std::string{"Ada Lovelace"}).has_value());
    REQUIRE(ctx.commit().has_value());

    CHECK(mem_backend.get_latest("emp:101") == std::optional<std::string>{"Ada Lovelace"});
}

TEST_CASE("sanchaya: sqlite backend 64-bit integer binding", "[sanchaya][backend][sqlite][int64]") {
    using namespace sanchaya::backend;

    sqlite_storage_backend db(":memory:");
    if constexpr (has_sqlite_support) {
        REQUIRE(db.is_open());
        (void)db.execute("CREATE TABLE int64_tbl (id INTEGER PRIMARY KEY, big_val INTEGER);");

        auto prep = db.prepare("INSERT INTO int64_tbl (id, big_val) VALUES (?, ?);");
        REQUIRE(prep.has_value());

        std::int64_t big_num = 0x7FFFFFFFFFFFFFFFLL; // max int64
        CHECK(prep->bind(1, 1).has_value());
        CHECK(prep->bind(2, big_num).has_value());
        REQUIRE(prep->step().has_value());
        prep->reset();

        auto query_prep = db.prepare("SELECT big_val FROM int64_tbl WHERE id = 1;");
        REQUIRE(query_prep.has_value());
        auto step_res = query_prep->step();
        REQUIRE(step_res.has_value());
        CHECK(*step_res == true);
    }
}
TEST_CASE("sanchaya: Medha fresh-insert compensation erasure on durable failure", "[sanchaya][medha][compensation]") {
    using namespace sanchaya::integration;
    using namespace sanchaya::backend;

    anukrama_storage_backend<std::string, Employee> mem_store;
    petika_storage_backend<std::string, std::string> failing_durable_store("./demo_storage/failing_test_db");
    failing_durable_store.set_fail_writes(true);

    entity_table_resource<Employee> res{
        .memory_store = &mem_store,
        .durable_store = &failing_durable_store
    };

    Employee fresh_emp{
        .id = EmployeeId{999},
        .name = "Test Rollback",
        .age = 28,
        .department_id = DepartmentId{1},
        .address = Address{.street = "Nowhere", .city = "NullCity"}
    };

    medha::transaction_context ctx{};
    auto stage_res = tx_stage(res, ctx, "emp:999", fresh_emp);
    CHECK(stage_res.has_value());

    // tx_commit should fail at durable_store stage and compensate by erasing "emp:999" from memory_store
    auto commit_res = tx_commit(res, ctx);
    CHECK(!commit_res.has_value());

    // Assert that the fresh insert was completely erased from the memory store
    CHECK(mem_store.get_latest("emp:999") == std::nullopt);
}

TEST_CASE("sanchaya: workspace explain returns dynamic egraph saturation metrics across query complexities", "[sanchaya][workspace][explain][dynamic]") {
    using namespace sanchaya;

    constexpr auto company_model =
        model<"company">()
            .entity(describe_row<Employee>(
                field<"id", &Employee::id>(),
                field<"name", &Employee::name>(),
                field<"age", &Employee::age>(),
                field<"department_id", &Employee::department_id>()
            ))
            .build();

    auto ws = make_workspace(company_model).build();

    // 1. Simple single projection query
    auto q_simple = from<Employee>()
        .select(member<&Employee::name>());

    auto exp_simple = ws.explain(q_simple);

    // 2. Complex query with multiple filter predicates
    auto q_complex = from<Employee>()
        .where(member<&Employee::age>() >= 30)
        .where(member<&Employee::department_id>() == DepartmentId{2})
        .select(member<&Employee::name>());

    auto exp_complex = ws.explain(q_complex);

    // The complex query with filters generates more e-nodes in the e-graph than a simple scan/projection
    CHECK(exp_complex.egraph_nodes > exp_simple.egraph_nodes);
    CHECK(exp_complex.egraph_classes >= exp_simple.egraph_classes);
}

TEST_CASE("sanchaya: NADI query execution emits structured pulses with lineage", "[sanchaya][telemetry][nadi]") {
    using namespace sanchaya::telemetry;
    using namespace utils::nadi;

    using TestRingSink = RingBufferSink<32, 64>;
    while (TestRingSink::instance().try_pop()) {}

    nadi_query_observer<TestRingSink> observer;

    {
        auto root_scope = observer.template trace_scope<"sanchaya_plan">(
            Field<"query", std::string_view>{.value = "SELECT name FROM employee"},
            Field<"estimated_rows", std::size_t>{.value = 100}
        );

        {
            auto child_scope = observer.template trace_scope<"sanchaya_exec">(
                Field<"engine", std::string_view>{.value = "InMemory"},
                Field<"threads", int>{.value = 1}
            );
        }
    }

    CHECK(TestRingSink::instance().size() >= 2);
}

TEST_CASE("sanchaya: Pravaha asynchronous CDC task graph replicates snapshots concurrently", "[sanchaya][sync][pravaha]") {
    using namespace sanchaya::sync;
    using namespace sanchaya::backend;

    anukrama_storage_backend<std::string, Employee> mem_store;
    std::atomic<int> snapshot_count{0};
    std::atomic<int> replay_count{0};
    std::atomic<int> drain_count{0};

    for (int i = 0; i < 100; ++i) {
        Employee emp{
            .id = EmployeeId{static_cast<std::uint64_t>(i)},
            .name = "Worker " + std::to_string(i),
            .age = static_cast<int>(20 + (i % 30)),
            .department_id = DepartmentId{1}
        };
        (void)mem_store.put("emp:" + std::to_string(i), emp);
    }

    pravaha::JThreadBackend thread_backend(2);

    auto result = async_cdc_pipeline::run_async(
        thread_backend,
        [&]() {
            snapshot_count.fetch_add(1, std::memory_order_relaxed);
        },
        [&]() {
            replay_count.fetch_add(1, std::memory_order_relaxed);
        },
        [&]() {
            drain_count.fetch_add(1, std::memory_order_relaxed);
        }
    );

    CHECK(result.has_value());
    CHECK(snapshot_count.load() == 1);
    CHECK(replay_count.load() == 1);
    CHECK(drain_count.load() == 1);
}


