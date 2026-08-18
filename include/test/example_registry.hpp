#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <expected>
#include <iostream>
#include <ranges>
#include <source_location>
#include <string>
#include "utils/log.hpp"
#include <span>
#include <string_view>

namespace testfw {
    // ─── Result/Error model ─────────────────────────────────────────────────────

    struct Error {
        std::string_view message;
        std::source_location where;
    };

    using Result = std::expected<void, Error>;

    [[nodiscard]] inline auto fail(
        const std::string_view message,
        const std::source_location where = std::source_location::current()
    ) -> std::unexpected<Error> {
        return std::unexpected{Error{message, where}};
    }

    // ─── Concepts ───────────────────────────────────────────────────────────────

    template <typename T>
    concept HasStaticRun = requires {
        { T::run() } -> std::convertible_to<Result>;
    };

    template <typename T>
    concept HasInstanceRun = !HasStaticRun<T> && requires(T t) {
        { t.run() } -> std::convertible_to<Result>;
    };

    template <typename T>
    concept HasStaticSetup = requires {
        { T::setup() } -> std::convertible_to<Result>;
    };

    template <typename T>
    concept HasInstanceSetup = !HasStaticSetup<T> && requires(T t) {
        { t.setup() } -> std::convertible_to<Result>;
    };

    template <typename T>
    concept HasStaticTeardown = requires {
        { T::teardown() } -> std::convertible_to<Result>;
    };

    template <typename T>
    concept HasInstanceTeardown = !HasStaticTeardown<T> && requires(T t) {
        { t.teardown() } -> std::convertible_to<Result>;
    };

    template <typename T>
    concept ExampleType = requires {
        { T::name() } -> std::convertible_to<std::string_view>;
        { T::description() } -> std::convertible_to<std::string_view>;
        { T::tags() } -> std::convertible_to<std::span<const std::string_view>>;
    } && (HasStaticRun<T> || HasInstanceRun<T>);

    // ─── Registry ───────────────────────────────────────────────────────────────

    template <ExampleType... Examples>
    class Registry {
    public:
        static int run_all() {
            int failed = 0;
            ([&] {
                if (!execute<Examples>()) {
                    ++failed;
                }
            }(), ...);
            return failed > 0 ? 2 : 0;
        }

        static int run_by_name(const std::string_view target) {
            bool passed = true;
            if (const bool found = (match_and_run<Examples>(target, passed) || ...); !found) {
                std::cerr << "Error: example '" << target << "' not found\n";
                return 1;
            }
            return passed ? 0 : 2;
        }

        static int run_by_tag(const std::string_view tag) {
            int failed = 0;
            bool found = false;
            (tag_run<Examples>(tag, failed, found), ...);
            if (!found) {
                std::cerr << "Error: no examples with tag '" << tag << "'\n";
                return 1;
            }
            return failed > 0 ? 2 : 0;
        }

        static void print_list() {
            (print_entry<Examples>(), ...);
        }

    private:
        // ─── Unified invocation with lifecycle ──────────────────────────────

        template <ExampleType T>
        static auto invoke() -> Result {
            if constexpr (HasStaticRun<T>) {
                return invoke_static<T>();
            }
            else {
                return invoke_instance<T>();
            }
        }

        template <ExampleType T>
        static auto invoke_static() -> Result {
            // Setup
            if constexpr (HasStaticSetup<T>) {
                if (Result setup_result = T::setup(); !setup_result) return setup_result;
            }

            // Run
            Result run_result = T::run();

            // Teardown
            if constexpr (HasStaticTeardown<T>) {
                Result teardown_result = T::teardown();
                if (!run_result) return run_result;
                if (!teardown_result) return teardown_result;
                return {};
            }
            else {
                return run_result;
            }
        }

        template <ExampleType T>
        static auto invoke_instance() -> Result {
            T instance{};

            // Setup
            if constexpr (HasInstanceSetup<T>) {
                if (Result setup_result = instance.setup(); !setup_result) return setup_result;
            }

            // Run
            Result run_result = instance.run();

            // Teardown
            if constexpr (HasInstanceTeardown<T>) {
                Result teardown_result = instance.teardown();
                if (!run_result) return run_result;
                if (!teardown_result) return teardown_result;
                return {};
            }
            else {
                return run_result;
            }
        }

        // ─── Execution with reporting ───────────────────────────────────────

        template <ExampleType T>
        static auto execute() -> bool {
            lg::info("[RUNNING] {}", T::name());
            if (Result result = invoke<T>()) {
                lg::info("[PASSED]  {}", T::name());
                return true;
            }
            else {
                const auto& [message, where] = result.error();
                lg::error("[FAILED]  {}: {} ({}:{})", T::name(), message, where.file_name(), where.line());
                return false;
            }
        }

        // ─── Helpers ────────────────────────────────────────────────────────

        template <ExampleType T>
        static auto match_and_run(const std::string_view target, bool& passed) -> bool {
            if (T::name() == target) {
                if (!execute<T>()) passed = false;
                return true;
            }
            return false;
        }

        template <ExampleType T>
        static auto has_tag(const std::string_view tag) -> bool {
            auto tags = T::tags();
            return std::ranges::find(tags, tag) != tags.end();
        }

        template <ExampleType T>
        static void tag_run(const std::string_view tag, int& failed, bool& found) {
            if (has_tag<T>(tag)) {
                found = true;
                if (!execute<T>()) ++failed;
            }
        }

        template <ExampleType T>
        static void print_entry() {
            const auto tags = T::tags();
            std::string tag_list;
            for (std::size_t i = 0; i < tags.size(); ++i) {
                if (i > 0) tag_list += ", ";
                tag_list += tags[i];
            }
            lg::info("  {} - {} [{}]", T::name(), T::description(), tag_list);
        }
    };
} // namespace testfw

