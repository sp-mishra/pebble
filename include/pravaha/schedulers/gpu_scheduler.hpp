#pragma once
#include "pravaha/schedulers/scheduler_policy.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace pravaha::sched {
    // ---------------------------------------------------------------------------
    // gpu_aware_scheduler_policy<BatchSize>
    // ---------------------------------------------------------------------------
    // Batches External-domain (GPU) tasks and flushes them as a group once either:
    //   (a) gpu_batch_.size() >= BatchSize, or
    //   (b) the batch has been open for longer than max_wait_ns nanoseconds.
    //
    // Non-GPU tasks are forwarded immediately to the inner priority_scheduler_policy.
    //
    // Convention (matching pravaha_hetero.hpp):
    //   GPU tasks are identified by task_token::is_gpu() returning true, i.e.
    //   locality_hint == ~0uz (all bits set).
    //
    // Flushing:
    //   When a flush is triggered, all accumulated GPU tasks are moved into
    //   flush_drain_ and returned one by one via select_next_task().  The batch
    //   is only re-opened after the drain is exhausted.
    //
    // Thread safety: all public methods are protected by mutex_.
    // ---------------------------------------------------------------------------
    template <std::size_t BatchSize = 8>
    class gpu_aware_scheduler_policy {
    public:
        static constexpr std::uint64_t kDefaultMaxWaitNs = 100'000; // 100 µs

        explicit gpu_aware_scheduler_policy(
            std::uint64_t max_wait_ns = kDefaultMaxWaitNs)
            : max_wait_ns_{max_wait_ns} {
            batch_start_ = std::chrono::steady_clock::now();
        }

        // No copy/move — contains a mutex.
        gpu_aware_scheduler_policy(const gpu_aware_scheduler_policy&) = delete;
        gpu_aware_scheduler_policy& operator=(const gpu_aware_scheduler_policy&) = delete;

        // on_task_ready — route GPU tasks to the batch, others to the base policy.
        void on_task_ready(task_token t) {
            std::scoped_lock lk{mutex_};
            if (t.is_gpu()) {
                if (gpu_batch_.empty()) {
                    // Start the batch timer on the first GPU task.
                    batch_start_ = std::chrono::steady_clock::now();
                }
                gpu_batch_.push_back(t);
            }
            else {
                base_.on_task_ready(t);
            }
        }

        void on_task_complete(task_token t) {
            std::scoped_lock lk{mutex_};
            base_.on_task_complete(t);
        }

        // select_next_task —
        //   1. If a flush drain is in progress, return the next task from it.
        //   2. Check whether a flush should be triggered (size OR timeout).
        //   3. Return from the base priority policy for non-GPU tasks.
        [[nodiscard]] std::optional<task_token> select_next_task(std::size_t worker_id) {
            std::scoped_lock lk{mutex_};

            // 1. Drain flushed GPU tasks first.
            if (!flush_drain_.empty()) {
                task_token t = flush_drain_.back();
                flush_drain_.pop_back();
                return t;
            }

            // 2. Evaluate flush condition.
            if (!gpu_batch_.empty()) {
                const bool size_trigger = gpu_batch_.size() >= BatchSize;
                const bool time_trigger = [&]() -> bool {
                    if (max_wait_ns_ == 0) {
                        return false;
                    }
                    const auto elapsed = std::chrono::steady_clock::now() - batch_start_;
                    const std::uint64_t ns =
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
                    return ns >= max_wait_ns_;
                }();

                if (size_trigger || time_trigger) {
                    // Move batch into drain (reverse so pop_back returns original order).
                    flush_drain_.reserve(gpu_batch_.size());
                    for (auto it = gpu_batch_.rbegin(); it != gpu_batch_.rend(); ++it) {
                        flush_drain_.push_back(*it);
                    }
                    gpu_batch_.clear();

                    // Return first drained task immediately.
                    task_token t = flush_drain_.back();
                    flush_drain_.pop_back();
                    return t;
                }
            }

            // 3. Fall through to non-GPU base scheduler.
            return base_.select_next_task(worker_id);
        }

        // Force-flush the current GPU batch regardless of size/timeout.
        // Useful at shutdown or barrier points.
        void force_flush() {
            std::scoped_lock lk{mutex_};
            if (gpu_batch_.empty()) {
                return;
            }
            flush_drain_.reserve(flush_drain_.size() + gpu_batch_.size());
            for (auto it = gpu_batch_.rbegin(); it != gpu_batch_.rend(); ++it) {
                flush_drain_.push_back(*it);
            }
            gpu_batch_.clear();
        }

        [[nodiscard]] std::size_t pending_gpu_batch_size() const noexcept {
            std::scoped_lock lk{mutex_};
            return gpu_batch_.size();
        }

        [[nodiscard]] std::size_t pending_drain_size() const noexcept {
            std::scoped_lock lk{mutex_};
            return flush_drain_.size();
        }

    private:
        mutable std::mutex mutex_;
        priority_scheduler_policy base_; // handles non-GPU tasks
        std::vector<task_token> gpu_batch_;
        std::vector<task_token> flush_drain_; // tasks ready to be returned
        std::chrono::steady_clock::time_point batch_start_;
        std::uint64_t max_wait_ns_;
    };

    static_assert(SchedulerPolicy<gpu_aware_scheduler_policy<8>>);
    static_assert(SchedulerPolicy<gpu_aware_scheduler_policy<1>>);
} // namespace pravaha::sched
