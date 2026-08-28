#pragma once

// ============================================================================
// log.hpp — turbo_twig structured logging
// ============================================================================
// A C++23, header-only logging facade over spdlog.
//
// Compile-time switch:
//   -DTT_LOG_ENABLED=0   hard-disables all logging (no-ops, zero arg eval)
//   -DTT_LOG_ENABLED=1   enables logging (default)
//
// Existing call-sites are unchanged:
//   lg::info("value={}", x);
//   lg::warn("..."); lg::error("..."); lg::debug("..."); lg::critical("...");
//
// New features:
//   lg::Level enum              — type-safe log levels
//   lg::mdc::*                  — Mapped Diagnostic Context (Spring MDC)
//   lg::ndc::*                  — Nested Diagnostic Context push/pop stack
//   lg::TraceContext            — trace_id / span_id propagation
//   auto source_location        — file:line injected automatically; no macros
//   lg::LogSpan                 — RAII span: logs begin+end with elapsed ns
//   lg::logger(name)            — per-module named loggers
//   lg::log_once(...)           — emit exactly once per call-site
//   lg::log_if(cond, ...)       — evaluate args only when condition is true
//   lg::rate_limited<N,P>(fn)   — emit at most N times per Period P
//   lg::log_exception(e, msg)   — format exception + nested chain
//   lg::log::set_json(true)     — switch to newline-delimited JSON output
//   lg::log::add_sink(ptr)      — attach additional spdlog sinks
//   lg::testing::CaptureSink    — in-process sink for unit-test assertions
// ============================================================================

#ifndef TT_LOG_ENABLED
#define TT_LOG_ENABLED 1
#endif

#include <atomic>
#include <chrono>
#include <concepts>
#include <exception>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ─── optional nadi lineage bridge ───────────────────────────────────────────
#if defined(__has_include) && __has_include("observability/nadi.hpp")
#  include "observability/nadi.hpp"
#  define TT_LOG_HAS_NADI 1
#else
#  define TT_LOG_HAS_NADI 0
#endif

// ─── spdlog ─────────────────────────────────────────────────────────────────
#if TT_LOG_ENABLED
#  if defined(__has_include)
#    if __has_include(<spdlog/spdlog.h>)
#      include <spdlog/spdlog.h>
#      include <spdlog/sinks/dist_sink.h>
#      include <spdlog/sinks/callback_sink.h>
#      include <spdlog/mdc.h>
#      include <spdlog/async.h>
#    else
#      undef  TT_LOG_ENABLED
#      define TT_LOG_ENABLED 0
#    endif
#  else
#    include <spdlog/spdlog.h>
#    include <spdlog/sinks/dist_sink.h>
#    include <spdlog/sinks/callback_sink.h>
#    include <spdlog/mdc.h>
#    include <spdlog/async.h>
#  endif
#endif

namespace lg {
    // ============================================================================
    // 1. Level enum — type-safe wrapper over spdlog::level
    // ============================================================================
    enum class Level : int {
        Trace = 0,
        Debug = 1,
        Info = 2,
        Warn = 3,
        Error = 4,
        Critical = 5,
        Off = 6,
    };

    // ============================================================================
    // 2. detail helpers
    // ============================================================================
    namespace detail {
        inline constexpr bool logging_enabled = (TT_LOG_ENABLED != 0);

        template <class... Ts>
        constexpr void swallow(Ts&&...) noexcept {}

#if TT_LOG_ENABLED
        [[nodiscard]] inline spdlog::source_loc to_spd(const std::source_location& loc) noexcept {
            return {loc.file_name(), static_cast<int>(loc.line()), loc.function_name()};
        }

        [[nodiscard]] inline spdlog::level::level_enum to_spd(Level lv) noexcept {
            return static_cast<spdlog::level::level_enum>(static_cast<int>(lv));
        }
#endif

        // ─── global dist_sink registry ──────────────────────────────────────────────
        // Returns the shared dist_sink that the default spdlog logger is wired to.
        // Called lazily on first log; subsequent calls return the same pointer.
#if TT_LOG_ENABLED
        inline std::shared_ptr<spdlog::sinks::dist_sink_mt>& global_dist_sink() {
            static auto sink = std::make_shared<spdlog::sinks::dist_sink_mt>();
            return sink;
        }

