#include "catch_amalgamated.hpp"
#include "pravaha/backends/coroutine.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
    struct RetryOncePolicy {
        static pravaha::RetryDecision on_failure(const pravaha::PravahaError&, std::size_t attempt_count,
                                                 std::size_t) noexcept {
            if (attempt_count == 0) {
                return pravaha::RetryDecision::RetryImmediate;
            }
            return pravaha::RetryDecision::FailFinal;
        }
    };

    struct ForceTimeoutPolicy {
        static bool on_timeout(std::chrono::nanoseconds) noexcept {
            return true;
        }
    };

    struct SuspendOnceAwaitable {
        std::atomic<int>* suspends{nullptr};
        std::atomic<int>* resumes{nullptr};
        std::atomic<void*>* handle{nullptr};

        bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> h) const noexcept {
            suspends->fetch_add(1, std::memory_order_relaxed);
            handle->store(h.address(), std::memory_order_release);
            return true;
        }

        void await_resume() const noexcept {
            resumes->fetch_add(1, std::memory_order_relaxed);
        }
    };

    struct StopControlledSuspendAwaitable {
        std::atomic<int>* suspended{nullptr};
        std::atomic<int>* resumed{nullptr};
        std::atomic<bool>* release_suspend{nullptr};

        bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(std::coroutine_handle<>) const noexcept {
            suspended->fetch_add(1, std::memory_order_acq_rel);
            while (!release_suspend->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        }

        void await_resume() const noexcept {
            resumed->fetch_add(1, std::memory_order_acq_rel);
        }
    };

    struct ManualReadyAwaitable {
        std::atomic<int>* suspended{nullptr};
        std::atomic<int>* resumed{nullptr};
        std::atomic<void*>* handle_address{nullptr};

        bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> h) const noexcept {
            suspended->fetch_add(1, std::memory_order_acq_rel);
            handle_address->store(h.address(), std::memory_order_release);
            return true;
        }

        void await_resume() const noexcept {
            resumed->fetch_add(1, std::memory_order_acq_rel);
        }
    };

    struct FrameDropProbe {
        std::atomic<int>* drops{nullptr};

        ~FrameDropProbe() {
            drops->fetch_add(1, std::memory_order_acq_rel);
        }
    };
}

TEST_CASE (



"CoroutineBackend API surface matches backend usage"
,
"[pravaha][coroutine_backend]"
)
 {
    static_assert(requires(pravaha::backends::CoroutineBackend backend, pravaha::TaskCommand cmd)
    {
        { backend.submit(std::move(cmd)) };
        { backend.request_stop() };
        { backend.wait_all() };
    });
}

TEST_CASE (



"Runner with CoroutineBackend executes simple task"
,
"[pravaha][coroutine_backend]"
)
 {
    std::atomic runs{0};
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto result = runner.submit(pravaha::task("a", [&runs]() -> pravaha::Unit {
        runs.fetch_add(1);
        return {};
    }));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(runs.load() == 1);
}

TEST_CASE (



"Runner with CoroutineBackend executes sequence"
,
"[pravaha][coroutine_backend]"
)
 {
    std::vector<int> order;
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::task("a", [&order]() -> pravaha::Unit {
        order.push_back(1);
        return {};
    }) | pravaha::task("b", [&order]() -> pravaha::Unit {
        order.push_back(2);
        return {};
    });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE (



"Runner with CoroutineBackend executes parallel"
,
"[pravaha][coroutine_backend]"
)
 {
    std::atomic a_runs{0};
    std::atomic b_runs{0};
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::task("a", [&a_runs]() -> pravaha::Unit {
        a_runs.fetch_add(1);
        return {};
    }) & pravaha::task("b", [&b_runs]() -> pravaha::Unit {
        b_runs.fetch_add(1);
        return {};
    });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(a_runs.load() == 1);
    REQUIRE(b_runs.load() == 1);
}

TEST_CASE (



"Runner with CoroutineBackend respects cancellation"
,
"[pravaha][coroutine_backend]"
)
 {
    std::atomic runs{0};
    pravaha::CancellationSource source;
    source.request_stop();

    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::task("a", [&runs]() -> pravaha::Unit {
        runs.fetch_add(1);
        return {};
    });

    auto result = runner.submit(std::move(expr), source.token());
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(runs.load() == 0);
}

TEST_CASE (



"Runner with CoroutineBackend respects retry"
,
"[pravaha][coroutine_backend]"
)
 {
    std::atomic attempts{0};

    pravaha::Runner<
        pravaha::backends::CoroutineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        pravaha::NoObserver,
        RetryOncePolicy> runner;

    auto expr = pravaha::with_retry<1>(pravaha::task("a", [&attempts]() -> pravaha::Outcome<pravaha::Unit> {
        const int current = attempts.fetch_add(1);
        if (current == 0) {
            return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "first failure"});
        }
        return pravaha::Unit{};
    }));

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(attempts.load() == 2);
}

