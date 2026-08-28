#pragma once

#include <pravaha/pravaha.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "containers/dynamic/SmallVector.hpp"

namespace pravaha::backends {
    class CoroutineBackend;

    struct yield_now {
        bool await_ready() noexcept {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> h);

        void await_resume() noexcept {}
    };

    struct suspend_once {
        bool armed{true};

        [[nodiscard]] bool await_ready() const noexcept {
            return !armed;
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept;

        void await_resume() noexcept {}
    };

    class manual_reset_awaitable {
        std::atomic<bool> signaled_{false};
        std::mutex waiters_mutex_{};
        std::deque<void*> waiters_{};
        CoroutineBackend* backend_{nullptr};

    public:
        manual_reset_awaitable() = default;

        explicit manual_reset_awaitable(bool initial_state) noexcept : signaled_{initial_state} {}

        bool await_ready() const noexcept {
            return signaled_.load(std::memory_order_acquire);
        }

        bool await_suspend(std::coroutine_handle<> h);

        void await_resume() noexcept {}

        void signal();

        void reset() noexcept {
            signaled_.store(false, std::memory_order_release);
        }
    };

    using yield_once = yield_now;

    struct sleep_for_awaitable {
        std::chrono::steady_clock::duration duration{};

        [[nodiscard]] bool await_ready() const noexcept {
            return duration <= std::chrono::steady_clock::duration::zero();
        }

        bool await_suspend(std::coroutine_handle<> h);

        void await_resume() noexcept {}
    };

    struct cancellation_point_awaitable {
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> h);

        void await_resume();
    };

    inline cancellation_point_awaitable cancellation_point() {
        return {};
    }

