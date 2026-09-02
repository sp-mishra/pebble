#pragma once

// samasa/core/context.hpp — Single mutable parse state threaded through matchers.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// parse_context<SK,TK,Policies...> — aggregates all mutable parse state:
//   cursor into token_stream, source text, event_stream, collecting_sink,
//   parse_tree_stats, depth, furthest_error, budgets.
//
// parse_checkpoint — snapshot for backtracking: cursor + event_id + diag count +
//   repair count. cut is seq-local (see combinators.hpp); no global committed flag.
//
// Matchers receive a reference to this; no globals, no std::function.
// Policies are template params — zero-cost compile-time dispatch.

#include <cstdint>
#include <string_view>
#include "diagnostic.hpp"
#include "limits.hpp"
#include "cursor.hpp"
#include "../tree/event_stream.hpp"
#include "../lex/token_stream.hpp"
#include "languages/generic/core/diagnostics.hpp"
#include "languages/generic/core/parse_stats.hpp"
#include "../policies/memo_policy.hpp"
#include "../policies/trace_policy.hpp"

namespace lang::samasa {
    // Full checkpoint for backtracking — covers cursor, event stream position, diagnostics,
    // and repair counter so that failed alternatives leave no observable side-effects.
    template <class CursorType, class EventMarker>
    struct parse_checkpoint {
        CursorType cursor;
        EventMarker event_snapshot;
        std::size_t diagnostic_count;
        std::uint32_t repair_count;
    };

    template <class SyntaxKind, class TokenKind,
              class MemoPolicy = no_memo, class TracePolicy = no_trace>
    class parse_context {
    public:
        using syntax_kind = SyntaxKind;
        using token_kind = TokenKind;
        using stream_type = token_stream<TokenKind>;
        using cursor_type = cursor<stream_type>;
        using event_marker = typename event_stream<SyntaxKind>::marker;
        using checkpoint_type = parse_checkpoint<cursor_type, event_marker>;
        using memo_policy = MemoPolicy;
        using trace_policy = TracePolicy;

        parse_context(
            const stream_type& stream,
            std::string_view source,
            event_stream<SyntaxKind>& events,
            lang::collecting_sink<diagnostic>& sink,
            lang::parse_tree_stats& stats,
            limits budget = {})
            : stream_(&stream)
              , source_(source)
              , events_(events)
              , sink_(sink)
              , stats_(stats)
              , budget_(budget)
              , cur_{stream_, 0} {}

        // ---- Cursor --------------------------------------------------------
        [[nodiscard]] cursor_type cursor() const noexcept { return cur_; }
        void set_cursor(cursor_type c) noexcept { cur_ = c; }

        // ---- Stream --------------------------------------------------------
        [[nodiscard]] const stream_type& stream() const noexcept { return *stream_; }

        // ---- Source text ---------------------------------------------------
        [[nodiscard]] std::string_view source_text(std::uint32_t offset, std::uint32_t length) const noexcept {
            if (offset + length > source_.size()) return {};
            return source_.substr(offset, length);
        }

        [[nodiscard]] std::string_view source() const noexcept { return source_; }

        // ---- Event stream --------------------------------------------------
        event_stream<SyntaxKind>& events() noexcept { return events_; }

        // ---- Checkpoint / rollback -----------------------------------------
        // Takes a full snapshot of cursor, event position, diagnostic count,
        // and repair counter. rollback() restores all four atomically.
        [[nodiscard]] checkpoint_type checkpoint() const noexcept {
            return {cur_, events_.snapshot(), sink_.size(), repairs_};
        }

        void rollback(const checkpoint_type& cp) noexcept {
            cur_ = cp.cursor;
            events_.rollback(cp.event_snapshot);
            sink_.truncate(cp.diagnostic_count);
            repairs_ = cp.repair_count;
        }

        // ---- Diagnostic sink -----------------------------------------------
        void emit(diagnostic d) { sink_.on_diagnostic(std::move(d)); }
        [[nodiscard]] bool has_errors() const noexcept { return sink_.has_errors(); }

        // ---- Depth ---------------------------------------------------------
        [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }
        [[nodiscard]] bool over_depth() const noexcept { return depth_ >= budget_.max_depth; }
        void push_depth() noexcept { ++depth_; }
        void pop_depth() noexcept { if (depth_ > 0) --depth_; }

        // ---- Furthest-error ------------------------------------------------
        void update_furthest(std::uint32_t offset) noexcept {
            if (offset > furthest_error_) furthest_error_ = offset;
        }

        [[nodiscard]] std::uint32_t furthest_error() const noexcept { return furthest_error_; }

        // ---- Node budget ---------------------------------------------------
        [[nodiscard]] bool over_node_limit() const noexcept {
            return stats_.production_nodes >= budget_.max_nodes;
        }

        void inc_nodes() noexcept { ++stats_.production_nodes; }

        // ---- Repair budget -------------------------------------------------
        [[nodiscard]] bool over_repair_limit() const noexcept { return repairs_ >= budget_.max_repairs; }
        void inc_repairs() noexcept { ++repairs_; }
        [[nodiscard]] std::uint32_t repairs() const noexcept { return repairs_; }

        // ---- Stats ---------------------------------------------------------
        lang::parse_tree_stats& stats() noexcept { return stats_; }
        [[nodiscard]] MemoPolicy& memo() noexcept { return memo_; }
        [[nodiscard]] TracePolicy& trace() noexcept { return trace_; }

        // ---- Limits --------------------------------------------------------
        [[nodiscard]] const limits& budget() const noexcept { return budget_; }

    private:
        const stream_type* stream_;
        std::string_view source_;
        event_stream<SyntaxKind>& events_;
        lang::collecting_sink<diagnostic>& sink_;
        lang::parse_tree_stats& stats_;
        limits budget_;
        cursor_type cur_;
        std::uint32_t depth_ = 0;
        std::uint32_t furthest_error_ = 0;
        std::uint32_t repairs_ = 0;
        [[no_unique_address]] MemoPolicy memo_{};
        [[no_unique_address]] TracePolicy trace_{};
    };
} // namespace lang::samasa