TEST_CASE (



"Runner with CoroutineBackend preserves value flow"
,
"[pravaha][coroutine_backend]"
)
 {
    int observed = 0;
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::task("a", []() -> int {
        return 11;
    }) | pravaha::task("b", [&observed](int v) -> pravaha::Unit {
        observed = v;
        return {};
    });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 11);
}

TEST_CASE (



"CoroutineBackend drains submitted coroutine-shaped tasks synchronously"
,
"[pravaha][coroutine_backend][awaitable]"
)
 {
    int observed = 0;
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::backends::awaitable_task("a", []() -> pravaha::backends::AwaitableTask<int> {
        co_return 13;
    }) | pravaha::task("b", [&observed](int v) -> pravaha::Unit {
        observed = v;
        return {};
    });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 13);
}

TEST_CASE (



"CoroutineBackend awaitable task returning Outcome<int> executes"
,
"[pravaha][coroutine_backend][awaitable]"
)
 {
    int observed = 0;
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::backends::awaitable_task(
                    "a", []() -> pravaha::backends::AwaitableTask<pravaha::Outcome<int> > {
                        co_return pravaha::Outcome<int>{17};
                    }) | pravaha::task("b", [&observed](int v) -> pravaha::Unit {
                    observed = v;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 17);
}

TEST_CASE (



"CoroutineBackend awaitable adapter preserves value flow"
,
"[pravaha][coroutine_backend][awaitable]"
)
 {
    int observed = 0;
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::task("src", []() -> int {
        return 5;
    }) | pravaha::backends::awaitable_task("double", [](int v) -> pravaha::backends::AwaitableTask<int> {
        co_return v * 2;
    }) | pravaha::task("sink", [&observed](int v) -> pravaha::Unit {
        observed = v;
        return {};
    });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 10);
}

TEST_CASE (



"CoroutineBackend failed awaitable blocks downstream"
,
"[pravaha][coroutine_backend][awaitable]"
)
 {
    std::atomic sink_runs{0};
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::backends::awaitable_task(
                    "a", []() -> pravaha::backends::AwaitableTask<pravaha::Outcome<int> > {
                        co_return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "boom"});
                    }) | pravaha::task("sink", [&sink_runs](int) -> pravaha::Unit {
                    sink_runs.fetch_add(1, std::memory_order_acq_rel);
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->succeeded());
    REQUIRE(sink_runs.load(std::memory_order_acquire) == 0);
}

TEST_CASE (



"CoroutineBackend awaitable task respects cancellation"
,
"[pravaha][coroutine_backend][awaitable]"
)
 {
    std::atomic runs{0};
    pravaha::CancellationSource source;
    source.request_stop();

    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::backends::awaitable_task("a", [&runs]() -> pravaha::backends::AwaitableTask<int> {
        runs.fetch_add(1);
        co_return 1;
    });

    auto result = runner.submit(std::move(expr), source.token());
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(runs.load() == 0);
}

TEST_CASE (



"CoroutineBackend awaitable task respects retry"
,
"[pravaha][coroutine_backend][awaitable]"
)
 {
    std::atomic attempts{0};
    pravaha::Runner<
        pravaha::backends::CoroutineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        pravaha::NoObserver,
        RetryOncePolicy> runner;

    auto expr = pravaha::with_retry<1>(
        pravaha::backends::awaitable_task(
            "a", [&attempts]() -> pravaha::backends::AwaitableTask<pravaha::Outcome<pravaha::Unit> > {
                const int current = attempts.fetch_add(1);
                if (current == 0) {
                    co_return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "first failure"});
                }
                co_return pravaha::Outcome<pravaha::Unit>{pravaha::Unit{}};
            })
    );

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(attempts.load() == 2);
}

TEST_CASE (



"CoroutineBackend awaitable task respects timeout cancellation"
,
"[pravaha][coroutine_backend][awaitable]"
)
 {
    std::atomic runs{0};
    pravaha::Runner<
        pravaha::backends::CoroutineBackend,
        pravaha::DefaultGraphAlgorithmPolicy,
        pravaha::DefaultReadyPolicy,
        pravaha::DefaultNoProgressPolicy,
        pravaha::NoObserver,
        pravaha::NoRetryPolicy,
        ForceTimeoutPolicy> runner;

    auto expr = pravaha::with_timeout(
        std::chrono::nanoseconds{1},
        pravaha::backends::awaitable_task("a", [&runs]() -> pravaha::backends::AwaitableTask<int> {
            runs.fetch_add(1);
            co_return 1;
        })
    );

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(runs.load() == 0);
}

TEST_CASE (



"CoroutineBackend wait_all drains all submitted tasks"
,
"[pravaha][coroutine_backend][scheduling]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic runs{0};

    std::array<std::future<pravaha::Outcome<pravaha::Unit> >, 16> submissions;
    for (int i = 0; i < 16; ++i) {
        submissions[static_cast<std::size_t>(i)] = std::async(std::launch::async, [&backend, &runs]() {
            return backend.submit(pravaha::TaskCommand::make([&runs]() -> pravaha::Unit {
                runs.fetch_add(1);
                return {};
            }));
        });
    }

    for (auto &f: submissions) {
        auto submit_result = f.get();
        REQUIRE(submit_result.has_value());
    }

    backend.wait_all();
    REQUIRE(runs.load() == 16);
}

