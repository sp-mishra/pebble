#pragma once

// samasa/policies/trace_policy.hpp — Parse trace policies.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// trace_event_kind  — enum of trace events emitted during parsing.
// trace_event       — one record: kind + rule name + token position.
//
// no_trace          — default; zero overhead; all on_event() calls compile away.
// collecting_trace  — accumulates trace_event records into a std::vector.
//
// Integration: parse_context<SK,TK,TracePolicy> forwards calls to trace().
// Opt in by passing a collecting_trace as parse_options::trace (when context supports it).
//
// Cost: when no_trace, the compiler eliminates all tracing call sites.
// When collecting_trace, each event is one push_back.

#include <cstdint>
#include <string_view>
#include <vector>

namespace lang::samasa {
    enum class trace_event_kind : std::uint8_t {
        enter_rule = 0,
        exit_rule = 1,
        match_token = 2,
        soft_fail = 3,
        hard_fail = 4,
        cut = 5,
        rollback = 6,
        recover = 7,
        emit_node = 8,
    };

    struct trace_event {
        trace_event_kind kind = trace_event_kind::enter_rule;
        std::string_view rule_name; // rule::name_sv — static storage
        std::uint32_t token_pos = 0; // cursor position at event time
    };

    // ---- no_trace ----------------------------------------------------------
    // Zero storage, zero overhead. All methods are constexpr no-ops.

    struct no_trace {
        static constexpr bool enabled = false;

        constexpr void on_event(trace_event) noexcept {}
        constexpr void enter(std::string_view, std::uint32_t) noexcept {}
        constexpr void exit(std::string_view, std::uint32_t) noexcept {}
        constexpr void token(std::uint32_t) noexcept {}
        constexpr void fail(std::string_view, std::uint32_t, bool /*hard*/) noexcept {}
        constexpr void cut(std::string_view, std::uint32_t) noexcept {}
        constexpr void roll(std::uint32_t) noexcept {}
        constexpr void node(std::string_view, std::uint32_t) noexcept {}
    };

    // ---- collecting_trace --------------------------------------------------
    // Collects all trace events. Attach to parse_options or context directly.

    struct collecting_trace {
        static constexpr bool enabled = true;

        std::vector<trace_event> events;

        void on_event(trace_event e) { events.push_back(e); }

        void enter(std::string_view rule, std::uint32_t pos) {
            events.push_back({trace_event_kind::enter_rule, rule, pos});
        }

        void exit(std::string_view rule, std::uint32_t pos) {
            events.push_back({trace_event_kind::exit_rule, rule, pos});
        }

        void token(std::uint32_t pos) {
            events.push_back({trace_event_kind::match_token, {}, pos});
        }

        void fail(std::string_view rule, std::uint32_t pos, bool hard) {
            events.push_back({
                hard
                    ? trace_event_kind::hard_fail
                    : trace_event_kind::soft_fail,
                rule, pos
            });
        }

        void cut(std::string_view rule, std::uint32_t pos) {
            events.push_back({trace_event_kind::cut, rule, pos});
        }

        void roll(std::uint32_t pos) {
            events.push_back({trace_event_kind::rollback, {}, pos});
        }

        void node(std::string_view kind_name, std::uint32_t pos) {
            events.push_back({trace_event_kind::emit_node, kind_name, pos});
        }

        [[nodiscard]] std::size_t size() const noexcept { return events.size(); }
        void clear() noexcept { events.clear(); }
    };
} // namespace lang::samasa
