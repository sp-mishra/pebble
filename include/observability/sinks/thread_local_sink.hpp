#pragma once

#include "observability/nadi.hpp"

#include <functional>
#include <string_view>

namespace utils::nadi {
    // ---------------------------------------------------------------------------
    // PulseRecord — type-erased, allocation-free pulse summary forwarded to the
    // thread-local handler. Contains only the fields common to all Pulse types.
    // ---------------------------------------------------------------------------

    struct PulseRecord {
        std::string_view category{};
        std::uint64_t event_id{};
        PulsePhase phase{};
        std::uint64_t timestamp_ns{};
        std::uint64_t trace_id{};
        std::uint64_t parent_id{};
    };

    using PulseHandler = std::function<void(const PulseRecord&)>;

    namespace detail {
        inline thread_local PulseHandler tl_pulse_handler{};
    } // namespace detail

    // Install a per-thread pulse handler. Pass an empty function to uninstall.
    inline void set_pulse_handler(PulseHandler h) noexcept {
        detail::tl_pulse_handler = std::move(h);
    }

    [[nodiscard]] inline bool has_pulse_handler() noexcept {
        return static_cast<bool>(detail::tl_pulse_handler);
    }

    // ---------------------------------------------------------------------------
    // ThreadLocalSink — runtime-configurable sink via a per-thread handler.
    //
    // Install a handler with set_pulse_handler(fn) to receive all pulses emitted
    // on that thread. When no handler is installed, emit() is a cheap null check.
    // enabled=true so PulseScope emits unconditionally; the null check is the
    // only overhead on the hot path when no handler is set.
    // ---------------------------------------------------------------------------

    struct ThreadLocalSink {
        static constexpr bool enabled = true;
        static constexpr DropNewest flow_control = {};

        static void emit(const auto& pulse) noexcept {
            if (detail::tl_pulse_handler) {
                detail::tl_pulse_handler(PulseRecord{
                    pulse.category.view(),
                    pulse.id.value,
                    pulse.phase,
                    pulse.timestamp_ns,
                    pulse.trace_id,
                    pulse.parent_id,
                });
            }
        }
    };
} // namespace utils::nadi
