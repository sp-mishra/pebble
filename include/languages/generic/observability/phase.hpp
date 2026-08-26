#pragma once

// Shared, policy-selected phase telemetry for language frontends and engines.
// Disabled policies produce an empty scope with no clock, atomic, TLS, or sink work.

#include "observability/nadi.hpp"

#include <concepts>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace lang::telemetry {
    enum class phase : std::uint8_t {
        lex,
        parse,
        syntax_build,
        resolve,
        semantic_check,
        generic_ir_lower,
        verify,
        optimize,
        physical_lower,
        artifact,
        backend_compile,
        backend_install,
        execute,
    };

    enum class phase_outcome : std::uint8_t { success, rejected, failed, fallback };

    struct phase_context {
        std::uint64_t unit_id = 0;
        phase stage = phase::parse;
        std::uint32_t entity_count = 0;
    };

    struct phase_metric {
        phase_context context{};
        phase_outcome outcome = phase_outcome::success;
        std::uint64_t elapsed = 0;
        std::uint64_t cycles = 0;
        std::uint32_t iterations = 0;
        std::uint32_t transformations = 0;
    };

    static_assert(std::is_trivially_copyable_v<phase_context>);
    static_assert(std::is_trivially_copyable_v<phase_metric>);

    template <class T>
    concept feedback_sink = requires(const phase_metric& metric) {
        { T::enabled } -> std::convertible_to<const bool&>;
        { T::submit(metric) } noexcept -> std::same_as<void>;
    };

    struct no_feedback {
        static constexpr bool enabled = false;
        static constexpr void submit(const phase_metric&) noexcept {}
    };

    template <utils::nadi::SinkPolicy Sink>
    struct nadi_feedback {
        static constexpr bool enabled = Sink::enabled;

        static void submit(const phase_metric& metric) noexcept {
            if constexpr (Sink::enabled) {
                using stage_field = utils::nadi::Field<"phase", std::uint8_t>;
                using outcome_field = utils::nadi::Field<"outcome", std::uint8_t>;
                using elapsed_field = utils::nadi::Field<"elapsed", std::uint64_t>;
                using metric_pulse = utils::nadi::Pulse<"language.phase.feedback",
                                                        stage_field, outcome_field, elapsed_field>;
                metric_pulse pulse;
                pulse.id = utils::nadi::generate_event_id();
                pulse.phase = utils::nadi::PulsePhase::Instant;
                pulse.timestamp_ns = utils::nadi::now_ns();
                const auto lineage = utils::nadi::capture_lineage();
                pulse.trace_id = lineage.root_id.value;
                pulse.parent_id = lineage.trace_id.value;
                std::get<stage_field>(pulse.payload).value =
                    static_cast<std::uint8_t>(metric.context.stage);
                std::get<outcome_field>(pulse.payload).value =
                    static_cast<std::uint8_t>(metric.outcome);
                std::get<elapsed_field>(pulse.payload).value = metric.elapsed;
                utils::nadi::route_pulse<Sink>(pulse);
            }
        }
    };

    template <utils::nadi::SinkPolicy NadiSink = utils::nadi::NoSink,
              feedback_sink Feedback = no_feedback,
              class Clock = utils::nadi::SteadyClockPolicy>
    struct phase_observer {
        using nadi_sink = NadiSink;
        using feedback = Feedback;
        using clock = Clock;
        static constexpr bool enabled = NadiSink::enabled || Feedback::enabled;
    };

    template <class Observer, bool Enabled = Observer::enabled>
    class basic_phase_scope;

    template <class Observer>
    class basic_phase_scope<Observer, false> {
    public:
        constexpr explicit basic_phase_scope(const phase_context&) noexcept {}
        constexpr void set_outcome(phase_outcome) noexcept {}
        constexpr void set_iterations(std::uint32_t) noexcept {}
        constexpr void set_transformations(std::uint32_t) noexcept {}
    };

    template <class Observer>
    class basic_phase_scope<Observer, true> {
        using stage_field = utils::nadi::Field<"phase", std::uint8_t>;
        using unit_field = utils::nadi::Field<"unit", std::uint64_t>;
        using trace_scope = utils::nadi::PulseScope<typename Observer::nadi_sink,
                                                     "language.phase", stage_field, unit_field>;
        using elapsed_field = utils::nadi::Field<"elapsed", std::uint64_t>;
        using outcome_field = utils::nadi::Field<"outcome", std::uint8_t>;
        using entities_field = utils::nadi::Field<"entities", std::uint32_t>;
        using metric_pulse = utils::nadi::Pulse<"language.phase.metric",
                                                stage_field, unit_field, elapsed_field,
                                                outcome_field, entities_field>;

    public:
        explicit basic_phase_scope(const phase_context& context) noexcept
            : context_(context),
              started_(Observer::clock::now()),
              trace_(stage_field{static_cast<std::uint8_t>(context.stage)}, unit_field{context.unit_id}) {}

        ~basic_phase_scope() noexcept {
            const auto ended = Observer::clock::now();
            const phase_metric metric{
                .context = context_,
                .outcome = outcome_,
                .elapsed = ended - started_,
                .iterations = iterations_,
                .transformations = transformations_,
            };
            Observer::feedback::submit(metric);

            if constexpr (Observer::nadi_sink::enabled) {
                metric_pulse pulse;
                pulse.id = utils::nadi::generate_event_id();
                pulse.phase = utils::nadi::PulsePhase::Instant;
                pulse.timestamp_ns = ended;
                const auto lineage = utils::nadi::capture_lineage();
                pulse.trace_id = lineage.root_id.value;
                pulse.parent_id = lineage.trace_id.value;
                std::get<stage_field>(pulse.payload).value = static_cast<std::uint8_t>(context_.stage);
                std::get<unit_field>(pulse.payload).value = context_.unit_id;
                std::get<elapsed_field>(pulse.payload).value = metric.elapsed;
                std::get<outcome_field>(pulse.payload).value = static_cast<std::uint8_t>(outcome_);
                std::get<entities_field>(pulse.payload).value = context_.entity_count;
                utils::nadi::route_pulse<typename Observer::nadi_sink>(pulse);
            }
        }

        void set_outcome(const phase_outcome value) noexcept { outcome_ = value; }
        void set_iterations(const std::uint32_t value) noexcept { iterations_ = value; }
        void set_transformations(const std::uint32_t value) noexcept { transformations_ = value; }

    private:
        phase_context context_;
        std::uint64_t started_ = 0;
        phase_outcome outcome_ = phase_outcome::success;
        std::uint32_t iterations_ = 0;
        std::uint32_t transformations_ = 0;
        [[no_unique_address]] trace_scope trace_;
    };

    template <class Observer = phase_observer<>>
    using phase_scope = basic_phase_scope<Observer>;
} // namespace lang::telemetry
