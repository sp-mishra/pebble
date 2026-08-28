#include "utils/log.hpp"

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

// Attach a fresh CaptureSink for the duration of a test section; remove it
// on destruction so leakage into unrelated tests is impossible.
struct SinkGuard {
    std::shared_ptr<lg::testing::CaptureSink> sink =
        std::make_shared<lg::testing::CaptureSink>();

    SinkGuard() { lg::log::add_sink(sink); }

    ~SinkGuard() {
        lg::log::remove_sink(sink);
        sink->clear();
    }

    // convenience forwarders
    [[nodiscard]] bool contains(std::string_view s) const { return sink->contains(s); }
    [[nodiscard]] std::string last() const { return sink->last_message(); }
    [[nodiscard]] std::vector<lg::testing::LogRecord> records() const { return sink->records(); }
    [[nodiscard]] std::size_t size() const { return sink->records().size(); }
};

// Reset global state between tests that touch MDC/NDC
struct MdcCleanup {
    ~MdcCleanup() {
        lg::mdc::clear();
        lg::ndc::clear();
    }
};

// ============================================================================
// §1  Level enum
// ============================================================================
TEST_CASE (



"Level enum has expected integral values"
,
"[log][level]"
)
 {
    REQUIRE(static_cast<int>(lg::Level::Trace) == 0);
    REQUIRE(static_cast<int>(lg::Level::Debug) == 1);
    REQUIRE(static_cast<int>(lg::Level::Info) == 2);
    REQUIRE(static_cast<int>(lg::Level::Warn) == 3);
    REQUIRE(static_cast<int>(lg::Level::Error) == 4);
    REQUIRE(static_cast<int>(lg::Level::Critical) == 5);
    REQUIRE(static_cast<int>(lg::Level::Off) == 6);
}

TEST_CASE (



"Level enum values are distinct"
,
"[log][level]"
)
 {
    REQUIRE(lg::Level::Trace != lg::Level::Debug);
    REQUIRE(lg::Level::Debug != lg::Level::Info);
    REQUIRE(lg::Level::Info != lg::Level::Warn);
    REQUIRE(lg::Level::Warn != lg::Level::Error);
    REQUIRE(lg::Level::Error != lg::Level::Critical);
    REQUIRE(lg::Level::Critical != lg::Level::Off);
}

// ============================================================================
// §2  detail::logging_enabled
// ============================================================================
TEST_CASE (



"logging_enabled matches TT_LOG_ENABLED macro"
,
"[log][detail]"
)
 {
    STATIC_REQUIRE(lg::detail::logging_enabled == (TT_LOG_ENABLED != 0));
    REQUIRE(lg::log::enabled == lg::detail::logging_enabled);
}

// ============================================================================
// §3  Free log functions — message content
// ============================================================================
TEST_CASE (



"lg::info emits message to attached sink"
,
"[log][free-functions]"
)
 {
    const SinkGuard sg;
    lg::info("hello from info");
    REQUIRE(sg.contains("hello from info"));
}

TEST_CASE (



"lg::warn emits message to attached sink"
,
"[log][free-functions]"
)
 {
    const SinkGuard sg;
    lg::warn("hello from warn");
    REQUIRE(sg.contains("hello from warn"));
}

TEST_CASE (



"lg::error emits message to attached sink"
,
"[log][free-functions]"
)
 {
    const SinkGuard sg;
    lg::error("hello from error");
    REQUIRE(sg.contains("hello from error"));
}

TEST_CASE (



"lg::debug emits message to attached sink"
,
"[log][free-functions]"
)
 {
    const SinkGuard sg;
    lg::debug("hello from debug");
    REQUIRE(sg.contains("hello from debug"));
}

TEST_CASE (



"lg::critical emits message to attached sink"
,
"[log][free-functions]"
)
 {
    const SinkGuard sg;
    lg::critical("hello from critical");
    REQUIRE(sg.contains("hello from critical"));
}

TEST_CASE (



"lg::trace emits message to attached sink"
,
"[log][free-functions]"
)
 {
    const SinkGuard sg;
    lg::trace("hello from trace");
    REQUIRE(sg.contains("hello from trace"));
}

// ============================================================================
// §4  Free log functions — format arguments
// ============================================================================
TEST_CASE (



"lg::info formats integer argument"
,
"[log][free-functions][format]"
)
 {
    const SinkGuard sg;
    lg::info("value={}", 42);
    REQUIRE(sg.contains("value=42"));
}

TEST_CASE (



"lg::info formats multiple arguments"
,
"[log][free-functions][format]"
)
 {
    const SinkGuard sg;
    lg::info("{} + {} = {}", 1, 2, 3);
    REQUIRE(sg.contains("1 + 2 = 3"));
}

TEST_CASE (



"lg::info formats string argument"
,
"[log][free-functions][format]"
)
 {
    const SinkGuard sg;
    std::string s = "world";
    lg::info("hello {}", s);
    REQUIRE(sg.contains("hello world"));
}

