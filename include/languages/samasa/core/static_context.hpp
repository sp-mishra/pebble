#pragma once

// samasa/core/static_context.hpp — Consteval parse context over fixed-capacity buffers.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// static_token_stream<TK, MaxTokens, MaxTrivia>
//   — consteval-compatible view over a static_token_buffer.
//   — satisfies cursor<> stream requirements.
//
// static_parse_context<SK, TK, MaxTokens, MaxTrivia, MaxEvents, MaxDiags>
//   — consteval-compatible parse context.
//   — satisfies the same interface as parse_context<SK,TK> for matchers and rules.
//   — holds static_event_stream, a static_diagnostic_sink, and cursor state.
//   — static_stats_type provides constexpr-compatible token/node counters
//     (a lightweight substitute for parse_tree_stats, which uses unordered_map).

#include <cstdint>
#include <string_view>
#include "cursor.hpp"
#include "limits.hpp"
#include "parse_output.hpp"

namespace lang::samasa {
    // ---- static_token_stream<TK, MaxTokens, MaxTrivia> ---------------------
    // A consteval-compatible token stream view over a static_token_buffer.
    // Satisfies cursor<>'s size()/operator[] requirements.

    template <class TK, std::uint32_t MaxTokens, std::uint32_t MaxTrivia>
    struct static_token_stream {
        using value_type = token<TK>;

        const static_token_buffer<TK, MaxTokens, MaxTrivia>* buf = nullptr;

        [[nodiscard]] constexpr std::uint32_t size() const noexcept {
            return buf ? buf->size() : 0u;
        }

        [[nodiscard]] constexpr const token<TK>& operator[](std::uint32_t i) const noexcept {
            return buf->tokens[i];
        }

        [[nodiscard]] constexpr bool empty() const noexcept { return size() == 0; }
    };

    // ---- static_parse_context<SK, TK, MaxTokens, MaxTrivia, MaxEvents, MaxDiags> ---

    template <class SK, class TK,
              std::uint32_t MaxTokens = 4096,
              std::uint32_t MaxTrivia = MaxTokens * 2,
              std::uint32_t MaxEvents = 4096,
              std::uint32_t MaxDiags = 256>
    class static_parse_context {
    public:
        using syntax_kind = SK;
        using token_kind = TK;
        using stream_type = static_token_stream<TK, MaxTokens, MaxTrivia>;
        using cursor_type = cursor<stream_type>;
        using event_marker = typename static_event_stream<SK, MaxEvents>::marker;

        struct checkpoint_type {
            cursor_type cur;
            event_marker ev_snap;
            std::uint32_t diag_count;
            std::uint32_t repair_count;
        };

        // Constexpr-compatible substitute for parse_tree_stats.
        // parse_tree_stats uses std::unordered_map and is not constexpr-viable.
        struct static_stats_type {
            std::uint32_t total_tokens = 0;
            std::uint32_t production_nodes = 0;
        };

        constexpr explicit static_parse_context(
            const static_token_buffer<TK, MaxTokens, MaxTrivia>& tokens,
            std::string_view source,
            limits budget = {}) noexcept
            : source_(source)
              , budget_(budget) {
            stream_.buf = &tokens;
            cur_ = {&stream_, 0};
        }

        // ---- Cursor --------------------------------------------------------
        [[nodiscard]] constexpr cursor_type cursor() const noexcept { return cur_; }
        constexpr void set_cursor(cursor_type c) noexcept { cur_ = c; }

        // ---- Stream (mirrors parse_context::stream() API) ------------------
        [[nodiscard]] constexpr const stream_type& stream() const noexcept { return stream_; }

        // ---- Events --------------------------------------------------------
        constexpr static_event_stream<SK, MaxEvents>& events() noexcept { return events_; }

        // ---- Checkpoint / rollback -----------------------------------------
        [[nodiscard]] constexpr checkpoint_type checkpoint() const noexcept {
            return {cur_, events_.snapshot(), diag_count_, repairs_};
        }

        constexpr void rollback(const checkpoint_type& cp) noexcept {
            cur_ = cp.cur;
            events_.rollback(cp.ev_snap);
            diag_count_ = cp.diag_count;
            repairs_ = cp.repair_count;
        }

        // ---- Diagnostics ---------------------------------------------------
        constexpr void emit(diagnostic d) noexcept {
            if (diag_count_ < MaxDiags) diags_[diag_count_++] = d;
        }

        [[nodiscard]] constexpr bool has_errors() const noexcept { return diag_count_ > 0; }
        [[nodiscard]] constexpr std::uint32_t diag_count() const noexcept { return diag_count_; }

        [[nodiscard]] constexpr const diagnostic& diag(std::uint32_t i) const noexcept {
            return diags_[i];
        }

        // ---- Depth ---------------------------------------------------------
        [[nodiscard]] constexpr bool over_depth() const noexcept { return depth_ >= budget_.max_depth; }
        constexpr void push_depth() noexcept { ++depth_; }
        constexpr void pop_depth() noexcept { if (depth_ > 0) --depth_; }

        // ---- Repair budget -------------------------------------------------
        [[nodiscard]] constexpr bool over_repair_limit() const noexcept { return repairs_ >= budget_.max_repairs; }
        constexpr void inc_repairs() noexcept { ++repairs_; }
        [[nodiscard]] constexpr std::uint32_t repairs() const noexcept { return repairs_; }

        // ---- Node budget ---------------------------------------------------
        [[nodiscard]] constexpr bool over_node_limit() const noexcept { return nodes_ >= budget_.max_nodes; }
        constexpr void inc_nodes() noexcept { ++nodes_; }

        // ---- Stats (constexpr-compatible subset of parse_tree_stats) -------
        constexpr static_stats_type& stats() noexcept { return stats_; }

        // ---- Source text ---------------------------------------------------
        [[nodiscard]] constexpr std::string_view source() const noexcept { return source_; }

        [[nodiscard]] constexpr std::string_view
        source_text(std::uint32_t offset, std::uint32_t length) const noexcept {
            if (offset + length > source_.size()) return {};
            return source_.substr(offset, length);
        }

        // ---- Furthest-error (no-op in static context) ----------------------
        constexpr void update_furthest([[maybe_unused]] std::uint32_t offset) noexcept {}

        // ---- Overflow check ------------------------------------------------
        [[nodiscard]] constexpr bool overflow() const noexcept {
            return events_.overflow() || diag_count_ >= MaxDiags;
        }

    private:
        stream_type stream_{};
        std::string_view source_;
        limits budget_;
        cursor_type cur_{};
        static_event_stream<SK, MaxEvents> events_{};
        std::array<diagnostic, MaxDiags> diags_{};
        std::uint32_t diag_count_ = 0;
        std::uint32_t depth_ = 0;
        std::uint32_t repairs_ = 0;
        std::uint32_t nodes_ = 0;
        static_stats_type stats_{};
    };
} // namespace lang::samasa
