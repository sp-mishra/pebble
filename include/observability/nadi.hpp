#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <source_location>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace utils::nadi {
    // ---------------------------------------------------------------------------
    // 1. Temporal Phases
    // ---------------------------------------------------------------------------

    enum class PulsePhase : std::uint8_t {
        Begin,
        End,
        Instant,
        Duration,
        Error,
    };

    // ---------------------------------------------------------------------------
    // 2. Compile-Time String Literal (NTTP)
    // ---------------------------------------------------------------------------

    template <std::size_t N>
    struct FixedString {
        std::array<char, N> data{};

        consteval FixedString(const char (&src)[N]) noexcept {
            std::copy(src, src + N, data.data());
        }

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            // N includes the null terminator; exclude it from the view.
            return {data.data(), N - 1};
        }

        [[nodiscard]] constexpr bool operator==(const FixedString&) const noexcept = default;
    };

    // Deduction guide: char[N] → FixedString<N>
    template <std::size_t N>
    FixedString(const char (&)[N]) -> FixedString<N>;

    // ---------------------------------------------------------------------------
    // 3. Typed Payload Channels
    // ---------------------------------------------------------------------------

    template <FixedString Name, typename T>
    struct Field {
        static constexpr auto name = Name;
        T value{};
    };

    struct EventId {
        std::uint64_t value{};
    };

    template <FixedString Category, typename... Fields>
    struct Pulse {
        static constexpr auto category = Category;

        EventId id{};
        PulsePhase phase{};
        std::uint64_t timestamp_ns{};
        // Lineage captured at construction time — not read from thread-local state
        // inside sinks, so async/coroutine migration cannot corrupt it.
        std::uint64_t trace_id{}; // root scope id for the current trace tree
        std::uint64_t parent_id{}; // immediate parent scope id (0 = root)
        std::tuple<Fields...> payload{};
    };

    // ---------------------------------------------------------------------------
    // 4. Flow Control Semantics
    // ---------------------------------------------------------------------------

    struct DropNewest {};

    struct OverwriteOldest {};

    struct Lossless {};

    // ---------------------------------------------------------------------------
    // 5. SinkPolicy Concept
    // ---------------------------------------------------------------------------

    // SinkPolicy — checks only the static control interface (enabled + flow_control).
    // Deliberately does NOT check emit() here: emit takes a templated pulse type so
    // there is no single probe pulse that works for all valid sinks. Use SinkFor<S,P>
    // to verify that a specific sink accepts a specific pulse type.
    template <typename T>
    concept SinkPolicy =
        // T::enabled must be a compile-time bool constant
        requires { { T::enabled } -> std::convertible_to<const bool&>; } &&
        // T::flow_control must be one of the recognised tag types (strip cv so
        // constexpr members like "static constexpr DropNewest flow_control{}"
        // whose decltype is "const DropNewest" still match)
        (std::same_as<std::remove_cv_t<decltype(T::flow_control)>, DropNewest> ||
            std::same_as<std::remove_cv_t<decltype(T::flow_control)>, OverwriteOldest> ||
            std::same_as<std::remove_cv_t<decltype(T::flow_control)>, Lossless>);

    // SinkFor<Sink, PulseType> — validates that a specific Sink can actually
    // receive a specific PulseType. Used inside route_pulse to give clear
    // compile errors when a sink's emit rejects a pulse type.
    template <typename T, typename PulseType>
    concept SinkFor = SinkPolicy<T> &&
        requires(const PulseType& p) {
            { T::emit(p) } noexcept;
        };

    // ---------------------------------------------------------------------------
    // 6. Core Sinks
    // ---------------------------------------------------------------------------

    struct NoSink {
        static constexpr bool enabled = false;
        static constexpr DropNewest flow_control = {};

        static constexpr void emit(const auto& /*pulse*/) noexcept {}
    };

    template <SinkPolicy... Sinks>
    struct MultiSink {
        static constexpr bool enabled = (Sinks::enabled || ...);

        static constexpr DropNewest flow_control = {};

        static constexpr void emit(const auto& pulse) noexcept {
            ([&pulse]() noexcept {
                if constexpr (Sinks::enabled)
                    Sinks::emit(pulse);
            }(), ...);
        }
    };

    // ---------------------------------------------------------------------------
    // 7. Execution Lineage
    // ---------------------------------------------------------------------------

    struct LineageToken {
        EventId trace_id{}; // current scope's own id (0 = no active scope)
        EventId parent_id{}; // parent scope's own id (0 = root scope)
        EventId root_id{}; // root scope id for the entire trace tree
    };

    namespace detail {
        inline thread_local LineageToken current_lineage{};
    } // namespace detail

    [[nodiscard]] inline LineageToken capture_lineage() noexcept {
        return detail::current_lineage;
    }

    inline void restore_lineage(LineageToken token) noexcept {
        detail::current_lineage = token;
    }

    struct ScopedLineage {
        explicit ScopedLineage(LineageToken token) noexcept
            : previous_{detail::current_lineage} {
            detail::current_lineage = token;
        }

        ~ScopedLineage() noexcept { detail::current_lineage = previous_; }

        ScopedLineage(const ScopedLineage&) = delete;

        ScopedLineage& operator=(const ScopedLineage&) = delete;

        ScopedLineage(ScopedLineage&&) = delete;

        ScopedLineage& operator=(ScopedLineage&&) = delete;

    private:
        LineageToken previous_;
    };

    // ---------------------------------------------------------------------------
    // 8. Ingress Plane — Clock Policies, Static Router, Utilities, PulseScope
    // ---------------------------------------------------------------------------

    // 8a. Clock Policies --------------------------------------------------------
    //
    // Configures the timestamp source used by now_ns() and PulseScope.
    // Pass as the ClockPolicy template parameter of PulseScope.

    struct SteadyClockPolicy {
        static constexpr bool is_wall_time = true;

        [[nodiscard]] static std::uint64_t now() noexcept {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
        }
    };

    // TscCycleClockPolicy — reads the CPU timestamp counter (rdtsc / equivalent).
    // Returns CPU *cycles*, NOT nanoseconds. is_wall_time = false signals this to
    // sinks; a calibration step is needed to convert cycles to wall time.
    // Falls back to SteadyClockPolicy on non-x86 targets (returns nanoseconds there).
    struct TscCycleClockPolicy {
        static constexpr bool is_wall_time = false;

        [[nodiscard]] static std::uint64_t now() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
            return __builtin_ia32_rdtsc(); // cycles, not nanoseconds
#else
            return SteadyClockPolicy::now();
#endif
        }
    };

    // Default free function — uses SteadyClockPolicy.
    [[nodiscard]] inline std::uint64_t now_ns() noexcept {
        return SteadyClockPolicy::now();
    }

    // 8b. ID Utility -----------------------------------------------------------
    // Note: If contention observed under high concurrency, switch to per-thread
    // counter pool with merge at read time.

    [[nodiscard]] inline EventId generate_event_id() noexcept {
        static std::atomic<std::uint64_t> counter{1};
        return EventId{counter.fetch_add(1, std::memory_order_relaxed)};
    }

    // 8c. Static Router --------------------------------------------------------
    //
    // Uses SinkFor to validate that Sink can receive PulseType, giving a clear
    // compile error if the sink's emit rejects the pulse type.

    template <SinkPolicy Sink, typename PulseType>
    constexpr void route_pulse(const PulseType& pulse) noexcept {
        if constexpr (Sink::enabled) {
            static_assert(SinkFor<Sink, PulseType>,
                          "Sink::emit does not accept this PulseType");
            Sink::emit(pulse);
        }
    }

    // 8d. Source location opt-in tag -------------------------------------------
    //
    // Add Field<"__loc__", SourceLocation> to your Fields pack to capture the
    // call site. Omitting it keeps PulseScope payload-minimal by default.

    struct SourceLocation {
        const char* file{};
        std::uint32_t line{};
    };

    // 8e. PulseScope -----------------------------------------------------------
    //
    // Design:
    //  - ClockPolicy is the first template parameter (default SteadyClockPolicy
    //    via the PulseScope alias below).
    //  - source_location is opt-in via Field<"__loc__", SourceLocation>.
    //  - End pulse is a bare Pulse<Category> — no payload duplication.
    //  - Lineage semantics:
    //      trace_id  = id of the root scope of the current trace tree.
    //                  Inherited from parent; if there is no parent, own id.
    //      parent_id = id of the immediate enclosing scope (0 = root scope).
    //  - Both trace_id and parent_id are embedded directly in the Pulse at
    //    construction time, so sinks never need to read thread-local state.
    //
    // Use PulseScope<Sink, Category, Fields...> for the default SteadyClockPolicy.
    // Use BasicPulseScope<ClockPolicy, Sink, Category, Fields...> to supply a
    // custom clock (e.g. TscCycleClockPolicy for cycle-count profiling).

    template <typename ClockPolicy, SinkPolicy Sink, FixedString Category, typename... Fields>
    struct BasicPulseScope {
        using BeginPulse = Pulse<Category, Fields...>;
        using EndPulse = Pulse<Category>;

        // source_location captured at constructor call site; wrapping disables location capture.
        explicit BasicPulseScope (
            Fields
        ...
        fields
        ,
        std::source_location loc = std::source_location::current()
        )
        noexcept
        :
        previous_lineage_ { detail::current_lineage }
        ,
        id_ { generate_event_id() }
        ,
        payload_ { fill_loc(std::move(fields)..., loc) }
 {
            // LineageToken semantics (thread-local):
            //   trace_id  = this scope's own id (children will read this as their parent_id)
            //   parent_id = the enclosing scope's own id (0 = we are root)
            //   root_id   = id of the root scope in this trace tree (propagated downward)
            //
            // Pulse fields:
            //   trace_id  = root scope id of the trace tree
            //   parent_id = immediate parent scope id (0 = we are root)
            const std::uint64_t prev_trace = previous_lineage_.trace_id.value;
            const bool is_root = (prev_trace == 0);
            const std::uint64_t root_trace_id =
                    is_root ? id_.value : previous_lineage_.root_id.value;
            const std::uint64_t parent_scope_id = prev_trace;

            detail::current_lineage = LineageToken{
                id_, // own id
                EventId{parent_scope_id}, // parent's id (0 for root)
                EventId{root_trace_id}, // propagated root id
            };

            trace_id_ = root_trace_id;
            parent_id_ = parent_scope_id;

            route_pulse<Sink>(BeginPulse{
                .id = id_,
                .phase = PulsePhase::Begin,
                .timestamp_ns = ClockPolicy::now(),
                .trace_id = root_trace_id,
                .parent_id = parent_scope_id,
                .payload = payload_,
            });
        }

        ~BasicPulseScope() noexcept {
            route_pulse<Sink>(EndPulse{
                .id = id_,
                .phase = PulsePhase::End,
                .timestamp_ns = ClockPolicy::now(),
                .trace_id = trace_id_,
                .parent_id = parent_id_,
            });
            detail::current_lineage = previous_lineage_;
        }

        BasicPulseScope(const BasicPulseScope&) = delete;

        BasicPulseScope& operator=(const BasicPulseScope&) = delete;

        BasicPulseScope(BasicPulseScope&&) = delete;

        BasicPulseScope& operator=(BasicPulseScope&&) = delete;

    private:
        static std::tuple<Fields...> fill_loc(
            Fields... fields,
            const std::source_location& loc
        ) noexcept {
            // ABI stability: payload layout must match sum of field sizes.
            if constexpr (sizeof...(Fields) > 0) {
                constexpr std::size_t field_sizes = (sizeof(Fields::value) + ...);
                static_assert(field_sizes <= sizeof(std::tuple < Fields
                ...
                >
                )
                ,
                "Tuple layout assumption violated"
                )
                ;
            }
            auto fill_one = [&loc](auto& f) noexcept {
                using VT = decltype(f.value);
                if constexpr (std::is_same_v<std::remove_cv_t<VT>, SourceLocation>) {
                    static_assert(std::is_trivially_copyable_v<SourceLocation> &&
                        std::is_trivially_default_constructible_v<SourceLocation>);
                    f.value = SourceLocation{
                        loc.file_name(),
                        (loc.line())
                    };
                }
            };
            (fill_one(fields), ...);
            return {std::move(fields)...};
        }

        LineageToken previous_lineage_;
        EventId id_;
        std::uint64_t trace_id_;
        std::uint64_t parent_id_;
        std::tuple<Fields...> payload_;
    };

    // PulseScope — convenience alias using SteadyClockPolicy.
    // PulseScope<Sink, Category, Fields...> is equivalent to
    // BasicPulseScope<SteadyClockPolicy, Sink, Category, Fields...>.
    template <SinkPolicy Sink, FixedString Category, typename... Fields>
    using PulseScope = BasicPulseScope<SteadyClockPolicy, Sink, Category, Fields...>;
} // namespace utils::nadi
