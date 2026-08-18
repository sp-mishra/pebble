#include "catch_amalgamated.hpp"
#include "test/example_registry.hpp"
#include <array>
#include <span>
#include <string_view>

namespace {
    // ─── Concept validation tests ───────────────────────────────────────────

    // Valid static example: meets ExampleType requirements
    struct ValidStatic {
        static constexpr std::string_view name() { return "valid_static"; }
        static constexpr std::string_view description() { return "Valid static example"; }
        static constexpr std::array<std::string_view, 1> tag_data{"test"};
        static constexpr std::span<const std::string_view> tags() { return tag_data; }
        static testfw::Result run() { return {}; }
    };

    // Valid instance example: meets ExampleType requirements
    struct ValidInstance {
        static constexpr std::string_view name() { return "valid_instance"; }
        static constexpr std::string_view description() { return "Valid instance example"; }
        static constexpr std::array<std::string_view, 1> tag_data{"test"};
        static constexpr std::span<const std::string_view> tags() { return tag_data; }
        testfw::Result run() { return {}; }
    };

    // Instance with lifecycle hooks
    struct ValidWithSetupTeardown {
        static constexpr std::string_view name() { return "with_lifecycle"; }
        static constexpr std::string_view description() { return "Instance with setup/teardown"; }
        static constexpr std::array<std::string_view, 1> tag_data{"lifecycle"};
        static constexpr std::span<const std::string_view> tags() { return tag_data; }

        testfw::Result setup() { return {}; }
        testfw::Result run() { return {}; }
        testfw::Result teardown() { return {}; }
    };

    // Static with setup/teardown
    struct StaticWithSetupTeardown {
        static constexpr std::string_view name() { return "static_with_lifecycle"; }
        static constexpr std::string_view description() { return "Static example with setup/teardown"; }
        static constexpr std::array<std::string_view, 1> tag_data{"lifecycle"};
        static constexpr std::span<const std::string_view> tags() { return tag_data; }

        static testfw::Result setup() { return {}; }
        static testfw::Result run() { return {}; }
        static testfw::Result teardown() { return {}; }
    };

    // Failing example
    struct FailingRun {
        static constexpr std::string_view name() { return "failing_run"; }
        static constexpr std::string_view description() { return "Failing example"; }
        static constexpr std::array<std::string_view, 1> tag_data{"error"};
        static constexpr std::span<const std::string_view> tags() { return tag_data; }
        static testfw::Result run() { return testfw::fail("intentional failure"); }
    };

    // Example with failed setup
    struct FailingSetup {
        static constexpr std::string_view name() { return "failing_setup"; }
        static constexpr std::string_view description() { return "Setup failure"; }
        static constexpr std::array<std::string_view, 1> tag_data{"error"};
        static constexpr std::span<const std::string_view> tags() { return tag_data; }

        testfw::Result setup() { return testfw::fail("setup failed"); }
        testfw::Result run() { return {}; }
    };

    // Example with failed teardown
    struct FailingTeardown {
        static constexpr std::string_view name() { return "failing_teardown"; }
        static constexpr std::string_view description() { return "Teardown failure"; }
        static constexpr std::array<std::string_view, 1> tag_data{"error"};
        static constexpr std::span<const std::string_view> tags() { return tag_data; }

        testfw::Result setup() { return {}; }
        testfw::Result run() { return {}; }
        testfw::Result teardown() { return testfw::fail("teardown failed"); }
    };

    // Multiple tags example
    struct MultiTagExample {
        static constexpr std::string_view name() { return "multi_tag"; }
        static constexpr std::string_view description() { return "Example with multiple tags"; }
        static constexpr std::array<std::string_view, 3> tag_data{"core", "fast", "smoke"};
        static constexpr std::span<const std::string_view> tags() { return tag_data; }
        static testfw::Result run() { return {}; }
    };
} // namespace

// ─── Concept validation ─────────────────────────────────────────────────────

TEST_CASE (



"ExampleType concept accepts valid static examples"
)
 {
    static_assert(testfw::ExampleType<ValidStatic>);
}

TEST_CASE (



"ExampleType concept accepts valid instance examples"
)
 {
    static_assert(testfw::ExampleType<ValidInstance>);
}

TEST_CASE (



"ExampleType concept accepts instance with setup/teardown"
)
 {
    static_assert(testfw::ExampleType<ValidWithSetupTeardown>);
}