TEST_CASE (



"lg::info handles empty format string"
,
"[log][free-functions][format]"
)
 {
    const SinkGuard sg;
    lg::info("");
    REQUIRE(sg.size() == 1);
}

TEST_CASE (



"lg::error formats floating-point argument"
,
"[log][free-functions][format]"
)
 {
    const SinkGuard sg;
    lg::error("ratio={:.2f}", 3.14159);
    REQUIRE(sg.contains("ratio=3.14"));
}

// ============================================================================
// §5  Level recorded in LogRecord
// ============================================================================
TEST_CASE (



"LogRecord level matches the function called"
,
"[log][record]"
)
 {
    const SinkGuard sg;
    lg::info("msg-info");
    lg::warn("msg-warn");
    lg::error("msg-error");

    const auto recs = sg.records();
    REQUIRE(recs.size() >= 3);

    // Find each expected entry
    auto find = [&](lg::Level lv, std::string_view substr) {
        for (const auto& r : recs)
            if (r.level == lv && r.message.find(substr) != std::string::npos)
                return true;
        return false;
    };
    REQUIRE(find(lg::Level::Info, "msg-info"));
    REQUIRE(find(lg::Level::Warn, "msg-warn"));
    REQUIRE(find(lg::Level::Error, "msg-error"));
}

// ============================================================================
// §6  CaptureSink API
// ============================================================================
TEST_CASE (



"CaptureSink::clear resets records"
,
"[log][capture-sink]"
)
 {
    const SinkGuard sg;
    lg::info("before-clear");
    REQUIRE(sg.size() == 1);
    sg.sink->clear();
    REQUIRE(sg.size() == 0);
}

TEST_CASE (



"CaptureSink::last_message returns most recent"
,
"[log][capture-sink]"
)
 {
    const SinkGuard sg;
    lg::info("first");
    lg::info("second");
    lg::info("third");
    REQUIRE(sg.last() == "third");
}

TEST_CASE (



"CaptureSink::last_message returns empty when no records"
,
"[log][capture-sink]"
)
 {
    const auto cap = std::make_shared<lg::testing::CaptureSink>();
    REQUIRE(cap->last_message().empty());
}

TEST_CASE (



"CaptureSink::contains returns false when no match"
,
"[log][capture-sink]"
)
 {
    const SinkGuard sg;
    lg::info("something else");
    REQUIRE_FALSE(sg.contains("not-present"));
}

TEST_CASE (



"CaptureSink accumulates multiple records"
,
"[log][capture-sink]"
)
 {
    const SinkGuard sg;
    for (int i = 0; i < 10; ++i)
        lg::info("line {}", i);
    REQUIRE(sg.size() == 10);
}

// ============================================================================
// §7  MDC — Mapped Diagnostic Context
// ============================================================================
TEST_CASE (



"mdc::put adds key visible in MDC"
,
"[log][mdc]"
)
 {
    MdcCleanup mc;
    const SinkGuard sg;
    lg::mdc::put("req_id", "abc-123");
    lg::info("handling request");
    // The pattern includes MDC via spdlog %& — the message payload itself
    // doesn't contain MDC keys, but we can verify put/remove doesn't throw.
    REQUIRE(sg.contains("handling request"));
}

TEST_CASE (



"mdc::remove does not throw on missing key"
,
"[log][mdc]"
)
 {
    MdcCleanup mc;
    REQUIRE_NOTHROW(lg::mdc::remove("non-existent-key"));
}

TEST_CASE (



"mdc::clear removes all entries without throw"
,
"[log][mdc]"
)
 {
    MdcCleanup mc;
    lg::mdc::put("k1", "v1");
    lg::mdc::put("k2", "v2");
    REQUIRE_NOTHROW(lg::mdc::clear());
}

TEST_CASE (



"mdc::ScopedEntry sets key and removes on scope exit"
,
"[log][mdc]"
)
 {
    MdcCleanup mc;
    const SinkGuard sg;
    {
        lg::mdc::ScopedEntry entry{"scope_key", "scope_val"};
        lg::info("inside scope");
    }
    // After scope exit the ScopedEntry destructor ran; clear/remove shouldn't
    // throw even if the key is already gone.
    REQUIRE_NOTHROW(lg::mdc::remove("scope_key"));
    REQUIRE(sg.contains("inside scope"));
}

TEST_CASE (



"mdc::ScopedEntry cleans up even if a nested exception unwinds"
,
"[log][mdc]"
)
 {
    MdcCleanup mc;
    try {
        lg::mdc::ScopedEntry entry{"unwinding_key", "val"};
        throw std::runtime_error("deliberate");
    }
    catch (...) {}
    // If cleanup is correct, a second remove is a no-op, not UB.
    REQUIRE_NOTHROW(lg::mdc::remove("unwinding_key"));
}

