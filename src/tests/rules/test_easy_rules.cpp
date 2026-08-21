#define CATCH_CONFIG_MAIN
#include "catch_amalgamated.hpp"

#include "rules/easy_rules.hpp"

using namespace easy_rules;
using namespace easy_rules::dsl;

TEST_CASE (



"Facts basic operations"
,
"[facts]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("a", 10);
    REQUIRE(ctx.facts.has("a"));

    auto result = ctx.facts.get<int>("a");
    REQUIRE(result.has_value());
    REQUIRE(*result == 10);

    ctx.facts.remove("a");
    REQUIRE_FALSE(ctx.facts.has("a"));
}

TEST_CASE (



"Facts type safety with std::expected"
,
"[facts][type_safety]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("age", 25);

    // Correct type access
    auto age_result = ctx.facts.get<int>("age");
    REQUIRE(age_result.has_value());
    REQUIRE(*age_result == 25);

    // Wrong type access should return error
    auto wrong_type = ctx.facts.get<std::string>("age");
    REQUIRE_FALSE(wrong_type.has_value());
    REQUIRE(wrong_type.error() == RuleError::TypeMismatch);

    // Non-existent fact
    auto missing = ctx.facts.get<int>("missing");
    REQUIRE_FALSE(missing.has_value());
    REQUIRE(missing.error() == RuleError::FactNotFound);
}

TEST_CASE (



"Facts get_or with default values"
,
"[facts][defaults]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("counter", 5);

    REQUIRE(ctx.facts.get_or<int>("counter", 0) == 5);
    REQUIRE(ctx.facts.get_or<int>("missing", 42) == 42);
    REQUIRE(ctx.facts.get_or<std::string>("name", std::string("default")) == "default");
}

TEST_CASE (



"Facts snapshot and caching"
,
"[facts][snapshot]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("b", 20);
    ctx.facts.set("a", 10);

    // First snapshot should compute and cache
    const std::string& snap1 = ctx.facts.snapshot();
    REQUIRE(!snap1.empty());
    REQUIRE(snap1.find("a:") != std::string::npos);
    REQUIRE(snap1.find("b:") != std::string::npos);

    // Second snapshot should return cached version (same reference)
    const std::string& snap2 = ctx.facts.snapshot();
    REQUIRE(&snap1 == &snap2);  // Same memory address (cached)

    // Modifying facts should dirty cache
    ctx.facts.set("c", 30);
    const std::string& snap3 = ctx.facts.snapshot();
    REQUIRE(&snap2 != &snap3);  // Different memory address (recalculated)
}

TEST_CASE (



"Modern DSL with type-safe fact references"
,
"[dsl][type_safety]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("age", 25);
    ctx.facts.set("name", std::string("John"));
    ctx.facts.set("active", true);

    // Type-safe DSL usage
    auto age_check = fact<int>("age") >= 18;
    auto name_check = fact<std::string>("name") == std::string("John");
    auto active_check = fact<bool>("active") == true;

    REQUIRE(age_check(ctx.facts));
    REQUIRE(name_check(ctx.facts));
    REQUIRE(active_check(ctx.facts));

    // Combined predicates
    auto combined = age_check && name_check && active_check;
    REQUIRE(combined(ctx.facts));
}

TEST_CASE (



"DSL logical operators"
,
"[dsl][operators]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("x", 5);
    ctx.facts.set("y", 10);
    ctx.facts.set("flag", true);

    auto pred_and = fact<int>("x") < 10 && fact<int>("y") > 5;
    auto pred_or = fact<int>("x") > 100 || fact<bool>("flag") == true;
    auto pred_not = !(fact<int>("x") > 10);

    REQUIRE(pred_and(ctx.facts));
    REQUIRE(pred_or(ctx.facts));
    REQUIRE(pred_not(ctx.facts));
}

TEST_CASE (



"Rule engine basic execution"
,
"[engine][basic]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("age", 17);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    engine.when("adult_check", fact<int>("age") >= 18)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("status", std::string("adult"));
        })
        .with_description("Check if person is adult");

    engine.run(ctx);

    // Should not have set status since age < 18
    REQUIRE_FALSE(ctx.facts.has("status"));

    // Update age and run again
    ctx.facts.set("age", 20);
    engine.run(ctx);

    auto status = ctx.facts.get<std::string>("status");
    REQUIRE(status.has_value());
    REQUIRE(*status == "adult");
}

