#pragma once
// ============================================================================
// kalpana/paint/paint.hpp — Paint Specifications (Fill, Stroke, Gradients, Blend Modes)
// ============================================================================

#include "../color/color.hpp"
#include "containers/numeric/math_vector.hpp"
#include <vector>

namespace kalpana {

enum class BlendMode : std::uint8_t {
    SrcOver,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    ColorDodge,
    ColorBurn,
    HardLight,
    SoftLight,
    Difference,
    Exclusion
};

enum class LineCap : std::uint8_t {
    Butt,
    Round,
    Square
};

enum class LineJoin : std::uint8_t {
    Miter,
    Round,
    Bevel
};

struct GradientStop {
    float offset = 0.0f; // [0, 1]
    Color color;
};

struct LinearGradient {
    pebble::math::vec2        start{0.0f, 0.0f};
    pebble::math::vec2        end{100.0f, 0.0f};
    std::vector<GradientStop> stops;
};

struct RadialGradient {
    pebble::math::vec2        center{0.0f, 0.0f};
    float                     radius = 50.0f;
    std::vector<GradientStop> stops;
};

struct Stroke {
    Color    color = colors::black();
    float    width = 1.0f;
    LineCap  cap = LineCap::Round;
    LineJoin join = LineJoin::Round;
    float    miter_limit = 4.0f;
};

class Paint {
public:
    Paint() = default;

    static Paint fill(Color c) noexcept {
        Paint p;
        p.has_fill_ = true;
        p.fill_color_ = c;
        return p;
    }

    static Paint stroke(Color c, float width = 1.0f) noexcept {
        Paint p;
        p.has_stroke_ = true;
        p.stroke_.color = c;
        p.stroke_.width = width;
        return p;
    }

    static Paint filled_outlined(Color fill_c, Color stroke_c, float width = 1.0f) noexcept {
        Paint p;
        p.has_fill_ = true;
        p.fill_color_ = fill_c;
        p.has_stroke_ = true;
        p.stroke_.color = stroke_c;
        p.stroke_.width = width;
        return p;
    }

    [[nodiscard]] bool has_fill() const noexcept { return has_fill_; }
    [[nodiscard]] bool has_stroke() const noexcept { return has_stroke_; }
    [[nodiscard]] Color fill_color() const noexcept { return fill_color_; }
    [[nodiscard]] const Stroke& stroke() const noexcept { return stroke_; }

private:
    bool   has_fill_ = false;
    Color  fill_color_ = colors::black();
    bool   has_stroke_ = false;
    Stroke stroke_{};
};

} // namespace kalpana