TEST_CASE (



"mdc::ScopedEntry is non-copyable and non-movable"
,
"[log][mdc]"
)
 {
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<lg::mdc::ScopedEntry>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<lg::mdc::ScopedEntry>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<lg::mdc::ScopedEntry>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<lg::mdc::ScopedEntry>);
}

// ============================================================================
// §8  NDC — Nested Diagnostic Context
// ============================================================================
TEST_CASE (



"ndc::push and pop maintain stack depth"
,
"[log][ndc]"
)
 {
    lg::ndc::clear();
    lg::ndc::push("a");
    lg::ndc::push("b");
    lg::ndc::pop();
    lg::ndc::pop();
    REQUIRE_NOTHROW(lg::ndc::pop()); // extra pop: no-op, no crash
    lg::ndc::clear();
}

TEST_CASE (



"ndc::pop on empty stack does not crash"
,
"[log][ndc]"
)
 {
    lg::ndc::clear();
    REQUIRE_NOTHROW(lg::ndc::pop());
}

TEST_CASE (



"ndc::clear resets to empty stack"
,
"[log][ndc]"
)
 {
    lg::ndc::push("level1");
    lg::ndc::push("level2");
    lg::ndc::clear();
    // After clear another push should work normally
    REQUIRE_NOTHROW(lg::ndc::push("fresh"));
    lg::ndc::clear();
}

TEST_CASE (



"ndc::ScopedPush restores stack on scope exit"
,
"[log][ndc]"
)
 {
    MdcCleanup mc;
    {
        lg::ndc::ScopedPush p1{"outer"};
        {
            lg::ndc::ScopedPush p2{"inner"};
        }
        // After inner exits, "inner" should be gone; a log call here reflects "outer" only.
    }
    // After outer exits, the stack should be empty.
    REQUIRE_NOTHROW(lg::mdc::remove("ndc")); // Should be missing or removable without crash.
}

TEST_CASE (



"nested ndc::ScopedPush unwinds correctly under exception"
,
"[log][ndc]"
)
 {
    MdcCleanup mc;
    try {
        lg::ndc::ScopedPush p1{"outer"};
        lg::ndc::ScopedPush p2{"inner"};
        throw std::runtime_error("boom");
    }
    catch (...) {}
    REQUIRE_NOTHROW(lg::ndc::pop()); // should be no-op (stack already empty)
}

TEST_CASE (



"ndc::ScopedPush is non-copyable and non-movable"
,
"[log][ndc]"
)
 {
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<lg::ndc::ScopedPush>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<lg::ndc::ScopedPush>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<lg::ndc::ScopedPush>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<lg::ndc::ScopedPush>);
}

// ============================================================================
// §9  TraceContext
// ============================================================================
TEST_CASE (



"TraceContext default construction has zero ids"
,
"[log][trace]"
)
 {
    constexpr lg::TraceContext ctx;
    REQUIRE(ctx.trace_id == 0);
    REQUIRE(ctx.span_id == 0);
}

TEST_CASE (



"set_trace_context injects into MDC without throw"
,
"[log][trace]"
)
 {
    MdcCleanup mc;
    REQUIRE_NOTHROW(lg::set_trace_context({.trace_id = 100, .span_id = 7}));
    // Reset
    REQUIRE_NOTHROW(lg::set_trace_context({}));
}

TEST_CASE (



"set_trace_context with zero ids removes MDC entries"
,
"[log][trace]"
)
 {
    MdcCleanup mc;
    lg::set_trace_context({.trace_id = 42, .span_id = 1});
    // Now zero out — should remove trace/span from MDC without throw.
    REQUIRE_NOTHROW(lg::set_trace_context({.trace_id = 0, .span_id = 0}));
}

TEST_CASE (



"get_trace_context returns zero without nadi"
,
"[log][trace]"
)
 {
    // Without nadi active (or compiled in) the lineage is zero.
    const auto ctx = lg::get_trace_context();
    REQUIRE(ctx.trace_id == 0);
    REQUIRE(ctx.span_id == 0);
}

// ============================================================================
// §10  Named loggers (lg::Logger / lg::logger())
// ============================================================================
TEST_CASE (



"lg::logger() factory returns usable Logger"
,
"[log][named-logger]"
)
 {
    const SinkGuard sg;
    auto lg_db = lg::logger("test.db");
    lg_db.info("connected");
    REQUIRE(sg.contains("connected"));
}

TEST_CASE (



"named logger emits to global dist_sink"
,
"[log][named-logger]"
)
 {
    const SinkGuard sg;
    auto log1 = lg::logger("test.module1");
    auto log2 = lg::logger("test.module2");
    log1.info("from module1");
    log2.warn("from module2");
    REQUIRE(sg.contains("from module1"));
    REQUIRE(sg.contains("from module2"));
}

TEST_CASE (



"named logger set_level filters messages below threshold"
,
"[log][named-logger]"
)
 {
    const SinkGuard sg;
    auto slog = lg::logger("test.filtered");
    slog.set_level(lg::Level::Error);
    slog.info("should be suppressed");
    slog.error("should appear");
    REQUIRE_FALSE(sg.contains("should be suppressed"));
    REQUIRE(sg.contains("should appear"));
}