TEST_CASE (



"CoroutineBackend request_stop rejects new submissions while allowing in-flight completion"
,
"[pravaha][coroutine_backend][scheduling]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    backend.request_stop();

    auto rejected = backend.submit(pravaha::TaskCommand::make([]() -> pravaha::Unit { return {}; }));
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error().kind == pravaha::ErrorKind::QueueRejected);

    backend.wait_all();
}

TEST_CASE (



"CoroutineBackend worker loop executes multiple coroutine tasks cooperatively"
,
"[pravaha][coroutine_backend][scheduling][awaitable]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic entered{0};
    std::atomic resumed{0};

    auto make_cmd = [&]() {
        return pravaha::TaskCommand::make(
            pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
                entered.fetch_add(1, std::memory_order_acq_rel);
                co_await pravaha::backends::yield_once{};
                resumed.fetch_add(1, std::memory_order_acq_rel);
                co_return pravaha::Unit{};
            })
        );
    };

    auto first = backend.submit(make_cmd());
    auto second = backend.submit(make_cmd());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    backend.wait_all();

    REQUIRE(entered.load(std::memory_order_acquire) == 2);
    REQUIRE(resumed.load(std::memory_order_acquire) == 2);
}

TEST_CASE (



"CoroutineBackend wait_all blocks until suspended coroutine is explicitly marked ready"
,
"[pravaha][coroutine_backend][scheduling][awaitable]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic suspended{0};
    std::atomic resumed{0};
    std::atomic<void *> handle_address{nullptr};

    auto submit_result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            co_await ManualReadyAwaitable{&suspended, &resumed, &handle_address};
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(submit_result.has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (suspended.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(suspended.load(std::memory_order_acquire) == 1);

    auto waiter = std::async(std::launch::async, [&]() {
        backend.wait_all();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(waiter.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

    auto *addr = handle_address.load(std::memory_order_acquire);
    REQUIRE(addr != nullptr);
    backend.mark_ready(std::coroutine_handle<>::from_address(addr));

    REQUIRE(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    waiter.get();
    REQUIRE(resumed.load(std::memory_order_acquire) == 1);
}

TEST_CASE (



"CoroutineBackend awaitable any_success preserves join release semantics"
,
"[pravaha][coroutine_backend][awaitable][join]"
)
 {
    int observed = 0;
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::any_success_of(
                    pravaha::backends::awaitable_task(
                        "a", []() -> pravaha::backends::AwaitableTask<pravaha::Outcome<int> > {
                            co_return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
                        }),
                    pravaha::backends::awaitable_task("b", []() -> pravaha::backends::AwaitableTask<int> {
                        co_return 7;
                    })
                ) | pravaha::task("sink", [&observed](int v) -> pravaha::Unit {
                    observed = v;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 7);
}

TEST_CASE (



"CoroutineBackend awaitable quorum preserves join semantics"
,
"[pravaha][coroutine_backend][awaitable][join]"
)
 {
    int observed = 0;
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::quorum_of<1>(
                    pravaha::backends::awaitable_task("a", []() -> pravaha::backends::AwaitableTask<int> {
                        co_return 3;
                    }),
                    pravaha::backends::awaitable_task(
                        "b", []() -> pravaha::backends::AwaitableTask<pravaha::Outcome<int> > {
                            co_return std::unexpected(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "fail"});
                        })
                ) | pravaha::task("sink", [&observed](int v) -> pravaha::Unit {
                    observed = v;
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 3);
}

TEST_CASE (



"CoroutineBackend canceled awaitable branch is not resumed"
,
"[pravaha][coroutine_backend][awaitable][cancellation]"
)
 {
    std::atomic resumes{0};
    pravaha::CancellationSource source;
    source.request_stop();

    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::backends::awaitable_task("a", [&resumes]() -> pravaha::backends::AwaitableTask<int> {
        resumes.fetch_add(1);
        co_return 1;
    });

    auto result = runner.submit(std::move(expr), source.token());
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(resumes.load() == 0);
}

TEST_CASE (



"Awaitable adapter writes ResultSlot exactly once per invocation"
,
"[pravaha][coroutine_backend][awaitable][result_slot]"
)
 {
    auto cmd = pravaha::TaskCommand::make(pravaha::backends::awaitable_adapter(
        [](int v) -> pravaha::backends::AwaitableTask<int> {
            co_return v + 1;
        }
    ));

    pravaha::ResultSlot input;
    input.emplace<int>(41);
    pravaha::ResultSlot output;
    auto result = cmd.run(&output, &input);
    REQUIRE(result.has_value());
    auto *value = output.get_if<int>();
    REQUIRE(value != nullptr);
    REQUIRE(*value == 42);
}

TEST_CASE (



"AwaitableTask<int> exposes done resume and result"
,
"[pravaha][coroutine_backend][awaitable][task_type]"
)
 {
    auto make_task = []() -> pravaha::backends::AwaitableTask<int> {
        co_return 41;
    };

    auto task = make_task();
    REQUIRE_FALSE(task.done());
    task.resume();
    REQUIRE(task.done());
    auto result = task.result();
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 41);
}

TEST_CASE (



"AwaitableTask<Unit> result succeeds"
,
"[pravaha][coroutine_backend][awaitable][task_type]"
)
 {
    auto make_task = []() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
        co_return pravaha::Unit{};
    };

    auto task = make_task();
    while (!task.done()) {
        task.resume();
    }
    auto result = task.result();
    REQUIRE(result.has_value());
}

TEST_CASE (



"AwaitableTask<void> result succeeds"
,
"[pravaha][coroutine_backend][awaitable][task_type]"
)
 {
    auto make_task = []() -> pravaha::backends::AwaitableTask<void> {
        co_return;
    };

    auto task = make_task();
    while (!task.done()) {
        task.resume();
    }
    auto result = task.result();
    REQUIRE(result.has_value());
}

TEST_CASE (



"AwaitableTask move construction transfers handle ownership"
,
"[pravaha][coroutine_backend][awaitable][task_type]"
)
 {
    auto make_task = []() -> pravaha::backends::AwaitableTask<int> {
        co_return 12;
    };

    auto original = make_task();
    auto moved = std::move(original);
    REQUIRE(original.native_handle() == nullptr);
    while (!moved.done()) {
        moved.resume();
    }
    auto result = moved.result();
    REQUIRE(result.has_value());
    REQUIRE(result.value() == 12);
}

TEST_CASE (



"AwaitableTask moved owner destroys frame once"
,
"[pravaha][coroutine_backend][awaitable][task_type]"
)
 {
    std::atomic drops{0};
    {
        auto make_task = [&]() -> pravaha::backends::AwaitableTask<int> {
            FrameDropProbe probe{&drops};
            co_return 7;
        };

        auto original = make_task();
        auto moved = std::move(original);
        while (!moved.done()) {
            moved.resume();
        }
        auto result = moved.result();
        REQUIRE(result.has_value());
    }
    REQUIRE(drops.load(std::memory_order_acquire) == 1);
}

TEST_CASE (



"AwaitableTask exception maps to failed Outcome"
,
"[pravaha][coroutine_backend][awaitable][task_type]"
)
 {
    auto make_task = []() -> pravaha::backends::AwaitableTask<int> {
        throw std::runtime_error("boom");
        co_return 0;
    };

    auto task = make_task();
    while (!task.done()) {
        task.resume();
    }
    auto result = task.result();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::TaskFailed);
}

TEST_CASE (



"CoroutineBackend awaitable suspends once and resumes after mark_ready"
,
"[pravaha][coroutine_backend][awaitable][suspended]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic suspended{0};
    std::atomic resumed{0};
    std::atomic continuation{0};
    std::atomic<void *> handle_address{nullptr};

    auto submission = std::async(std::launch::async, [&]() {
        return backend.submit(pravaha::TaskCommand::make(
            pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
                co_await ManualReadyAwaitable{&suspended, &resumed, &handle_address};
                continuation.fetch_add(1, std::memory_order_acq_rel);
                co_return pravaha::Unit{};
            })
        ));
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (suspended.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(suspended.load(std::memory_order_acquire) == 1);
    REQUIRE(resumed.load(std::memory_order_acquire) == 0);
    REQUIRE(continuation.load(std::memory_order_acquire) == 0);
    auto *addr = handle_address.load(std::memory_order_acquire);
    REQUIRE(addr != nullptr);

    backend.mark_ready(std::coroutine_handle<>::from_address(addr));

    REQUIRE(submission.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto submit_result = submission.get();
    REQUIRE(submit_result.has_value());
    backend.wait_all();

    REQUIRE(resumed.load(std::memory_order_acquire) == 1);
    REQUIRE(continuation.load(std::memory_order_acquire) == 1);
}

TEST_CASE (



"CoroutineBackend wait_all does not spin on permanently suspended coroutine"
,
"[pravaha][coroutine_backend][scheduling][awaitable][suspended]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic suspends{0};
    std::atomic resumes{0};
    std::atomic continuation{0};
    std::atomic<void *> handle_address{nullptr};

    auto submission = std::async(std::launch::async, [&]() {
        return backend.submit(pravaha::TaskCommand::make(
            pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
                co_await ManualReadyAwaitable{&suspends, &resumes, &handle_address};
                continuation.fetch_add(1, std::memory_order_acq_rel);
                co_return pravaha::Unit{};
            })
        ));
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (suspends.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    backend.request_stop();

    REQUIRE(submission.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto submit_result = submission.get();
    REQUIRE(submit_result.has_value());

    auto waiter = std::async(std::launch::async, [&]() {
        backend.wait_all();
    });

    REQUIRE(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    waiter.get();

    REQUIRE(suspends.load(std::memory_order_acquire) == 1);
    REQUIRE(resumes.load(std::memory_order_acquire) == 0);
    REQUIRE(continuation.load(std::memory_order_acquire) == 0);
    REQUIRE(handle_address.load(std::memory_order_acquire) != nullptr);
}

TEST_CASE (



"CoroutineBackend suspend-once awaitable preserves value result"
,
"[pravaha][coroutine_backend][awaitable][suspended]"
)
 {
    int observed = 0;
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::backends::awaitable_task("a", []() -> pravaha::backends::AwaitableTask<int> {
        co_await pravaha::backends::yield_once{};
        co_return 37;
    }) | pravaha::task("sink", [&observed](int v) -> pravaha::Unit {
        observed = v;
        return {};
    });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 37);
}

TEST_CASE (



"CoroutineBackend yield_now suspends and resumes through backend readiness"
,
"[pravaha][coroutine_backend][awaitable][suspended]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic stage{0};

    auto submit_result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            stage.store(1, std::memory_order_release);
            co_await pravaha::backends::yield_now{};
            stage.store(2, std::memory_order_release);
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(submit_result.has_value());

    backend.wait_all();

    REQUIRE(stage.load(std::memory_order_acquire) == 2);
}

TEST_CASE (



"CoroutineBackend suspend_once suspends only once"
,
"[pravaha][coroutine_backend][awaitable][suspended]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic suspends{0};

    auto submit_result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<int> {
            pravaha::backends::suspend_once once;
            co_await once;
            suspends.fetch_add(1, std::memory_order_acq_rel);
            co_await once;
            suspends.fetch_add(1, std::memory_order_acq_rel);
            co_return 1;
        })
    ));
    REQUIRE(submit_result.has_value());

    backend.wait_all();

    REQUIRE(suspends.load(std::memory_order_acquire) == 2);
}

TEST_CASE (



"CoroutineBackend manual_reset_awaitable resumes only after explicit signal"
,
"[pravaha][coroutine_backend][awaitable][suspended]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    pravaha::backends::manual_reset_awaitable gate;
    std::atomic resumed{0};

    auto submit_result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            co_await gate;
            resumed.fetch_add(1, std::memory_order_acq_rel);
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(submit_result.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(resumed.load(std::memory_order_acquire) == 0);

    gate.signal();
    backend.wait_all();

    REQUIRE(resumed.load(std::memory_order_acquire) == 1);
}

TEST_CASE (



"CoroutineBackend manual_reset_awaitable resumes multiple suspended tasks deterministically"
,
"[pravaha][coroutine_backend][awaitable][suspended]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    pravaha::backends::manual_reset_awaitable gate;
    std::vector<int> order;
    std::atomic registrations{0};

    auto first = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            registrations.fetch_add(1, std::memory_order_acq_rel);
            co_await gate;
            order.push_back(1);
            co_return pravaha::Unit{};
        })
    ));

    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (registrations.load(std::memory_order_acquire) < 1 && std::chrono::steady_clock::now() < first_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto second = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            registrations.fetch_add(1, std::memory_order_acq_rel);
            co_await gate;
            order.push_back(2);
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(registrations.load(std::memory_order_acquire) == 2);
    REQUIRE(order.empty());

    gate.signal();
    backend.wait_all();

    REQUIRE(order == std::vector<int>{2, 1});
}

TEST_CASE (



"CoroutineBackend async_channel sender wakes suspended receiver"
,
"[pravaha][coroutine_backend][awaitable][channel]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    pravaha::backends::async_channel<int> channel;
    std::atomic received{-1};

    auto receiver_submit = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            int v = co_await channel.recv();
            received.store(v, std::memory_order_release);
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(receiver_submit.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(received.load(std::memory_order_acquire) == -1);

    auto send_result = channel.send(42);
    REQUIRE(send_result.has_value());

    backend.wait_all();
    REQUIRE(received.load(std::memory_order_acquire) == 42);
}

TEST_CASE (



"CoroutineBackend async_channel receiver suspends when empty"
,
"[pravaha][coroutine_backend][awaitable][channel]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    pravaha::backends::async_channel<int> channel;
    std::atomic resumed{0};

    auto receiver_submit = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            int v = co_await channel.recv();
            if (v == 7) {
                resumed.fetch_add(1, std::memory_order_acq_rel);
            }
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(receiver_submit.has_value());

    auto waiter = std::async(std::launch::async, [&]() {
        backend.wait_all();
    });

    REQUIRE(waiter.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);

    auto send_result = channel.send(7);
    REQUIRE(send_result.has_value());

    REQUIRE(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    waiter.get();
    REQUIRE(resumed.load(std::memory_order_acquire) == 1);
}

TEST_CASE (



"CoroutineBackend async_channel supports multiple producers and consumers"
,
"[pravaha][coroutine_backend][awaitable][channel]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    pravaha::backends::async_channel<int> channel;
    std::atomic sum{0};
    std::atomic consumed{0};

    auto consumer_a = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            int local = 0;
            for (int i = 0; i < 3; ++i) {
                local += co_await channel.recv();
            }
            sum.fetch_add(local, std::memory_order_acq_rel);
            consumed.fetch_add(3, std::memory_order_acq_rel);
            co_return pravaha::Unit{};
        })
    ));
    auto consumer_b = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            int local = 0;
            for (int i = 0; i < 3; ++i) {
                local += co_await channel.recv();
            }
            sum.fetch_add(local, std::memory_order_acq_rel);
            consumed.fetch_add(3, std::memory_order_acq_rel);
            co_return pravaha::Unit{};
        })
    ));
    auto producer_a = backend.submit(pravaha::TaskCommand::make(
        [&channel]() -> pravaha::Unit {
            for (int v: std::vector{1, 2, 3}) {
                auto send_result = channel.send(v);
                if (!send_result.has_value()) {
                    throw std::runtime_error("send failed");
                }
            }
            return {};
        }
    ));
    auto producer_b = backend.submit(pravaha::TaskCommand::make(
        [&channel]() -> pravaha::Unit {
            for (int v: std::vector{10, 20, 30}) {
                auto send_result = channel.send(v);
                if (!send_result.has_value()) {
                    throw std::runtime_error("send failed");
                }
            }
            return {};
        }
    ));

    REQUIRE(consumer_a.has_value());
    REQUIRE(consumer_b.has_value());
    REQUIRE(producer_a.has_value());
    REQUIRE(producer_b.has_value());

    backend.wait_all();

    REQUIRE(consumed.load(std::memory_order_acquire) == 6);
    REQUIRE(sum.load(std::memory_order_acquire) == 66);
}

