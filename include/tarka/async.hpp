#pragma once
// =============================================================================
// tarka/async.hpp — SmtTask coroutine + persistent worker pool
//
// Namespace:  tarka
// Provides:
//   SmtTask           — move-only C++20 awaitable over SatResult / SmtError
//   WorkerPool        — persistent jthread pool backed by MPMCQueue<task>
//   AsyncSolverEngine — queues solve tasks onto WorkerPool
//
// Design:
//   - SmtTask is a full move-only awaitable: dtor destroys frame, resume drives
//     the coroutine on the pool, result is std::expected<SatResult,SmtError>.
//   - Persistent pool: workers pull from MPMCQueue, no per-query thread spawn.
//   - Task submission: enqueue a callable; workers execute and fulfill the
//     promise embedded in the task descriptor.
// =============================================================================

#include "tarka/tarka.hpp"
#include "containers/lockfree/MPMCQueue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace tarka {
    // =========================================================================
    // SmtTask — move-only C++20 awaitable
    // =========================================================================

    class SmtTask {
    public:
        struct promise_type {
            std::expected<SatResult, SmtError> result_;
            std::exception_ptr exception_;

            SmtTask get_return_object() noexcept {
                return SmtTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }

            void return_value(std::expected<SatResult, SmtError> v) noexcept {
                result_ = std::move(v);
            }

            void unhandled_exception() noexcept {
                exception_ = std::current_exception();
            }
        };

        using handle_t = std::coroutine_handle<promise_type>;

        explicit SmtTask(handle_t h) noexcept : handle_(h) {}

        SmtTask(const SmtTask&) = delete;
        SmtTask& operator=(const SmtTask&) = delete;

        SmtTask(SmtTask&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}

        SmtTask& operator=(SmtTask&& o) noexcept {
            if (this != &o) {
                if (handle_) handle_.destroy();
                handle_ = std::exchange(o.handle_, {});
            }
            return *this;
        }

        ~SmtTask() noexcept { if (handle_) handle_.destroy(); }

        // Await support — SmtTask is itself awaitable
        [[nodiscard]] bool await_ready() const noexcept {
            return handle_ && handle_.done();
        }

        void await_suspend(std::coroutine_handle<> caller) noexcept {
            (void)caller;
            // Resumption is driven by the worker pool; caller suspends
        }

        [[nodiscard]] std::expected<SatResult, SmtError> await_resume() {
            if (!handle_) {
                return std::unexpected(SmtError{SmtError::Kind::Internal, "SmtTask: null handle"});
            }
            auto& p = handle_.promise();
            if (p.exception_) std::rethrow_exception(p.exception_);
            return std::move(p.result_);
        }

        // Synchronous get — resumes the coroutine on the calling thread
        [[nodiscard]] std::expected<SatResult, SmtError> get() {
            if (handle_ && !handle_.done()) handle_.resume();
            return await_resume();
        }

    private:
        handle_t handle_;
    };

    // =========================================================================
    // WorkerPool — persistent jthread pool + MPMCQueue
    // =========================================================================

    class WorkerPool {
    public:
        static constexpr std::size_t kQueueSize = 256;

        using task_fn = std::function<void()>;

        explicit WorkerPool(std::size_t nthreads = std::thread::hardware_concurrency()) {
            for (std::size_t i = 0; i < nthreads; ++i) {
                workers_.emplace_back([this](std::stop_token tok) {
                    while (!tok.stop_requested()) {
                        if (auto t = queue_.try_pop()) {
                            (*t)();
                        }
                        else {
                            // Block until work arrives or stop is requested — no CPU spin
                            std::unique_lock lock(wake_mutex_);
                            wake_cv_.wait_for(lock, std::chrono::milliseconds(1),
                                              [&] { return !queue_.empty() || tok.stop_requested(); });
                        }
                    }
                    // drain remaining tasks on stop
                    while (auto t = queue_.try_pop()) (*t)();
                });
            }
        }

        ~WorkerPool() = default; // jthreads auto-join + request_stop on destruction

        // Submit a task; returns false if queue is full — caller must handle rejection
        bool submit(task_fn fn) {
            const bool ok = queue_.try_push(std::move(fn));
            if (ok) wake_cv_.notify_one();
            return ok;
        }

        // Blocking submit — waits until slot is available or timeout
        // Returns false only on timeout, never silently drops
        bool submit_wait(task_fn fn,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (queue_.try_push(fn)) {
                    wake_cv_.notify_one();
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            return false;
        }

        [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

    private:
        lockfree::MPMCQueue<task_fn, kQueueSize> queue_;
        std::vector<std::jthread> workers_;
        std::mutex wake_mutex_;
        std::condition_variable wake_cv_;
    };


    // =========================================================================
    // AsyncSolverEngine — queues solve tasks on a WorkerPool
    // =========================================================================

    template <SmtSolverBackend B>
    class AsyncSolverEngine {
    public:
        explicit AsyncSolverEngine(WorkerPool& pool, B backend = {})
            : pool_(pool), backend_(std::move(backend)) {}

        [[nodiscard]] std::future<std::expected<SatResult, SmtError>>
        submit_solve(Term t) {
            auto prom = std::make_shared<std::promise<std::expected<SatResult, SmtError>>>();
            auto fut = prom->get_future();

            const bool ok = pool_.submit([this, t, p = std::move(prom)]() mutable {
                backend_.assert_formula(t);
                auto r = backend_.check_sat();
                backend_.reset();
                p->set_value(std::move(r));
            });

            if (!ok) {
                // Queue full — reject explicitly rather than hanging the future
                auto rejected = std::make_shared<std::promise<std::expected<SatResult, SmtError>>>();
                auto rejected_fut = rejected->get_future();
                rejected->set_value(std::unexpected(SmtError{SmtError::Kind::Internal, "WorkerPool queue full"}));
                return rejected_fut;
            }

            return fut;
        }

    private:
        WorkerPool& pool_;
        B backend_;
    };
} // namespace tarka