        inline std::shared_ptr<spdlog::logger>& global_logger() {
            static auto lgr = [] {
                auto& ds = global_dist_sink();
                // Seed the dist_sink with the current default logger's sinks so
                // users who configured spdlog before including this header are
                // not surprised.
                if (auto* raw = spdlog::default_logger_raw()) {
                    for (const auto& s : raw->sinks())
                        ds->add_sink(s);
                }
                auto l = std::make_shared<spdlog::logger>("turbo_twig", ds);
                l->set_level(spdlog::level::trace);
                spdlog::set_default_logger(l);
                return l;
            }();
            return lgr;
        }
#endif
    } // namespace detail

    // ============================================================================
    // 3. MDC — Mapped Diagnostic Context
    // ============================================================================
    // Wraps spdlog::mdc (thread-local key→value map).  Every log line in the
    // same thread automatically includes all active MDC entries.
    //
    // Usage (Spring-style):
    //   lg::mdc::put("user_id", "42");
    //   lg::info("processing order");        // → [...] [user_id:42] processing order
    //   lg::mdc::remove("user_id");
    //
    //   {
    //       lg::mdc::ScopedEntry _{" req_id", "abc-123"};
    //       lg::info("handling request");    // includes req_id
    //   }                                   // req_id removed automatically
    // ============================================================================
    namespace mdc {
        inline void put(const std::string_view key, const std::string_view value) {
            if constexpr (detail::logging_enabled) {
#if TT_LOG_ENABLED
                spdlog::mdc::put(std::string{key}, std::string{value});
#endif
            }
        }

        inline void remove(const std::string_view key) {
            if constexpr (detail::logging_enabled) {
#if TT_LOG_ENABLED
                spdlog::mdc::remove(std::string{key});
#endif
            }
        }

        inline void clear() noexcept {
            if constexpr (detail::logging_enabled) {
#if TT_LOG_ENABLED
                spdlog::mdc::clear();
#endif
            }
        }

        // RAII: sets key on construction, removes on destruction.
        struct ScopedEntry {
            std::string key_;

            ScopedEntry(const std::string_view k, const std::string_view v) : key_{k} {
                put(k, v);
            }

            ~ScopedEntry() noexcept { remove(key_); }

            ScopedEntry(const ScopedEntry&) = delete;

            ScopedEntry& operator=(const ScopedEntry&) = delete;

            ScopedEntry(ScopedEntry&&) = delete;

            ScopedEntry& operator=(ScopedEntry&&) = delete;
        };
    } // namespace mdc

    // ============================================================================
    // 4. NDC — Nested Diagnostic Context
    // ============================================================================
    // Thread-local push/pop stack whose entries are joined and injected into the
    // MDC under the key "ndc" on every push/pop.
    //
    // Usage:
    //   lg::ndc::ScopedPush _{" login-flow"};
    //   {
    //       lg::ndc::ScopedPush _2{"validate-token"};
    //       lg::info("checking token");  // ndc: login-flow > validate-token
    //   }
    // ============================================================================
    namespace ndc { namespace detail_ndc {
            inline std::vector<std::string>& stack() noexcept {
                thread_local std::vector<std::string> s;
                return s;
            }

            inline void sync_mdc() {
                const auto& st = stack();
                if (st.empty()) {
                    mdc::remove("ndc");
                    return;
                }
                std::string joined;
                for (std::size_t i = 0; i < st.size(); ++i) {
                    if (i) joined += " > ";
                    joined += st[i];
                }
                mdc::put("ndc", joined);
            }
        } // namespace detail_ndc

        inline void push(std::string_view entry) {
            detail_ndc::stack().emplace_back(entry);
            detail_ndc::sync_mdc();
        }

        inline void pop() noexcept {
            if (auto& st = detail_ndc::stack(); !st.empty()) st.pop_back();
            detail_ndc::sync_mdc();
        }

        inline void clear() noexcept {
            detail_ndc::stack().clear();
            mdc::remove("ndc");
        }

        struct ScopedPush {
            explicit ScopedPush(const std::string_view entry) { push(entry); }
            ~ScopedPush() noexcept { pop(); }

            ScopedPush(const ScopedPush&) = delete;

