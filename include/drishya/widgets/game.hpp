#pragma once
// ============================================================================
// drishya/widgets/game.hpp — game-UI / HUD widgets
// ----------------------------------------------------------------------------
// Widgets aimed at in-game overlays: gauge / health_bar, radial_menu, crosshair,
// damage_number, nine_patch, world_anchor. These favour immediate readability
// and fixed sizing over text flow; several are positioned by the app relative to
// a world/screen point rather than by the flex solver.
//
// No virtual, no macros; value types, nothrow-move so they fit AnyWidget inline.
// ============================================================================

#include "drishya/widgets/base.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace pebble::drishya::widgets {

using akruti::layout::SizeSpec;

// ----------------------------------------------------------------------------
// Gauge / HealthBar — a fraction bar in [0,1] with a track, fill and optional
// low-threshold color. Horizontal by default; set vertical for a stamina column.
// ----------------------------------------------------------------------------
struct Gauge : WidgetBase {
    float value = 1.0f;             // clamped to [0,1]
    float low_threshold = 0.25f;    // below this, fill switches to low_color
    std::uint32_t track = 0xC0202832u;
    std::uint32_t fill = 0xFF22C55Eu;
    std::uint32_t low_color = 0xFFEF4444u;
    float radius = 3.0f;
    bool vertical = false;

    Gauge() noexcept { style_.height = SizeSpec::Px(12.0f); }
    explicit Gauge(float v) noexcept : value(v) { style_.height = SizeSpec::Px(12.0f); }

    Gauge& set_value(float v) noexcept { value = clamp01(v); return *this; }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        const float t = clamp01(value);
        const std::uint32_t fc = (t <= low_threshold) ? low_color : fill;
        if constexpr (ColorPainter<P>) painter.set_color(track);
        painter.round_rect(box, radius);
        if constexpr (ColorPainter<P>) painter.set_color(fc);
        if (vertical) {
            const float h = box.h * t;
            painter.round_rect(Rect2D{box.x, box.y + box.h - h, box.w, h}, radius);
        } else {
            painter.round_rect(Rect2D{box.x, box.y, box.w * t, box.h}, radius);
        }
    }

private:
    static float clamp01(float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
};
// HealthBar is a Gauge preset with the danger palette.
[[nodiscard]] inline Gauge health_bar(float v = 1.0f) noexcept {
    Gauge g{v};
    g.fill = 0xFF22C55Eu;
    g.low_color = 0xFFEF4444u;
    return g;
}

// ----------------------------------------------------------------------------
// RadialMenu — a ring of slots around a center; `count` slots, `selected` index,
// `sweep` fraction (0..1) for open/close animation the app drives. Paints slot
// pips on the ring; interaction is resolved by the app from pointer angle.
// ----------------------------------------------------------------------------
struct RadialMenu : WidgetBase {
    std::uint32_t count = 6;
    std::uint32_t selected = 0;    // kInvalid-like sentinel via >= count means none
    float sweep = 1.0f;            // open amount [0,1]
    float inner = 24.0f;
    float outer = 72.0f;
    std::uint32_t pip = 0xFFE6EDF3u;
    std::uint32_t pip_selected = 0xFF3B82F6u;

    RadialMenu() noexcept { square(outer * 2.0f); }
    explicit RadialMenu(std::uint32_t n) noexcept : count(n) { square(outer * 2.0f); }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        return Size2D{outer * 2.0f, outer * 2.0f};
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if (count == 0) return;
        const float cx = box.x + box.w * 0.5f;
        const float cy = box.y + box.h * 0.5f;
        const float ring = (inner + outer) * 0.5f;
        const float dot = (outer - inner) * 0.35f;
        // Two-term polynomial cosine/sine per slot: avoid <cmath> to stay light;
        // approximate around the unit circle using the identity via successive
        // rotation is overkill — sample fixed slot fractions with a tiny table.
        for (std::uint32_t i = 0; i < count; ++i) {
            const float frac = (static_cast<float>(i) / static_cast<float>(count)) * sweep;
            const float ang = frac * 6.28318530718f - 1.57079632679f;
            const float c = cos_approx(ang);
            const float s = sin_approx(ang);
            const float px = cx + c * ring;
            const float py = cy + s * ring;
            if constexpr (ColorPainter<P>) painter.set_color(i == selected ? pip_selected : pip);
            painter.round_rect(Rect2D{px - dot * 0.5f, py - dot * 0.5f, dot, dot}, dot * 0.5f);
        }
    }

private:
    void square(float d) noexcept { style_.width = SizeSpec::Px(d); style_.height = SizeSpec::Px(d); }
    // Bhaskara I minimax-style rational approximations, adequate for HUD pips.
    static float sin_approx(float x) noexcept {
        // wrap to [-pi,pi]
        const float pi = 3.14159265359f, two_pi = 6.28318530718f;
        while (x > pi) x -= two_pi;
        while (x < -pi) x += two_pi;
        const float b = 4.0f / pi, c = -4.0f / (pi * pi);
        return b * x + c * x * (x < 0.0f ? -x : x);
    }
    static float cos_approx(float x) noexcept { return sin_approx(x + 1.57079632679f); }
};

