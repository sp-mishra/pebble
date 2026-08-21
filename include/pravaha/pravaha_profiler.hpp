#pragma once
#include "pravaha/schedulers/scheduler_policy.hpp"
#include <chrono>
#include <vector>
#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

// pravaha_profiler.hpp — Non-intrusive profiling wrapper for SchedulerPolicy types.
//
// Wraps any SchedulerPolicy and intercepts on_task_ready / on_task_complete /
// select_next_task to record timing samples without modifying the underlying
// policy.  The wrapper itself satisfies SchedulerPolicy so it can be dropped
// in anywhere a policy is expected.
//
// No virtual dispatch. Header-only C++23.

namespace pravaha::profile {
    // Import scheduling primitives into this namespace for convenience.
    using ::pravaha::sched::task_token;
    using ::pravaha::TaskPriority;

    // Forward declaration — full definition in § 4 (used by the ctor overload below).
    struct task_profile_sample;
    using profile_sink = std::function<void(const task_profile_sample&)>;

    // ============================================================================
    // § 1  task_profile_sample — one completed task's timing record
    // ============================================================================

    struct task_profile_sample {
        std::size_t id = 0;
        std::uint64_t queue_wait_ns = 0; // ready → first select
        std::uint64_t execution_ns = 0; // first select → complete
        std::uint64_t backend_dispatch_ns = 0; // reserved for backend timing overlay
        TaskPriority priority = TaskPriority::Normal;
    };

    // ============================================================================
    // § 2  execution_profile_report — aggregated report from take_report()
    // ============================================================================

    struct execution_profile_report {
        std::vector<task_profile_sample> samples;
        std::uint64_t total_wall_ns = 0;
        float cpu_utilization = 0.f; // reserved; set by runtime overlay
        float gpu_utilization = 0.f; // reserved; set by runtime overlay
        std::size_t tasks_executed = 0;
        std::size_t tasks_stolen = 0;
    };

    // ============================================================================
    // § 3  profiling_scheduler_policy<Base>
    //
    // Thread-safety model:
    //   - on_task_ready / on_task_complete / select_next_task all hold report_mutex_
    //     only for the map update (minimal critical section).
    //   - take_report() swaps the internal sample vector under the lock.
    //   - tasks_stolen_ is std::atomic, updated lock-free.
    // ============================================================================

    template <sched::SchedulerPolicy Base>
    class profiling_scheduler_policy {
    public:
        profiling_scheduler_policy()
            : start_time_{std::chrono::steady_clock::now()} {}

        /// Construct with a sink invoked per drained sample on take_report().
        explicit profiling_scheduler_policy(profile_sink sink)
            : start_time_{std::chrono::steady_clock::now()}
              , sink_{std::move(sink)} {}

        // Satisfy SchedulerPolicy ------------------------------------------------

        void on_task_ready(task_token t) {
            const auto now = std::chrono::steady_clock::now();
            {
                std::scoped_lock lk{report_mutex_};
                ready_times_[t.id] = now;
            }
            base_.on_task_ready(t);
        }

        void on_task_complete(task_token t) {
            const auto now = std::chrono::steady_clock::now();
            {
                std::scoped_lock lk{report_mutex_};
                const auto exec_it = exec_start_times_.find(t.id);
                const auto ready_it = ready_times_.find(t.id);

                task_profile_sample s;
                s.id = t.id;
                s.priority = t.priority;

                if (exec_it != exec_start_times_.end()) {
                    const auto exec_start = exec_it->second;
                    const auto exec_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            now - exec_start).count());
                    s.execution_ns = exec_ns;
                    exec_start_times_.erase(exec_it);
                }

                if (ready_it != ready_times_.end()) {
                    // queue_wait = exec_start - ready_time
                    // If exec_start was not recorded, use (now - ready) as fallback.
                    const auto ref = (s.execution_ns > 0)
                                         ? (now - std::chrono::nanoseconds{s.execution_ns})
                                         : now;
                    const auto wait_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            ref - ready_it->second).count());
                    s.queue_wait_ns = wait_ns;
                    ready_times_.erase(ready_it);
                }

                samples_.push_back(s);
            }
            base_.on_task_complete(t);
        }

        [[nodiscard]] std::optional<task_token> select_next_task(std::size_t worker_id) {
            auto token = base_.select_next_task(worker_id);
            if (token.has_value()) {
                const auto now = std::chrono::steady_clock::now();
                std::scoped_lock lk{report_mutex_};
                exec_start_times_[token->id] = now;
            }
            return token;
        }

        // Extended interface ------------------------------------------------------

        /// Atomically swap out all accumulated samples and compute summary stats.
        [[nodiscard]] execution_profile_report take_report() {
            const auto wall_end = std::chrono::steady_clock::now();
            execution_profile_report rep;
            {
                std::scoped_lock lk{report_mutex_};
                rep.samples = std::exchange(samples_, {});
            }
            rep.tasks_executed = rep.samples.size();
            rep.tasks_stolen = tasks_stolen_.exchange(0, std::memory_order_relaxed);
            rep.total_wall_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    wall_end - start_time_).count());
            // Reset the wall-clock reference for the next window.
            start_time_ = std::chrono::steady_clock::now();
            // Notify the sink outside the lock (matches the stated contract).
            if (sink_)
                for (const auto& s : rep.samples) sink_(s);
            return rep;
        }

        /// Increment the stolen task counter (called by work-stealing schedulers).
        void record_steal() noexcept {
            tasks_stolen_.fetch_add(1, std::memory_order_relaxed);
        }

        /// Direct access to the wrapped policy (e.g., for priority configuration).
        [[nodiscard]] Base& base_policy() noexcept { return base_; }
        [[nodiscard]] const Base& base_policy() const noexcept { return base_; }

    private:
        Base base_;
        profile_sink sink_{};
        mutable std::mutex report_mutex_;
        std::vector<task_profile_sample> samples_;
        std::chrono::steady_clock::time_point start_time_;
        std::unordered_map<std::size_t, std::chrono::steady_clock::time_point> ready_times_;
        std::unordered_map<std::size_t, std::chrono::steady_clock::time_point> exec_start_times_;
        std::atomic<std::size_t> tasks_stolen_{0};
    };

    // Concept conformance check (verified at compile time).
    static_assert(sched::SchedulerPolicy<profiling_scheduler_policy<sched::fifo_scheduler_policy>>);
    static_assert(sched::SchedulerPolicy<profiling_scheduler_policy<sched::priority_scheduler_policy>>);

    // ============================================================================
    // § 4  profile_sink — declared in § 0 (forward) as std::function<void(const
    //       task_profile_sample&)>.  Definition of task_profile_sample above makes
    //       the alias fully usable from here on.
    // ============================================================================

    // ============================================================================
    // § 5  with_profiling — factory function
    //
    // Returns a profiling_scheduler_policy<P> that default-constructs the inner policy.
    // The policy argument is used only for template type deduction — scheduler policies
    // contain std::mutex and cannot be moved or copied; the wrapper owns its own instance.
    // If a non-null sink is provided, it is stored and invoked (under no lock) each time
    // take_report() drains a sample — the integration point for external consumers.
    // ============================================================================

    template <sched::SchedulerPolicy P>
    [[nodiscard]] auto with_profiling(P /*policy*/, profile_sink sink = nullptr)
        -> profiling_scheduler_policy<P> {
        return profiling_scheduler_policy<P>{std::move(sink)};
    }
} // namespace pravaha::profile
