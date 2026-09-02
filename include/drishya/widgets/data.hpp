#pragma once
// ============================================================================
// drishya/widgets/data.hpp — data-display widgets for dashboards
// ----------------------------------------------------------------------------
// Widgets aimed at AI/ML dashboards: sparkline, stat tile / KPI, a virtualized
// list_view and table, and a chat transcript. These hold their data by value
// (or a view the app owns) and paint a compact visualization. list_view is
// virtualization-aware: it exposes the visible row range from a scroll offset so
// the builder only realizes on-screen rows.
//
// No virtual, no macros; nothrow-move so they fit AnyWidget's inline storage.
// ============================================================================

#include "drishya/widgets/base.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pebble::drishya::widgets {
    using akruti::layout::SizeSpec;
    using akruti::layout::Edges;

    // ----------------------------------------------------------------------------
    // Sparkline — a tiny inline line chart over a series of floats. Auto-scales to
    // its box. Values are stored by value; the app updates them.
    // ----------------------------------------------------------------------------
    struct Sparkline : WidgetBase {
        std::vector<float> values{};
        std::uint32_t color = 0xFF60A5FAu;
        float line_width = 1.5f;

        Sparkline() noexcept { style_.height = SizeSpec::Px(32.0f); }

        explicit Sparkline(std::vector<float> v) : values(std::move(v)) {
            style_.height = SizeSpec::Px(32.0f);
        }

        template <ITextMetrics Metrics>
        [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
            return Size2D{80.0f, 32.0f};
        }

        template <typename P>
            requires Painter<P>
        void paint(P& painter, Rect2D box) const {
            if (values.size() < 2) return;
            float lo = values[0], hi = values[0];
            for (float v : values) {
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
            const float span = (hi - lo) > 0.0f ? (hi - lo) : 1.0f;
            const float step = box.w / static_cast<float>(values.size() - 1);
            if constexpr (ColorPainter<P>) painter.set_color(color);
            for (std::size_t i = 1; i < values.size(); ++i) {
                const float x0 = box.x + step * static_cast<float>(i - 1);
                const float x1 = box.x + step * static_cast<float>(i);
                const float y0 = box.y + box.h - (values[i - 1] - lo) / span * box.h;
                const float y1 = box.y + box.h - (values[i] - lo) / span * box.h;
                painter.line(Vec2{x0, y0}, Vec2{x1, y1}, line_width);
            }
        }
    };

    // ----------------------------------------------------------------------------
    // StatTile / KPI — a labelled metric: big value, small caption, optional delta.
    // ----------------------------------------------------------------------------
    struct StatTile : WidgetBase {
        std::string caption{};
        std::string value{};
        std::string delta{};
        std::uint32_t value_color = 0xFFE6EDF3u;
        std::uint32_t caption_color = 0xFF9BA7B4u;
        std::uint32_t delta_color = 0xFF22C55Eu;
        float value_size = 24.0f;
        float caption_size = 12.0f;

        StatTile() = default;

        StatTile(std::string cap, std::string val) : caption(std::move(cap)), value(std::move(val)) {
            style_.axis = akruti::layout::Axis::Column;
            style_.padding = Edges{12.0f, 12.0f, 12.0f, 12.0f};
        }

        template <ITextMetrics Metrics>
        [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>& mc) const {
            const Size2D v = mc.text.measure(value.c_str(), 0.0f);
            const Size2D c = mc.text.measure(caption.c_str(), 0.0f);
            const float w = (v.w > c.w ? v.w : c.w) + 24.0f;
            return Size2D{w, value_size + caption_size + 24.0f};
        }

        template <typename P>
            requires Painter<P>
        void paint(P& painter, Rect2D box) const {
            if constexpr (ColorPainter<P>) painter.set_color(caption_color);
            painter.text(std::string_view{caption}, Vec2{box.x + 12.0f, box.y + 12.0f + caption_size}, caption_size);
            if constexpr (ColorPainter<P>) painter.set_color(value_color);
            painter.text(std::string_view{value},
                         Vec2{box.x + 12.0f, box.y + 12.0f + caption_size + 6.0f + value_size}, value_size);
            if (!delta.empty()) {
                if constexpr (ColorPainter<P>) painter.set_color(delta_color);
                painter.text(std::string_view{delta},
                             Vec2{box.x + box.w - 48.0f, box.y + 12.0f + caption_size}, caption_size);
            }
        }
    };

    // ----------------------------------------------------------------------------
    // ListView — virtualization-aware vertical list. Holds row_count + row_height +
    // scroll offset; computes the visible [first,last) range so the builder realizes
    // only on-screen rows. Painting draws nothing itself (rows are child widgets);
    // it clips its viewport.
    // ----------------------------------------------------------------------------
    struct ListView : WidgetBase {
        std::size_t row_count = 0;
        float row_height = 24.0f;
        float scroll_y = 0.0f;

        ListView() noexcept {
            style_.axis = akruti::layout::Axis::Column;
            style_.overflow_y = akruti::layout::Overflow::Scroll;
        }

        explicit ListView(std::size_t rows, float rh = 24.0f) noexcept
            : row_count(rows), row_height(rh) {
            style_.axis = akruti::layout::Axis::Column;
            style_.overflow_y = akruti::layout::Overflow::Scroll;
        }

        // Visible row range given the viewport height, with a small overscan.
        struct Range {
            std::size_t first;
            std::size_t last;
        };

        [[nodiscard]] Range visible(float viewport_h, std::size_t overscan = 4) const noexcept {
            if (row_height <= 0.0f || row_count == 0) return Range{0, 0};
            const auto first_f = static_cast<std::size_t>(scroll_y / row_height);
            const auto vis = static_cast<std::size_t>(viewport_h / row_height) + 1;
            const std::size_t first = first_f > overscan ? first_f - overscan : 0;
            std::size_t last = first_f + vis + overscan;
            if (last > row_count) last = row_count;
            return Range{first, last};
        }

        [[nodiscard]] float content_height() const noexcept {
            return static_cast<float>(row_count) * row_height;
        }
    };

    // ----------------------------------------------------------------------------
    // Table — a header row + virtualized body. Column widths are Fr/Px specs the
    // builder applies to cells; this widget carries the schema + row count.
    // ----------------------------------------------------------------------------
    struct Table : WidgetBase {
        std::vector<std::string> headers{};
        std::size_t row_count = 0;
        float row_height = 22.0f;
        float scroll_y = 0.0f;
        std::uint32_t header_color = 0xFF9BA7B4u;
        std::uint32_t grid_color = 0xFF2A333Du;

        Table() noexcept { style_.axis = akruti::layout::Axis::Column; }

        explicit Table(std::vector<std::string> cols) : headers(std::move(cols)) {
            style_.axis = akruti::layout::Axis::Column;
        }

        [[nodiscard]] std::size_t column_count() const noexcept { return headers.size(); }

        template <typename P>
            requires Painter<P>
        void paint(P& painter, Rect2D box) const {
            // Header separator; cells are realized as child widgets by the builder.
            if constexpr (ColorPainter<P>) painter.set_color(grid_color);
            painter.line(Vec2{box.x, box.y + row_height}, Vec2{box.x + box.w, box.y + row_height}, 1.0f);
        }
    };

    // ----------------------------------------------------------------------------
    // Chat — a scrolling transcript of role-tagged messages. Stores messages by
    // value; paints a simple bubble list. Suited to LLM chat panels.
    // ----------------------------------------------------------------------------
    struct ChatMessage {
        std::string author{};
        std::string body{};
        bool own = false; // right-aligned "me" bubble when true
    };

    struct Chat : WidgetBase {
        std::vector<ChatMessage> messages{};
        std::uint32_t bubble_them = 0xFF1F262Eu;
        std::uint32_t bubble_me = 0xFF2563EBu;
        std::uint32_t color = 0xFFE6EDF3u;
        float font_size = 14.0f;
        float scroll_y = 0.0f;

        Chat() noexcept {
            style_.axis = akruti::layout::Axis::Column;
            style_.overflow_y = akruti::layout::Overflow::Scroll;
        }

        void push(std::string author, std::string body, bool own = false) {
            messages.push_back(ChatMessage{std::move(author), std::move(body), own});
        }

        template <typename P>
            requires Painter<P>
        void paint(P& painter, Rect2D box) const {
            float y = box.y + 8.0f - scroll_y;
            const float line = font_size + 8.0f;
            for (const ChatMessage& m : messages) {
                const float bw = box.w * 0.7f;
                const float bx = m.own ? (box.x + box.w - bw - 8.0f) : (box.x + 8.0f);
                if constexpr (ColorPainter<P>) painter.set_color(m.own ? bubble_me : bubble_them);
                painter.round_rect(Rect2D{bx, y, bw, line + 4.0f}, 6.0f);
                if constexpr (ColorPainter<P>) painter.set_color(color);
                painter.text(std::string_view{m.body}, Vec2{bx + 8.0f, y + font_size}, font_size);
                y += line + 10.0f;
            }
        }
    };
} // namespace pebble::drishya::widgets