// ----------------------------------------------------------------------------
// Crosshair — a center reticle: four ticks + optional center dot, with a spread
// value (recoil/bloom) the app feeds in. Fixed square; drawn about its center.
// ----------------------------------------------------------------------------
struct Crosshair : WidgetBase {
    float spread = 4.0f;     // gap from center to each tick
    float length = 6.0f;     // tick length
    float thickness = 2.0f;
    std::uint32_t color = 0xFFFFFFFFu;
    bool center_dot = false;

    Crosshair() noexcept { square(32.0f); }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        const float e = (spread + length) * 2.0f;
        return Size2D{e, e};
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        const float cx = box.x + box.w * 0.5f;
        const float cy = box.y + box.h * 0.5f;
        if constexpr (ColorPainter<P>) painter.set_color(color);
        const float half = thickness * 0.5f;
        // up / down / left / right ticks
        painter.fill_rect(Rect2D{cx - half, cy - spread - length, thickness, length});
        painter.fill_rect(Rect2D{cx - half, cy + spread, thickness, length});
        painter.fill_rect(Rect2D{cx - spread - length, cy - half, length, thickness});
        painter.fill_rect(Rect2D{cx + spread, cy - half, length, thickness});
        if (center_dot) painter.fill_rect(Rect2D{cx - half, cy - half, thickness, thickness});
    }

private:
    void square(float d) noexcept { style_.width = SizeSpec::Px(d); style_.height = SizeSpec::Px(d); }
};

// ----------------------------------------------------------------------------
// DamageNumber — a floating combat number that rises and fades. `life` in [0,1]
// (1 = just spawned, 0 = expired) is advanced by the app; paint offsets upward
// and drops alpha as life decreases.
// ----------------------------------------------------------------------------
struct DamageNumber : WidgetBase {
    std::string text{};
    float life = 1.0f;          // 1 → 0 over its lifetime
    float rise = 24.0f;         // total upward travel in px
    std::uint32_t color = 0xFFEF4444u; // base ARGB; alpha scaled by life
    float font_size = 16.0f;

    DamageNumber() = default;
    explicit DamageNumber(std::string t) : text(std::move(t)) {}

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>& mc) const {
        return mc.text.measure(text.c_str(), 0.0f);
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        const float l = life < 0.0f ? 0.0f : (life > 1.0f ? 1.0f : life);
        const float y = box.y + (1.0f - l) * -rise + font_size;
        const auto alpha = static_cast<std::uint32_t>(l * 255.0f) & 0xFFu;
        const std::uint32_t argb = (alpha << 24) | (color & 0x00FFFFFFu);
        if constexpr (ColorPainter<P>) painter.set_color(argb);
        painter.text(std::string_view{text}, Vec2{box.x, y}, font_size);
    }
};

// ----------------------------------------------------------------------------
// NinePatch — a scalable bordered frame: fixed corners, stretched edges/center.
// `inset` is the corner size (all four equal here). Paints as five fills using
// the frame color; a textured backend can override with image slices later.
// ----------------------------------------------------------------------------
struct NinePatch : WidgetBase {
    float inset = 8.0f;
    std::uint32_t frame = 0xFF2A333Du;
    std::uint32_t center = 0x80101418u;

    NinePatch() = default;
    explicit NinePatch(float corner) noexcept : inset(corner) {}

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        const float i = inset;
        // center fill
        if ((center >> 24) != 0) {
            if constexpr (ColorPainter<P>) painter.set_color(center);
            painter.fill_rect(Rect2D{box.x + i, box.y + i, box.w - 2 * i, box.h - 2 * i});
        }
        if constexpr (ColorPainter<P>) painter.set_color(frame);
        // top & bottom edges (full width), then left & right edges (inner height)
        painter.fill_rect(Rect2D{box.x, box.y, box.w, i});
        painter.fill_rect(Rect2D{box.x, box.y + box.h - i, box.w, i});
        painter.fill_rect(Rect2D{box.x, box.y + i, i, box.h - 2 * i});
        painter.fill_rect(Rect2D{box.x + box.w - i, box.y + i, i, box.h - 2 * i});
    }
};

// ----------------------------------------------------------------------------
// WorldAnchor — pins a child overlay to a projected world position. The app
// projects world→screen and writes `screen` each frame; the widget contributes
// absolute anchoring so the flex solver leaves it at that point. Painting is a
// no-op (its child draws); it can optionally show an off-screen edge marker.
// ----------------------------------------------------------------------------
struct WorldAnchor : WidgetBase {
    Vec2 screen{0.0f, 0.0f};   // projected screen position, app-updated
    bool on_screen = true;
    std::uint32_t marker = 0xFFFBBF24u;
    float marker_size = 6.0f;

    WorldAnchor() noexcept {
        style_.anchor_left = true;
        style_.anchor_top = true;
    }
    explicit WorldAnchor(Vec2 p) noexcept : screen(p) {
        style_.anchor_left = true;
        style_.anchor_top = true;
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if (on_screen) return; // child renders normally
        if constexpr (ColorPainter<P>) painter.set_color(marker);
        painter.round_rect(Rect2D{box.x, box.y, marker_size, marker_size}, marker_size * 0.5f);
    }
};

} // namespace pebble::drishya::widgets
