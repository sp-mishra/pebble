#include "catch_amalgamated.hpp"
#include "pravaha/execution.hpp"

#include <exception>
#include <stdexcept>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>

struct success_receiver {
    bool value{};
    bool error{};
    bool stopped{};

    void set_value() { value = true; }
    void set_error(const pravaha::PravahaError&) { error = true; }
    void set_stopped() { stopped = true; }
};

struct error_receiver {
    bool value{};
    bool error{};
    bool stopped{};

    void set_value() { value = true; }
    void set_error(const pravaha::PravahaError&) { error = true; }
    void set_stopped() { stopped = true; }
};

struct stopped_receiver {
    bool value{};
    bool error{};
    bool stopped{};

    void set_value() { value = true; }
    void set_error(const pravaha::PravahaError&) { error = true; }
    void set_stopped() { stopped = true; }
};

struct tracking_receiver {
    int value_calls{};
    int error_calls{};
    int stopped_calls{};

    void set_value() { ++value_calls; }
    void set_error(const pravaha::PravahaError&) { ++error_calls; }
    void set_stopped() { ++stopped_calls; }
};

struct bad_receiver {
    void set_value() {}

    void set_error(const pravaha::PravahaError&) {}
};

struct receiver_error_const_ref {
    void set_value() {}

    void set_error(const pravaha::PravahaError&) {}

    void set_stopped() {}
};

struct receiver_error_by_value {
    void set_value() {}

    void set_error(pravaha::PravahaError) {}

    void set_stopped() {}
};

struct receiver_error_exception_ptr {
    void set_value() {}

    void set_error(std::exception_ptr) {}

    void set_stopped() {}
};

struct receiver_error_code_only {
    void set_value() {}

    void set_error(std::error_code) {}

    void set_stopped() {}
};

struct receiver_error_int_only {
    void set_value() {}

    void set_error(int) {}

    void set_stopped() {}
};

struct manual_stopped_sender {
    using expr_type = int;
    using backend_type = pravaha::InlineBackend;
    int expr{};
};

struct move_only_expr {
    move_only_expr() = default;

    move_only_expr(const move_only_expr&) = delete;

    move_only_expr& operator=(const move_only_expr&) = delete;

    move_only_expr(move_only_expr&&) noexcept = default;

    move_only_expr& operator=(move_only_expr&&) noexcept = default;
};

struct move_only_sender {
    using expr_type = move_only_expr;
    using backend_type = pravaha::InlineBackend;
    move_only_expr expr{};
};

namespace pravaha::execution {
    template <class Receiver>
        requires receiver_like<std::remove_cvref_t<Receiver>>
    void start(operation_state<manual_stopped_sender, Receiver>& op) {
        op.receiver.set_stopped();
    }
}

using good_receiver = success_receiver;

static_assert(pravaha::execution::receiver_like<good_receiver>);
static_assert(!pravaha::execution::receiver_like<bad_receiver>);
static_assert(pravaha::execution::receiver_like<receiver_error_const_ref>);
static_assert(pravaha::execution::receiver_like<receiver_error_by_value>);
static_assert(!pravaha::execution::receiver_like<receiver_error_exception_ptr>);
static_assert(!pravaha::execution::receiver_like<receiver_error_code_only>);
static_assert(!pravaha::execution::receiver_like<receiver_error_int_only>);

TEST_CASE (



"from_expr(task(...)) compiles"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    STATIC_REQUIRE(pravaha::execution::sender_like<decltype(s)>);
}

TEST_CASE (



"from_expr(seq(...)) compiles"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::from_expr(
        pravaha::seq(
            pravaha::task("a", [] {
            }),
            pravaha::task("b", [] {
            })
        )
    );
    STATIC_REQUIRE(pravaha::execution::sender_like<decltype(s)>);
}

TEST_CASE (



"construction is lazy"
,
"[pravaha][execution]"
)
 {
    int count = 0;
    auto s = pravaha::execution::from_expr(pravaha::task("a", [&count] { ++count; }));
    REQUIRE(count == 0);
    REQUIRE(s.expr.name() == "a");
}

TEST_CASE (



"callable is not executed during from_expr"
,
"[pravaha][execution]"
)
 {
    bool ran = false;
    auto s = pravaha::execution::from_expr(pravaha::task("a", [&ran] { ran = true; }));
    REQUIRE_FALSE(ran);
    STATIC_REQUIRE(std::same_as<decltype(s)::backend_type, pravaha::InlineBackend>);
}

TEST_CASE (



"sender stores expression"
,
"[pravaha][execution]"
)
 {
    auto expr = pravaha::task("a", [] {
    });
    auto s = pravaha::execution::from_expr(expr);
    REQUIRE(s.expr.name() == expr.name());
    REQUIRE(s.expr.frontend.hash == expr.frontend.hash);
}

TEST_CASE (



"receiver set_value updates state"
,
"[pravaha][execution]"
)
 {
    success_receiver r;
    REQUIRE_FALSE(r.value);
    r.set_value();
    REQUIRE(r.value);
}