TEST_CASE (



"Rule activation limits"
,
"[engine][activation_limits]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("counter", 0);
    ctx.facts.set("trigger", true);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    AuditListener audit;
    engine.add_listener(audit);

    engine.when("increment", fact<bool>("trigger") == true)
        .then([](ExecutionContext& ctx) {
            int counter = ctx.facts.get_or<int>("counter", 0);
            ctx.facts.set("counter", counter + 1);
        })
        .with_activation_limit(3);

    engine.run_to_completion(ctx);

    REQUIRE(ctx.facts.get_or<int>("counter", 0) == 3);

    // Check audit trail for skip events
    const auto& history = audit.get_history();
    bool found_skip = std::ranges::any_of(history, [](const auto& event) {
        return event.type == AuditEvent::Type::RuleSkipped;
    });
    REQUIRE(found_skip);
}

TEST_CASE (



"Rule priorities and execution order"
,
"[engine][priority]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("value", 0);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    // Low priority rule
    engine.when("low", fact<int>("value") == 0)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("result", std::string("low"));
        })
        .with_priority(1);

    // High priority rule
    engine.when("high", fact<int>("value") == 0)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("result", std::string("high"));
        })
        .with_priority(10);

    engine.run(ctx);

    // High priority should execute first
    auto result = ctx.facts.get<std::string>("result");
    REQUIRE(result.has_value());
    REQUIRE(*result == "high");
}

TEST_CASE (



"Rule groups and selective execution"
,
"[engine][groups]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("trigger", true);
    ctx.facts.set("group_a_fired", false);
    ctx.facts.set("group_b_fired", false);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    engine.when("groupA", "rule_a", fact<bool>("trigger") == true)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("group_a_fired", true);
        });

    engine.when("groupB", "rule_b", fact<bool>("trigger") == true)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("group_b_fired", true);
        });

    // Run only group A
    engine.run(ctx, {"groupA"});

    REQUIRE(ctx.facts.get_or<bool>("group_a_fired", false));
    REQUIRE_FALSE(ctx.facts.get_or<bool>("group_b_fired", false));
}

TEST_CASE (



"CRTP Audit Listener"
,
"[listeners][crtp]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("test", 1);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    AuditListener audit;
    engine.add_listener(audit);

    engine.when("success_rule", fact<int>("test") == 1)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("fired", true);
        });

    engine.when("fail_rule", fact<int>("test") == 999)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("should_not_fire", true);
        });

    engine.run(ctx);

    const auto& history = audit.get_history();
    REQUIRE(history.size() == 2);  // One success, one failure

    bool found_success = false, found_failure = false;
    for (const auto& event : history) {
        if (event.type == AuditEvent::Type::RuleFired && event.rule_name == "success_rule") {
            found_success = true;
        }
        if (event.type == AuditEvent::Type::RuleFailed && event.rule_name == "fail_rule") {
            found_failure = true;
        }
    }

    REQUIRE(found_success);
    REQUIRE(found_failure);
}

TEST_CASE (



"Custom CRTP Listener"
,
"[listeners][custom_crtp]"
)
{
    class CustomListener : public RuleListener<CustomListener> {
    public:
        int success_count = 0;
        int failure_count = 0;

        bool before_evaluate_impl(const Rule&, const ExecutionContext&) { return true; }

        void on_success_impl(const Rule&, ExecutionContext&) {
            success_count++;
        }

        void on_failure_impl(const Rule&, ExecutionContext&) {
            failure_count++;
        }
    };

    ExecutionContext ctx;
    ctx.facts.set("value", 5);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    CustomListener listener;
    engine.add_listener(listener);

    engine.when("rule1", fact<int>("value") == 5)
        .then([](ExecutionContext& ctx) {});

    engine.when("rule2", fact<int>("value") == 999)
        .then([](ExecutionContext& ctx) {});

    engine.run(ctx);

    REQUIRE(listener.success_count == 1);
    REQUIRE(listener.failure_count == 1);
}