            ScopedPush& operator=(const ScopedPush&) = delete;

            ScopedPush(ScopedPush&&) = delete;

            ScopedPush& operator=(ScopedPush&&) = delete;
        };
    } // namespace ndc

    // ============================================================================
    // 5. TraceContext — W3C-style trace_id / span_id propagation
    // ============================================================================
    // Bridges with nadi::LineageToken when available; otherwise stores the pair
    // directly in the thread-local MDC so every log line in the same thread
    // carries trace correlation without any call-site changes.
    //
    // Usage:
    //   lg::TraceContext ctx = lg::get_trace_context();   // read current
    //   lg::set_trace_context({.trace_id=42, .span_id=7});
    //   lg::info("inside span");    // → [...] [trace=42 span=7] ...
    // ============================================================================
    struct TraceContext {
        std::uint64_t trace_id{0};
        std::uint64_t span_id{0};
    };

    namespace detail {
        inline void inject_trace_mdc(const TraceContext& ctx) {
            if (ctx.trace_id != 0) {
                mdc::put("trace", std::to_string(ctx.trace_id));
                mdc::put("span", std::to_string(ctx.span_id));
            }
            else {
                mdc::remove("trace");
                mdc::remove("span");
            }
        }

        [[nodiscard]] inline TraceContext read_nadi_lineage() noexcept {
#if TT_LOG_HAS_NADI
            const auto& lin = utils::nadi::detail::current_lineage;
            return {lin.root_id.value, lin.trace_id.value};
#else
            return {};
#endif
        }
    } // namespace detail

    [[nodiscard]] inline TraceContext get_trace_context() noexcept {
        return detail::read_nadi_lineage();
    }

    inline void set_trace_context(const TraceContext& ctx) {
        detail::inject_trace_mdc(ctx);
    }

    // ============================================================================
    // 6. Core logging functions
    // ============================================================================
    // Each free function captures std::source_location automatically (no macros).
    // Nadi lineage is read from the thread-local at every call and injected into
    // the MDC so it appears in the log line without any extra annotation.
    // ============================================================================
    namespace detail {
        // Snapshot nadi lineage and push into MDC for this log call, then restore.
        // Uses a local ScopedEntry pair so the MDC is clean after the call.
        struct NadiGuard {
            bool active_{false};

            NadiGuard() noexcept {
#if TT_LOG_HAS_NADI
                if (const auto& lin = utils::nadi::detail::current_lineage; lin.root_id.value != 0) {
                    spdlog::mdc::put("trace", std::to_string(lin.root_id.value));
                    spdlog::mdc::put("span", std::to_string(lin.trace_id.value));
                    active_ = true;
                }
#endif
            }

            ~NadiGuard() noexcept {
#if TT_LOG_HAS_NADI && TT_LOG_ENABLED
                if (active_) {
                    spdlog::mdc::remove("trace");
                    spdlog::mdc::remove("span");
                }
#endif
            }

            NadiGuard(const NadiGuard&) = delete;

            NadiGuard& operator=(const NadiGuard&) = delete;
        };

        // emit — pre-format with std::vformat and forward a string_view to spdlog
        // This avoids the template deduction problem with format_string_t<Args...>
        // in FmtWithLoc while still correctly rendering all arguments.
        template <typename... Args>
        void emit(
            Level level,
            std::source_location loc,
            std::string_view fmt,
            Args&&... args) {
            if constexpr (logging_enabled) {
#if TT_LOG_ENABLED
                NadiGuard guard;
                const std::string msg = std::vformat(fmt, std::make_format_args(args...));
                global_logger()->log(to_spd(loc), to_spd(level), spdlog::string_view_t{msg});
#else
                swallow(level, loc, fmt, std::forward<Args>(args)...);
#endif
            }
            else {
                swallow(level, loc, fmt, std::forward<Args>(args)...);
            }
        }
    } // namespace detail

    // ─── FmtWithLoc — non-template wrapper that captures source_location ─────────
    // The constructor is consteval so source_location::current() is evaluated at the
    // call site.  Args are deduced solely from the trailing variadic parameters of
    // the log functions below, which is the only pattern that works in C++23 when
    // mixing a defaulted source_location with a variadic pack.
    namespace detail {
        struct FmtWithLoc {
            std::string_view fmt;
            std::source_location loc;