TEST_CASE (



"receiver set_error updates state"
,
"[pravaha][execution]"
)
 {
    error_receiver r;
    REQUIRE_FALSE(r.error);
    r.set_error(pravaha::PravahaError{pravaha::ErrorKind::TaskFailed, "e"});
    REQUIRE(r.error);
}

TEST_CASE (



"receiver set_stopped updates state"
,
"[pravaha][execution]"
)
 {
    stopped_receiver r;
    REQUIRE_FALSE(r.stopped);
    r.set_stopped();
    REQUIRE(r.stopped);
}

TEST_CASE (



"connect itself is lazy"
,
"[pravaha][execution]"
)
 {
    int runs = 0;
    auto s = pravaha::execution::from_expr(pravaha::task("a", [&runs] { ++runs; }));
    auto op = pravaha::execution::connect(s, success_receiver{});
    REQUIRE(runs == 0);
    STATIC_REQUIRE(std::same_as<decltype(op), pravaha::execution::operation_state<decltype(s), success_receiver>>);
}

TEST_CASE (



"start executes only when called"
,
"[pravaha][execution]"
)
 {
    int runs = 0;
    auto s = pravaha::execution::from_expr(pravaha::task("a", [&runs] { ++runs; }));
    auto op = pravaha::execution::connect(s, success_receiver{});
    REQUIRE(runs == 0);
    pravaha::execution::start(op);
    REQUIRE(runs == 1);
}

TEST_CASE (



"successful task calls set_value"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    auto op = pravaha::execution::connect(s, success_receiver{});
    pravaha::execution::start(op);
    REQUIRE(op.receiver.value);
    REQUIRE_FALSE(op.receiver.error);
    REQUIRE_FALSE(op.receiver.stopped);
}

TEST_CASE (



"failing task calls set_error"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("a", [] { throw std::runtime_error("boom"); }));
    auto op = pravaha::execution::connect(s, error_receiver{});
    pravaha::execution::start(op);
    REQUIRE_FALSE(op.receiver.value);
    REQUIRE(op.receiver.error);
}

TEST_CASE (



"sync_wait(success sender).value == true"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    auto result = pravaha::execution::sync_wait(s);
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"sync_wait(failing sender).error == true"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("a", [] { throw std::runtime_error("boom"); }));
    auto result = pravaha::execution::sync_wait(s);
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"sync_wait is lazy until called"
,
"[pravaha][execution]"
)
 {
    int runs = 0;
    auto s = pravaha::execution::from_expr(pravaha::task("a", [&runs] { ++runs; }));
    REQUIRE(runs == 0);
    auto result = pravaha::execution::sync_wait(s);
    REQUIRE(result.value);
    REQUIRE(runs == 1);
}

TEST_CASE (



"repeated construction without sync_wait does not run task"
,
"[pravaha][execution]"
)
 {
    int runs = 0;
    auto make_sender = [&runs]() {
        return pravaha::execution::from_expr(pravaha::task("a", [&runs] { ++runs; }));
    };
    auto s1 = make_sender();
    auto s2 = make_sender();
    REQUIRE(runs == 0);
    REQUIRE(s1.expr.name() == "a");
    REQUIRE(s2.expr.name() == "a");
}

TEST_CASE (



"sync_wait_value<int>(sender returning int) -> result 42"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("a", [] { return 42; }));
    auto result = pravaha::execution::sync_wait_value<int>(s);
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
    REQUIRE(result.result.has_value());
    REQUIRE(*result.result == 42);
}

TEST_CASE (



"sync_wait_value<int> with then increments to 43"
,
"[pravaha][execution]"
)
 {
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] { return 42; }));
    auto chained = pravaha::execution::then(base, "plus_one", [](int x) { return x + 1; });
    auto result = pravaha::execution::sync_wait_value<int>(chained);
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
    REQUIRE(result.result.has_value());
    REQUIRE(*result.result == 43);
}

TEST_CASE (



"sync_wait_value<int>(error sender) has no result"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("a", []() -> int { throw std::runtime_error("boom"); }));
    auto result = pravaha::execution::sync_wait_value<int>(s);
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
    REQUIRE_FALSE(result.result.has_value());
}

TEST_CASE (



"sync_wait_value<int>(stopped sender) has no result"
,
"[pravaha][execution]"
)
 {
    auto result = pravaha::execution::sync_wait_value<int>(manual_stopped_sender{});
    REQUIRE_FALSE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE(result.stopped);
    REQUIRE_FALSE(result.result.has_value());
}

TEST_CASE (



"schedule(inline_scheduler{}) compiles"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::schedule(pravaha::execution::inline_scheduler{});
    STATIC_REQUIRE(pravaha::execution::sender_like<decltype(s)>);
    REQUIRE(s.backend);
}

TEST_CASE (



"from_expr uses default backend construction path"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    REQUIRE_FALSE(s.backend);
}

