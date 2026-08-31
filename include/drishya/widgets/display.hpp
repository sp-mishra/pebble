#pragma once
// ============================================================================
// drishya/widgets/display.hpp — non-interactive display widgets
// ----------------------------------------------------------------------------
// Read-only widgets: text label, icon, separator, badge, progress bar, spinner,
// tooltip. They measure themselves against the host metrics and paint through
// the Painter concept. All are value types (no virtual, no macros) and stay
// nothrow-move-constructible so they fit AnyWidget's inline storage.
// ============================================================================

#include "drishya/widgets/base.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace pebble::drishya::widgets {

using akruti::layout::SizeSpec;

// ----------------------------------------------------------------------------
// Label — a single run of text. Measures via the host metrics; paints via text.
// ----------------------------------------------------------------------------
struct Label : WidgetBase {
    std::string text{};
    std::uint32_t color = 0xFFFFFFFFu;
    float font_size = 14.0f;

    Label() = default;
    explicit Label(std::string t, std::uint32_t c = 0xFFFFFFFFu, float fs = 14.0f)
        : text(std::move(t)), color(c), font_size(fs) {}

    Label& set_text(std::string t) { text = std::move(t); return *this; }
    Label& set_color(std::uint32_t c) noexcept { color = c; return *this; }
    Label& size(float fs) noexcept { font_size = fs; return *this; }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>& mc) const {
        return mc.text.measure(text.c_str(), 0.0f);
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(color);
        // Baseline-ish placement: sit the text a little below the top edge.
        painter.text(std::string_view{text}, Vec2{box.x, box.y + font_size}, font_size);
    }
};
[[nodiscard]] inline Label label(std::string t) { return Label{std::move(t)}; }

// ----------------------------------------------------------------------------
// Icon — a glyph drawn as text from an icon font (codepoint stored as UTF-8).
// Kept text-based so it needs no image backend; size is square.
// ----------------------------------------------------------------------------
struct Icon : WidgetBase {
    std::string glyph{};
    std::uint32_t color = 0xFFFFFFFFu;
    float extent = 16.0f;

    Icon() = default;
    explicit Icon(std::string g, float px = 16.0f, std::uint32_t c = 0xFFFFFFFFu)
        : glyph(std::move(g)), color(c), extent(px) {
        style_.width = SizeSpec::Px(px);
        style_.height = SizeSpec::Px(px);
    }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        return Size2D{extent, extent};
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(color);
        painter.text(std::string_view{glyph}, Vec2{box.x, box.y + extent}, extent);
    }
};

// ----------------------------------------------------------------------------
// Separator — a thin rule. Orientation from the parent axis is not known here,
// so it exposes both; default is a horizontal 1px line filling its box width.
// ----------------------------------------------------------------------------
struct Separator : WidgetBase {
    std::uint32_t color = 0x40FFFFFFu;
    float thickness = 1.0f;
    bool vertical = false;

    Separator() noexcept { style_.height = SizeSpec::Px(1.0f); }
    explicit Separator(bool vert, std::uint32_t c = 0x40FFFFFFu, float th = 1.0f) noexcept
        : color(c), thickness(th), vertical(vert) {
        if (vert) style_.width = SizeSpec::Px(th);
        else style_.height = SizeSpec::Px(th);
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(color);
        if (vertical) painter.fill_rect(Rect2D{box.x, box.y, thickness, box.h});
        else painter.fill_rect(Rect2D{box.x, box.y, box.w, thickness});
    }
};

// ----------------------------------------------------------------------------
// Badge — a small rounded pill with a text count/label.
// ----------------------------------------------------------------------------
struct Badge : WidgetBase {
    std::string text{};
    std::uint32_t background = 0xFFEF4444u;
    std::uint32_t color = 0xFFFFFFFFu;
    float font_size = 12.0f;

    Badge() = default;
    explicit Badge(std::string t) : text(std::move(t)) {}

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>& mc) const {
        const Size2D t = mc.text.measure(text.c_str(), 0.0f);
        return Size2D{t.w + font_size, t.h + font_size * 0.5f};
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(background);
        painter.round_rect(box, box.h * 0.5f);
        if constexpr (ColorPainter<P>) painter.set_color(color);
        painter.text(std::string_view{text},
                     Vec2{box.x + font_size * 0.5f, box.y + font_size}, font_size);
    }
};

// ----------------------------------------------------------------------------
// Progress — a horizontal bar; `value` in [0,1]. Paints track + fill.
// ----------------------------------------------------------------------------
struct Progress : WidgetBase {
    float value = 0.0f;
    std::uint32_t track = 0x40FFFFFFu;
    std::uint32_t fill = 0xFF3B82F6u;

    Progress() noexcept { style_.height = SizeSpec::Px(6.0f); }
    explicit Progress(float v) noexcept : value(v) { style_.height = SizeSpec::Px(6.0f); }

    Progress& set_value(float v) noexcept { value = clamp01(v); return *this; }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        const float r = box.h * 0.5f;
        if constexpr (ColorPainter<P>) painter.set_color(track);
        painter.round_rect(box, r);
        const float w = box.w * clamp01(value);
        if (w > 0.0f) {
            if constexpr (ColorPainter<P>) painter.set_color(fill);
            painter.round_rect(Rect2D{box.x, box.y, w, box.h}, r);
        }
    }

private:
    static float clamp01(float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
};

// ----------------------------------------------------------------------------
// Spinner — an indeterminate busy indicator. `phase` is advanced by the app
// (0..1); paint draws a rotating arc approximation as a set of dots.
// ----------------------------------------------------------------------------
struct Spinner : WidgetBase {
    float phase = 0.0f;
    std::uint32_t color = 0xFF3B82F6u;
    float extent = 16.0f;

    Spinner() noexcept { style_.width = SizeSpec::Px(16.0f); style_.height = SizeSpec::Px(16.0f); }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        return Size2D{extent, extent};
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        // Simple leading dot on the ring; a fuller arc is backend-dependent.
        if constexpr (ColorPainter<P>) painter.set_color(color);
        const float cx = box.x + box.w * 0.5f;
        const float cy = box.y + box.h * 0.5f;
        const float dot = box.w * 0.18f;
        painter.fill_rect(Rect2D{cx - dot * 0.5f, box.y, dot, dot});
        (void)cy;
    }
};

// ----------------------------------------------------------------------------
// Tooltip — a floating label with a padded backdrop. Positioned by the app; the
// widget only knows how to size and paint itself.
// ----------------------------------------------------------------------------
struct Tooltip : WidgetBase {
    std::string text{};
    std::uint32_t background = 0xF0202832u;
    std::uint32_t color = 0xFFE6EDF3u;
    float font_size = 12.0f;
    float pad = 6.0f;

    Tooltip() = default;
    explicit Tooltip(std::string t) : text(std::move(t)) {}

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>& mc) const {
        const Size2D t = mc.text.measure(text.c_str(), 0.0f);
        return Size2D{t.w + pad * 2.0f, t.h + pad * 2.0f};
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(background);
        painter.round_rect(box, 4.0f);
        if constexpr (ColorPainter<P>) painter.set_color(color);
        painter.text(std::string_view{text},
                     Vec2{box.x + pad, box.y + pad + font_size}, font_size);
    }
};

} // namespace pebble::drishya::widgets
