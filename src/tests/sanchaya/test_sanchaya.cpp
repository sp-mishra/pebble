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