TEST_CASE (



"CoroutineBackend async_channel shutdown cleanup is safe"
,
"[pravaha][coroutine_backend][awaitable][channel][cancellation]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic resumed{0};

    {
        pravaha::backends::async_channel<int> channel;
        auto receiver_submit = backend.submit(pravaha::TaskCommand::make(
            pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
                int v = co_await channel.recv();
                resumed.fetch_add(v, std::memory_order_acq_rel);
                co_return pravaha::Unit{};
            })
        ));
        REQUIRE(receiver_submit.has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        backend.request_stop();
    }

    auto waiter = std::async(std::launch::async, [&]() {
        backend.wait_all();
    });
    REQUIRE(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    waiter.get();

    REQUIRE(resumed.load(std::memory_order_acquire) == 0);
}

TEST_CASE (



"CoroutineBackend sleep_for resumes after delay"
,
"[pravaha][coroutine_backend][awaitable][timer]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic<long long> elapsed_ms{0};
    std::atomic resumed{0};

    auto submit_result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            auto start = std::chrono::steady_clock::now();
            co_await pravaha::backends::sleep_for(std::chrono::milliseconds(30));
            auto end = std::chrono::steady_clock::now();
            elapsed_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(),
                             std::memory_order_release);
            resumed.fetch_add(1, std::memory_order_acq_rel);
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(submit_result.has_value());

    backend.wait_all();

    REQUIRE(resumed.load(std::memory_order_acquire) == 1);
    REQUIRE(elapsed_ms.load(std::memory_order_acquire) >= 20);
}