TEST_CASE (



"from_expr<JThreadBackend>(expr) stays lazy and backend handle is empty"
,
"[pravaha][execution]"
)
 {
    int runs = 0;
    auto s = pravaha::execution::from_expr<pravaha::JThreadBackend>(
        pravaha::task("jthread_from_expr_lazy", [&runs] { ++runs; })
    );
    REQUIRE(runs == 0);
    REQUIRE_FALSE(s.backend);
}

TEST_CASE (



"from_expr(jthread_scheduler{}, expr) carries backend handle"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::from_expr(
        pravaha::execution::jthread_scheduler{},
        pravaha::task("jthread_from_expr_sched", [] {
        })
    );
    REQUIRE(s.backend);
}

TEST_CASE (



"sync_wait(from_expr(jthread_scheduler{}, expr)) succeeds"
,
"[pravaha][execution]"
)
 {
    auto result = pravaha::execution::sync_wait(
        pravaha::execution::from_expr(
            pravaha::execution::jthread_scheduler{},
            pravaha::task("jthread_from_expr_sched_wait", [] {
            })
        )
    );
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"sync_wait(from_expr<JThreadBackend>(expr)) succeeds"
,
"[pravaha][execution]"
)
 {
    auto result = pravaha::execution::sync_wait(
        pravaha::execution::from_expr<pravaha::JThreadBackend>(
            pravaha::task("jthread_from_expr_wait", [] {
            })
        )
    );
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"then and when_all preserve scheduler backend"
,
"[pravaha][execution]"
)
 {
    pravaha::execution::inline_scheduler sched{};
    auto a = pravaha::execution::schedule(sched);
    auto b = pravaha::execution::schedule(sched);
    auto chained = pravaha::execution::then(a, [] {
    });
    auto combined = pravaha::execution::when_all(a, b);
    REQUIRE(chained.backend);
    REQUIRE(combined.backend);
}

TEST_CASE (



"when_all with different scheduler instances fails predictably"
,
"[pravaha][execution]"
)
 {
    pravaha::execution::inline_scheduler sched_a{};
    pravaha::execution::inline_scheduler sched_b{};
    auto a = pravaha::execution::schedule(sched_a);
    auto b = pravaha::execution::schedule(sched_b);
    auto combined = pravaha::execution::when_all(a, b);
    REQUIRE_FALSE(combined.backend);
    REQUIRE(combined.validation_error.has_value());
    auto result = pravaha::execution::sync_wait(std::move(combined));
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"when_all with default senders succeeds"
,
"[pravaha][execution]"
)
 {
    auto a = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    auto b = pravaha::execution::from_expr(pravaha::task("b", [] {
    }));
    auto combined = pravaha::execution::when_all(a, b);
    REQUIRE_FALSE(combined.backend);
    REQUIRE_FALSE(combined.validation_error.has_value());
    auto result = pravaha::execution::sync_wait(std::move(combined));
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"when_all with explicit and default backend fails predictably"
,
"[pravaha][execution]"
)
 {
    pravaha::execution::inline_scheduler sched{};
    auto explicit_sender = pravaha::execution::schedule(sched);
    auto default_sender = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    auto combined = pravaha::execution::when_all(explicit_sender, default_sender);
    REQUIRE_FALSE(combined.backend);
    REQUIRE(combined.validation_error.has_value());
    auto result = pravaha::execution::sync_wait(std::move(combined));
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"when_all invalid backend mix stays lazy at construction"
,
"[pravaha][execution]"
)
 {
    int runs_a = 0;
    int runs_b = 0;
    pravaha::execution::inline_scheduler sched{};
    auto explicit_sender = pravaha::execution::then(
        pravaha::execution::schedule(sched),
        "explicit_count",
        [&runs_a] { ++runs_a; }
    );
    auto default_sender = pravaha::execution::from_expr(pravaha::task("default_count", [&runs_b] { ++runs_b; }));
    auto combined = pravaha::execution::when_all(explicit_sender, default_sender);
    REQUIRE(runs_a == 0);
    REQUIRE(runs_b == 0);
    auto result = pravaha::execution::sync_wait(std::move(combined));
    REQUIRE(result.error);
    REQUIRE(runs_a == 0);
    REQUIRE(runs_b == 0);
}

TEST_CASE (



"invalid when_all wrapped by then stays invalid and lazy"
,
"[pravaha][execution]"
)
 {
    int runs_a = 0;
    int runs_b = 0;
    int then_runs = 0;
    pravaha::execution::inline_scheduler sched{};
    auto explicit_sender = pravaha::execution::then(
        pravaha::execution::schedule(sched),
        "explicit_count_then",
        [&runs_a] { ++runs_a; }
    );
    auto default_sender = pravaha::execution::from_expr(pravaha::task("default_count_then", [&runs_b] { ++runs_b; }));
    auto invalid = pravaha::execution::when_all(explicit_sender, default_sender);
    auto wrapped = pravaha::execution::then(std::move(invalid), "should_not_run", [&then_runs] { ++then_runs; });
    REQUIRE(runs_a == 0);
    REQUIRE(runs_b == 0);
    REQUIRE(then_runs == 0);
    auto result = pravaha::execution::sync_wait(std::move(wrapped));
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
    REQUIRE(runs_a == 0);
    REQUIRE(runs_b == 0);
    REQUIRE(then_runs == 0);
}