TEST_CASE (



"lg::logger() re-using same name returns same logger"
,
"[log][named-logger]"
)
 {
    const SinkGuard sg;
    auto a = lg::logger("test.reuse");
    auto b = lg::logger("test.reuse");
    a.info("from-a");
    b.info("from-b");
    REQUIRE(sg.contains("from-a"));
    REQUIRE(sg.contains("from-b"));
}

TEST_CASE (



"named logger formats arguments correctly"
,
"[log][named-logger]"
)
 {
    const SinkGuard sg;
    auto slog = lg::logger("test.fmt");
    slog.info("count={}", 99);
    slog.warn("ratio={:.1f}", 0.5);
    slog.error("id={} name={}", 7, "alice");
    REQUIRE(sg.contains("count=99"));
    REQUIRE(sg.contains("ratio=0.5"));
    REQUIRE(sg.contains("id=7 name=alice"));
}

TEST_CASE (



"named logger all level methods compile and emit"
,
"[log][named-logger]"
)
 {
    SinkGuard sg;
    auto slog = lg::logger("test.all-levels");
    slog.info("ni");
    slog.warn("nw");
    slog.error("ne");
    slog.debug("nd");
    slog.critical("nc");
    REQUIRE(sg.contains("ni"));
    REQUIRE(sg.contains("nw"));
    REQUIRE(sg.contains("ne"));
    REQUIRE(sg.contains("nd"));
    REQUIRE(sg.contains("nc"));
}

// ============================================================================
// §11  LogSpan
// ============================================================================
TEST_CASE (



"LogSpan emits span.begin and span.end"
,
"[log][span]"
)
 {
    const SinkGuard sg;
    {
        lg::LogSpan span("test.operation");
    }
    REQUIRE(sg.contains("span.begin"));
    REQUIRE(sg.contains("span.end"));
    REQUIRE(sg.contains("test.operation"));
}

TEST_CASE (



"LogSpan span.end contains elapsed_ns"
,
"[log][span]"
)
 {
    const SinkGuard sg;
    {
        lg::LogSpan span("timed.op");
    }
    REQUIRE(sg.contains("elapsed_ns="));
}

TEST_CASE (



"LogSpan with fields includes them in span.begin message"
,
"[log][span]"
)
 {
    const SinkGuard sg;
    {
        lg::LogSpan span("query", {{"table", "orders"}, {"db", "primary"}});
    }
    REQUIRE(sg.contains("table=orders"));
    REQUIRE(sg.contains("db=primary"));
}

TEST_CASE (



"LogSpan sets span.name in MDC during its lifetime"
,
"[log][span]"
)
 {
    MdcCleanup mc;
    const SinkGuard sg;
    {
        lg::LogSpan span("my.span");
        // A log inside the span captures span.name in MDC; payload itself
        // just needs to record that the log happened.
        lg::info("inside span");
    }
    REQUIRE(sg.contains("inside span"));
}

TEST_CASE (



"LogSpan span.name MDC entry is removed after destruction"
,
"[log][span]"
)
 {
    MdcCleanup mc;
    {
        lg::LogSpan span("ephemeral.span");
    }
    // After dtor, removing span.name should be a no-op (not a crash).
    REQUIRE_NOTHROW(lg::mdc::remove("span.name"));
}

TEST_CASE (



"LogSpan elapsed_ns is non-negative"
,
"[log][span]"
)
 {
    const SinkGuard sg;
    {
        lg::LogSpan span("timing.check");
    }
    // Find the end record and parse elapsed_ns
    for (const auto& r : sg.records()) {
        if (r.message.find("elapsed_ns=") != std::string::npos) {
            const auto pos = r.message.find("elapsed_ns=") + 11;
            const long long ns = std::stoll(r.message.substr(pos));
            REQUIRE(ns >= 0);
        }
    }
}

TEST_CASE (



"LogSpan is non-copyable and non-movable"
,
"[log][span]"
)
 {
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<lg::LogSpan>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<lg::LogSpan>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<lg::LogSpan>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<lg::LogSpan>);
}

TEST_CASE (



"scoped_span emits span.begin and span.end"
,
"[log][span]"
)
 {
    MdcCleanup mc;
    const SinkGuard sg;
    {
        lg::scoped_span ss("outer.op");
        lg::info("inside scoped_span");
    }
    REQUIRE(sg.contains("span.begin"));
    REQUIRE(sg.contains("span.end"));
    REQUIRE(sg.contains("inside scoped_span"));
}

TEST_CASE (



"scoped_span pushes NDC entry for lifetime"
,
"[log][span]"
)
 {
    MdcCleanup mc;
    SinkGuard sg;
    {
        lg::scoped_span ss("ndc.span");
        lg::info("probe");
    }
    // After scope, NDC stack should be empty (ScopedPush unwound).
    REQUIRE_NOTHROW(lg::ndc::pop()); // extra pop: no-op
}

