#pragma once

#include "LiteGraphAlgorithms.hpp"
#include "observability/nadi.hpp"

#include <optional>
#include <string_view>
#include <tuple>

namespace litegraph::observability {

    /**
     * @brief Zero-overhead lifecycle observer bridging LiteGraph algorithm phases with Nadi.
     *
     * Incurs minimal cycles (TSC clock) only at macro phase boundaries (start/end/iteration),
     * completely decoupled from fine-grained inner loops.
     *
     * @tparam Sink The Nadi sink policy (e.g. ChromeTraceSink, RingBufferSink, NoSink).
     * @tparam Clock The ClockPolicy to use (default: TscCycleClockPolicy for 3-5 cycle overhead).
     */
    template <
        utils::nadi::SinkPolicy Sink = utils::nadi::NoSink,
        typename Clock = utils::nadi::TscCycleClockPolicy>
    class NadiGraphObserver {
    public:
        NadiGraphObserver() = default;
        ~NadiGraphObserver() = default;

        NadiGraphObserver(const NadiGraphObserver&) = delete;
        NadiGraphObserver& operator=(const NadiGraphObserver&) = delete;

        NadiGraphObserver(NadiGraphObserver&&) noexcept = default;
        NadiGraphObserver& operator=(NadiGraphObserver&&) noexcept = default;

        void on_phase_start(std::string_view /*phase*/) noexcept {
            scope_.emplace();
        }

        void on_phase_end(std::string_view /*phase*/) noexcept {
            scope_.reset();
        }

        void on_iteration(std::size_t iter) noexcept {
            if constexpr (Sink::enabled) {
                using IterPulse = utils::nadi::Pulse<"graph_algo_iter", utils::nadi::Field<"iteration", std::size_t>>;
                utils::nadi::route_pulse<Sink>(IterPulse{
                    .id = utils::nadi::generate_event_id(),
                    .phase = utils::nadi::PulsePhase::Instant,
                    .timestamp_ns = Clock::now(),
                    .trace_id = utils::nadi::detail::current_lineage.root_id.value,
                    .parent_id = utils::nadi::detail::current_lineage.trace_id.value,
                    .payload = std::make_tuple(utils::nadi::Field<"iteration", std::size_t>{.value = iter})
                });
            }
        }

    private:
        std::optional<utils::nadi::BasicPulseScope<Clock, Sink, "graph_algo_phase">> scope_;
    };

} // namespace litegraph::observability