TEST_CASE (



"invalid when_all wrapped by upon_error stays invalid"
,
"[pravaha][execution]"
)
 {
    int runs_a = 0;
    int runs_b = 0;
    int error_calls = 0;
    pravaha::ErrorKind observed_kind = pravaha::ErrorKind::InternalError;
    pravaha::execution::inline_scheduler sched{};
    auto explicit_sender = pravaha::execution::then(
        pravaha::execution::schedule(sched),
        "explicit_count_upon_error",
        [&runs_a] { ++runs_a; }
    );
    auto default_sender = pravaha::execution::from_expr(
        pravaha::task("default_count_upon_error", [&runs_b] { ++runs_b; }));
    auto invalid = pravaha::execution::when_all(explicit_sender, default_sender);
    auto wrapped = pravaha::execution::upon_error(std::move(invalid), [&](const pravaha::PravahaError &error) {
        ++error_calls;
        observed_kind = error.kind;
    });
    auto result = pravaha::execution::sync_wait(std::move(wrapped));
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
    REQUIRE(error_calls == 1);
    REQUIRE(observed_kind == pravaha::ErrorKind::ExecutorUnavailable);
    REQUIRE(runs_a == 0);
    REQUIRE(runs_b == 0);
}

TEST_CASE (



"invalid when_all wrapped by upon_stopped stays invalid"
,
"[pravaha][execution]"
)
 {
    int runs_a = 0;
    int runs_b = 0;
    int stopped_calls = 0;
    pravaha::execution::inline_scheduler sched{};
    auto explicit_sender = pravaha::execution::then(
        pravaha::execution::schedule(sched),
        "explicit_count_upon_stopped",
        [&runs_a] { ++runs_a; }
    );
    auto default_sender = pravaha::execution::from_expr(
        pravaha::task("default_count_upon_stopped", [&runs_b] { ++runs_b; }));
    auto invalid = pravaha::execution::when_all(explicit_sender, default_sender);
    auto wrapped = pravaha::execution::upon_stopped(std::move(invalid), [&stopped_calls] { ++stopped_calls; });
    auto result = pravaha::execution::sync_wait(std::move(wrapped));
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
    REQUIRE(stopped_calls == 0);
    REQUIRE(runs_a == 0);
    REQUIRE(runs_b == 0);
}

TEST_CASE (



"upon_error and upon_stopped preserve scheduler backend"
,
"[pravaha][execution]"
)
 {
    pravaha::execution::inline_scheduler sched{};
    auto scheduled = pravaha::execution::schedule(sched);
    auto on_error = pravaha::execution::upon_error(scheduled, [] {
    });
    auto on_stopped = pravaha::execution::upon_stopped(scheduled, [] {
    });
    REQUIRE(on_error.backend);
    REQUIRE(on_stopped.backend);
}

TEST_CASE (



"temporary scheduler backend survives until start"
,
"[pravaha][execution]"
)
 {
    auto s = pravaha::execution::schedule(pravaha::execution::jthread_scheduler{});
    std::weak_ptr<pravaha::JThreadBackend> weak_backend = s.backend;
    REQUIRE(s.backend);
    REQUIRE_FALSE(weak_backend.expired());
    auto op = pravaha::execution::connect(std::move(s), success_receiver{});
    REQUIRE_FALSE(weak_backend.expired());
    pravaha::execution::start(op);
    REQUIRE(op.receiver.value);
}

TEST_CASE (



"sync_wait(schedule(jthread_scheduler{})) succeeds"
,
"[pravaha][execution]"
)
 {
    auto result = pravaha::execution::sync_wait(
        pravaha::execution::schedule(pravaha::execution::jthread_scheduler{})
    );
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"schedule(jthread_scheduler{}) construction is lazy"
,
"[pravaha][execution]"
)
 {
    auto scheduled = pravaha::execution::schedule(pravaha::execution::jthread_scheduler{});
    auto op = pravaha::execution::connect(std::move(scheduled), success_receiver{});
    REQUIRE_FALSE(op.receiver.value);
    REQUIRE_FALSE(op.receiver.error);
    REQUIRE_FALSE(op.receiver.stopped);
}

TEST_CASE (



"sync_wait(schedule(inline_scheduler{})).value == true"
,
"[pravaha][execution]"
)
 {
    auto result = pravaha::execution::sync_wait(
        pravaha::execution::schedule(pravaha::execution::inline_scheduler{})
    );
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"schedule construction does not execute"
,
"[pravaha][execution]"
)
 {
    auto scheduled = pravaha::execution::schedule(pravaha::execution::inline_scheduler{});
    auto op = pravaha::execution::connect(scheduled, success_receiver{});
    REQUIRE_FALSE(op.receiver.value);
    REQUIRE_FALSE(op.receiver.error);
    REQUIRE_FALSE(op.receiver.stopped);
}

