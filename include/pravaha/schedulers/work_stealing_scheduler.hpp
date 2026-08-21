#pragma once
#include "pravaha/schedulers/scheduler_policy.hpp"
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace pravaha::sched {
    // ---------------------------------------------------------------------------
    // work_stealing_scheduler_policy
    // ---------------------------------------------------------------------------
    // Per-worker deques with steal-from-longest-victim strategy.
    //
    // on_task_ready  — assigns incoming tasks to workers round-robin.
    // select_next_task(worker_id)
    //   1. Pop from local deque (back — LIFO for cache warmth).
    //   2. If local deque is empty, find the worker with the most tasks and steal
    //      one from the *front* of their deque (FIFO steal avoids cache thrash
    //      on the victim).
    //   3. Returns nullopt only when all queues are empty.
    //
    // The worker_count is fixed at construction time.
    // ---------------------------------------------------------------------------
    class work_stealing_scheduler_policy {
    public:
        explicit work_stealing_scheduler_policy(
            std::size_t worker_count = std::thread::hardware_concurrency())
            : worker_queues_(worker_count == 0 ? 1u : worker_count) {}

        // No copy/move — contains non-moveable mutexes.
        work_stealing_scheduler_policy(const work_stealing_scheduler_policy&) = delete;
        work_stealing_scheduler_policy& operator=(const work_stealing_scheduler_policy&) = delete;

        void on_task_ready(task_token t) {
            const std::size_t target =
                next_worker_.fetch_add(1, std::memory_order_relaxed) % worker_queues_.size();
            std::scoped_lock lk{worker_queues_[target].mutex};
            worker_queues_[target].tasks.push_back(t);
        }

        void on_task_complete(task_token /*t*/) {
            // No bookkeeping required.
        }

        [[nodiscard]] std::optional<task_token> select_next_task(std::size_t worker_id) {
            const std::size_t n = worker_queues_.size();
            worker_id %= n;

            // 1. Try local deque first (pop from back — LIFO).
            {
                std::scoped_lock lk{worker_queues_[worker_id].mutex};
                if (!worker_queues_[worker_id].tasks.empty()) {
                    task_token t = worker_queues_[worker_id].tasks.back();
                    worker_queues_[worker_id].tasks.pop_back();
                    return t;
                }
            }

            // 2. Steal from the worker with the most tasks.
            std::size_t victim = n; // sentinel: "none found yet"
            std::size_t max_size = 0;

            for (std::size_t i = 0; i < n; ++i) {
                if (i == worker_id) {
                    continue;
                }
                // Snapshot size without holding the lock long.
                std::size_t sz;
                {
                    std::scoped_lock lk{worker_queues_[i].mutex};
                    sz = worker_queues_[i].tasks.size();
                }
                if (sz > max_size) {
                    max_size = sz;
                    victim = i;
                }
            }

            if (victim == n || max_size == 0) {
                return std::nullopt;
            }

            // Steal from front of victim's deque.
            std::scoped_lock lk{worker_queues_[victim].mutex};
            if (worker_queues_[victim].tasks.empty()) {
                return std::nullopt; // Raced — victim drained between snapshot and lock.
            }
            task_token t = worker_queues_[victim].tasks.front();
            worker_queues_[victim].tasks.pop_front();
            total_steals_.fetch_add(1, std::memory_order_relaxed);
            return t;
        }

        [[nodiscard]] std::size_t total_steals() const noexcept {
            return total_steals_.load(std::memory_order_relaxed);
        }

        [[nodiscard]] std::size_t worker_count() const noexcept {
            return worker_queues_.size();
        }

    private:
        struct worker_deque {
            mutable std::mutex mutex;
            std::deque<task_token> tasks;
        };

        std::vector<worker_deque> worker_queues_;
        std::atomic<std::size_t> next_worker_{0};
        std::atomic<std::size_t> total_steals_{0};
    };

    static_assert(SchedulerPolicy<work_stealing_scheduler_policy>);
} // namespace pravaha::sched
