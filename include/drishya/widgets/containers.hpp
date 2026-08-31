#pragma once
// ============================================================================
// drishya/widgets/containers.hpp — layout container widgets
// ----------------------------------------------------------------------------
// Containers carry layout intent (axis / alignment / justification / padding)
// and, optionally, a background. Their children are separate nodes in the
// retained tree, not owned by the container value — so a container widget is
// tiny: it contributes a LayoutStyle and (for card/panel) paints a backdrop.
//
// akruti's LayoutStyle has no inter-child "gap" field; spacing between children
// is expressed with the spacer widget or per-child margins applied by the UI
// builder. Containers here set only what the solver understands.
//
// Provided: vstack, hstack, grid, spacer, card, panel, scroll_area, splitter,
// tabs. Concept-satisfying value types, no virtual, no macros.
// ============================================================================

#include "drishya/widgets/base.hpp"

#include <cstdint>

namespace pebble::drishya::widgets {

using akruti::layout::Axis;
using akruti::layout::Align;
using akruti::layout::Justify;
using akruti::layout::SizeSpec;
using akruti::layout::Edges;
using akruti::layout::Overflow;

// ----------------------------------------------------------------------------
// Stack — vertical/horizontal flex container. vstack()/hstack() are factories.
// ----------------------------------------------------------------------------
struct Stack : WidgetBase {
    explicit Stack(Axis axis, float padding = 0.0f) noexcept {
        style_.axis = axis;
        style_.padding = Edges{padding, padding, padding, padding};
    }
    Stack& align(Align a) noexcept { style_.align_items = a; return *this; }
    Stack& justify(Justify j) noexcept { style_.justify_content = j; return *this; }
    Stack& padding(float p) noexcept { style_.padding = Edges{p, p, p, p}; return *this; }
    Stack& padding(Edges e) noexcept { style_.padding = e; return *this; }
    Stack& grow(float g) noexcept { style_.flex_grow = g; return *this; }
    Stack& width(SizeSpec w) noexcept { style_.width = w; return *this; }
    Stack& height(SizeSpec h) noexcept { style_.height = h; return *this; }
};

[[nodiscard]] inline Stack vstack(float padding = 0.0f) noexcept {
    return Stack{Axis::Column, padding};
}
[[nodiscard]] inline Stack hstack(float padding = 0.0f) noexcept {
    return Stack{Axis::Row, padding};
}

// ----------------------------------------------------------------------------
// Spacer — flexible gap. A leaf that grows to fill free space (grow default 1),
// or a fixed-size strut when constructed with a pixel size.
// ----------------------------------------------------------------------------
struct Spacer : WidgetBase {
    Spacer() noexcept { style_.flex_grow = 1.0f; }
    explicit Spacer(float px) noexcept {
        // Fixed strut: no grow, fixed main-axis size on both axes (the parent's
        // axis picks the relevant one).
        style_.flex_grow = 0.0f;
        style_.width = SizeSpec::Px(px);
        style_.height = SizeSpec::Px(px);
    }
};
[[nodiscard]] inline Spacer spacer() noexcept { return Spacer{}; }
[[nodiscard]] inline Spacer strut(float px) noexcept { return Spacer{px}; }

// ----------------------------------------------------------------------------
// Grid — fixed-column grid. akruti has no dedicated grid solver, so a grid is a
// Row/Column flex whose children are expected to carry Fr/Px widths; this widget
// records the intended column count for the builder and lays children on a row.
// ----------------------------------------------------------------------------
struct Grid : WidgetBase {
    std::uint32_t columns = 1;
    explicit Grid(std::uint32_t cols, float padding = 0.0f) noexcept : columns(cols) {
        style_.axis = Axis::Row;
        style_.align_items = Align::Stretch;
        style_.padding = Edges{padding, padding, padding, padding};
    }
};
[[nodiscard]] inline Grid grid(std::uint32_t cols, float padding = 0.0f) noexcept {
    return Grid{cols, padding};
}

// ----------------------------------------------------------------------------
// Panel / Card — a container that paints a filled (optionally rounded) backdrop
// behind its children. Card = panel with a default radius + surface color.
// ----------------------------------------------------------------------------
struct Panel : WidgetBase {
    std::uint32_t background = 0x00000000u; // ARGB; 0 alpha = no fill
    float radius = 0.0f;