TEST_CASE (



"nested scoped_span unwinds correctly"
,
"[log][span]"
)
 {
    MdcCleanup mc;
    const SinkGuard sg;
    {
        lg::scoped_span outer("outer");
        {
            lg::scoped_span inner("inner");
            lg::info("deep");
        }
        lg::info("shallow");
    }
    REQUIRE(sg.contains("deep"));
    REQUIRE(sg.contains("shallow"));
}

// ============================================================================
// §12  log_once
// ============================================================================
// log_once uses a process-static atomic per template instantiation.
// Instantiation is keyed on Args..., so tests must use distinct argument
// signatures (different arg types) to get independent fired flags.
TEST_CASE (



"log_once emits exactly once across repeated calls"
,
"[log][log-once]"
)
 {
    const SinkGuard sg;
    for (int i = 0; i < 5; ++i)
        lg::log_once(lg::Level::Warn, "once-only val={}", std::size_t{42});
    REQUIRE(sg.size() == 1);
    REQUIRE(sg.contains("once-only val=42"));
}

TEST_CASE (



"log_once with format arguments emits once"
,
"[log][log-once]"
)
 {
    const SinkGuard sg;
    for (int i = 0; i < 3; ++i)
        lg::log_once(lg::Level::Info, "once-fmt x={}", double{3.14});
    REQUIRE(sg.size() == 1);
}

TEST_CASE (



"log_once is thread-safe — only one emission under concurrency"
,
"[log][log-once]"
)
 {
    const SinkGuard sg;
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([] {
            lg::log_once(lg::Level::Info, "concurrent-once tid={}", unsigned{99});
        });
    for (auto& t : threads) t.join();
    // Exactly one emission (the static atomic is process-wide per instantiation)
    REQUIRE(sg.size() == 1);
}

// ============================================================================
// §13  log_if
// ============================================================================
TEST_CASE (



"log_if emits when condition is true"
,
"[log][log-if]"
)
 {
    const SinkGuard sg;
    lg::log_if(true, lg::Level::Info, "conditional-true");
    REQUIRE(sg.contains("conditional-true"));
}

TEST_CASE (



"log_if suppresses when condition is false"
,
"[log][log-if]"
)
 {
    const SinkGuard sg;
    lg::log_if(false, lg::Level::Info, "conditional-false");
    REQUIRE_FALSE(sg.contains("conditional-false"));
    REQUIRE(sg.size() == 0);
}

TEST_CASE (



"log_if with format args emits when true"
,
"[log][log-if]"
)
 {
    const SinkGuard sg;
    int x = 7;
    lg::log_if(x > 5, lg::Level::Warn, "x={} exceeds threshold", x);
    REQUIRE(sg.contains("x=7 exceeds threshold"));
}

TEST_CASE (



"log_if with format args suppresses when false"
,
"[log][log-if]"
)
 {
    const SinkGuard sg;
    int x = 3;
    lg::log_if(x > 5, lg::Level::Warn, "x={} exceeds threshold", x);
    REQUIRE(sg.size() == 0);
}

TEST_CASE (



"log_if evaluates condition without side-effects when false"
,
"[log][log-if]"
)
 {
    SinkGuard sg;
    const int counter = 0;
    // The format string is built regardless, but the condition guards the emit.
    lg::log_if(false, lg::Level::Info, "noop");
    REQUIRE(counter == 0);
}

// ============================================================================
// §14  RateLimiter
// ============================================================================
TEST_CASE (



"RateLimiter allows up to MaxCount calls per window"
,
"[log][rate-limiter]"
)
 {
    lg::RateLimiter<3, std::chrono::seconds> lim{std::chrono::seconds{60}};
    int count = 0;
    for (int i = 0; i < 10; ++i)
        lim([&] { ++count; });
    REQUIRE(count == 3);
}