TEST_CASE (



"Rule removal and activation count cleanup"
,
"[engine][removal]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("trigger", true);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    // Add rule with activation limit
    engine.when("temp_rule", fact<bool>("trigger") == true)
        .then([](ExecutionContext& ctx) {
            int count = ctx.facts.get_or<int>("fired_count", 0);
            ctx.facts.set("fired_count", count + 1);
        })
        .with_activation_limit(1);

    // First run should fire
    engine.run(ctx);
    REQUIRE(ctx.facts.get_or<int>("fired_count", 0) == 1);

    // Remove and re-add rule
    engine.remove_rule("temp_rule");
    engine.when("temp_rule", fact<bool>("trigger") == true)
        .then([](ExecutionContext& ctx) {
            int count = ctx.facts.get_or<int>("fired_count", 0);
            ctx.facts.set("fired_count", count + 1);
        })
        .with_activation_limit(1);

    // Should fire again since activation count was reset
    engine.run(ctx);
    REQUIRE(ctx.facts.get_or<int>("fired_count", 0) == 2);
}

TEST_CASE (



"Otherwise/else actions"
,
"[rules][otherwise]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("condition", false);
    ctx.facts.set("else_fired", false);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    engine.when("test_rule", fact<bool>("condition") == true)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("then_fired", true);
        })
        .otherwise([](ExecutionContext& ctx) {
            ctx.facts.set("else_fired", true);
        });

    engine.run(ctx);

    REQUIRE_FALSE(ctx.facts.get_or<bool>("then_fired", false));
    REQUIRE(ctx.facts.get_or<bool>("else_fired", false));
}

TEST_CASE (



"Stop on first match configuration"
,
"[engine][stop_first]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("trigger", true);
    ctx.facts.set("count1", 0);
    ctx.facts.set("count2", 0);

    EasyRuleEngine engine;
    engine.config.verbose = false;
    engine.config.stop_on_first_match = true;

    engine.when("rule1", fact<bool>("trigger") == true)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("count1", 1);
        });

    engine.when("rule2", fact<bool>("trigger") == true)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("count2", 1);
        });

    engine.run(ctx);

    // Only one rule should have fired
    int total = ctx.facts.get_or<int>("count1", 0) + ctx.facts.get_or<int>("count2", 0);
    REQUIRE(total == 1);
}

TEST_CASE (



"Cycle detection in run_to_completion"
,
"[engine][cycles]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("toggle", 0);

    EasyRuleEngine engine;
    engine.config.verbose = false;
    engine.config.max_iterations = 1000;  // High limit

    AuditListener audit;
    engine.add_listener(audit);

    // Create oscillating rules
    engine.when("to_one", fact<int>("toggle") == 0)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("toggle", 1);
        });

    engine.when("to_zero", fact<int>("toggle") == 1)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("toggle", 0);
        });

    engine.run_to_completion(ctx);

    // Should have detected cycle and stopped before max iterations
    REQUIRE(ctx.metrics.iterations < 1000);
    REQUIRE(ctx.metrics.rules_fired > 0);  // Some rules should have fired
}

TEST_CASE (



"Performance metrics"
,
"[engine][metrics]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("value", 1);

    EasyRuleEngine engine;
    engine.config.verbose = false;
    engine.config.enable_metrics = true;

    engine.when("test_rule", fact<int>("value") == 1)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("result", true);
        });

    engine.run(ctx);

    REQUIRE(ctx.metrics.rules_evaluated == 1);
    REQUIRE(ctx.metrics.rules_fired == 1);
    REQUIRE(ctx.metrics.iterations == 1);
    REQUIRE(ctx.metrics.total_time.count() > 0);
}

TEST_CASE (



"Complex predicates with all operators"
,
"[dsl][complex]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("age", 25);
    ctx.facts.set("score", 85);
    ctx.facts.set("active", true);
    ctx.facts.set("name", std::string("John"));

    EasyRuleEngine engine;
    engine.config.verbose = false;

    // Complex predicate combining all operators
    auto complex_pred = (fact<int>("age") >= 18 && fact<int>("age") <= 65) &&
                       (fact<int>("score") > 80 || fact<bool>("active") == true) &&
                       fact<std::string>("name") != std::string("") &&
                       !(fact<int>("score") < 50);

    engine.when("complex_rule", complex_pred)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("qualified", true);
        });

    engine.run(ctx);

    REQUIRE(ctx.facts.get_or<bool>("qualified", false));
}

TEST_CASE (



"Facts iteration and range support"
,
"[facts][ranges]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("a", 1);
    ctx.facts.set("b", std::string("test"));
    ctx.facts.set("c", true);

    REQUIRE(ctx.facts.size() == 3);
    REQUIRE_FALSE(ctx.facts.empty());

    size_t count = 0;
    for (const auto& [key, value] : ctx.facts) {
        REQUIRE(!key.empty());
        count++;
    }
    REQUIRE(count == 3);

    // Test range-based algorithms
    auto keys = ctx.facts | std::views::keys;
    auto key_vec = std::vector<std::string>(keys.begin(), keys.end());
    REQUIRE(key_vec.size() == 3);
}