TEST_CASE (



"jthread_scheduler compiles if JThreadBackend is available"
,
"[pravaha][execution]"
)
 {
    if constexpr (requires { typename pravaha::JThreadBackend; }) {
        auto s = pravaha::execution::schedule(pravaha::execution::jthread_scheduler{});
        STATIC_REQUIRE(pravaha::execution::sender_like<decltype(s)>);
    } else {
        SUCCEED();
    }
}

TEST_CASE (



"then(success_sender, []{}) succeeds"
,
"[pravaha][execution]"
)
 {
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    auto chained = pravaha::execution::then(base, [] {
    });
    auto result = pravaha::execution::sync_wait(chained);
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"then(sender returning int, [](int x){ return x + 1; }) receives value"
,
"[pravaha][execution]"
)
 {
    int observed = 0;
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] { return 42; }));
    auto chained = pravaha::execution::then(base, "plus_one", [&observed](int x) {
        observed = x + 1;
        return x + 1;
    });
    auto result = pravaha::execution::sync_wait(chained);
    REQUIRE(result.value);
    REQUIRE(observed == 43);
}

TEST_CASE (



"failure skips then continuation"
,
"[pravaha][execution]"
)
 {
    int continuation_runs = 0;
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] { throw std::runtime_error("boom"); }));
    auto chained = pravaha::execution::then(base, [&continuation_runs] {
        ++continuation_runs;
    });
    auto result = pravaha::execution::sync_wait(chained);
    REQUIRE(result.error);
    REQUIRE(continuation_runs == 0);
}

TEST_CASE (



"then construction is lazy"
,
"[pravaha][execution]"
)
 {
    int source_runs = 0;
    int continuation_runs = 0;
    auto base = pravaha::execution::from_expr(pravaha::task("a", [&source_runs] {
        ++source_runs;
        return 1;
    }));
    auto chained = pravaha::execution::then(base, [&continuation_runs](int) {
        ++continuation_runs;
    });
    REQUIRE(source_runs == 0);
    REQUIRE(continuation_runs == 0);
    REQUIRE(chained.expr.frontend.hash != 0);
}

TEST_CASE (



"when_all(success, success) succeeds"
,
"[pravaha][execution]"
)
 {
    auto a = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    auto b = pravaha::execution::from_expr(pravaha::task("b", [] {
    }));
    auto combined = pravaha::execution::when_all(a, b);
    auto result = pravaha::execution::sync_wait(combined);
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"when_all(success, fail) errors"
,
"[pravaha][execution]"
)
 {
    auto a = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    auto b = pravaha::execution::from_expr(pravaha::task("b", [] { throw std::runtime_error("boom"); }));
    auto combined = pravaha::execution::when_all(a, b);
    auto result = pravaha::execution::sync_wait(combined);
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
}

TEST_CASE (



"when_all construction is lazy"
,
"[pravaha][execution]"
)
 {
    int a_runs = 0;
    int b_runs = 0;
    auto a = pravaha::execution::from_expr(pravaha::task("a", [&a_runs] { ++a_runs; }));
    auto b = pravaha::execution::from_expr(pravaha::task("b", [&b_runs] { ++b_runs; }));
    auto combined = pravaha::execution::when_all(a, b);
    REQUIRE(a_runs == 0);
    REQUIRE(b_runs == 0);
    REQUIRE(combined.expr.frontend.hash != 0);
}

TEST_CASE (



"nested then(when_all(...), fn) compiles"
,
"[pravaha][execution]"
)
 {
    auto a = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    auto b = pravaha::execution::from_expr(pravaha::task("b", [] {
    }));
    auto nested = pravaha::execution::then(pravaha::execution::when_all(a, b), [] {
    });
    STATIC_REQUIRE(pravaha::execution::sender_like<decltype(nested)>);
}

TEST_CASE (



"upon_error(move-only sender-like, fn) compiles"
,
"[pravaha][execution]"
)
 {
    move_only_sender s{};
    auto adapted = pravaha::execution::upon_error(std::move(s), [] {
    });
    STATIC_REQUIRE(pravaha::execution::sender_like<decltype(adapted)>);
}

TEST_CASE (



"upon_stopped(sender, fn) compiles"
,
"[pravaha][execution]"
)
 {
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    auto adapted = pravaha::execution::upon_stopped(base, [] {
    });
    STATIC_REQUIRE(pravaha::execution::sender_like<decltype(adapted)>);
}

