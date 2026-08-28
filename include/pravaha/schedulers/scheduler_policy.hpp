#pragma once
#include "pravaha/pravaha.hpp"
#include <optional>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace pravaha::sched {
    // task_token — lightweight scheduling descriptor.
    // id mirrors TaskId::value (std::size_t).
    // dag_depth is set by critical-path analysis; 0 = unknown.
    // locality_hint is a NUMA/cluster index; 0 = any.
    // GPU tasks use locality_hint == ~0uz (all bits set).
    struct task_token {
        std::size_t id{~std::size_t{0}};
        TaskPriority priority{TaskPriority::Normal};
        std::size_t dag_depth{0};
        std::size_t locality_hint{0};

        [[nodiscard]] constexpr bool is_gpu() const noexcept {
            return locality_hint == ~std::size_t{0};
        }
    };

    // SchedulerPolicy — static interface concept.
    // Implementations must be default-constructible and must provide:
    //   on_task_ready   — called when a task becomes schedulable.
    //   on_task_complete — called after a worker finishes a task.
    //   select_next_task — returns the best next task for worker_id, or nullopt.
    template <class P>
    concept SchedulerPolicy = requires(P& p, task_token t, std::size_t worker_id) {
        { p.on_task_ready(t) } -> std::same_as<void>;
        { p.on_task_complete(t) } -> std::same_as<void>;
        { p.select_next_task(worker_id) } -> std::same_as<std::optional<task_token>>;
    };

    // ---------------------------------------------------------------------------
    // fifo_scheduler_policy
    // ---------------------------------------------------------------------------
    // Single shared FIFO deque protected by a mutex.
    // Suitable as a correctness baseline and for low-contention workloads.
    struct fifo_scheduler_policy {
        void on_task_ready(task_token t) {
            std::scoped_lock lk{mutex_};
            queue_.push_back(t);
        }

        void on_task_complete(task_token) {
            // No bookkeeping needed for pure FIFO.
        }

        [[nodiscard]] std::optional<task_token> select_next_task(std::size_t /*worker_id*/) {
            std::scoped_lock lk{mutex_};
            if (queue_.empty()) {
                return std::nullopt;
            }
            task_token t = queue_.front();
            queue_.pop_front();
            return t;
        }

        [[nodiscard]] std::size_t pending_count() const noexcept {
            std::scoped_lock lk{mutex_};
            return queue_.size();
        }

    private:
        mutable std::mutex mutex_;
        std::deque<task_token> queue_;
    };

    static_assert(SchedulerPolicy<fifo_scheduler_policy>);

    // ---------------------------------------------------------------------------
    // priority_scheduler_policy
    // ---------------------------------------------------------------------------
    // Reproduces JThreadBackend's priority-scan behaviour exactly:
    //   1. Scan deque for High, pick earliest.
    //   2. Scan deque for Normal, pick earliest.
    //   3. Scan deque for Low, pick earliest.
    // All under a single mutex to match JThreadBackend's shared-deque semantics.
    struct priority_scheduler_policy {
        void on_task_ready(task_token t) {
            std::scoped_lock lk{mutex_};
            queue_.push_back(t);
        }

        void on_task_complete(task_token) {
            // No bookkeeping needed.
        }

        [[nodiscard]] std::optional<task_token> select_next_task(std::size_t /*worker_id*/) {
            std::scoped_lock lk{mutex_};
            if (queue_.empty()) {
                return std::nullopt;
            }

            // Priority scan: High → Normal → Low (matches JThreadBackend).
            for (TaskPriority pri : {TaskPriority::High, TaskPriority::Normal, TaskPriority::Low}) {
                for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                    if (it->priority == pri) {
                        task_token t = *it;
                        queue_.erase(it);
                        return t;
                    }
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::size_t pending_count() const noexcept {
            std::scoped_lock lk{mutex_};
            return queue_.size();
        }

    private:
        mutable std::mutex mutex_;
        std::deque<task_token> queue_;
    };

    static_assert(SchedulerPolicy<priority_scheduler_policy>);
} // namespace pravaha::sched