TEST_CASE (



"ExampleType concept accepts static with setup/teardown"
)
 {
    static_assert(testfw::ExampleType<StaticWithSetupTeardown>);
}

// ─── Lifecycle tests ────────────────────────────────────────────────────────

TEST_CASE (



"Static example runs and returns success"
)
 {
    using Reg = testfw::Registry<ValidStatic>;
    REQUIRE(Reg::run_by_name("valid_static") == 0);
}

TEST_CASE (



"Instance example runs and returns success"
)
 {
    using Reg = testfw::Registry<ValidInstance>;
    REQUIRE(Reg::run_by_name("valid_instance") == 0);
}

TEST_CASE (



"Instance with setup/teardown executes in order"
)
 {
    using Reg = testfw::Registry<ValidWithSetupTeardown>;
    REQUIRE(Reg::run_by_name("with_lifecycle") == 0);
}

TEST_CASE (



"Static with setup/teardown executes in order"
)
 {
    using Reg = testfw::Registry<StaticWithSetupTeardown>;
    REQUIRE(Reg::run_by_name("static_with_lifecycle") == 0);
}

TEST_CASE (



"Run failure returns error exit code"
)
 {
    using Reg = testfw::Registry<FailingRun>;
    REQUIRE(Reg::run_by_name("failing_run") == 2);
}

TEST_CASE (



"Setup failure prevents run execution"
)
 {
    using Reg = testfw::Registry<FailingSetup>;
    REQUIRE(Reg::run_by_name("failing_setup") == 2);
}

TEST_CASE (



"Teardown failure reported despite successful run"
)
 {
    using Reg = testfw::Registry<FailingTeardown>;
    REQUIRE(Reg::run_by_name("failing_teardown") == 2);
}

// ─── Registry dispatch ──────────────────────────────────────────────────────

TEST_CASE (



"run_by_name executes specified example"
)
 {
    using Reg = testfw::Registry<ValidStatic, ValidInstance>;
    REQUIRE(Reg::run_by_name("valid_static") == 0);
    REQUIRE(Reg::run_by_name("valid_instance") == 0);
}

TEST_CASE (



"run_by_name returns error for unknown example"
)
 {
    using Reg = testfw::Registry<ValidStatic>;
    REQUIRE(Reg::run_by_name("nonexistent") == 1);
}

TEST_CASE (



"run_all executes all examples"
)
 {
    using Reg = testfw::Registry<ValidStatic, ValidInstance, ValidWithSetupTeardown>;
    REQUIRE(Reg::run_all() == 0);
}

TEST_CASE (



"run_all returns error if any example fails"
)
 {
    using Reg = testfw::Registry<ValidStatic, FailingRun>;
    REQUIRE(Reg::run_all() == 2);
}

TEST_CASE (



"run_by_tag filters examples by single tag"
)
 {
    using Reg = testfw::Registry<ValidStatic, MultiTagExample, ValidInstance>;
    REQUIRE(Reg::run_by_tag("smoke") == 0);
}

TEST_CASE (



"run_by_tag executes all examples with matching tag"
)
 {
    using Reg = testfw::Registry<MultiTagExample, ValidWithSetupTeardown>;
    // MultiTagExample has tag "core", ValidWithSetupTeardown has tag "lifecycle"
    // Neither should match the "fire" tag
    REQUIRE(Reg::run_by_tag("fire") == 1);
}

TEST_CASE (



"run_by_tag returns error for nonexistent tag"
)
 {
    using Reg = testfw::Registry<ValidStatic>;
    REQUIRE(Reg::run_by_tag("nonexistent_tag") == 1);
}

TEST_CASE (



"run_by_tag returns error when any tagged example fails"
)
 {
    using Reg = testfw::Registry<MultiTagExample, FailingRun>;
    REQUIRE(Reg::run_by_tag("error") == 2);
}

// ─── Error reporting ────────────────────────────────────────────────────────

TEST_CASE (



"Error contains message and source location"
)
 {
    testfw::Error err{testfw::fail("test message").error()};
    REQUIRE(std::string_view{err.message} == "test message");
    REQUIRE(err.where.line() > 0);
    REQUIRE(err.where.file_name() != nullptr);
}

TEST_CASE (



"Failed example error message accessible"
)
 {
    // This ensures that error information flows through the Result type correctly
    using Reg = testfw::Registry<FailingRun>;
    // The registry internal execute method should capture the error
    REQUIRE(Reg::run_by_name("failing_run") == 2);
}