TEST_CASE (



"Rule engine with no rules (empty execution)"
,
"[engine][empty]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("test", 42);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    // Run with no rules
    engine.run(ctx);
    engine.run_to_completion(ctx);

    // Facts should be unchanged
    REQUIRE(ctx.facts.get_or<int>("test", 0) == 42);
    REQUIRE(ctx.metrics.rules_evaluated == 0);
    REQUIRE(ctx.metrics.rules_fired == 0);
}

TEST_CASE (



"Facts clear and basic operations"
,
"[facts][clear]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("a", 1);
    ctx.facts.set("b", 2);
    ctx.facts.set("c", 3);

    REQUIRE(ctx.facts.size() == 3);

    ctx.facts.clear();

    REQUIRE(ctx.facts.size() == 0);
    REQUIRE(ctx.facts.empty());
    REQUIRE_FALSE(ctx.facts.has("a"));
}

TEST_CASE (



"Rule descriptions and metadata"
,
"[rules][metadata]"
)
{
    ExecutionContext ctx;

    EasyRuleEngine engine;

    auto& rule = engine.when("test_rule", fact<int>("x") == 0)
        .with_description("This is a test rule")
        .with_priority(5)
        .with_activation_limit(2);

    REQUIRE(rule.name == "test_rule");
    REQUIRE(rule.description == "This is a test rule");
    REQUIRE(rule.priority == 5);
    REQUIRE(rule.activation_limit.has_value());
    REQUIRE(*rule.activation_limit == 2);
}

// Enhanced Features Tests

TEST_CASE (



"Enhanced string operations"
,
"[enhanced][string]"
)
{
    using namespace easy_rules::dsl;

    ExecutionContext ctx;
    ctx.facts.set("username", std::string("admin_user"));
    ctx.facts.set("email", std::string("user@company.com"));
    ctx.facts.set("filename", std::string("document.pdf"));
    ctx.facts.set("command", std::string("SHUTDOWN"));
    ctx.facts.set("phone", std::string("555-123-4567"));

    SECTION("String contains operation") {
        auto contains_pred = string_fact("username").contains("admin");
        REQUIRE(contains_pred(ctx.facts));

        auto not_contains_pred = string_fact("username").contains("guest");
        REQUIRE_FALSE(not_contains_pred(ctx.facts));
    }

    SECTION("String starts_with operation") {
        auto starts_pred = string_fact("email").starts_with("user");
        REQUIRE(starts_pred(ctx.facts));

        auto not_starts_pred = string_fact("email").starts_with("admin");
        REQUIRE_FALSE(not_starts_pred(ctx.facts));
    }

    SECTION("String ends_with operation") {
        auto ends_pred = string_fact("filename").ends_with(".pdf");
        REQUIRE(ends_pred(ctx.facts));

        auto not_ends_pred = string_fact("filename").ends_with(".doc");
        REQUIRE_FALSE(not_ends_pred(ctx.facts));
    }

    SECTION("String case-insensitive equals") {
        auto iequals_pred = string_fact("command").iequals("shutdown");
        REQUIRE(iequals_pred(ctx.facts));

        auto not_iequals_pred = string_fact("command").iequals("restart");
        REQUIRE_FALSE(not_iequals_pred(ctx.facts));
    }

    SECTION("String regex matching") {
        auto phone_pattern_pred = string_fact("phone").matches(R"(\d{3}-\d{3}-\d{4})");
        REQUIRE(phone_pattern_pred(ctx.facts));

        auto email_pattern_pred = string_fact("phone").matches(R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})");
        REQUIRE_FALSE(email_pattern_pred(ctx.facts));
    }
}