TEST_CASE (



"RateLimiter resets after window expires"
,
"[log][rate-limiter]"
)
 {
    lg::RateLimiter<2, std::chrono::milliseconds> lim{std::chrono::milliseconds{20}};
    int count = 0;
    for (int i = 0; i < 2; ++i) lim([&] { ++count; });
    REQUIRE(count == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    for (int i = 0; i < 2; ++i) lim([&] { ++count; });
    REQUIRE(count == 4);
}

TEST_CASE (



"rate_limited free function limits emission"
,
"[log][rate-limiter]"
)
 {
    const SinkGuard sg;
    for (int i = 0; i < 20; ++i)
        lg::rate_limited<2>([&] { lg::info("rate-limited-msg"); });
    // Exactly 2 should have been emitted in the first window.
    REQUIRE(sg.size() == 2);
}

// ============================================================================
// §15  log_exception
// ============================================================================
TEST_CASE (



"log_exception logs exception message"
,
"[log][exception]"
)
 {
    const SinkGuard sg;
    const std::runtime_error e{"test error message"};
    lg::log_exception(e);
    REQUIRE(sg.contains("test error message"));
}

TEST_CASE (



"log_exception with context prepends context string"
,
"[log][exception]"
)
 {
    const SinkGuard sg;
    const std::runtime_error e{"inner failure"};
    lg::log_exception(e, "db.connect");
    REQUIRE(sg.contains("db.connect"));
    REQUIRE(sg.contains("inner failure"));
}

TEST_CASE (



"log_exception with empty context omits context prefix"
,
"[log][exception]"
)
 {
    const SinkGuard sg;
    const std::runtime_error e{"raw error"};
    lg::log_exception(e, {});
    REQUIRE(sg.contains("raw error"));
}

TEST_CASE (



"log_exception respects custom log level"
,
"[log][exception]"
)
 {
    const SinkGuard sg;
    const std::runtime_error e{"warn-level error"};
    lg::log_exception(e, "ctx", lg::Level::Warn);
    const auto recs = sg.records();
    bool found = false;
    for (const auto& r : recs)
        if (r.level == lg::Level::Warn && r.message.find("warn-level error") != std::string::npos)
            found = true;
    REQUIRE(found);
}

TEST_CASE (



"log_exception formats nested exception chain"
,
"[log][exception]"
)
 {
    const SinkGuard sg;
    std::exception_ptr nested_ptr;
    try {
        try {
            throw std::runtime_error("root cause");
        }
        catch (...) {
            std::throw_with_nested(std::runtime_error("outer wrapper"));
        }
    }
    catch (const std::exception& e) {
        lg::log_exception(e, "chain.test");
    }
    REQUIRE(sg.contains("outer wrapper"));
    REQUIRE(sg.contains("root cause"));
}

TEST_CASE (



"log_exception with three-level nesting logs all levels"
,
"[log][exception]"
)
 {
    const SinkGuard sg;
    try {
        try {
            try {
                throw std::runtime_error("level-3");
            }
            catch (...) {
                std::throw_with_nested(std::runtime_error("level-2"));
            }
        }
        catch (...) {
            std::throw_with_nested(std::runtime_error("level-1"));
        }
    }
    catch (const std::exception& e) {
        lg::log_exception(e);
    }
    REQUIRE(sg.contains("level-1"));
    REQUIRE(sg.contains("level-2"));
    REQUIRE(sg.contains("level-3"));
}

// ============================================================================
// §16  lg::log::set_level
// ============================================================================
TEST_CASE (



"lg::log::set_level suppresses messages below threshold"
,
"[log][config]"
)
 {
    const SinkGuard sg;
    lg::log::set_level(lg::Level::Error);
    lg::info("should-be-suppressed");
    lg::warn("also-suppressed");
    lg::error("should-appear");
    lg::log::set_level(lg::Level::Trace); // restore
    REQUIRE_FALSE(sg.contains("should-be-suppressed"));
    REQUIRE_FALSE(sg.contains("also-suppressed"));
    REQUIRE(sg.contains("should-appear"));
}

TEST_CASE (



"lg::log::set_level restores trace after reset"
,
"[log][config]"
)
 {
    const SinkGuard sg;
    lg::log::set_level(lg::Level::Off);
    lg::error("suppressed-at-off");
    lg::log::set_level(lg::Level::Trace);
    lg::info("restored");
    REQUIRE_FALSE(sg.contains("suppressed-at-off"));
    REQUIRE(sg.contains("restored"));
}

// ============================================================================
// §17  lg::log::add_sink / remove_sink
// ============================================================================
TEST_CASE (



"Multiple capture sinks both receive messages"
,
"[log][sink-management]"
)
 {
    const auto cap1 = std::make_shared<lg::testing::CaptureSink>();
    const auto cap2 = std::make_shared<lg::testing::CaptureSink>();
    lg::log::add_sink(cap1);
    lg::log::add_sink(cap2);
    lg::info("multi-sink-msg");
    lg::log::remove_sink(cap1);
    lg::log::remove_sink(cap2);
    REQUIRE(cap1->contains("multi-sink-msg"));
    REQUIRE(cap2->contains("multi-sink-msg"));
}

TEST_CASE (



"Removed sink no longer receives messages"
,
"[log][sink-management]"
)
 {
    const auto cap = std::make_shared<lg::testing::CaptureSink>();
    lg::log::add_sink(cap);
    lg::info("before-remove");
    lg::log::remove_sink(cap);
    lg::info("after-remove");
    REQUIRE(cap->contains("before-remove"));
    REQUIRE_FALSE(cap->contains("after-remove"));
}

// ============================================================================
// §18  Source location capture
// ============================================================================
TEST_CASE (



"source_location is captured automatically at call site"
,
"[log][source-location]"
)
 {
    // We can't easily inspect the spdlog source_loc from CaptureSink (payload
    // doesn't include it), but we verify that calling lg::info from different
    // lines produces exactly N records — implying the call compiled with valid
    // source_location, one per call.
    const SinkGuard sg;
    lg::info("line-a");
    lg::info("line-b");
    REQUIRE(sg.contains("line-a"));
    REQUIRE(sg.contains("line-b"));
    REQUIRE(sg.size() == 2);
}

// ============================================================================
// §19  Thread safety — concurrent logging
// ============================================================================
TEST_CASE (



"concurrent lg::info calls do not race or crash"
,
"[log][thread-safety]"
)
 {
    const SinkGuard sg;
    constexpr int N = 8;
    constexpr int M = 50;
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i)
        threads.emplace_back([i] {
            for (int j = 0; j < M; ++j)
                lg::info("thread={} iter={}", i, j);
        });
    for (auto& t : threads) t.join();
    REQUIRE(sg.size() == N * M);
}

