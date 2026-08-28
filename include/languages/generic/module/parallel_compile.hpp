#pragma once

// Opt-in Pravaha batch compilation for independent, dependency-ordered modules.
// Sequential consumers do not include this header or create worker threads.

#include "pravaha/pravaha.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace lang::module {
    struct parallel_compile_policy {
        std::size_t minimum_parallel_modules = 8;
        std::size_t maximum_tasks_per_worker = 2;
    };

    struct parallel_compile_error {
        std::string message;
    };

    template <typename Result>
    struct parallel_compile_result {
        std::vector<Result> results;
        bool used_parallelism = false;
    };

    namespace detail {
        struct parallel_compile_failure {
            std::mutex mutex;
            std::string message;
            std::atomic_bool failed = false;

            void record(std::string value) noexcept {
                std::lock_guard lock{mutex};
                if (!failed.load(std::memory_order_relaxed)) {
                    message = std::move(value);
                    failed.store(true, std::memory_order_release);
                }
            }
        };

        [[nodiscard]] inline std::size_t ceil_divide(const std::size_t value,
                                                      const std::size_t divisor) noexcept {
            return value / divisor + (value % divisor != 0 ? 1 : 0);
        }
    } // namespace detail

    // Modules must already be dependency-ordered and compilation must not mutate
    // shared frontend state. Each task writes a disjoint contiguous result range;
    // the returned vector always preserves input source order.
    template <std::ranges::sized_range Modules, typename CompileOne>
        requires std::invocable<CompileOne&, const std::ranges::range_value_t<Modules>&>
    [[nodiscard]] auto compile_modules_pravaha(
        const Modules& modules,
        CompileOne&& compile_one,
        pravaha::JThreadBackend& backend,
        const parallel_compile_policy policy = {})
        -> std::expected<parallel_compile_result<std::remove_cvref_t<std::invoke_result_t<
            CompileOne&, const std::ranges::range_value_t<Modules>&>>>, parallel_compile_error> {
        using module_type = std::ranges::range_value_t<Modules>;
        using result_type = std::remove_cvref_t<std::invoke_result_t<CompileOne&, const module_type&>>;
        static_assert(!std::is_void_v<result_type>, "module compilation must return a value");
        static_assert(std::movable<result_type>, "module compilation result must be movable");

        const auto count = static_cast<std::size_t>(std::ranges::size(modules));
        parallel_compile_result<result_type> result;
        result.results.reserve(count);
        if (count == 0) return result;

        std::vector<const module_type*> ordered_modules;
        ordered_modules.reserve(count);
        for (const auto& module : modules) ordered_modules.push_back(std::addressof(module));
        std::vector<std::optional<result_type>> staged(count);
        detail::parallel_compile_failure failure;

        const auto compile_range = [&](const std::size_t first, const std::size_t last) noexcept {
            for (std::size_t index = first; index < last; ++index) {
                if (failure.failed.load(std::memory_order_acquire)) return;
                try {
                    staged[index].emplace(std::invoke(compile_one, *ordered_modules[index]));
                }
                catch (const std::exception& error) {
                    failure.record(error.what());
                    return;
                }
                catch (...) {
                    failure.record("module compilation failed with an unknown exception");
                    return;
                }
            }
        };

        const auto workers = std::max<std::size_t>(1, backend.worker_count());
        const auto task_limit = workers * std::max<std::size_t>(1, policy.maximum_tasks_per_worker);
        const bool run_parallel = count >= policy.minimum_parallel_modules && task_limit > 1;

        if (run_parallel) {
            const auto task_count = std::min(count, task_limit);
            const auto chunk_size = detail::ceil_divide(count, task_count);
            result.used_parallelism = task_count > 1;
            for (std::size_t first = 0; first < count; first += chunk_size) {
                const auto last = std::min(count, first + chunk_size);
                if (!backend.submit(pravaha::TaskCommand::make(
                        [first, last, &compile_range]() noexcept { compile_range(first, last); },
                        "lang.module.compile"))) {
                    failure.record("Pravaha rejected module compilation task submission");
                    break;
                }
            }
            backend.drain();
        }
        else {
            compile_range(0, count);
        }

        if (failure.failed.load(std::memory_order_acquire))
            return std::unexpected(parallel_compile_error{std::move(failure.message)});
        for (auto& value : staged) {
            if (!value)
                return std::unexpected(parallel_compile_error{"module compilation produced no result"});
            result.results.push_back(std::move(*value));
        }
        return result;
    }
} // namespace lang::module
