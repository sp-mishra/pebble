#pragma once

// languages/generic/tree/event_log.hpp — Generic flat parse-event log.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// event_kind              — shared 5-value discriminant for all parse events.
// parse_event<KE,DC>      — one event record; KE = node-kind enum, DC = diag-code type.
// event_log<KE,DC>        — vector-backed log; marker/rollback enable O(1) backtrack.
//
// Backtracking semantics (mirrors samasa event_stream):
//   begin() opens a node, returns marker.
//   rollback(m): if tokens were committed after m, tombstone the begin event (don't truncate);
//                otherwise truncate cleanly. samasa event_stream §82-93 contract preserved.
//
// DiagCode defaults to std::uint16_t for frontends that don't define an enum.
// samasa wires DiagCode = samasa_diag_code in Stage 3.

#include <cstdint>
#include <vector>
#include "spans.hpp"

namespace lang {
    // ---- event_kind --------------------------------------------------------

    enum class event_kind : std::uint8_t {
        begin_node = 0,
        token = 1,
        end_node = 2,
        error = 3,
        tombstone = 4, // rolled-back begin_node with committed tokens
    };

    // ---- parse_event -------------------------------------------------------

    template <class KindEnum, class DiagCode = std::uint16_t>
    struct parse_event {
        event_kind kind = event_kind::tombstone;
        // node_kind and syntax are the same storage — samasa uses .syntax;
        // generic users use .node_kind. Both valid: same offset, same type.
        union {
            KindEnum node_kind{};
            KindEnum syntax;
        };

        std::uint32_t token_index = 0; // token event
        byte_span span = {}; // error / end_node
        DiagCode diag_code = {}; // error events
    };

    // ---- event_log ---------------------------------------------------------

    template <class KindEnum, class DiagCode = std::uint16_t>
    class event_log {
    public:
        using event_type = parse_event<KindEnum, DiagCode>;

        struct marker {
            std::uint32_t event_index = 0;
            std::uint32_t token_count = 0;
        };

        marker begin(KindEnum k) {
            marker m{static_cast<std::uint32_t>(events_.size()), token_count_};
            events_.push_back({event_kind::begin_node, k, 0, {}, {}});
            ++depth_;
            return m;
        }

        void token(std::uint32_t idx) {
            events_.push_back({event_kind::token, {}, idx, {}, {}});
            ++token_count_;
        }

        void error(DiagCode code, byte_span span) {
            event_type ev{};
            ev.kind = event_kind::error;
            ev.span = span;
            ev.diag_code = code;
            events_.push_back(ev);
        }

        void end(marker m, byte_span span = {}) {
            if (depth_ > 0) --depth_;
            events_.push_back({
                event_kind::end_node,
                events_[m.event_index].node_kind, 0, span, {}
            });
        }

        // Insert a begin_node event at the position recorded in m (retroactive open).
        // All events at [m.event_index, end) are shifted right by one slot.
        // Returns a marker whose event_index points to the newly inserted begin_node.
        // Use for operator-tree building (Pratt): open a node around an already-emitted
        // left operand by snapshotting before the operand, then calling this after the
        // infix operator is recognised.
        marker insert_begin_at(marker m, KindEnum k) {
            events_.insert(events_.begin() + m.event_index,
                           event_type{event_kind::begin_node, k, 0, {}, {}});
            ++depth_;
            return m; // m.event_index now points to the inserted begin_node
        }

        void rollback(marker m) {
            const bool committed = token_count_ > m.token_count;
            if (!committed) {
                events_.resize(m.event_index);
                token_count_ = m.token_count;
            }
            else {
                events_[m.event_index].kind = event_kind::tombstone;
            }
            if (depth_ > 0) --depth_;
        }

        [[nodiscard]] marker snapshot() const noexcept {
            return {static_cast<std::uint32_t>(events_.size()), token_count_};
        }

        [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }

        [[nodiscard]] std::uint32_t event_count() const noexcept {
            return static_cast<std::uint32_t>(events_.size());
        }

        [[nodiscard]] const std::vector<event_type>& all() const noexcept { return events_; }

        void reserve(std::uint32_t n) { events_.reserve(n); }

    private:
        std::vector<event_type> events_;
        std::uint32_t depth_ = 0;
        std::uint32_t token_count_ = 0;
    };
} // namespace lang