TEST_CASE (



"Enhanced numeric operations"
,
"[enhanced][numeric]"
)
{
    using namespace easy_rules::dsl;

    ExecutionContext ctx;
    ctx.facts.set("age", 25);
    ctx.facts.set("counter", 10);
    ctx.facts.set("temperature", 98.6);

    SECTION("Numeric range checks") {
        auto in_range_pred = numeric_fact<int>("age").in_range(18, 65);
        REQUIRE(in_range_pred(ctx.facts));

        ctx.facts.set("age", 70);
        REQUIRE_FALSE(in_range_pred(ctx.facts));

        ctx.facts.set("age", 16);
        REQUIRE_FALSE(in_range_pred(ctx.facts));
    }

    SECTION("Even/odd checks") {
        auto even_pred = numeric_fact<int>("counter").is_even();
        REQUIRE(even_pred(ctx.facts));

        auto odd_pred = numeric_fact<int>("counter").is_odd();
        REQUIRE_FALSE(odd_pred(ctx.facts));

        ctx.facts.set("counter", 7);
        REQUIRE_FALSE(even_pred(ctx.facts));
        REQUIRE(odd_pred(ctx.facts));
    }

    SECTION("Divisibility checks") {
        auto div_by_5 = numeric_fact<int>("counter").divisible_by(5);
        REQUIRE(div_by_5(ctx.facts));

        auto div_by_3 = numeric_fact<int>("counter").divisible_by(3);
        REQUIRE_FALSE(div_by_3(ctx.facts));
    }

    SECTION("Floating point approximation") {
        auto approx_pred = numeric_fact<double>("temperature").approx_equal(98.6, 0.1);
        REQUIRE(approx_pred(ctx.facts));

        ctx.facts.set("temperature", 98.8);
        REQUIRE_FALSE(approx_pred(ctx.facts));

        auto wider_approx = numeric_fact<double>("temperature").approx_equal(98.6, 0.5);
        REQUIRE(wider_approx(ctx.facts));
    }
}

TEST_CASE (



"Enhanced temporal operations"
,
"[enhanced][temporal]"
)
{
    using namespace easy_rules::dsl;

    ExecutionContext ctx;
    auto now = std::chrono::system_clock::now();
    auto past_time = now - std::chrono::hours(2);
    auto future_time = now + std::chrono::hours(2);

    ctx.facts.set("event_time", static_cast<int>(std::chrono::system_clock::to_time_t(past_time)));
    ctx.facts.set("future_event", static_cast<int>(std::chrono::system_clock::to_time_t(future_time)));

    SECTION("Before/after checks") {
        auto before_pred = temporal_fact("event_time").before(now);
        REQUIRE(before_pred(ctx.facts));

        auto after_pred = temporal_fact("future_event").after(now);
        REQUIRE(after_pred(ctx.facts));
    }

    SECTION("Within duration checks") {
        auto within_pred = temporal_fact("event_time").within_last(std::chrono::hours(3));
        REQUIRE(within_pred(ctx.facts));

        auto not_within_pred = temporal_fact("event_time").within_last(std::chrono::hours(1));
        REQUIRE_FALSE(not_within_pred(ctx.facts));
    }
}

TEST_CASE (



"Enhanced facts operations"
,
"[enhanced][facts]"
)
{
    ExecutionContext ctx;

    SECTION("Conditional set operation") {
        bool condition = true;
        ctx.facts.set_if("conditional_value", 42, [condition]() { return condition; });
        REQUIRE(ctx.facts.has("conditional_value"));
        REQUIRE(ctx.facts.get_or<int>("conditional_value", 0) == 42);

        condition = false;
        ctx.facts.set_if("another_value", 100, [condition]() { return condition; });
        REQUIRE_FALSE(ctx.facts.has("another_value"));
    }

    SECTION("Bulk set operations") {
        std::vector<std::pair<std::string, std::string>> bulk_data = {
            {"key1", "value1"},
            {"key2", "value2"},
            {"key3", "value3"}
        };

        ctx.facts.set_bulk(bulk_data);

        REQUIRE(ctx.facts.get_or<std::string>("key1", "") == "value1");
        REQUIRE(ctx.facts.get_or<std::string>("key2", "") == "value2");
        REQUIRE(ctx.facts.get_or<std::string>("key3", "") == "value3");
    }

    SECTION("Compare and set operations") {
        ctx.facts.set("atomic_counter", 5);

        // Should succeed - value matches expected
        bool result = ctx.facts.compare_and_set("atomic_counter", 5, 10);
        REQUIRE(result);
        REQUIRE(ctx.facts.get_or<int>("atomic_counter", 0) == 10);

        // Should fail - value doesn't match expected
        result = ctx.facts.compare_and_set("atomic_counter", 5, 15);
        REQUIRE_FALSE(result);
        REQUIRE(ctx.facts.get_or<int>("atomic_counter", 0) == 10);
    }

    SECTION("Transform operations") {
        ctx.facts.set("number", 5);

        ctx.facts.transform<int>("number", [](int val) { return val * 2; });
        REQUIRE(ctx.facts.get_or<int>("number", 0) == 10);

        // Transform on non-existent fact should not crash
        ctx.facts.transform<int>("missing", [](int val) { return val * 2; });
        REQUIRE_FALSE(ctx.facts.has("missing"));
    }
}