    Panel() noexcept { style_.axis = Axis::Column; }
    Panel& fill(std::uint32_t argb) noexcept { background = argb; return *this; }
    Panel& rounded(float r) noexcept { radius = r; return *this; }
    Panel& pad(float p) noexcept { style_.padding = Edges{p, p, p, p}; return *this; }
    Panel& axis(Axis a) noexcept { style_.axis = a; return *this; }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if ((background >> 24) == 0) return; // fully transparent → skip
        if constexpr (ColorPainter<P>) painter.set_color(background);
        if (radius > 0.0f) painter.round_rect(box, radius);
        else painter.fill_rect(box);
    }
};

[[nodiscard]] inline Panel panel() noexcept { return Panel{}; }
[[nodiscard]] inline Panel card(std::uint32_t surface, float radius = 8.0f) noexcept {
    Panel p{};
    p.fill(surface).rounded(radius).pad(12.0f);
    return p;
}

// ----------------------------------------------------------------------------
// ScrollArea — a clipping viewport. Sets overflow=Scroll so the solver clips
// children to the box; the scroll offset is a value the app drives.
// ----------------------------------------------------------------------------
struct ScrollArea : WidgetBase {
    ScrollArea() noexcept {
        style_.axis = Axis::Column;
        style_.overflow_y = Overflow::Scroll;
        style_.overflow_x = Overflow::Clip;
    }
    ScrollArea& offset(float x, float y) noexcept {
        style_.scroll_offset_x = x;
        style_.scroll_offset_y = y;
        style_.scroll_offset = akruti::layout::Vec2{x, y};
        return *this;
    }
};
[[nodiscard]] inline ScrollArea scroll_area() noexcept { return ScrollArea{}; }

// ----------------------------------------------------------------------------
// Splitter — two-pane proportional split driven by a ratio in [0,1]. Realized as
// a Row/Column whose two children take Fr(ratio) / Fr(1-ratio); this widget
// carries the ratio + orientation for the builder and the drag handle.
// ----------------------------------------------------------------------------
struct Splitter : WidgetBase {
    float ratio = 0.5f;
    bool horizontal = true; // panes side by side

    explicit Splitter(bool horiz = true, float r = 0.5f) noexcept
        : ratio(r), horizontal(horiz) {
        style_.axis = horiz ? Axis::Row : Axis::Column;
        style_.align_items = Align::Stretch;
    }
    [[nodiscard]] SizeSpec first_share() const noexcept { return SizeSpec::Fr(ratio); }
    [[nodiscard]] SizeSpec second_share() const noexcept { return SizeSpec::Fr(1.0f - ratio); }
};
[[nodiscard]] inline Splitter hsplit(float ratio = 0.5f) noexcept { return Splitter{true, ratio}; }
[[nodiscard]] inline Splitter vsplit(float ratio = 0.5f) noexcept { return Splitter{false, ratio}; }

// ----------------------------------------------------------------------------
// Tabs — a tab strip container. Holds the active index; the builder places one
// header row + the active page. Selection is a plain index the app updates.
// ----------------------------------------------------------------------------
struct Tabs : WidgetBase {
    std::uint32_t active = 0;
    Tabs() noexcept { style_.axis = Axis::Column; }
    explicit Tabs(std::uint32_t active_index) noexcept : active(active_index) {
        style_.axis = Axis::Column;
    }
};
[[nodiscard]] inline Tabs tabs(std::uint32_t active = 0) noexcept { return Tabs{active}; }

} // namespace pebble::drishya::widgets
