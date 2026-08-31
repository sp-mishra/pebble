#pragma once
// ============================================================================
// drishya/widgets/base.hpp — shared widget scaffolding
// ----------------------------------------------------------------------------
// Widgets are plain value types that satisfy the Widget + PaintableWith
// concepts. Most share the same trivial defaults (empty measure, default style,
// ignored events, no-op paint), so this header provides small non-virtual base
// structs a widget can inherit to fill in only what it overrides. Inheritance
// here is pure code reuse — there are no virtual functions and dispatch stays
// static through AnyWidget's free-function vtable.
//
// A widget overriding a hook simply *hides* the base member with its own; the
// concept picks up the most-derived one at the call site. This keeps leaf
// widgets to a few lines without any macro or CRTP machinery.
// ============================================================================

#include "drishya/widget_concept.hpp"

namespace pebble::drishya::widgets {

// Defaults every widget can inherit. Override any subset in the derived type.
struct WidgetBase {
    LayoutStyle style_{};

    [[nodiscard]] LayoutStyle style() const noexcept { return style_; }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        return Size2D{};
    }

    [[nodiscard]] EventResult on_event(EventCtx&) noexcept { return EventResult::Ignored; }

    template <typename P>
        requires Painter<P>
    void paint(P&, Rect2D) const noexcept {}
};

// Helpers widgets use to author their LayoutStyle fluently in constructors.
namespace detail {
    using akruti::layout::SizeSpec;

    [[nodiscard]] inline LayoutStyle& set_size(LayoutStyle& s, SizeSpec w, SizeSpec h) noexcept {
        s.width = w;
        s.height = h;
        return s;
    }
} // namespace detail

} // namespace pebble::drishya::widgets