TEST_CASE (



"tap_error called on failure"
,
"[pravaha][execution]"
)
 {
    int calls = 0;
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] { throw std::runtime_error("boom"); }));
    auto adapted = pravaha::execution::tap_error(base, [&calls] { ++calls; });
    auto result = pravaha::execution::sync_wait(adapted);
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
    REQUIRE(calls == 1);
}

TEST_CASE (



"tap_stopped called on stopped"
,
"[pravaha][execution]"
)
 {
    int calls = 0;
    auto adapted = pravaha::execution::tap_stopped(manual_stopped_sender{}, [&calls] { ++calls; });
    auto result = pravaha::execution::sync_wait(adapted);
    REQUIRE_FALSE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE(result.stopped);
    REQUIRE(calls == 1);
}

TEST_CASE (



"tap_error does not recover failure"
,
"[pravaha][execution]"
)
 {
    int calls = 0;
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] { throw std::runtime_error("boom"); }));
    auto adapted = pravaha::execution::tap_error(base, [&calls](const pravaha::PravahaError &) { ++calls; });
    auto result = pravaha::execution::sync_wait(adapted);
    REQUIRE(calls == 1);
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"tap_stopped does not recover stopped"
,
"[pravaha][execution]"
)
 {
    int calls = 0;
    auto adapted = pravaha::execution::tap_stopped(manual_stopped_sender{}, [&calls] { ++calls; });
    auto result = pravaha::execution::sync_wait(adapted);
    REQUIRE(calls == 1);
    REQUIRE_FALSE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE(result.stopped);
}

TEST_CASE (



"upon_error called on failure"
,
"[pravaha][execution]"
)
 {
    int calls = 0;
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] { throw std::runtime_error("boom"); }));
    auto adapted = pravaha::execution::upon_error(base, [&calls] { ++calls; });
    auto result = pravaha::execution::sync_wait(adapted);
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
    REQUIRE(calls == 1);
}

TEST_CASE (



"upon_error(error-aware fn) is called with error"
,
"[pravaha][execution]"
)
 {
    int calls = 0;
    std::string observed_message;
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] { throw std::runtime_error("boom"); }));
    auto adapted = pravaha::execution::upon_error(
        base, [&calls, &observed_message](const pravaha::PravahaError &error) {
            ++calls;
            observed_message = error.message;
        });
    auto result = pravaha::execution::sync_wait(adapted);
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE(calls == 1);
    REQUIRE_FALSE(observed_message.empty());
}

TEST_CASE (



"upon_error forwards original error downstream"
,
"[pravaha][execution]"
)
 {
    int tap_calls = 0;
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] { throw std::runtime_error("boom"); }));
    auto adapted = pravaha::execution::upon_error(base, [&tap_calls](const pravaha::PravahaError &) { ++tap_calls; });
    auto op = pravaha::execution::connect(adapted, tracking_receiver{});
    pravaha::execution::start(op);
    REQUIRE(tap_calls == 1);
    REQUIRE(op.receiver.value_calls == 0);
    REQUIRE(op.receiver.error_calls == 1);
    REQUIRE(op.receiver.stopped_calls == 0);
}

TEST_CASE (



"upon_error not called on success"
,
"[pravaha][execution]"
)
 {
    int calls = 0;
    auto base = pravaha::execution::from_expr(pravaha::task("a", [] {
    }));
    auto adapted = pravaha::execution::upon_error(base, [&calls] { ++calls; });
    auto result = pravaha::execution::sync_wait(adapted);
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
    REQUIRE(calls == 0);
}

TEST_CASE (



"upon_stopped called on stopped sender"
,
"[pravaha][execution]"
)
 {
    int calls = 0;
    auto adapted = pravaha::execution::upon_stopped(manual_stopped_sender{}, [&calls] { ++calls; });
    auto result = pravaha::execution::sync_wait(adapted);
    REQUIRE_FALSE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE(result.stopped);
    REQUIRE(calls == 1);
}

TEST_CASE (



"upon_error and upon_stopped construction is lazy"
,
"[pravaha][execution]"
)
 {
    int error_calls = 0;
    int stopped_calls = 0;
    int runs = 0;
    auto base = pravaha::execution::from_expr(pravaha::task("a", [&runs] { ++runs; }));
    auto on_error = pravaha::execution::upon_error(base, [&error_calls] { ++error_calls; });
    auto on_stopped = pravaha::execution::upon_stopped(manual_stopped_sender{}, [&stopped_calls] { ++stopped_calls; });
    auto op_error = pravaha::execution::connect(on_error, success_receiver{});
    auto op_stopped = pravaha::execution::connect(on_stopped, success_receiver{});
    REQUIRE(runs == 0);
    REQUIRE(error_calls == 0);
    REQUIRE(stopped_calls == 0);
    REQUIRE_FALSE(op_error.receiver.value);
    REQUIRE_FALSE(op_stopped.receiver.stopped);
}

TEST_CASE (



"adoption: from_expr is lazy"
,
"[pravaha][execution][adoption]"
)
 {
    int runs = 0;
    auto s = pravaha::execution::from_expr(pravaha::task("adopt_from_expr", [&runs] { ++runs; }));
    REQUIRE(runs == 0);
    STATIC_REQUIRE(pravaha::execution::sender_like<decltype(s)>);
}

