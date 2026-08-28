#pragma once

#include "observability/nadi.hpp"
#include "utils/profiler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace utils::nadi {
    // ---------------------------------------------------------------------------
    // ThreadLocalProfilerSink<TargetCategory, MaxSamples>
    //
    // Bridges Nadi's real-time zero-allocation pulse stream into the offline
    // statistical profiler (utils/profiler.hpp).
    //
    // Hot path (emit):
    //   - Compile-time category filter: pulses whose category != TargetCategory
    //     are discarded as a constexpr no-op.
    //   - Begin pulses push (id, timestamp_ns) onto a thread_local pending stack.
    //   - End pulses search the stack for the matching id, compute the duration,
    //     and store it in a lock-free pre-allocated array.
    //   - DropNewest flow control: samples beyond MaxSamples are silently dropped.
    //     claimed_count_ is unbounded; stored = min(claimed, MaxSamples),
    //     dropped = max(0, claimed - MaxSamples). No saturation race.
    //   - No heap allocation, no locking, no syscall on the critical path.
    //
    // Thread-safety constraint:
    //   A Begin pulse and its matching End pulse MUST be emitted on the same thread.
    //   If a scope migrates across threads (e.g. coroutines, task-stealing schedulers),
    //   the thread_local pending_stack_ on the End-thread will not contain the Begin
    //   entry, and the sample will be silently lost. For cross-thread spans, use a
    //   global EventId-keyed map instead. See GlobalProfilerSink (not yet implemented).
    //
    // Offline path (build_result / reset):
    //   - Call build_result() after the benchmark to obtain a profiler::ProfileResult
    //     with all statistical methods (median, percentiles, Mann-Whitney, etc.).
    //   - Call reset() between benchmark runs.
    // ---------------------------------------------------------------------------

    template <FixedString TargetCategory, std::size_t MaxSamples = 100'000>
    struct ThreadLocalProfilerSink {
        static constexpr bool enabled = true;
        static constexpr DropNewest flow_control = {};

        static void emit(const auto& pulse) noexcept {
            using PulseType = std::remove_cvref_t<decltype(pulse)>;

            // Compile-time category filter — zero cost for non-matching pulse types.
            if constexpr (PulseType::category.view() == TargetCategory.view()) {
                if (pulse.phase == PulsePhase::Begin) {
                    if (pending_count_ < MaxNestingDepth)
                        pending_stack_[pending_count_++] = {pulse.id.value, pulse.timestamp_ns};
                }
                else if (pulse.phase == PulsePhase::End) {
                    // Search backwards so the deepest nested scope matches first.
                    for (auto i = static_cast<std::ptrdiff_t>(pending_count_) - 1; i >= 0; --i) {
                        if (pending_stack_[i].id != pulse.id.value) continue;

                        const std::uint64_t duration =
                            pulse.timestamp_ns - pending_stack_[i].start_ns;

                        // Swap-and-pop: O(1) removal without shifting.
                        pending_stack_[i] = pending_stack_[--pending_count_];

                        // Claim a slot lock-free. claimed_count_ is unbounded so
                        // there is no saturation race; stored vs dropped is derived
                        // at read time in build_result().
                        const std::size_t idx =
                            claimed_count_.fetch_add(1, std::memory_order_relaxed);
                        if (idx < MaxSamples)
                            durations_ns_[idx] = duration;
                        break;
                    }
                }
            }
        }

        // -----------------------------------------------------------------------
        // Offline Analysis API — call outside the critical path
        // -----------------------------------------------------------------------

        // Build a fully-populated ProfileResult from the collected samples.
        // Heap allocations inside individual_runs are intentional: this is the
        // offline analysis step, not the real-time path.
        [[nodiscard]] static profiler::ProfileResult
        build_result(const std::string_view label = "") {
            profiler::ProfileResult res;
            res.label = label.empty()
                            ? std::string(TargetCategory.view())
                            : std::string(label);

            const std::size_t claimed = claimed_count_.load(std::memory_order_relaxed);
            const std::size_t count = std::min(claimed, MaxSamples);

            res.iterations_attempted = claimed; // includes dropped
            res.iterations_succeeded = count;
            res.individual_runs.reserve(count);

            std::uint64_t total = 0;
            std::uint64_t min_d = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t max_d = 0;

            for (std::size_t i = 0; i < count; ++i) {
                const std::uint64_t d = durations_ns_[i];
                res.individual_runs.emplace_back(std::chrono::nanoseconds(d));
                total += d;
                if (d < min_d) min_d = d;
                if (d > max_d) max_d = d;
            }

            if (count > 0) {
                res.total_duration = std::chrono::nanoseconds(total);
                res.average_duration = std::chrono::nanoseconds(total / count);
                res.min_duration = std::chrono::nanoseconds(min_d);
                res.max_duration = std::chrono::nanoseconds(max_d);
            }

            // Run profiler.hpp's outlier trimming (requires >= 20 samples).
            profiler::internal::trim_vector(res.individual_runs, 0.0, res.outlier_info);

            return res;
        }

        // Reset for a new benchmark run. Safe to call between runs; not thread-safe
        // with concurrent emitters — drain all writers before resetting.
        static void reset() noexcept {
            claimed_count_.store(0, std::memory_order_relaxed);
            pending_count_ = 0;
        }

    private:
        struct PendingPulse {
            std::uint64_t id{};
            std::uint64_t start_ns{};
        };

        static constexpr std::size_t MaxNestingDepth = 256;

        // Thread-local stack: matches Begin/End pulses per-thread without locking.
        inline static thread_local std::array<PendingPulse, MaxNestingDepth> pending_stack_{};
        inline static thread_local std::size_t pending_count_ = 0;

        // Lock-free global duration store.
        // claimed_count_ is unbounded — no saturation race. Samples beyond MaxSamples
        // are simply not written; build_result() derives dropped count from the difference.
        inline static std::array<std::uint64_t, MaxSamples> durations_ns_{};
        inline static std::atomic<std::size_t> claimed_count_{0};
    };

    // Convenience alias: ProfilerSink<Cat, N> = ThreadLocalProfilerSink<Cat, N>.
    template <FixedString Category, std::size_t MaxSamples = 100'000>
    using ProfilerSink = ThreadLocalProfilerSink<Category, MaxSamples>;
} // namespace utils::nadi
