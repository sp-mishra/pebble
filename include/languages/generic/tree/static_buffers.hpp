#pragma once

// languages/generic/tree/static_buffers.hpp — Constexpr parse buffers over static_vector.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// static_event_buffer<KE,DC,MaxEvents> — compile-time event log backed by static_vector.
//   Same marker/rollback/tombstone semantics as event_log<KE,DC>.
//   Overflow recorded via static_vector::overflow() — no throw, consteval-safe.
//
// static_span_buffer<T,N> — thin alias of containers::static_vector<T,N>.
//   Preserves samasa call-site naming; logic lives in the container.

#include <cstdint>
#include "spans.hpp"
#include "event_log.hpp"
#include "../../../containers/static/static_vector.hpp"

namespace lang {

    // ---- static_event_buffer -----------------------------------------------

    template <class KindEnum, class DiagCode = std::uint16_t, std::uint32_t MaxEvents = 4096>
    class static_event_buffer {
    public:
        using event_type = parse_event<KindEnum, DiagCode>;

        struct marker {
            std::uint32_t event_index = 0;
            std::uint32_t token_count = 0;
        };

        [[nodiscard]] constexpr marker begin(KindEnum k) noexcept {
            marker m{static_cast<std::uint32_t>(events_.size()), token_count_};
            static_cast<void>(events_.push_back({event_kind::begin_node, k, 0, {}, {}}));
            ++depth_;
            return m;
        }

        constexpr void token(std::uint32_t idx) noexcept {
            static_cast<void>(events_.push_back({event_kind::token, {}, idx, {}, {}}));
            ++token_count_;
        }

        constexpr void error(DiagCode code, byte_span span) noexcept {
            event_type ev{};
            ev.kind      = event_kind::error;
            ev.span      = span;
            ev.diag_code = code;
            static_cast<void>(events_.push_back(ev));
        }

        constexpr void end(marker m, byte_span span = {}) noexcept {
            if (depth_ > 0) --depth_;
            KindEnum k = (m.event_index < static_cast<std::uint32_t>(events_.size()))
                         ? events_[m.event_index].node_kind : KindEnum{};
            static_cast<void>(events_.push_back({event_kind::end_node, k, 0, span, {}}));
        }

        [[nodiscard]] constexpr marker snapshot() const noexcept {
            return {static_cast<std::uint32_t>(events_.size()), token_count_};
        }

        constexpr void rollback(marker m) noexcept {
            const bool committed = token_count_ > m.token_count;
            if (!committed) {
                truncate_events(m.event_index);
                token_count_ = m.token_count;
            } else {
                if (m.event_index < static_cast<std::uint32_t>(events_.size()))
                    events_[m.event_index].kind = event_kind::tombstone;
            }
            if (depth_ > 0) --depth_;
        }

        [[nodiscard]] constexpr std::uint32_t depth()       const noexcept { return depth_; }
        [[nodiscard]] constexpr std::uint32_t event_count() const noexcept {
            return static_cast<std::uint32_t>(events_.size());
        }
        [[nodiscard]] constexpr bool overflow() const noexcept { return events_.overflow(); }

        // Read-only access to underlying buffer.
        [[nodiscard]] constexpr const event_type& operator[](std::uint32_t i) const noexcept {
            return events_[i];
        }

    private:
        constexpr void truncate_events(std::uint32_t new_size) noexcept {
            if (new_size >= static_cast<std::uint32_t>(events_.size())) return;
            containers::static_vector<event_type, MaxEvents> tmp;
            for (std::uint32_t i = 0; i < new_size; ++i)
                static_cast<void>(tmp.push_back(events_[i]));
            events_ = tmp;
        }

        containers::static_vector<event_type, MaxEvents> events_;
        std::uint32_t                                     token_count_ = 0;
        std::uint32_t                                     depth_       = 0;
    };

    // ---- static_span_buffer ------------------------------------------------
    // Alias of containers::static_vector, named for readability at samasa call-sites.

    template <class T, std::size_t N>
    using static_span_buffer = containers::static_vector<T, N>;

} // namespace lang