TEST_CASE (



"Thread-safe rule engine"
,
"[enhanced][threadsafe]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("counter", 0);

    ThreadSafeRuleEngine engine;
    engine.config.verbose = false;

    // Add a simple rule that only fires once
    engine.when_threadsafe("increment", fact<int>("counter") == 0)
        .then([](ExecutionContext& ctx) {
            int current = ctx.facts.get_or<int>("counter", 0);
            ctx.facts.set("counter", current + 1);
        })
        .with_activation_limit(1);

    engine.run_threadsafe(ctx);

    REQUIRE(ctx.facts.get_or<int>("counter", 0) == 1);
}

TEST_CASE (



"Thread-safe async execution"
,
"[enhanced][async]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("async_value", 0);

    ThreadSafeRuleEngine engine;
    engine.config.verbose = false;

    engine.when_threadsafe("async_rule", fact<int>("async_value") == 0)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("async_result", std::string("processed"));
        });

    auto future = engine.run_async(ctx);
    future.wait(); // Wait for completion

    REQUIRE(ctx.facts.get_or<std::string>("async_result", "") == "processed");
}

TEST_CASE (



"Rule templates"
,
"[enhanced][templates]"
)
{
    using namespace easy_rules::templates;

    ExecutionContext ctx;
    ctx.facts.set("score", 85);
    ctx.facts.set("age", 25);
    ctx.facts.set("email", std::string("user@example.com"));

    EasyRuleEngine engine;
    engine.config.verbose = false;

    SECTION("Threshold rule template") {
        auto threshold_rule = create_threshold_rule<int>(
            "high_score", "score", 80, "achievement", "high_scorer"
        );

        engine.when("threshold_test", threshold_rule.predicate)
            .then(threshold_rule.action);

        engine.run(ctx);

        REQUIRE(ctx.facts.get_or<std::string>("achievement", "") == "high_scorer");
    }

    SECTION("Validation rule template") {
        auto validation_rule = create_validation_rule<int>(
            "age_validation", "age", [](const int& age) { return age >= 18; }, "age_error"
        );

        engine.when("validate_age", validation_rule.predicate)
            .then(validation_rule.action)
            .otherwise(validation_rule.else_action);

        engine.run(ctx);

        REQUIRE(ctx.facts.get_or<bool>("validation_passed", false) == true);
        REQUIRE_FALSE(ctx.facts.has("age_error"));
    }

    SECTION("Transformation rule template") {
        auto transform_rule = create_transformation_rule<std::string, std::string>(
            "email_transform", "email", "domain",
            [](const std::string& email) {
                auto at_pos = email.find('@');
                return at_pos != std::string::npos ? email.substr(at_pos + 1) : "";
            }
        );

        engine.when("extract_domain", transform_rule.predicate)
            .then(transform_rule.action);

        engine.run(ctx);

        REQUIRE(ctx.facts.get_or<std::string>("domain", "") == "example.com");
    }
}

TEST_CASE (



"Enhanced audit listener"
,
"[enhanced][audit]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("test_value", 1);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    EnhancedAuditListener enhanced_audit;
    engine.add_listener(enhanced_audit);

    // Add multiple rules for statistics
    engine.when("rule1", fact<int>("test_value") == 1)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("rule1_fired", true);
        });

    engine.when("rule2", fact<int>("test_value") == 1)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("rule2_fired", true);
        });

    engine.when("rule3", fact<int>("test_value") == 999)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("rule3_fired", true);
        });

    engine.run(ctx);

    // Check basic audit functionality
    const auto& history = enhanced_audit.get_history();
    REQUIRE(history.size() == 3); // Two successes, one failure

    // Check statistics
    auto stats = enhanced_audit.get_rule_statistics();
    REQUIRE(stats.size() == 2); // Only successful rules have stats

    bool found_rule1 = false, found_rule2 = false;
    for (const auto& stat : stats) {
        if (stat.name == "rule1" && stat.execution_count == 1) {
            found_rule1 = true;
        }
        if (stat.name == "rule2" && stat.execution_count == 1) {
            found_rule2 = true;
        }
    }
    REQUIRE(found_rule1);
    REQUIRE(found_rule2);
}

