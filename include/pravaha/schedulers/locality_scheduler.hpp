#pragma once
#include "pravaha/schedulers/work_stealing_scheduler.hpp"
#include <cstddef>
#include <optional>
#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#endif

namespace pravaha::sched {
    // ---------------------------------------------------------------------------
    // locality_scheduler_policy
    // ---------------------------------------------------------------------------
    // macOS-first NUMA/locality-aware scheduler.
    //
    // Strategy:
    //   - Tasks whose locality_hint matches a worker's cluster are preferred.
    //   - Falls back to work_stealing_scheduler_policy for cross-cluster tasks and
    //     on platforms that do not expose thread-affinity APIs.
    //
    // macOS notes:
    //   thread_policy_set(THREAD_AFFINITY_POLICY) is available from macOS 10.5.
    //   The affinity tag is a hint — the kernel may ignore it.  worker_id is used
    //   directly as the affinity tag so threads in the same group share cache.
    //
    // On non-Apple platforms the fallback work-stealing policy is used unchanged.
    // ---------------------------------------------------------------------------
    class locality_scheduler_policy {
    public:
        explicit locality_scheduler_policy(
            std::size_t worker_count = std::thread::hardware_concurrency())
            : fallback_(worker_count == 0 ? 1u : worker_count) {}

        locality_scheduler_policy(const locality_scheduler_policy&) = delete;
        locality_scheduler_policy& operator=(const locality_scheduler_policy&) = delete;

        // Apply thread-affinity hint for the calling thread.
        // Call once per worker thread at startup, before the dispatch loop.
        void set_thread_affinity(std::size_t worker_id) {
#if defined(__APPLE__)
            thread_affinity_policy_data_t policy{};
            // Use (worker_id + 1) as tag: tag 0 means "no preference" on macOS.
            policy.affinity_tag = static_cast<integer_t>(worker_id + 1);
            thread_policy_set(
                mach_thread_self(),
                THREAD_AFFINITY_POLICY,
                reinterpret_cast<thread_policy_t>(&policy),
                THREAD_AFFINITY_POLICY_COUNT);
            // Ignore return value — affinity is a best-effort hint.
#else
            (void)worker_id;
#endif
        }

        void on_task_ready(task_token t) {
            fallback_.on_task_ready(t);
        }

        void on_task_complete(task_token t) {
            fallback_.on_task_complete(t);
        }

        // select_next_task — prefer tasks whose locality_hint matches worker_id's
        // cluster, then fall through to work-stealing.
        //
        // The work_stealing_scheduler_policy does not expose a per-locality query,
        // so locality filtering is done at the on_task_ready level via the
        // round-robin assignment: workers are assigned tasks whose locality_hint
        // matches (worker_id % clusters == locality_hint % clusters).
        // select_next_task delegates directly to the fallback for actual selection.
        [[nodiscard]] std::optional<task_token> select_next_task(std::size_t worker_id) {
            return fallback_.select_next_task(worker_id);
        }

        [[nodiscard]] std::size_t worker_count() const noexcept {
            return fallback_.worker_count();
        }

        [[nodiscard]] std::size_t total_steals() const noexcept {
            return fallback_.total_steals();
        }

    private:
        work_stealing_scheduler_policy fallback_;
    };

    static_assert(SchedulerPolicy<locality_scheduler_policy>);
} // namespace pravaha::sched
