#pragma once

// Explicit Lithe interop.  Pravaha's core and compute headers never include
// this file: applications opt in only when scheduling Lithe runtime values.

#include "pravaha/detail/availability.hpp"
#include "pravaha/pravaha.hpp"

#if PEBBLE_PRAVAHA_DETAIL_HAS_LITHE_RUNTIME
#include "edsl/lithe_runtime.hpp"

#include <concepts>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pravaha::adapters::lithe_runtime {
    inline constexpr bool available = true;
    using runtime_value = ::lithe::runtime::mop::runtime_value;

    struct budget_policy {
        ::lithe::runtime::ExecutionSandbox sandbox{};

        void reset(std::int64_t fuel, std::size_t max_memory = 0) noexcept {
            sandbox.fuel_counter = fuel;
            sandbox.max_memory = max_memory;
        }

        [[nodiscard]] bool fuel_exhausted() const noexcept {
            return sandbox.fuel_counter <= 0;
        }
    };

    static_assert(BudgetPolicy<budget_policy>);

    template <::lithe::runtime::safepoint::GarbageCollector GC>
    struct gc_observer {
        static constexpr bool enabled = true;

        GC* collector = nullptr;
        ::lithe::runtime::safepoint::stack_map_table* stack_maps = nullptr;

        void on_task_event(const TaskEvent& event) noexcept {
            if (collector == nullptr || stack_maps == nullptr) return;
            if (event.kind == EventKind::TaskCompleted || event.kind == EventKind::TaskFailed) {
                ::lithe::runtime::safepoint::trigger_safepoint(event.task_name, *stack_maps, *collector);
            }
        }

        void on_join_event(const JoinEvent&) noexcept {}
        void on_graph_event(const GraphEvent&) noexcept {}
    };

    template <std::size_t Arity>
    [[nodiscard]] inline std::int64_t call_native(void* address, const std::int64_t* arguments) {
        return [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            using function_type = std::int64_t (*)(decltype(arguments[Index])...);
            return reinterpret_cast<function_type>(address)(arguments[Index]...);
        }(std::make_index_sequence<Arity>{});
    }

    [[nodiscard]] inline auto make_ffi_task(
        ::lithe::runtime::ffi::native_proxy proxy,
        std::span<const runtime_value> arguments,
        std::string_view name = "ffi_task") {
        if (!proxy.valid() || arguments.size() != proxy.arity || proxy.arity > 8) {
            return task(std::string{name}, std::function<Outcome<runtime_value>()>{
                [proxy, count = arguments.size()]() -> Outcome<runtime_value> {
                const auto message = !proxy.valid() ? "invalid native_proxy"
                    : proxy.arity > 8 ? "arity > 8"
                    : "arity mismatch: expected " + std::to_string(proxy.arity) + " got " + std::to_string(count);
                return std::unexpected(PravahaError{ErrorKind::InvalidArgument, message});
                }});
        }

        std::vector<runtime_value> captured{arguments.begin(), arguments.end()};
        return task(std::string{name}, std::function<Outcome<runtime_value>()>{
            [proxy, captured = std::move(captured)]() mutable -> Outcome<runtime_value> {
            std::int64_t native_arguments[8]{};
            for (std::uint8_t index = 0; index < proxy.arity; ++index) {
                native_arguments[index] = ::lithe::runtime::ffi::marshal_to_native(captured[index]);
            }
            std::int64_t result{};
            switch (proxy.arity) {
            case 0: result = call_native<0>(proxy.fn_ptr, native_arguments); break;
            case 1: result = call_native<1>(proxy.fn_ptr, native_arguments); break;
            case 2: result = call_native<2>(proxy.fn_ptr, native_arguments); break;
            case 3: result = call_native<3>(proxy.fn_ptr, native_arguments); break;
            case 4: result = call_native<4>(proxy.fn_ptr, native_arguments); break;
            case 5: result = call_native<5>(proxy.fn_ptr, native_arguments); break;
            case 6: result = call_native<6>(proxy.fn_ptr, native_arguments); break;
            case 7: result = call_native<7>(proxy.fn_ptr, native_arguments); break;
            case 8: result = call_native<8>(proxy.fn_ptr, native_arguments); break;
            default: std::unreachable();
            }
            return ::lithe::runtime::ffi::unmarshal_from_native(result, proxy.ret_type);
            }});
    }
} // namespace pravaha::adapters::lithe_runtime
#else
namespace pravaha::adapters::lithe_runtime {
    inline constexpr bool available = false;
} // namespace pravaha::adapters::lithe_runtime
#endif