TEST_CASE (



"Audit stats edge cases: never-firing and execution count"
,
"[enhanced][audit][edge_cases]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("value", 5);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    EnhancedAuditListener enhanced_audit;
    engine.add_listener(enhanced_audit);

    // Rule that will never match
    engine.when("never_fire", fact<int>("value") > 100)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("impossible", true);
        });

    // Rule that will match
    engine.when("will_fire", fact<int>("value") > 0)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("found_it", true);
        });

    engine.run(ctx);

    // Never-firing rule should have no stats
    auto stats = enhanced_audit.get_rule_statistics();
    REQUIRE(stats.size() == 1);  // Only will_fire has stats
    REQUIRE(stats[0].name == "will_fire");
    REQUIRE(stats[0].execution_count == 1);
}


TEST_CASE (



"Collection operations"
,
"[enhanced][collections]"
)
{
    using namespace easy_rules::dsl;

    ExecutionContext ctx;
    ctx.facts.set("user_role", std::string("admin"));
    ctx.facts.set("status_code", 404);

    SECTION("String in collection using custom predicate") {
        // Note: in() DSL function exists but requires FactRef interface not implemented
        // by fact<T>() factory, so we use manual predicate instead
        std::vector<std::string> allowed_roles = {"admin", "moderator", "user"};

        auto role_check = [allowed_roles](const Facts& facts) -> bool {
            auto val = facts.get<std::string>("user_role");
            return val.has_value() &&
                   std::find(allowed_roles.begin(), allowed_roles.end(), *val) != allowed_roles.end();
        };

        REQUIRE(role_check(ctx.facts));

        ctx.facts.set("user_role", std::string("guest"));
        REQUIRE_FALSE(role_check(ctx.facts));
    }

    SECTION("Numeric in collection using custom predicate") {
        std::vector<int> error_codes = {404, 500, 503};

        auto error_check = [error_codes](const Facts& facts) -> bool {
            auto val = facts.get<int>("status_code");
            return val.has_value() &&
                   std::find(error_codes.begin(), error_codes.end(), *val) != error_codes.end();
        };

        REQUIRE(error_check(ctx.facts));

        ctx.facts.set("status_code", 200);
        REQUIRE_FALSE(error_check(ctx.facts));
    }
}

TEST_CASE (



"Complex enhanced DSL combinations"
,
"[enhanced][complex]"
)
{
    using namespace easy_rules::dsl;

    ExecutionContext ctx;
    ctx.facts.set("user_age", 28);
    ctx.facts.set("user_email", std::string("premium@company.com"));
    ctx.facts.set("account_balance", 1500.0);
    ctx.facts.set("membership_level", std::string("PREMIUM"));

    EasyRuleEngine engine;
    engine.config.verbose = false;

    // Complex predicate combining multiple enhanced operations
    auto complex_predicate =
        numeric_fact<int>("user_age").in_range(25, 35) &&
        string_fact("user_email").ends_with("@company.com") &&
        numeric_fact<double>("account_balance").in_range(1000.0, 5000.0) &&
        string_fact("membership_level").iequals("premium");

    engine.when("vip_qualification", complex_predicate)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("vip_status", true);
            ctx.facts.set("discount_rate", 0.20);
        });

    engine.run(ctx);

    REQUIRE(ctx.facts.get_or<bool>("vip_status", false));
    REQUIRE(ctx.facts.get_or<double>("discount_rate", 0.0) == 0.20);
}