            // Accept string literals (consteval so location is captured at call site)
            // NOLINTNEXTLINE(google-explicit-constructor)
            consteval FmtWithLoc(
                const char* f,
                const std::source_location l = std::source_location::current()) noexcept
                : fmt{f}, loc{l} {}

            // Accept std::string_view / std::string for runtime format strings
            // NOLINTNEXTLINE(google-explicit-constructor)
            FmtWithLoc(
                const std::string_view f,
                const std::source_location l = std::source_location::current()) noexcept
                : fmt{f}, loc{l} {}
        };
    } // namespace detail (FmtWithLoc)

    // ─── Primary log API ────────────────────────────────────────────────────────

    template <typename... Args>
    void info(detail::FmtWithLoc fl, Args&&... args) {
        detail::emit(Level::Info, fl.loc, fl.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(detail::FmtWithLoc fl, Args&&... args) {
        detail::emit(Level::Warn, fl.loc, fl.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(detail::FmtWithLoc fl, Args&&... args) {
        detail::emit(Level::Error, fl.loc, fl.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(detail::FmtWithLoc fl, Args&&... args) {
        detail::emit(Level::Debug, fl.loc, fl.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void critical(detail::FmtWithLoc fl, Args&&... args) {
        detail::emit(Level::Critical, fl.loc, fl.fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void trace(detail::FmtWithLoc fl, Args&&... args) {
        detail::emit(Level::Trace, fl.loc, fl.fmt, std::forward<Args>(args)...);
    }

    // ============================================================================
    // 7. Named loggers
    // ============================================================================
    // Each named logger shares the global dist_sink so it respects all attached
    // sinks.  Log levels are independent per logger.
    //
    // Usage:
    //   auto db_log = lg::logger("db");
    //   db_log.set_level(lg::Level::Debug);
    //   db_log.info("connected to {}", host);
    // ============================================================================
    class Logger {
    public:
        explicit Logger(const std::string_view name) {
#if TT_LOG_ENABLED
            auto existing = spdlog::get(std::string{name});
            if (existing) {
                inner_ = std::move(existing);
            }
            else {
                inner_ = std::make_shared<spdlog::logger>(
                    std::string{name}, detail::global_dist_sink());
                inner_->set_level(spdlog::level::trace);
                spdlog::register_logger(inner_);
            }
#endif
        }

        void set_level(const Level lv) const noexcept {
#if TT_LOG_ENABLED
            if (inner_) inner_->set_level(detail::to_spd(lv));
#endif
        }

        template <typename... Args>
        void info(detail::FmtWithLoc fl, Args&&... args) {
            log_impl(Level::Info, fl.loc, fl.fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void warn(detail::FmtWithLoc fl, Args&&... args) {
            log_impl(Level::Warn, fl.loc, fl.fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void error(detail::FmtWithLoc fl, Args&&... args) {
            log_impl(Level::Error, fl.loc, fl.fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void debug(detail::FmtWithLoc fl, Args&&... args) {
            log_impl(Level::Debug, fl.loc, fl.fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void critical(detail::FmtWithLoc fl, Args&&... args) {
            log_impl(Level::Critical, fl.loc, fl.fmt, std::forward<Args>(args)...);
        }

    private:
        template <typename... Args>
        void log_impl(Level lv, const std::source_location& loc, std::string_view fmt, Args&&... args) {
            if constexpr (detail::logging_enabled) {
#if TT_LOG_ENABLED
                if (!inner_) return;
                detail::NadiGuard guard;
                const std::string msg = std::vformat(fmt, std::make_format_args(args...));
                inner_->log(detail::to_spd(loc), detail::to_spd(lv),
                            spdlog::string_view_t{msg});
#else
                detail::swallow(lv, loc, fmt, std::forward<Args>(args)...);
#endif
            }
        }

#if TT_LOG_ENABLED
        std::shared_ptr<spdlog::logger> inner_;
#endif
    };

    // Factory — returns a Logger by value (cheap: shared_ptr inside).
    [[nodiscard]] inline Logger logger(const std::string_view name) {
        return Logger{name};
    }

    // ============================================================================
    // 8. LogSpan — RAII tracing span
    // ============================================================================
    // Logs a BEGIN event on construction and an END event with elapsed nanoseconds
    // on destruction.  Automatically captures the nadi trace_id/parent_id in
    // the MDC for the lifetime of the span, so all log calls inside the span
    // carry the span identity.
    //
    // Usage:
    //   {
    //       lg::LogSpan span("db.query", {{"table", "orders"}});
    //       // ... do work ...
    //   }  // → logs end event with elapsed_ns
    //
    // With nadi active inside a PulseScope the span picks up the lineage
    // automatically — no extra annotation needed.
    // ============================================================================
    struct SpanField {
        std::string_view key;
        std::string value;
    };

    class LogSpan {
    public:
        static constexpr std::string_view FMT_BEGIN = "[span.begin] {}{}";
        static constexpr std::string_view FMT_END = "[span.end]   {} elapsed_ns={}";

        explicit LogSpan(
            const std::string_view name,
            const std::initializer_list<SpanField> fields = {},
            const std::source_location loc = std::source_location::current())
            : name_{name}
              , start_{std::chrono::steady_clock::now()}
              , loc_{loc} {
            // Snapshot nadi lineage at span open
#if TT_LOG_HAS_NADI
            const auto& lin = utils::nadi::detail::current_lineage;
            trace_id_ = lin.root_id.value;
            span_id_ = lin.trace_id.value;
#endif
            // Push span identity into MDC so all log lines inside carry it
            if (trace_id_ != 0) {
                mdc::put("trace", std::to_string(trace_id_));
                mdc::put("span", std::to_string(span_id_));
            }
            mdc::put("span.name", name_);

            // Collect extra fields into a flat string for the begin message
            std::string field_str;
            for (const auto& [key, value] : fields) {
                field_str += ' ';
                field_str += key;
                field_str += '=';
                field_str += value;
            }
            if constexpr (detail::logging_enabled) {
#if TT_LOG_ENABLED
                detail::global_logger()->log(
                    detail::to_spd(loc_), spdlog::level::trace,
                    FMT_BEGIN, name_, field_str);
#endif
            }
        }

        ~LogSpan() noexcept {
            const auto elapsed_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start_)
                .count();

            if constexpr (detail::logging_enabled) {
#if TT_LOG_ENABLED
                detail::global_logger()->log(
                    detail::to_spd(loc_), spdlog::level::trace,
                    FMT_END, name_, elapsed_ns);
#endif
            }
            mdc::remove("span.name");
            if (trace_id_ != 0) {
                mdc::remove("trace");
                mdc::remove("span");
            }
        }

        LogSpan(const LogSpan&) = delete;

        LogSpan& operator=(const LogSpan&) = delete;

        LogSpan(LogSpan&&) = delete;

        LogSpan& operator=(LogSpan&&) = delete;

    private:
        std::string name_;
        std::chrono::steady_clock::time_point start_;
        std::source_location loc_;
        std::uint64_t trace_id_{0};
        std::uint64_t span_id_{0};
    };

    // scoped_span — alias that also pushes an NDC entry, matching Spring's
    // @SpanTag / Slf4j MDC.put behavior: every log line inside the span shows
    // the span name in the ndc prefix.
    class scoped_span {
    public:
        explicit scoped_span(
            const std::string_view name,
            const std::initializer_list<SpanField> fields = {},
            const std::source_location loc = std::source_location::current())
            : ndc_guard_{name}
              , span_{name, fields, loc} {}

    private:
        ndc::ScopedPush ndc_guard_;
        LogSpan span_;
    };

    // ============================================================================
    // 9. log_once — emit a given log line exactly once per call-site
    // ============================================================================
    // Uses a function-static atomic flag so the call is truly once-per-process
    // regardless of how many threads race at the call-site.
    //
    // Usage:
    //   lg::log_once(lg::Level::Warn, "config file missing, using defaults");
    // ============================================================================
    template <typename... Args>
    void log_once(Level lv, detail::FmtWithLoc fl, Args&&... args) {
        static std::atomic fired{false};
        if (fired.exchange(true, std::memory_order_relaxed)) return;
        detail::emit(lv, fl.loc, fl.fmt, std::forward<Args>(args)...);
    }

    // ============================================================================
    // 10. log_if — evaluate and emit only when condition is true
    // ============================================================================
    template <typename... Args>
    void log_if(const bool condition, Level lv, detail::FmtWithLoc fl, Args&&... args) {
        if (condition)
            detail::emit(lv, fl.loc, fl.fmt, std::forward<Args>(args)...);
    }

    // ============================================================================
    // 11. rate_limited — emit at most N times per duration Period
    // ============================================================================
    // Usage:
    //   lg::rate_limited<5, std::chrono::seconds<1>>([]{ lg::warn("overload"); });
    //
    // The template form takes a count and a chrono duration type.
    // ============================================================================
    template <std::size_t MaxCount, typename Period = std::chrono::seconds>
    class RateLimiter {
    public:
        explicit RateLimiter(Period period = Period{1}) : period_{period} {}

        template <std::invocable Fn>
        void operator()(Fn&& fn) {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard lock{mu_};
            if (now - window_start_ >= period_) {
                window_start_ = now;
                count_ = 0;
            }
            if (count_ < MaxCount) {
                ++count_;
                std::forward < Fn > (fn)();
            }
        }

    private:
        std::mutex mu_;
        std::chrono::steady_clock::time_point window_start_{std::chrono::steady_clock::now()};
        std::size_t count_{0};
        Period period_;
    };

    // Convenience free-function wrapper that owns a static RateLimiter per call-site.
    // Usage: lg::rate_limited<5>([]{ lg::warn("hot path warning"); });
    template <std::size_t MaxCount, typename Fn>
        requires std::invocable<Fn>
    void rate_limited(Fn&& fn) {
        static RateLimiter<MaxCount> limiter{};
        limiter(std::forward<Fn>(fn));
    }

    // ============================================================================
    // 12. log_exception — format exception + nested chain
    // ============================================================================
    namespace detail {
        inline std::string exception_chain(const std::exception& e, const int depth = 0) {
            std::string msg(depth * 2, ' ');
            msg += e.what();
            try {
                std::rethrow_if_nested(e);
            }
            catch (const std::exception& nested) {
                msg += '\n';
                msg += exception_chain(nested, depth + 1);
            }
            catch (...) {
                msg += "\n  (unknown nested exception)";
            }
            return msg;
        }
    } // namespace detail

    inline void log_exception(
        const std::exception& e,
        std::string_view context = {},
        const Level lv = Level::Error,
        const std::source_location loc = std::source_location::current()) {
        const auto chain = detail::exception_chain(e);
        if constexpr (detail::logging_enabled) {
#if TT_LOG_ENABLED
            if (context.empty())
                detail::global_logger()->log(detail::to_spd(loc), detail::to_spd(lv),
                                             "exception: {}", chain);
            else
                detail::global_logger()->log(detail::to_spd(loc), detail::to_spd(lv),
                                             "{}: {}", context, chain);
#endif
        }
    }

    // ============================================================================
    // 13. Global configuration (namespace lg::log)
    // ============================================================================
    namespace log {
        inline constexpr bool enabled = detail::logging_enabled;

        inline void set_level(const Level lv) noexcept {
            if constexpr (enabled) {
#if TT_LOG_ENABLED
                detail::global_logger()->set_level(detail::to_spd(lv));
#endif
            }
        }

        inline void set_pattern(const std::string_view pattern) noexcept {
            if constexpr (enabled) {
#if TT_LOG_ENABLED
                spdlog::set_pattern(std::string{pattern});
#endif
            }
        }

        inline void flush_on(const Level lv) noexcept {
            if constexpr (enabled) {
#if TT_LOG_ENABLED
                spdlog::flush_on(detail::to_spd(lv));
#endif
            }
        }

        // Explicit process-boundary flush for short-lived tools and examples.
        // Normal library code remains fully asynchronous and pays nothing extra.
        inline void flush() noexcept {
            if constexpr (enabled) {
#if TT_LOG_ENABLED
                detail::global_logger()->flush();
#endif
            }
        }

        // Attach an additional spdlog sink (e.g. file sink, network sink).
        inline void add_sink(
#if TT_LOG_ENABLED
            std::shared_ptr<spdlog::sinks::sink> sink
#else
            std::nullptr_t
#endif
        ) {
            if constexpr (enabled) {
#if TT_LOG_ENABLED
                detail::global_dist_sink()->add_sink(std::move(sink));
#endif
            }
        }

        // Remove a previously-added sink by pointer identity.
        inline void remove_sink(
#if TT_LOG_ENABLED
            const std::shared_ptr<spdlog::sinks::sink>& sink
#else
            std::nullptr_t
#endif
        ) {
            if constexpr (enabled) {
#if TT_LOG_ENABLED
                detail::global_dist_sink()->remove_sink(sink);
#endif
            }
        }

        // Switch to newline-delimited JSON output (for log aggregators).
        // Each log line becomes:  {"ts":"...","level":"info","msg":"...","mdc":{...}}
        // spdlog's pattern syntax covers most fields; MDC is included via %v when
        // the mdc pattern flag is active.
        inline void set_json(const bool enable) noexcept {
            if constexpr (enabled) {
#if TT_LOG_ENABLED
                if (enable) {
                    // JSON pattern: timestamp, level, logger, mdc, message
                    spdlog::set_pattern(
                        R"({"ts":"%Y-%m-%dT%H:%M:%S.%e","level":"%l","logger":"%n","mdc":{%&},"msg":"%v"})");
                }
                else {
                    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %& %v");
                }
#endif
            }
        }

        // Switch to async logging (thread-pool backed queue).
        // queue_size: number of log records to buffer (power-of-two recommended).
        inline void set_async(const std::size_t queue_size = 8192) {
            if constexpr (enabled) {
#if TT_LOG_ENABLED
                spdlog::init_thread_pool(queue_size, 1);
                // Rebuild global logger as async
                const auto async_lgr = std::make_shared<spdlog::async_logger>(
                    "turbo_twig",
                    detail::global_dist_sink(),
                    spdlog::thread_pool(),
                    spdlog::async_overflow_policy::block);
                async_lgr->set_level(spdlog::level::trace);
                spdlog::set_default_logger(async_lgr);
                detail::global_logger() = async_lgr;
#endif
            }
        }
    } // namespace log

    // ============================================================================
    // 14. testing::CaptureSink — in-process sink for unit-test assertions
    // ============================================================================
    // Usage:
    //   auto cap = std::make_shared<lg::testing::CaptureSink>();
    //   lg::log::add_sink(cap);
    //   lg::info("hello world");
    //   REQUIRE(cap->contains("hello world"));
    //   lg::log::remove_sink(cap);
    //   cap->clear();
    // ============================================================================
    namespace testing {
        struct LogRecord {
            Level level;
            std::string message;
            std::string logger_name;
        };

#if TT_LOG_ENABLED
        class CaptureSink final : public spdlog::sinks::base_sink<std::mutex> {
        public:
            [[nodiscard]] std::vector<LogRecord> records() const {
                std::lock_guard lock{mutex_};
                return records_;
            }

            [[nodiscard]] bool contains(const std::string_view substr) const {
                std::lock_guard lock{mutex_};
                for (const auto& r : records_)
                    if (r.message.find(substr) != std::string::npos) return true;
                return false;
            }

            [[nodiscard]] std::string last_message() const {
                std::lock_guard lock{mutex_};
                return records_.empty() ? "" : records_.back().message;
            }

            void clear() {
                std::lock_guard lock{mutex_};
                records_.clear();
            }

        protected:
            void sink_it_(const spdlog::details::log_msg& msg) override {
                spdlog::memory_buf_t formatted;
                formatter_->format(msg, formatted);
                std::lock_guard lock{mutex_};
                records_.push_back({
                    static_cast<Level>(static_cast<int>(msg.level)),
                    std::string{msg.payload.data(), msg.payload.size()},
                    std::string{msg.logger_name.data(), msg.logger_name.size()},
                });
            }

            void flush_() override {}

        private:
            mutable std::mutex mutex_;
            std::vector<LogRecord> records_;
        };
#else
        // Stub when logging is disabled — satisfies the API without spdlog types.
        class CaptureSink {
        public:
            [[nodiscard]] std::vector<LogRecord> records() const { return {}; }
            [[nodiscard]] bool contains(std::string_view) const { return false; }
            [[nodiscard]] std::string last_message() const { return {}; }

            void clear() {}
        };
#endif
    } // namespace testing
} // namespace lg