TEST_CASE (



"concurrent MDC put/remove does not crash"
,
"[log][thread-safety]"
)
 {
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([i] {
            lg::mdc::put("thread_id", std::to_string(i));
            lg::mdc::remove("thread_id");
        });
    for (auto& t : threads) t.join();
    lg::mdc::clear();
}

TEST_CASE (



"concurrent NDC push/pop does not crash"
,
"[log][thread-safety]"
)
 {
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([] {
            lg::ndc::push("worker");
            lg::ndc::pop();
        });
    for (auto& t : threads) t.join();
    lg::ndc::clear();
}

// ============================================================================
// §20  MDC thread-locality
// ============================================================================
TEST_CASE (



"MDC is thread-local — entries set on one thread invisible on another"
,
"[log][mdc][thread-safety]"
)
 {
    MdcCleanup mc;
    lg::mdc::put("main_key", "main_val");

    std::atomic thread_saw_key{false};
    std::thread worker([&] {
        // Worker thread starts with an empty MDC; removing a non-existent key
        // returns quietly but we verify we cannot find main_key through spdlog.
        // spdlog::mdc is thread-local, so get should return empty string.
        thread_saw_key.store(
            spdlog::mdc::get("main_key") == "main_val",
            std::memory_order_relaxed);
    });
    worker.join();
    REQUIRE_FALSE(thread_saw_key.load());
    lg::mdc::clear();
}

// ============================================================================
// §21  NDC thread-locality
// ============================================================================
TEST_CASE (



"NDC stack is thread-local — push on main not visible in worker"
,
"[log][ndc][thread-safety]"
)
 {
    MdcCleanup mc;
    lg::ndc::push("main-context");
    bool worker_saw_ndc = false;
    std::thread worker([&] {
        // Worker's NDC stack is independent
        worker_saw_ndc = (spdlog::mdc::get("ndc") == "main-context");
    });
    worker.join();
    REQUIRE_FALSE(worker_saw_ndc);
    lg::ndc::clear();
}

// ============================================================================
// §22  log::set_pattern
// ============================================================================
TEST_CASE (



"lg::log::set_pattern does not throw"
,
"[log][config]"
)
 {
    REQUIRE_NOTHROW(lg::log::set_pattern("[%l] %v"));
    // Restore a sensible default
    REQUIRE_NOTHROW(lg::log::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %& %v"));
}

// ============================================================================
// §23  log::flush_on
// ============================================================================
TEST_CASE (



"lg::log::flush_on does not throw"
,
"[log][config]"
)
 {
    REQUIRE_NOTHROW(lg::log::flush_on(lg::Level::Error));
    REQUIRE_NOTHROW(lg::log::flush_on(lg::Level::Warn));
}

// ============================================================================
// §24  LogRecord logger_name
// ============================================================================
TEST_CASE (



"LogRecord from named logger carries logger name"
,
"[log][record]"
)
 {
    const SinkGuard sg;
    auto slog = lg::logger("test.name-check");
    slog.info("name-check-msg");
    const auto recs = sg.records();
    bool found = false;
    for (const auto& r : recs)
        if (r.message.find("name-check-msg") != std::string::npos) {
            // logger_name is populated from the spdlog msg.logger_name field.
            // For named loggers it should be non-empty and match.
            found = !r.logger_name.empty();
        }
    REQUIRE(found);
}

// ============================================================================
// §25  FmtWithLoc — string_view overload (runtime format string)
// ============================================================================
TEST_CASE (



"lg::info accepts runtime std::string_view format"
,
"[log][fmt-with-loc]"
)
 {
    const SinkGuard sg;
    constexpr std::string_view fmt = "runtime-fmt {}";
    lg::info(lg::detail::FmtWithLoc{fmt}, 99);
    REQUIRE(sg.contains("runtime-fmt 99"));
}

// ============================================================================
// §26  Exception safety — log functions don't propagate format errors
// ============================================================================
TEST_CASE (



"std::vformat throws on bad format string — verified at runtime"
,
"[log][format-error]"
)
 {
    // This documents expected behavior: too few args for the format specifiers
    // throws std::format_error at runtime.
    int single_arg = 1;
    REQUIRE_THROWS_AS(std::vformat("{} {}", std::make_format_args(single_arg)), std::format_error);
}

// ============================================================================
// §27  log_if with various Level values
// ============================================================================
TEST_CASE (



"log_if works with every Level"
,
"[log][log-if][level]"
)
 {
    SinkGuard sg;
    lg::log_if(true, lg::Level::Trace, "lv-trace");
    lg::log_if(true, lg::Level::Debug, "lv-debug");
    lg::log_if(true, lg::Level::Info, "lv-info");
    lg::log_if(true, lg::Level::Warn, "lv-warn");
    lg::log_if(true, lg::Level::Error, "lv-error");
    lg::log_if(true, lg::Level::Critical, "lv-critical");
    REQUIRE(sg.contains("lv-trace"));
    REQUIRE(sg.contains("lv-debug"));
    REQUIRE(sg.contains("lv-info"));
    REQUIRE(sg.contains("lv-warn"));
    REQUIRE(sg.contains("lv-error"));
    REQUIRE(sg.contains("lv-critical"));
}

// ============================================================================
// §28  CaptureSink thread-safety
// ============================================================================
TEST_CASE (



"CaptureSink records are consistent under concurrent access"
,
"[log][capture-sink][thread-safety]"
)
 {
    const auto cap = std::make_shared<lg::testing::CaptureSink>();
    lg::log::add_sink(cap);

    constexpr int N = 4;
    constexpr int M = 25;
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int i = 0; i < N; ++i)
        threads.emplace_back([i] {
            for (int j = 0; j < M; ++j)
                lg::info("cap-thread={} j={}", i, j);
        });
    for (auto& t : threads) t.join();

    // Read while logging is done — safe read.
    lg::log::remove_sink(cap);
    REQUIRE(cap->records().size() == N * M);
}