TEST_CASE (



"CoroutineBackend sleep_for wakes multiple timers in order"
,
"[pravaha][coroutine_backend][awaitable][timer]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::vector<int> order;

    auto first = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            co_await pravaha::backends::sleep_for(std::chrono::milliseconds(40));
            order.push_back(2);
            co_return pravaha::Unit{};
        })
    ));
    auto second = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            co_await pravaha::backends::sleep_for(std::chrono::milliseconds(10));
            order.push_back(1);
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    backend.wait_all();

    REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE (



"CoroutineBackend wait_all blocks until sleeping coroutine wakes"
,
"[pravaha][coroutine_backend][awaitable][timer]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic resumed{0};

    auto submit_result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            co_await pravaha::backends::sleep_for(std::chrono::milliseconds(60));
            resumed.fetch_add(1, std::memory_order_acq_rel);
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(submit_result.has_value());

    auto waiter = std::async(std::launch::async, [&]() {
        backend.wait_all();
    });

    REQUIRE(waiter.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    REQUIRE(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    waiter.get();

    REQUIRE(resumed.load(std::memory_order_acquire) == 1);
}

TEST_CASE (



"CoroutineBackend request_stop cancels sleeping coroutine safely"
,
"[pravaha][coroutine_backend][awaitable][timer][cancellation]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic resumed{0};

    auto submit_result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            co_await pravaha::backends::sleep_for(std::chrono::milliseconds(500));
            resumed.fetch_add(1, std::memory_order_acq_rel);
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(submit_result.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    backend.request_stop();

    auto waiter = std::async(std::launch::async, [&]() {
        backend.wait_all();
    });
    REQUIRE(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    waiter.get();

    REQUIRE(resumed.load(std::memory_order_acquire) == 0);
}

TEST_CASE (



"CoroutineBackend canceled suspended coroutine never resumes"
,
"[pravaha][coroutine_backend][awaitable][cancellation]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic resumed{0};

    auto submit_result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            co_await pravaha::backends::sleep_for(std::chrono::milliseconds(300));
            resumed.fetch_add(1, std::memory_order_acq_rel);
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(submit_result.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    backend.request_stop();
    backend.wait_all();

    REQUIRE(resumed.load(std::memory_order_acquire) == 0);
}

TEST_CASE (



"CoroutineBackend cancellation_point exits task on cancellation"
,
"[pravaha][coroutine_backend][awaitable][cancellation]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic suspended{0};
    std::atomic resumed{0};
    std::atomic<void *> handle_address{nullptr};
    std::atomic after_point{0};

    auto submit_result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            co_await ManualReadyAwaitable{&suspended, &resumed, &handle_address};
            co_await pravaha::backends::cancellation_point();
            after_point.fetch_add(1, std::memory_order_acq_rel);
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(submit_result.has_value());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (suspended.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(suspended.load(std::memory_order_acquire) == 1);

    auto *addr = handle_address.load(std::memory_order_acquire);
    REQUIRE(addr != nullptr);

    backend.request_stop();
    backend.mark_ready(std::coroutine_handle<>::from_address(addr));
    backend.wait_all();

    REQUIRE(resumed.load(std::memory_order_acquire) == 0);
    REQUIRE(after_point.load(std::memory_order_acquire) == 0);
}

TEST_CASE (



"CoroutineBackend cancellation_point preserves downstream skip semantics"
,
"[pravaha][coroutine_backend][awaitable][cancellation]"
)
 {
    std::atomic sink_runs{0};
    pravaha::CancellationSource source;
    source.request_stop();
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;

    auto expr = pravaha::backends::awaitable_task("cancel", []() -> pravaha::backends::AwaitableTask<int> {
        co_await pravaha::backends::cancellation_point();
        co_return 7;
    }) | pravaha::task("sink", [&sink_runs](int) -> pravaha::Unit {
        sink_runs.fetch_add(1, std::memory_order_acq_rel);
        return {};
    });

    auto result = runner.submit(std::move(expr), source.token());
    REQUIRE(result.has_value());
    REQUIRE(result->final_state == pravaha::TaskState::Canceled);
    REQUIRE(sink_runs.load(std::memory_order_acquire) == 0);
}


TEST_CASE (



"CoroutineBackend cancellation does not leak suspended coroutine frames"
,
"[pravaha][coroutine_backend][awaitable][cancellation]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic drops{0};

    auto submit_result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
            FrameDropProbe probe{&drops};
            co_await pravaha::backends::sleep_for(std::chrono::milliseconds(300));
            co_return pravaha::Unit{};
        })
    ));
    REQUIRE(submit_result.has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    backend.request_stop();
    backend.wait_all();

    REQUIRE(drops.load(std::memory_order_acquire) == 1);
}

TEST_CASE (



"CoroutineBackend wait_all resumes multiple marked-ready coroutine handles"
,
"[pravaha][coroutine_backend][scheduling][awaitable]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic suspends{0};
    std::atomic resumes{0};
    std::atomic completed{0};
    std::atomic<void *> first_handle_address{nullptr};
    std::atomic<void *> second_handle_address{nullptr};

    auto first_submission = std::async(std::launch::async, [&]() {
        return backend.submit(pravaha::TaskCommand::make(
            pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
                co_await ManualReadyAwaitable{&suspends, &resumes, &first_handle_address};
                completed.fetch_add(1, std::memory_order_acq_rel);
                co_return pravaha::Unit{};
            })
        ));
    });

    auto second_submission = std::async(std::launch::async, [&]() {
        return backend.submit(pravaha::TaskCommand::make(
            pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
                co_await ManualReadyAwaitable{&suspends, &resumes, &second_handle_address};
                completed.fetch_add(1, std::memory_order_acq_rel);
                co_return pravaha::Unit{};
            })
        ));
    });

    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (first_handle_address.load(std::memory_order_acquire) == nullptr
           && std::chrono::steady_clock::now() < first_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto *first_addr = first_handle_address.load(std::memory_order_acquire);
    REQUIRE(first_addr != nullptr);
    REQUIRE(completed.load(std::memory_order_acquire) == 0);

    backend.mark_ready(std::coroutine_handle<>::from_address(first_addr));

    const auto second_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (second_handle_address.load(std::memory_order_acquire) == nullptr
           && std::chrono::steady_clock::now() < second_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto *second_addr = second_handle_address.load(std::memory_order_acquire);
    REQUIRE(second_addr != nullptr);
    backend.mark_ready(std::coroutine_handle<>::from_address(second_addr));

    REQUIRE(first_submission.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    REQUIRE(second_submission.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto first_result = first_submission.get();
    auto second_result = second_submission.get();
    REQUIRE(first_result.has_value());
    REQUIRE(second_result.has_value());
    backend.wait_all();

    REQUIRE(suspends.load(std::memory_order_acquire) == 2);
    REQUIRE(resumes.load(std::memory_order_acquire) == 2);
    REQUIRE(completed.load(std::memory_order_acquire) == 2);
}

TEST_CASE (



"CoroutineBackend request_stop clears ready and suspended handles"
,
"[pravaha][coroutine_backend][scheduling][awaitable][cancellation]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic suspended{0};
    std::atomic resumed{0};
    std::atomic continuation{0};
    std::atomic<void *> handle_address{nullptr};

    auto submission = std::async(std::launch::async, [&]() {
        return backend.submit(pravaha::TaskCommand::make(
            pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<pravaha::Unit> {
                co_await ManualReadyAwaitable{&suspended, &resumed, &handle_address};
                continuation.fetch_add(1, std::memory_order_acq_rel);
                co_return pravaha::Unit{};
            })
        ));
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (suspended.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto *addr = handle_address.load(std::memory_order_acquire);
    REQUIRE(addr != nullptr);

    backend.mark_ready(std::coroutine_handle<>::from_address(addr));
    backend.request_stop();

    REQUIRE(submission.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto submit_result = submission.get();
    REQUIRE(submit_result.has_value());

    auto waiter = std::async(std::launch::async, [&]() {
        backend.wait_all();
    });
    REQUIRE(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    waiter.get();

    REQUIRE(resumed.load(std::memory_order_acquire) == 0);
    REQUIRE(continuation.load(std::memory_order_acquire) == 0);
}

TEST_CASE (



"CoroutineBackend cancellation before resumption is detected for awaitable adapter"
,
"[pravaha][coroutine_backend][scheduling][awaitable][cancellation]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic ran{0};
    backend.request_stop();

    auto result = backend.submit(pravaha::TaskCommand::make(
        pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<int> {
            ran.fetch_add(1);
            co_await std::suspend_always{};
            co_return 1;
        })
    ));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == pravaha::ErrorKind::QueueRejected);
    REQUIRE(ran.load() == 0);
}

TEST_CASE (



"CoroutineBackend request_stop prevents suspended continuation resumption"
,
"[pravaha][coroutine_backend][scheduling][awaitable][cancellation]"
)
 {
    pravaha::backends::CoroutineBackend backend;
    std::atomic suspended{0};
    std::atomic resumed{0};
    std::atomic continuation_runs{0};
    std::atomic release_suspend{false};

    auto submission = std::async(std::launch::async, [&]() {
        return backend.submit(pravaha::TaskCommand::make(
            pravaha::backends::awaitable_adapter([&]() -> pravaha::backends::AwaitableTask<int> {
                co_await StopControlledSuspendAwaitable{&suspended, &resumed, &release_suspend};
                continuation_runs.fetch_add(1, std::memory_order_acq_rel);
                co_return 9;
            })
        ));
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (suspended.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(suspended.load(std::memory_order_acquire) == 1);

    backend.request_stop();
    release_suspend.store(true, std::memory_order_release);

    REQUIRE(submission.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto submit_result = submission.get();
    REQUIRE(submit_result.has_value());

    auto waiter = std::async(std::launch::async, [&]() {
        backend.wait_all();
    });
    REQUIRE(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    waiter.get();

    REQUIRE(resumed.load(std::memory_order_acquire) == 0);
    REQUIRE(continuation_runs.load(std::memory_order_acquire) == 0);

    backend.wait_all();
    REQUIRE(continuation_runs.load(std::memory_order_acquire) == 0);
}

TEST_CASE (



"CoroutineBackend runs multiple awaitables"
,
"[pravaha][coroutine_backend][awaitable]"
)
 {
    int observed = 0;
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::backends::awaitable_task("a", []() -> pravaha::backends::AwaitableTask<int> {
        co_return 2;
    }) | pravaha::backends::awaitable_task("b", [](int v) -> pravaha::backends::AwaitableTask<int> {
        co_return v + 3;
    }) | pravaha::backends::awaitable_task("c", [](int v) -> pravaha::backends::AwaitableTask<int> {
        co_return v * 4;
    }) | pravaha::task("sink", [&observed](int v) -> pravaha::Unit {
        observed = v;
        return {};
    });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == 20);
}

TEST_CASE (



"CoroutineBackend parallel awaitables preserve join and value-flow"
,
"[pravaha][coroutine_backend][awaitable][join]"
)
 {
    std::vector<int> observed;
    pravaha::Runner<pravaha::backends::CoroutineBackend> runner;
    auto expr = pravaha::collect_all_of(
                    pravaha::backends::awaitable_task("a", []() -> pravaha::backends::AwaitableTask<int> {
                        co_return 1;
                    }),
                    pravaha::backends::awaitable_task("b", []() -> pravaha::backends::AwaitableTask<int> {
                        co_return 2;
                    })
                ) | pravaha::task("sink", [&observed](std::vector<int> values) -> pravaha::Unit {
                    observed = std::move(values);
                    return {};
                });

    auto result = runner.submit(std::move(expr));
    REQUIRE(result.has_value());
    REQUIRE(result->succeeded());
    REQUIRE(observed == std::vector<int>{1, 2});
}