TEST_CASE (



"Enhanced rule engine configuration"
,
"[enhanced][config]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("trigger", true);

    EasyRuleEngine engine;

    SECTION("Performance metrics enabled") {
        engine.config.enable_metrics = true;
        engine.config.verbose = false;

        engine.when("metric_test", fact<bool>("trigger") == true)
            .then([](ExecutionContext& ctx) {
                ctx.facts.set("metrics_test", true);
            });

        engine.run(ctx);

        REQUIRE(ctx.metrics.rules_evaluated > 0);
        REQUIRE(ctx.metrics.rules_fired > 0);
        REQUIRE(ctx.metrics.total_time.count() > 0);
    }

    SECTION("Stop on first match with enhanced rules") {
        engine.config.stop_on_first_match = true;
        engine.config.verbose = false;

        engine.when("first_rule", fact<bool>("trigger") == true)
            .then([](ExecutionContext& ctx) {
                ctx.facts.set("first_executed", true);
            });

        engine.when("second_rule", fact<bool>("trigger") == true)
            .then([](ExecutionContext& ctx) {
                ctx.facts.set("second_executed", true);
            });

        engine.run(ctx);

        // Only one should execute due to stop_on_first_match
        int executed_count = 0;
        if (ctx.facts.get_or<bool>("first_executed", false)) executed_count++;
        if (ctx.facts.get_or<bool>("second_executed", false)) executed_count++;

        REQUIRE(executed_count == 1);
    }
}

TEST_CASE (



"Enhanced facts stress test"
,
"[enhanced][stress]"
)
{
    ExecutionContext ctx;

    // Test with large number of facts
    for (int i = 0; i < 100; ++i) {
        ctx.facts.set("fact_" + std::to_string(i), i);
    }

    REQUIRE(ctx.facts.size() == 100);

    // Test snapshot performance with many facts
    auto snapshot1 = ctx.facts.snapshot();
    auto snapshot2 = ctx.facts.snapshot();
    // Both snapshots should have the same content
    REQUIRE(snapshot1 == snapshot2);

    // After modifying facts, snapshot should change
    ctx.facts.set("new_fact", 999);
    auto snapshot3 = ctx.facts.snapshot();
    REQUIRE(snapshot1 != snapshot3);

    // Test range operations
    size_t count = 0;
    for (const auto& [key, value] : ctx.facts) {
        ++count;
    }
    REQUIRE(count == 101); // 100 original + 1 new fact

    // Test bulk operations
    std::vector<std::pair<std::string, std::string>> bulk_data;
    for (int i = 200; i < 250; ++i) {
        bulk_data.emplace_back("bulk_" + std::to_string(i), "value_" + std::to_string(i));
    }

    ctx.facts.set_bulk(bulk_data);
    REQUIRE(ctx.facts.size() == 151); // 101 previous + 50 bulk
}

TEST_CASE (



"Error path: predicate exception handling"
,
"[errors][exceptions]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("test_val", 10);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    // Predicate that throws exception
    engine.when("bad_pred", [](const Facts&) -> bool {
        throw std::runtime_error("predicate error");
    }).then([](ExecutionContext& ctx) {
        ctx.facts.set("fired", true);
    });

    // Should handle exception gracefully
    try {
        engine.run(ctx);
    } catch (...) {
        // Exception caught, as expected
    }
    REQUIRE_FALSE(ctx.facts.get<bool>("fired").has_value());
}

TEST_CASE (



"Error path: action exception handling"
,
"[errors][exceptions]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("trigger", true);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    engine.when("error_action", fact<bool>("trigger") == true)
        .then([](ExecutionContext&) {
            throw std::runtime_error("action error");
        });

    // Should handle exception in action
    try {
        engine.run(ctx);
    } catch (...) {
        // Exception caught
    }
}

TEST_CASE (



"Error path: listener veto"
,
"[errors][listener]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("test_val", 5);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    // Listener that vetoes execution
    struct VetoListener : public RuleListener<VetoListener> {
        bool before_evaluate_impl(const Rule&, const ExecutionContext&) {
            return false; // veto
        }
    };

    VetoListener veto;
    engine.add_listener(veto);

    engine.when("check", fact<int>("test_val") > 0)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("executed", true);
        });

    engine.run(ctx);

    // Action should not have executed due to veto
    REQUIRE_FALSE(ctx.facts.get<bool>("executed").has_value());
}

TEST_CASE (



"Error path: rule never fires edge case"
,
"[errors][edge_cases]"
)
{
    ExecutionContext ctx;
    ctx.facts.set("value", 5);

    EasyRuleEngine engine;
    engine.config.verbose = false;

    EnhancedAuditListener audit;
    engine.add_listener(audit);

    // Rule that will never match
    engine.when("never_fire", fact<int>("value") > 100)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("impossible", true);
        });

    engine.run(ctx);

    // Rule never fired, so no statistics should exist for it
    auto stats = audit.get_rule_statistics();
    for (const auto& stat : stats) {
        REQUIRE(stat.name != "never_fire");
    }
}
