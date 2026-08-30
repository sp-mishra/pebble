#pragma once
// ============================================================================
// kalpa/introspect/telemetry.hpp — per-iteration telemetry sinks + Diagnosis
// ============================================================================
// The Solver calls telem_.record(state) once per iteration. NoTelemetry (in
// solver.hpp) is the zero-overhead default. The sinks here add observation:
//
//   FullTrace<T>   — keep every IterState row in memory (post-hoc analysis).
//   Callback<T,Fn> — invoke a user functor each iteration (custom monitor,
//                    early-stop hook, live plotting).
//   ProgressBar<T> — terse TTY line each iteration.
//   NadiSink<Sink> — bridge each iteration into a utils::nadi PulseScope,
//                    carrying f / ‖g‖ / step / α as nadi Fields. The nadi Sink
//                    template arg selects the backend; utils::nadi::NoSink
//                    compiles the emission away (mirrors kalpa::NoTelemetry).
//
// All model kalpa::TelemetrySink (a `record(state)->void` method). They are
// stored [[no_unique_address]] in the Solver, so an empty sink costs 0 bytes.
//
// Diagnosis (defined in solver.hpp) is enriched here with a formatting catalog
// mapping each Cause to a human-readable remediation hint.
// ============================================================================

#ifndef PEBBLE_KALPA_INTROSPECT_TELEMETRY_HPP
#define PEBBLE_KALPA_INTROSPECT_TELEMETRY_HPP

#include <kalpa/core/solver.hpp>
#include <observability/nadi.hpp>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>
#include <limits>

namespace kalpa {

    // =======================================================================
    // FullTrace — records every iteration for post-hoc inspection.
    // =======================================================================
    template<typename T>
    struct FullTrace {
        struct Row { T f, grad_norm, step, alpha; std::size_t iter; };
        std::vector<Row> rows;

        template<typename State>
        void record(const State& s) {
            rows.push_back(Row{s.f, s.grad_norm, s.step, s.alpha, s.iter});
        }
        [[nodiscard]] std::size_t size() const noexcept { return rows.size(); }
        [[nodiscard]] const Row& back() const { return rows.back(); }
    };

    // =======================================================================
    // Callback — user functor fired each iteration. The functor may inspect
    // the state (and, if it captures a flag, request an early stop through the
    // Stop policy). Fn signature: void(const State&).
    // =======================================================================
    template<typename Fn>
    struct Callback {
        Fn fn;
        explicit Callback(Fn f) : fn(std::move(f)) {}
        template<typename State>
        void record(const State& s) { fn(s); }
    };
    template<typename Fn>
    [[nodiscard]] Callback<Fn> on_iteration(Fn&& fn) { return Callback<Fn>{std::forward<Fn>(fn)}; }

    // =======================================================================
    // ProgressBar — one terse TTY line per iteration.
    // =======================================================================
    template<typename T>
    struct ProgressBar {
        std::FILE* out{stderr};
        template<typename State>
        void record(const State& s) const {
            std::fprintf(out, "\r[kalpa] it=%-5zu  f=%- .6e  |g|=%- .3e  a=%- .3e",
                         s.iter, static_cast<double>(s.f),
                         static_cast<double>(s.grad_norm), static_cast<double>(s.alpha));
            std::fflush(out);
        }
    };

    // =======================================================================
    // NadiSink — emit each iteration as a utils::nadi pulse. The nadi Sink
    // policy is the backend; utils::nadi::NoSink zeroes the cost. Category is
    // fixed "kalpa.iter"; Fields carry the scalar metrics.
    // =======================================================================
    template<typename NadiBackend = utils::nadi::NoSink>
    struct NadiSink {
        template<typename State>
        void record(const State& s) const {
            using namespace utils::nadi;
            PulseScope<NadiBackend, "kalpa.iter",
                       Field<"f",     double>,
                       Field<"g_norm", double>,
                       Field<"step",  double>,
                       Field<"alpha", double>>
                scope{ Field<"f",     double>{ static_cast<double>(s.f) },
                       Field<"g_norm", double>{ static_cast<double>(s.grad_norm) },
                       Field<"step",  double>{ static_cast<double>(s.step) },
                       Field<"alpha", double>{ static_cast<double>(s.alpha) } };
            // scope emits Begin now + End at destruction — one pulse per iter.
        }
    };

    // =======================================================================
    // SparseTrace — records only selected iterations to cap telemetry memory.
    // Records iter 0 always, then every `stride` iter and/or when objective
    // drops by at least `min_rel_drop` relative to the previous recorded row.
    // =======================================================================
    template<typename T>
    struct SparseTrace {
        struct Row { T f, grad_norm, step, alpha; std::size_t iter; };
        std::vector<Row> rows;
        std::size_t stride{10};
        T min_rel_drop{static_cast<T>(0)};
        T last_recorded_f{std::numeric_limits<T>::infinity()};

        template<typename State>
        void record(const State& s) {
            const bool first = rows.empty();
            const bool stride_hit = (stride > 0) && ((s.iter % stride) == 0);
            const bool significant_drop =
                std::isfinite(last_recorded_f) && last_recorded_f != T{0} &&
                ((last_recorded_f - s.f) / std::abs(last_recorded_f) >= min_rel_drop);
            if (!(first || stride_hit || significant_drop)) return;
            rows.push_back(Row{s.f, s.grad_norm, s.step, s.alpha, s.iter});
            last_recorded_f = s.f;
        }

        [[nodiscard]] std::size_t size() const noexcept { return rows.size(); }
        [[nodiscard]] const Row& back() const { return rows.back(); }
    };

    // =======================================================================
    // Diagnosis catalog — Cause → remediation hint. Enriches the Diagnosis
    // built by the Solver with actionable guidance.
    // =======================================================================
    [[nodiscard]] inline const char* remediation(Cause c) noexcept {
        switch (c) {
        case Cause::Infeasible:
            return "starting point/iterate cannot be made feasible — relax constraints or supply a feasible x0";
        case Cause::Unbounded:
            return "objective decreases without bound — add box/norm bounds or check the model sign";
        case Cause::SingularKKT:
            return "KKT/Newton system singular — constraints may be redundant or Hessian rank-deficient; try trust-region";
        case Cause::LineSearchFail:
            return "no step satisfied the descent test — direction may be non-descent; check gradient or loosen Wolfe c2";
        case Cause::NaNTrap:
            return "NaN/Inf in f or gradient — check domain (log/sqrt of negatives) and scale the variables";
        case Cause::NumericalError:
        default:
            return "delegated kernel reported a numerical failure — inspect conditioning / iteration budget";
        }
    }

    [[nodiscard]] inline std::string explain(const Diagnosis& d) {
        return "kalpa: " + d.message + " [iter " + std::to_string(d.iteration) + "]\n  hint: " + remediation(d.cause);
    }

} // namespace kalpa

#endif // PEBBLE_KALPA_INTROSPECT_TELEMETRY_HPP