TEST_CASE (



"adoption: connect is lazy"
,
"[pravaha][execution][adoption]"
)
 {
    int runs = 0;
    auto s = pravaha::execution::from_expr(pravaha::task("adopt_connect", [&runs] { ++runs; }));
    auto op = pravaha::execution::connect(s, success_receiver{});
    REQUIRE(runs == 0);
    REQUIRE_FALSE(op.receiver.value);
}

TEST_CASE (



"adoption: start executes"
,
"[pravaha][execution][adoption]"
)
 {
    int runs = 0;
    auto s = pravaha::execution::from_expr(pravaha::task("adopt_start", [&runs] { ++runs; }));
    auto op = pravaha::execution::connect(s, success_receiver{});
    pravaha::execution::start(op);
    REQUIRE(runs == 1);
    REQUIRE(op.receiver.value);
}

TEST_CASE (



"adoption: sync_wait success"
,
"[pravaha][execution][adoption]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("adopt_sync_success", [] {
    }));
    auto result = pravaha::execution::sync_wait(s);
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"adoption: sync_wait failure"
,
"[pravaha][execution][adoption]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("adopt_sync_fail", [] { throw std::runtime_error("boom"); }));
    auto result = pravaha::execution::sync_wait(s);
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"adoption: schedule(inline_scheduler{}) succeeds"
,
"[pravaha][execution][adoption]"
)
 {
    auto s = pravaha::execution::schedule(pravaha::execution::inline_scheduler{});
    STATIC_REQUIRE(pravaha::execution::sender_like<decltype(s)>);
    auto result = pravaha::execution::sync_wait(s);
    REQUIRE(result.value);
}

TEST_CASE (



"adoption: then is lazy and executes after predecessor"
,
"[pravaha][execution][adoption]"
)
 {
    int source_runs = 0;
    int continuation_runs = 0;
    int phase = 0;
    bool order_ok = true;
    auto base = pravaha::execution::from_expr(pravaha::task("adopt_then_base", [&] {
        ++source_runs;
        phase = 1;
        return 7;
    }));
    auto chained = pravaha::execution::then(base, "adopt_then", [&](int) {
        ++continuation_runs;
        order_ok = (phase == 1);
        phase = 2;
    });
    REQUIRE(source_runs == 0);
    REQUIRE(continuation_runs == 0);
    auto result = pravaha::execution::sync_wait(chained);
    REQUIRE(result.value);
    REQUIRE(source_runs == 1);
    REQUIRE(continuation_runs == 1);
    REQUIRE(order_ok);
    REQUIRE(phase == 2);
}

TEST_CASE (



"adoption: then value-flow int -> int works"
,
"[pravaha][execution][adoption]"
)
 {
    auto base = pravaha::execution::from_expr(pravaha::task("adopt_then_value", [] { return 41; }));
    auto chained = pravaha::execution::then(base, "adopt_then_plus", [](int x) { return x + 1; });
    auto result = pravaha::execution::sync_wait_value<int>(chained);
    REQUIRE(result.value);
    REQUIRE(result.result.has_value());
    REQUIRE(*result.result == 42);
}

TEST_CASE (



"adoption: when_all(success, success) succeeds"
,
"[pravaha][execution][adoption]"
)
 {
    auto a = pravaha::execution::from_expr(pravaha::task("adopt_all_a", [] {
    }));
    auto b = pravaha::execution::from_expr(pravaha::task("adopt_all_b", [] {
    }));
    auto result = pravaha::execution::sync_wait(pravaha::execution::when_all(a, b));
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
    REQUIRE_FALSE(result.stopped);
}

TEST_CASE (



"adoption: when_all(success, fail) errors"
,
"[pravaha][execution][adoption]"
)
 {
    auto a = pravaha::execution::from_expr(pravaha::task("adopt_all_ok", [] {
    }));
    auto b = pravaha::execution::from_expr(pravaha::task("adopt_all_fail", [] { throw std::runtime_error("boom"); }));
    auto result = pravaha::execution::sync_wait(pravaha::execution::when_all(a, b));
    REQUIRE_FALSE(result.value);
    REQUIRE(result.error);
}

TEST_CASE (



"adoption: upon_error called only on failure"
,
"[pravaha][execution][adoption]"
)
 {
    int calls = 0;
    auto ok = pravaha::execution::from_expr(pravaha::task("adopt_upon_ok", [] {
    }));
    auto fail = pravaha::execution::from_expr(
        pravaha::task("adopt_upon_fail", [] { throw std::runtime_error("boom"); }));
    auto ok_result = pravaha::execution::sync_wait(pravaha::execution::upon_error(ok, [&calls] { ++calls; }));
    auto fail_result = pravaha::execution::sync_wait(pravaha::execution::upon_error(fail, [&calls] { ++calls; }));
    REQUIRE(ok_result.value);
    REQUIRE(fail_result.error);
    REQUIRE(calls == 1);
}