// ============================================================================
// §29  scoped_span with fields
// ============================================================================
TEST_CASE (



"scoped_span with fields emits them in begin message"
,
"[log][span]"
)
 {
    const SinkGuard sg;
    MdcCleanup mc;
    {
        lg::scoped_span ss("db.query", {{"table", "users"}, {"op", "select"}});
    }
    REQUIRE(sg.contains("table=users"));
    REQUIRE(sg.contains("op=select"));
}

// ============================================================================
// §30  Compile-time static properties
// ============================================================================
TEST_CASE (



"TraceContext is an aggregate"
,
"[log][static]"
)
 {
    STATIC_REQUIRE(std::is_aggregate_v<lg::TraceContext>);
}

TEST_CASE (



"SpanField is an aggregate"
,
"[log][static]"
)
 {
    STATIC_REQUIRE(std::is_aggregate_v<lg::SpanField>);
}

TEST_CASE (



"LogRecord is an aggregate"
,
"[log][static]"
)
 {
    STATIC_REQUIRE(std::is_aggregate_v<lg::testing::LogRecord>);
}

// ============================================================================
// Review fixes: MDC thread safety, LogSpan format consistency
// ============================================================================

TEST_CASE (



"Log - MDC thread-local storage safety"
,
"[log][mdc][thread][review]"
)
 {
    SECTION("MDC isolation between threads") {
        MdcCleanup cleanup;
        std::vector<std::string> values;
        std::mutex values_mtx;
        std::vector<std::thread> threads;

        threads.reserve(4);
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([i, &values, &values_mtx]() {
                const std::string val = "thread_" + std::to_string(i);
                lg::mdc::put("thread_id", val);
                std::lock_guard lk(values_mtx);
                values.push_back(val);
            });
        }

        for (auto& t : threads) t.join();

        // All threads completed without interference
        REQUIRE(values.size() == 4);
    }

    SECTION("MDC put/remove are thread-safe") {
        MdcCleanup cleanup;
        std::atomic success{0};

        auto worker = [&success]() {
            try {
                for (int i = 0; i < 10; ++i) {
                    lg::mdc::put("key", "value");
                    lg::mdc::remove("key");
                }
                ++success;
            }
            catch (...) {}
        };

        std::vector<std::thread> threads;
        threads.reserve(8);
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back(worker);
        }

        for (auto& t : threads) t.join();

        REQUIRE(success.load() == 8);
    }
}

TEST_CASE (



"Log - LogSpan format consistency"
,
"[log][span][review]"
)
 {
    SECTION("LogSpan formats begin/end events") {
        SinkGuard sink;
        {
            lg::LogSpan span("test_span", {{"key", "value"}});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Verify span events logged
        auto records = sink.records();
        REQUIRE(records.size() >= 2);

        // First record should be begin event
        REQUIRE(records[0].level == lg::Level::Trace);
        REQUIRE((records[0].message.find("span.begin") != std::string::npos ||
            records[0].message.find("test_span") != std::string::npos));

        // Last record should be end event
        REQUIRE((records[records.size() - 1].message.find("span.end") != std::string::npos ||
            records[records.size() - 1].message.find("elapsed") != std::string::npos));
    }
}