    template <class Rep, class Period>
    sleep_for_awaitable sleep_for(std::chrono::duration<Rep, Period> duration) {
        return sleep_for_awaitable{std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration)};
    }

    template <class T>
    class AwaitableTask {
    public:
        struct promise_type {
            std::optional<Outcome<T>> result{};
            bool completed{false};

            AwaitableTask get_return_object() noexcept {
                return AwaitableTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept {
                return {};
            }

            std::suspend_always final_suspend() noexcept {
                return {};
            }

            template <class U>
            void return_value(U&& value) {
                result = Outcome<T>{std::forward<U>(value)};
                completed = true;
            }

            void unhandled_exception() noexcept {
                try {
                    throw;
                }
                catch (const PravahaError& error) {
                    result = std::unexpected(error);
                }
                catch (const std::exception& error) {
                    result = std::unexpected(PravahaError{ErrorKind::TaskFailed, error.what()});
                }
                catch (...) {
                    result = std::unexpected(PravahaError{ErrorKind::TaskFailed, "unknown exception"});
                }
                completed = true;
            }
        };

        using handle_type = std::coroutine_handle<promise_type>;

        AwaitableTask() noexcept = default;

        explicit AwaitableTask(handle_type handle) noexcept : handle_{handle} {}

        AwaitableTask(const AwaitableTask&) = delete;

        AwaitableTask& operator=(const AwaitableTask&) = delete;

        AwaitableTask(AwaitableTask&& other) noexcept : handle_{other.handle_} {
            other.handle_ = {};
        }

        AwaitableTask& operator=(AwaitableTask&& other) noexcept {
            if (this == &other) {
                return *this;
            }
            if (handle_) {
                handle_.destroy();
            }
            handle_ = other.handle_;
            other.handle_ = {};
            return *this;
        }

        ~AwaitableTask() {
            if (handle_) {
                handle_.destroy();
            }
        }

        [[nodiscard]] bool done() const noexcept {
            return !handle_ || handle_.done();
        }

        [[nodiscard]] handle_type native_handle() const noexcept {
            return handle_;
        }

        [[nodiscard]] handle_type release_handle() noexcept {
            handle_type out = handle_;
            handle_ = {};
            return out;
        }

        void resume() {
            if (handle_ && !handle_.done()) {
                handle_.resume();
            }
        }

        Outcome<T> result() {
            if (!handle_) {
                return std::unexpected(PravahaError{ErrorKind::TaskFailed, "awaitable coroutine handle unavailable"});
            }
            auto& promise = handle_.promise();
            if (!promise.completed || !promise.result.has_value()) {
                return std::unexpected(PravahaError{
                    ErrorKind::TaskFailed, "awaitable coroutine completed without result"
                });
            }
            Outcome<T> out = std::move(*promise.result);
            promise.result.reset();
            return out;
        }

        Outcome<T> consume_result() {
            return result();
        }

    private:
        handle_type handle_{};
    };

    template <>
    class AwaitableTask<void> {
    public:
        struct promise_type {
            std::optional<Outcome<Unit>> result{};
            bool completed{false};

            AwaitableTask get_return_object() noexcept {
                return AwaitableTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() noexcept {
                return {};
            }

            std::suspend_always final_suspend() noexcept {
                return {};
            }

            void return_void() {
                result = Outcome<Unit>{Unit{}};
                completed = true;
            }

            void unhandled_exception() noexcept {
                try {
                    throw;
                }
                catch (const PravahaError& error) {
                    result = std::unexpected(error);
                }
                catch (const std::exception& error) {
                    result = std::unexpected(PravahaError{ErrorKind::TaskFailed, error.what()});
                }
                catch (...) {
                    result = std::unexpected(PravahaError{ErrorKind::TaskFailed, "unknown exception"});
                }
                completed = true;
            }
        };

        using handle_type = std::coroutine_handle<promise_type>;

        AwaitableTask() noexcept = default;

        explicit AwaitableTask(handle_type handle) noexcept : handle_{handle} {}

        AwaitableTask(const AwaitableTask&) = delete;

        AwaitableTask& operator=(const AwaitableTask&) = delete;

        AwaitableTask(AwaitableTask&& other) noexcept : handle_{other.handle_} {
            other.handle_ = {};
        }

        AwaitableTask& operator=(AwaitableTask&& other) noexcept {
            if (this == &other) {
                return *this;
            }
            if (handle_) {
                handle_.destroy();
            }
            handle_ = other.handle_;
            other.handle_ = {};
            return *this;
        }

        ~AwaitableTask() {
            if (handle_) {
                handle_.destroy();
            }
        }

        [[nodiscard]] bool done() const noexcept {
            return !handle_ || handle_.done();
        }

        [[nodiscard]] handle_type native_handle() const noexcept {
            return handle_;
        }

        [[nodiscard]] handle_type release_handle() noexcept {
            handle_type out = handle_;
            handle_ = {};
            return out;
        }

        void resume() {
            if (handle_ && !handle_.done()) {
                handle_.resume();
            }
        }

        [[nodiscard]] Outcome<Unit> result() const {
            if (!handle_) {
                return std::unexpected(PravahaError{ErrorKind::TaskFailed, "awaitable coroutine handle unavailable"});
            }
            auto& promise = handle_.promise();
            if (!promise.completed || !promise.result.has_value()) {
                return std::unexpected(PravahaError{
                    ErrorKind::TaskFailed, "awaitable coroutine completed without result"
                });
            }
            Outcome<Unit> out = std::move(*promise.result);
            promise.result.reset();
            return out;
        }

        [[nodiscard]] Outcome<Unit> consume_result() const {
            return result();
        }

    private:
        handle_type handle_{};
    };

    template <class T>
    Outcome<T> run_awaitable(AwaitableTask<T>&& task) {
        while (!task.done()) {
            task.resume();
        }
        return task.result();
    }

    template <class R>
    auto collapse_awaitable_execution(Outcome<R> run_result) {
        if constexpr (is_outcome_v<R>) {
            using ValueT = std::remove_cvref_t<R>::value_type;
            if (!run_result.has_value()) {
                return Outcome<ValueT>{std::unexpected(run_result.error())};
            }
            return run_result.value();
        }
        else {
            return run_result;
        }
    }

    template <class T>
    Outcome<T> run_awaitable_with_backend(CoroutineBackend& backend, AwaitableTask<T>&& task);

    template <class F>
    auto awaitable_adapter(F&& callable);

    template <class F>
    [[nodiscard]] auto awaitable_task(std::string name, F&& callable);

    class CoroutineBackend {
        using Clock = std::chrono::steady_clock;

        struct SuspendedFrame {
            std::coroutine_handle<> handle{};
            std::size_t task_id{0};
            const CancellationToken* token{nullptr};
        };

        std::mutex mutex_{};
        std::condition_variable cv_{};
        std::deque<TaskCommand> queue_{};
        std::map<void*, SuspendedFrame> suspended_{};
        containers::dynamic::SmallVector<void*, 512> canceled_frames_{};
        std::deque<std::coroutine_handle<>> ready_coroutines_{};
        std::multimap<Clock::time_point, std::coroutine_handle<>> timers_{};
        std::atomic<bool> stop_requested_{false};
        std::atomic<std::size_t> next_task_id_{1};
        std::thread worker_{};
        std::size_t in_flight_{0};
        std::size_t active_coroutine_frames_{0};
        static inline thread_local CoroutineBackend* active_backend_{nullptr};
        static inline thread_local std::size_t active_task_id_{0};
        static inline thread_local const CancellationToken* active_cancellation_token_{nullptr};

        struct ActiveBackendScope {
            CoroutineBackend* previous{nullptr};

            explicit ActiveBackendScope(CoroutineBackend* current) noexcept : previous{
                active_backend_
            } {
                active_backend_ = current;
            }

            ~ActiveBackendScope() {
                active_backend_ = previous;
            }
        };

        struct ActiveTaskScope {
            std::size_t previous_task_id{0};
            const CancellationToken* previous_token{nullptr};

            ActiveTaskScope(std::size_t task_id, const CancellationToken* token) noexcept
                : previous_task_id{active_task_id_},
                  previous_token{active_cancellation_token_} {
                active_task_id_ = task_id;
                active_cancellation_token_ = token;
            }

            ~ActiveTaskScope() {
                active_task_id_ = previous_task_id;
                active_cancellation_token_ = previous_token;
            }
        };

        [[nodiscard]] bool idle_locked() const noexcept {
            return queue_.empty() && ready_coroutines_.empty() && timers_.empty() && in_flight_ == 0 &&
                active_coroutine_frames_ == 0;
        }

        [[nodiscard]] bool has_handle_address(const std::deque<std::coroutine_handle<>>& handles,
                                              const void* address) {
            for (const auto& handle : handles) {
                if (handle.address() == address) {
                    return true;
                }
            }
            return false;
        }

        void remove_ready_locked(const void* address) {
            for (auto it = ready_coroutines_.begin(); it != ready_coroutines_.end();) {
                if (it->address() == address) {
                    it = ready_coroutines_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        void remove_timers_locked(const void* address) {
            for (auto it = timers_.begin(); it != timers_.end();) {
                if (it->second.address() == address) {
                    it = timers_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        [[nodiscard]] bool is_frame_canceled_locked(const void* address) const {
            auto canceled_it = std::find(canceled_frames_.begin(), canceled_frames_.end(), const_cast<void*>(address));
            if (canceled_it != canceled_frames_.end()) {
                return true;
            }
            auto it = suspended_.find(const_cast<void*>(address));
            if (it == suspended_.end()) {
                return false;
            }
            if (it->second.token != nullptr && it->second.token->stop_requested()) {
                return true;
            }
            return false;
        }

        [[nodiscard]] bool take_ready_locked(const void* address) {
            for (auto it = ready_coroutines_.begin(); it != ready_coroutines_.end(); ++it) {
                if (it->address() == address) {
                    ready_coroutines_.erase(it);
                    return true;
                }
            }
            return false;
        }

        void finish_tracked_coroutine_locked(std::coroutine_handle<> handle) {
            if (!handle) {
                return;
            }
            const void* address = handle.address();
            suspended_.erase(const_cast<void*>(address));
            auto cf_it = std::find(canceled_frames_.begin(), canceled_frames_.end(), const_cast<void*>(address));
            if (cf_it != canceled_frames_.end()) {
                canceled_frames_.erase(cf_it);
            }
            remove_ready_locked(address);
            remove_timers_locked(address);
            if (active_coroutine_frames_ > 0) {
                --active_coroutine_frames_;
            }
            cv_.notify_all();
        }

        void finish_tracked_coroutine(std::coroutine_handle<> handle) {
            if (!handle) {
                return;
            }
            std::lock_guard lock(mutex_);
            finish_tracked_coroutine_locked(handle);
        }

        void move_expired_timers_to_ready_locked(Clock::time_point now) {
            auto it = timers_.begin();
            while (it != timers_.end() && it->first <= now) {
                auto handle = it->second;
                const auto* address = handle.address();
                it = timers_.erase(it);
                if (!handle || handle.done()) {
                    suspended_.erase(const_cast<void*>(address));
                    auto cf_it = std::find(canceled_frames_.begin(), canceled_frames_.end(),
                                           const_cast<void*>(address));
                    if (cf_it != canceled_frames_.end()) {
                        canceled_frames_.erase(cf_it);
                    }
                    continue;
                }
                if (is_frame_canceled_locked(address)) {
                    suspended_.erase(const_cast<void*>(address));
                    canceled_frames_.push_back(const_cast<void*>(address));
                    continue;
                }
                if (!has_handle_address(ready_coroutines_, address)) {
                    ready_coroutines_.push_back(handle);
                }
                suspended_.erase(const_cast<void*>(address));
            }
        }

        void refresh_cancellation_locked() {
            for (const auto& [address, frame] : suspended_) {
                if (frame.token != nullptr && frame.token->stop_requested()) {
                    auto cf_it = std::find(canceled_frames_.begin(), canceled_frames_.end(), address);
                    if (cf_it == canceled_frames_.end()) {
                        canceled_frames_.push_back(address);
                    }
                    remove_ready_locked(address);
                    remove_timers_locked(address);
                }
            }
        }

        void run_loop() {
            while (true) {
                TaskCommand cmd;
                std::coroutine_handle ready_handle{};
                bool has_task = false;
                bool has_ready = false;
                {
                    std::unique_lock lock(mutex_);
                    while (true) {
                        refresh_cancellation_locked();
                        move_expired_timers_to_ready_locked(Clock::now());
                        if (stop_requested_.load(std::memory_order_acquire) && queue_.empty() && ready_coroutines_.
                            empty()) {
                            cv_.notify_all();
                            return;
                        }
                        if (!queue_.empty() || !ready_coroutines_.empty()) {
                            break;
                        }
                        if (timers_.empty()) {
                            cv_.wait(lock, [this]() {
                                return stop_requested_.load(std::memory_order_acquire) || !queue_.empty() || !
                                    ready_coroutines_.empty() || !timers_.empty();
                            });
                        }
                        else {
                            cv_.wait_until(lock, timers_.begin()->first);
                        }
                    }
                    if (!queue_.empty()) {
                        auto selected = queue_.begin();
                        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                            if (static_cast<int>(it->priority()) > static_cast<int>(selected->priority())) {
                                selected = it;
                            }
                        }
                        cmd = std::move(*selected);
                        queue_.erase(selected);
                        ++in_flight_;
                        has_task = true;
                    }
                    else if (!ready_coroutines_.empty()) {
                        ready_handle = ready_coroutines_.front();
                        ready_coroutines_.pop_front();
                        has_ready = true;
                    }
                }

                if (has_task) {
                    ActiveBackendScope scope{this};
                    cmd.run();
                    std::lock_guard lock(mutex_);
                    if (in_flight_ > 0) {
                        --in_flight_;
                    }
                    cv_.notify_all();
                    continue;
                }

                if (!has_ready || !ready_handle || ready_handle.done()) {
                    continue;
                }
                bool canceled_ready = false;
                {
                    std::lock_guard lock(mutex_);
                    canceled_ready = is_frame_canceled_locked(ready_handle.address());
                }
                if (canceled_ready) {
                    finish_tracked_coroutine(ready_handle);
                    continue;
                }
                ActiveBackendScope scope{this};
                ready_handle.resume();
                if (ready_handle.done()) {
                    finish_tracked_coroutine(ready_handle);
                }
                std::lock_guard lock(mutex_);
                cv_.notify_all();
            }
        }

        bool drive_until_ready_or_stopped(const void* target) {
            while (true) {
                TaskCommand cmd;
                std::coroutine_handle ready_handle{};
                bool has_task = false;
                bool has_ready = false;
                {
                    std::unique_lock lock(mutex_);
                    while (true) {
                        refresh_cancellation_locked();
                        move_expired_timers_to_ready_locked(Clock::now());
                        if (stop_requested_.load(std::memory_order_acquire)) {
                            return false;
                        }
                        if (is_frame_canceled_locked(target)) {
                            return false;
                        }
                        if (take_ready_locked(target)) {
                            return true;
                        }
                        if (!queue_.empty() || !ready_coroutines_.empty()) {
                            break;
                        }
                        if (timers_.empty()) {
                            cv_.wait(lock, [this]() {
                                return stop_requested_.load(std::memory_order_acquire) || !queue_.empty() || !
                                    ready_coroutines_.empty() || !timers_.empty();
                            });
                        }
                        else {
                            cv_.wait_until(lock, timers_.begin()->first);
                        }
                    }
                    if (!queue_.empty()) {
                        auto selected = queue_.begin();
                        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                            if (static_cast<int>(it->priority()) > static_cast<int>(selected->priority())) {
                                selected = it;
                            }
                        }
                        cmd = std::move(*selected);
                        queue_.erase(selected);
                        ++in_flight_;
                        has_task = true;
                    }
                    else if (!ready_coroutines_.empty()) {
                        ready_handle = ready_coroutines_.front();
                        ready_coroutines_.pop_front();
                        has_ready = true;
                    }
                }

                if (has_task) {
                    ActiveBackendScope scope{this};
                    cmd.run();
                    std::lock_guard lock(mutex_);
                    if (in_flight_ > 0) {
                        --in_flight_;
                    }
                    cv_.notify_all();
                    continue;
                }

                if (!has_ready || !ready_handle || ready_handle.done()) {
                    continue;
                }
                bool canceled_ready = false;
                {
                    std::lock_guard lock(mutex_);
                    canceled_ready = is_frame_canceled_locked(ready_handle.address());
                }
                if (canceled_ready) {
                    finish_tracked_coroutine(ready_handle);
                    continue;
                }
                if (ready_handle.address() == target) {
                    std::lock_guard lock(mutex_);
                    ready_coroutines_.push_front(ready_handle);
                    cv_.notify_all();
                    continue;
                }
                ActiveBackendScope scope{this};
                ready_handle.resume();
                if (ready_handle.done()) {
                    finish_tracked_coroutine(ready_handle);
                }
                std::lock_guard lock(mutex_);
                cv_.notify_all();
            }
        }

    public:
        CoroutineBackend() {
            worker_ = std::thread([this]() {
                ActiveBackendScope scope{this};
                run_loop();
            });
        }

        CoroutineBackend(const CoroutineBackend&) = delete;

        CoroutineBackend& operator=(const CoroutineBackend&) = delete;

        ~CoroutineBackend() {
            request_stop();
            if (worker_.joinable()) {
                worker_.join();
            }
        }

        void suspend(std::coroutine_handle<> handle) {
            if (!handle || handle.done()) {
                return;
            }
            std::lock_guard lock(mutex_);
            if (stop_requested_.load(std::memory_order_acquire)) {
                return;
            }
            suspended_[handle.address()] = SuspendedFrame{handle, active_task_id_, active_cancellation_token_};
            auto cf_it = std::find(canceled_frames_.begin(), canceled_frames_.end(), handle.address());
            if (cf_it != canceled_frames_.end()) {
                canceled_frames_.erase(cf_it);
            }
            cv_.notify_all();
        }

        void mark_ready(std::coroutine_handle<> handle) {
            std::lock_guard lock(mutex_);
            if (stop_requested_.load(std::memory_order_acquire) || !handle || handle.done()) {
                return;
            }
            const auto address = handle.address();
            if (has_handle_address(ready_coroutines_, address)) {
                return;
            }
            if (is_frame_canceled_locked(address)) {
                auto cf_it = std::find(canceled_frames_.begin(), canceled_frames_.end(), const_cast<void*>(address));
                if (cf_it == canceled_frames_.end()) {
                    canceled_frames_.push_back(const_cast<void*>(address));
                }
                suspended_.erase(address);
                remove_timers_locked(address);
                cv_.notify_all();
                return;
            }
            suspended_.erase(address);
            remove_timers_locked(address);
            ready_coroutines_.push_back(handle);
            cv_.notify_all();
        }

        void schedule_after(std::coroutine_handle<> handle, Clock::duration duration) {
            if (!handle || handle.done()) {
                return;
            }
            std::lock_guard lock(mutex_);
            if (stop_requested_.load(std::memory_order_acquire)) {
                return;
            }
            const auto wakeup = Clock::now() + duration;
            suspended_[handle.address()] = SuspendedFrame{handle, active_task_id_, active_cancellation_token_};
            auto cf_it = std::find(canceled_frames_.begin(), canceled_frames_.end(), handle.address());
            if (cf_it != canceled_frames_.end()) {
                canceled_frames_.erase(cf_it);
            }
            timers_.emplace(wakeup, handle);
            cv_.notify_all();
        }

        Outcome<Unit> submit(TaskCommand cmd) {
            {
                std::lock_guard lock(mutex_);
                if (stop_requested_.load(std::memory_order_acquire)) {
                    return std::unexpected(PravahaError{
                        ErrorKind::QueueRejected, "backend rejected task submission: stopped"
                    });
                }
                queue_.push_back(std::move(cmd));
            }
            cv_.notify_all();
            return Outcome<Unit>{Unit{}};
        }

        template <class T>
        Outcome<T> run_awaitable_task(AwaitableTask<T>&& task) {
            auto handle = task.native_handle();
            if (!handle) {
                return std::unexpected(PravahaError{ErrorKind::TaskFailed, "awaitable coroutine handle unavailable"});
            }
            {
                std::lock_guard lock(mutex_);
                if (stop_requested_.load(std::memory_order_acquire)) {
                    return std::unexpected(PravahaError{
                        ErrorKind::TaskCanceled, "awaitable task canceled before resumption"
                    });
                }
                ++active_coroutine_frames_;
            }
            const auto task_id = next_task_id_.fetch_add(1, std::memory_order_relaxed);
            ActiveTaskScope task_scope{task_id, active_cancellation_token_};
            const auto* target = handle.address();
            while (!task.done()) {
                if (stop_requested_.load(std::memory_order_acquire)) {
                    finish_tracked_coroutine(handle);
                    return std::unexpected(PravahaError{
                        ErrorKind::TaskCanceled, "awaitable task canceled before resumption"
                    });
                }
                task.resume();
                if (!task.done()) {
                    suspend(task.native_handle());
                    if (!drive_until_ready_or_stopped(target)) {
                        finish_tracked_coroutine(handle);
                        return std::unexpected(PravahaError{
                            ErrorKind::TaskCanceled, "awaitable task canceled before resumption"
                        });
                    }
                    // drive_until_ready_or_stopped released the mutex before
                    // returning; a request_stop() may have raced in during that
                    // window. Re-validate under the lock so a frame that was
                    // marked ready but then stopped is never resumed.
                    {
                        std::lock_guard lock(mutex_);
                        if (stop_requested_.load(std::memory_order_acquire) ||
                            is_frame_canceled_locked(target)) {
                            finish_tracked_coroutine_locked(handle);
                            return std::unexpected(PravahaError{
                                ErrorKind::TaskCanceled, "awaitable task canceled before resumption"
                            });
                        }
                    }
                }
            }
            finish_tracked_coroutine(handle);
            return task.result();
        }

        void request_stop() noexcept {
            {
                std::lock_guard lock(mutex_);
                stop_requested_.store(true, std::memory_order_release);
                queue_.clear();
                for (const auto& [address, _] : suspended_) {
                    auto cf_it = std::find(canceled_frames_.begin(), canceled_frames_.end(), address);
                    if (cf_it == canceled_frames_.end()) {
                        canceled_frames_.push_back(address);
                    }
                }
                // Ready-but-not-yet-resumed frames must also be cancellable so a
                // driver that raced past the ready-take re-validates and refuses
                // to resume them.
                for (const auto& handle : ready_coroutines_) {
                    auto* address = handle.address();
                    auto cf_it = std::find(canceled_frames_.begin(), canceled_frames_.end(), address);
                    if (cf_it == canceled_frames_.end()) {
                        canceled_frames_.push_back(address);
                    }
                }
                suspended_.clear();
                ready_coroutines_.clear();
                timers_.clear();
                active_coroutine_frames_ = 0;
            }
            cv_.notify_all();
        }

        void wait_all() {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]() {
                return idle_locked() || (stop_requested_.load(std::memory_order_acquire) && queue_.empty() && in_flight_
                    == 0);
            });
        }

        void drain() {
            wait_all();
        }

        [[nodiscard]] bool stopped() const noexcept {
            return stop_requested_.load(std::memory_order_acquire);
        }

        void set_active_cancellation_token(const CancellationToken* token) noexcept {
            active_cancellation_token_ = token;
        }

        void notify_cancellation_state_changed() {
            {
                std::lock_guard lock(mutex_);
                refresh_cancellation_locked();
            }
            cv_.notify_all();
        }

        static CoroutineBackend* current_backend() noexcept {
            return active_backend_;
        }

        static const CancellationToken* current_cancellation_token() noexcept {
            return active_cancellation_token_;
        }
    };

    template <class T>
    class async_channel {
        struct waiting_receiver_entry {
            void* address{nullptr};
            std::optional<T>* delivery{nullptr};
        };

        std::mutex mutex_{};
        std::deque<T> values_{};
        std::deque<waiting_receiver_entry> waiting_receivers_{};
        CoroutineBackend* backend_{nullptr};
        bool closed_{false};

    public:
        async_channel() = default;

        async_channel(const async_channel&) = delete;

        async_channel& operator=(const async_channel&) = delete;

        ~async_channel() {
            close();
        }

        Outcome<Unit> send(T value) {
            waiting_receiver_entry receiver{};
            CoroutineBackend* backend = nullptr;
            std::optional<T> pending{std::move(value)};
            {
                std::lock_guard lock(mutex_);
                if (closed_) {
                    return std::unexpected(PravahaError{ErrorKind::QueueRejected, "channel is closed"});
                }
                if (!waiting_receivers_.empty()) {
                    receiver = waiting_receivers_.front();
                    waiting_receivers_.pop_front();
                    backend = backend_;
                    if (receiver.delivery != nullptr && pending.has_value()) {
                        *receiver.delivery = std::move(*pending);
                        pending.reset();
                    }
                }
                if (pending.has_value()) {
                    values_.push_back(std::move(*pending));
                }
            }
            if (receiver.address != nullptr && backend != nullptr) {
                backend->mark_ready(std::coroutine_handle<>::from_address(receiver.address));
            }
            return Outcome<Unit>{Unit{}};
        }

        void close() noexcept {
            std::deque<waiting_receiver_entry> waiting;
            CoroutineBackend* backend = nullptr;
            {
                std::lock_guard lock(mutex_);
                if (closed_) {
                    return;
                }
                closed_ = true;
                waiting.swap(waiting_receivers_);
                backend = backend_;
            }
            if (backend == nullptr) {
                return;
            }
            for (const auto& entry : waiting) {
                if (entry.address != nullptr) {
                    backend->mark_ready(std::coroutine_handle<>::from_address(entry.address));
                }
            }
        }

        class recv_awaitable {
            async_channel* channel_{nullptr};
            std::optional<T> value_{};

        public:
            explicit recv_awaitable(async_channel* channel) noexcept : channel_{channel} {}

            bool await_ready() {
                std::lock_guard<std::mutex> lock(channel_->mutex_);
                if (!channel_->values_.empty()) {
                    value_ = std::move(channel_->values_.front());
                    channel_->values_.pop_front();
                    return true;
                }
                return false;
            }

            bool await_suspend(std::coroutine_handle<> h) {
                auto* backend = CoroutineBackend::current_backend();
                if (backend == nullptr) {
                    return false;
                }
                {
                    std::lock_guard<std::mutex> lock(channel_->mutex_);
                    if (!channel_->values_.empty()) {
                        value_ = std::move(channel_->values_.front());
                        channel_->values_.pop_front();
                        return false;
                    }
                    if (channel_->closed_) {
                        return false;
                    }
                    if (channel_->backend_ == nullptr) {
                        channel_->backend_ = backend;
                    }
                    channel_->waiting_receivers_.push_back(waiting_receiver_entry{h.address(), &value_});
                }
                backend->suspend(h);
                return true;
            }

            T await_resume() {
                if (value_.has_value()) {
                    T out = std::move(*value_);
                    value_.reset();
                    return out;
                }
                std::lock_guard<std::mutex> lock(channel_->mutex_);
                if (!channel_->values_.empty()) {
                    T out = std::move(channel_->values_.front());
                    channel_->values_.pop_front();
                    return out;
                }
                throw PravahaError{ErrorKind::TaskCanceled, "channel receive canceled"};
            }
        };

        recv_awaitable recv() {
            return recv_awaitable{this};
        }
    };

    template <class T>
    Outcome<T> run_awaitable_with_backend(CoroutineBackend& backend, AwaitableTask<T>&& task) {
        return backend.run_awaitable_task(std::move(task));
    }

    inline bool yield_now::await_suspend(std::coroutine_handle<> h) {
        if (auto* backend = CoroutineBackend::current_backend(); backend != nullptr) {
            backend->suspend(h);
            backend->mark_ready(h);
            return true;
        }
        return false;
    }

    inline bool suspend_once::await_suspend(std::coroutine_handle<> h) noexcept {
        if (!armed) {
            return false;
        }
        armed = false;
        if (auto* backend = CoroutineBackend::current_backend(); backend != nullptr) {
            backend->suspend(h);
            backend->mark_ready(h);
            return true;
        }
        return false;
    }

    inline bool manual_reset_awaitable::await_suspend(std::coroutine_handle<> h) {
        if (signaled_.load(std::memory_order_acquire)) {
            return false;
        }
        auto* backend = CoroutineBackend::current_backend();
        if (backend == nullptr) {
            return false;
        }
        {
            std::lock_guard lock(waiters_mutex_);
            if (signaled_.load(std::memory_order_acquire)) {
                return false;
            }
            if (backend_ == nullptr) {
                backend_ = backend;
            }
            waiters_.push_back(h.address());
        }
        backend->suspend(h);
        return true;
    }

    inline bool sleep_for_awaitable::await_suspend(std::coroutine_handle<> h) {
        if (auto* backend = CoroutineBackend::current_backend(); backend != nullptr) {
            backend->schedule_after(h, duration);
            return true;
        }
        return false;
    }

    inline bool cancellation_point_awaitable::await_suspend(std::coroutine_handle<> h) {
        if (auto* backend = CoroutineBackend::current_backend(); backend != nullptr) {
            if (auto* token = CoroutineBackend::current_cancellation_token();
                token != nullptr && token->stop_requested()) {
                return false;
            }
            backend->suspend(h);
            backend->mark_ready(h);
            return true;
        }
        return false;
    }

    inline void cancellation_point_awaitable::await_resume() {
        if (auto* token = CoroutineBackend::current_cancellation_token(); token != nullptr && token->stop_requested()) {
            throw PravahaError{ErrorKind::TaskCanceled, "cancellation point canceled"};
        }
        if (auto* backend = CoroutineBackend::current_backend(); backend != nullptr && backend->stopped()) {
            throw PravahaError{ErrorKind::TaskCanceled, "cancellation point canceled"};
        }
    }

    inline void manual_reset_awaitable::signal() {
        signaled_.store(true, std::memory_order_release);
        std::deque<void*> ready_waiters;
        CoroutineBackend* backend = nullptr;
        {
            std::lock_guard lock(waiters_mutex_);
            ready_waiters.swap(waiters_);
            backend = backend_;
        }
        if (backend == nullptr) {
            return;
        }
        for (void* address : ready_waiters) {
            backend->mark_ready(std::coroutine_handle<>::from_address(address));
        }
    }

    template <class F>
    auto awaitable_adapter(F&& callable) {
        using Fn = std::decay_t<F>;
        if constexpr (detail::callable_traits<Fn>::arity == 0) {
            return [fn = std::forward<F>(callable)]() mutable {
                auto awaitable = std::invoke(fn);
                auto run_result = [&]() {
                    if (auto* backend = CoroutineBackend::current_backend(); backend != nullptr) {
                        return run_awaitable_with_backend(*backend, std::move(awaitable));
                    }
                    return run_awaitable(std::move(awaitable));
                }();
                return collapse_awaitable_execution(std::move(run_result));
            };
        }
        else {
            using Arg = detail::callable_traits<Fn>::template arg_t<0>;
            return [fn = std::forward<F>(callable)](Arg arg) mutable {
                auto awaitable = std::invoke(fn, std::move(arg));
                auto run_result = [&]() {
                    if (auto* backend = CoroutineBackend::current_backend(); backend != nullptr) {
                        return run_awaitable_with_backend(*backend, std::move(awaitable));
                    }
                    return run_awaitable(std::move(awaitable));
                }();
                return collapse_awaitable_execution(std::move(run_result));
            };
        }
    }

    template <class F>
    [[nodiscard]] auto awaitable_task(std::string name, F&& callable) {
        return pravaha::task(std::move(name), awaitable_adapter(std::forward<F>(callable)));
    }
}