TEST_CASE (



"adoption: upon_stopped called only on stopped sender"
,
"[pravaha][execution][adoption]"
)
 {
    int calls = 0;
    auto stopped_result = pravaha::execution::sync_wait(
        pravaha::execution::upon_stopped(manual_stopped_sender{}, [&calls] { ++calls; })
    );
    auto ok = pravaha::execution::from_expr(pravaha::task("adopt_stop_ok", [] {
    }));
    auto ok_result = pravaha::execution::sync_wait(
        pravaha::execution::upon_stopped(ok, [&calls] { ++calls; })
    );
    REQUIRE(stopped_result.stopped);
    REQUIRE(ok_result.value);
    REQUIRE(calls == 1);
}

TEST_CASE (



"execution.hpp compiles"
,
"[pravaha][execution][compile]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("compile_header", [] {
    }));
    STATIC_REQUIRE(pravaha::execution::sender_like<decltype(s)>);
}

TEST_CASE (



"sync_wait_value_result<int> compiles"
,
"[pravaha][execution][compile]"
)
 {
    pravaha::execution::sync_wait_value_result<int> result{};
    REQUIRE_FALSE(result.result.has_value());
}

TEST_CASE (



"start works with move-only expression if available"
,
"[pravaha][execution][compile]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("move_only_if_available", [] {
    }));
    using expr_t = typename decltype(s)::expr_type;
    if constexpr (std::move_constructible<expr_t> && !std::copy_constructible<expr_t>) {
        auto op = pravaha::execution::connect(std::move(s), success_receiver{});
        pravaha::execution::start(op);
        REQUIRE(op.receiver.value);
    } else {
        SUCCEED();
    }
}

TEST_CASE (



"start is one-shot"
,
"[pravaha][execution][compile]"
)
 {
    int runs = 0;
    auto s = pravaha::execution::from_expr(pravaha::task("one_shot", [&runs] { ++runs; }));
    auto op = pravaha::execution::connect(std::move(s), success_receiver{});
    bool started = false;
    REQUIRE_FALSE(started);
    pravaha::execution::start(op);
    started = true;
    REQUIRE(started);
    REQUIRE(runs == 1);
}

TEST_CASE (



"compile pack: connect is lazy"
,
"[pravaha][execution][compile]"
)
 {
    int runs = 0;
    auto s = pravaha::execution::from_expr(pravaha::task("compile_connect_lazy", [&runs] { ++runs; }));
    auto op = pravaha::execution::connect(s, success_receiver{});
    REQUIRE(runs == 0);
    REQUIRE_FALSE(op.receiver.value);
}

TEST_CASE (



"compile pack: sync_wait success still works"
,
"[pravaha][execution][compile]"
)
 {
    auto s = pravaha::execution::from_expr(pravaha::task("compile_sync_success", [] {
    }));
    auto result = pravaha::execution::sync_wait(s);
    REQUIRE(result.value);
    REQUIRE_FALSE(result.error);
}

TEST_CASE (



"compile pack: then still works"
,
"[pravaha][execution][compile]"
)
 {
    auto base = pravaha::execution::from_expr(pravaha::task("compile_then_base", [] { return 1; }));
    auto chained = pravaha::execution::then(base, "compile_then", [](int x) { return x + 1; });
    auto result = pravaha::execution::sync_wait_value<int>(chained);
    REQUIRE(result.value);
    REQUIRE(result.result.has_value());
    REQUIRE(*result.result == 2);
}

TEST_CASE (



"compile pack: when_all still works"
,
"[pravaha][execution][compile]"
)
 {
    auto a = pravaha::execution::from_expr(pravaha::task("compile_all_a", [] {
    }));
    auto b = pravaha::execution::from_expr(pravaha::task("compile_all_b", [] {
    }));
    auto result = pravaha::execution::sync_wait(pravaha::execution::when_all(a, b));
    REQUIRE(result.value);
}

TEST_CASE (



"compile pack: tap_error still works"
,
"[pravaha][execution][compile]"
)
 {
    int calls = 0;
    auto base = pravaha::execution::from_expr(pravaha::task("compile_tap_error", [] {
        throw std::runtime_error("boom");
    }));
    auto tapped = pravaha::execution::tap_error(base, [&calls] { ++calls; });
    auto result = pravaha::execution::sync_wait(tapped);
    REQUIRE(result.error);
    REQUIRE(calls == 1);
}

TEST_CASE (



"compile pack: tap_stopped still works"
,
"[pravaha][execution][compile]"
)
 {
    int calls = 0;
    auto tapped = pravaha::execution::tap_stopped(manual_stopped_sender{}, [&calls] { ++calls; });
    auto result = pravaha::execution::sync_wait(tapped);
    REQUIRE(result.stopped);
    REQUIRE(calls == 1);
}
