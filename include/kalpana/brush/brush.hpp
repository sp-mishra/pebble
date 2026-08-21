#pragma once
// ============================================================================
// kalpana/brush/brush.hpp — Realtime Brush Engine & Pressure Dynamics
// ============================================================================
// Emits stamped dabs along a stroke trajectory with spacing, jitter, and pressure.
// ============================================================================

#include "../color/color.hpp"
#include "containers/numeric/math_vector.hpp"
#include <vector>
#include <cmath>

namespace kalpana {

struct BrushPoint {
    pebble::math::vec2 pos{0.0f, 0.0f};
    float              pressure = 1.0f;
    float              tilt_x = 0.0f;
    float              tilt_y = 0.0f;
};

struct BrushStamp {
    pebble::math::vec2 pos{0.0f, 0.0f};
    float              radius = 5.0f;
    float              opacity = 1.0f;
    Color              color = colors::black();
};

class Brush {
public:
    Brush() = default;

    Brush& size(float s) noexcept { size_ = s; return *this; }
    Brush& spacing(float sp) noexcept { spacing_ = sp; return *this; }
    Brush& color(Color c) noexcept { color_ = c; return *this; }
    Brush& opacity(float op) noexcept { opacity_ = op; return *this; }

    // Generates stamps along a segment between two points
    [[nodiscard]] std::vector<BrushStamp> stroke_segment(const BrushPoint& p0, const BrushPoint& p1) const {
        std::vector<BrushStamp> stamps;

        const float dx = p1.pos[0] - p0.pos[0];
        const float dy = p1.pos[1] - p0.pos[1];
        const float dist = std::sqrt(dx * dx + dy * dy);

        const float step = std::max(1.0f, size_ * spacing_);
        const int count = static_cast<int>(dist / step);

        if (count <= 0) {
            stamps.push_back(BrushStamp{
                .pos = p1.pos,
                .radius = size_ * p1.pressure * 0.5f,
                .opacity = opacity_ * p1.pressure,
                .color = color_
            });
            return stamps;
        }

        for (int i = 0; i <= count; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(count);
            const pebble::math::vec2 p(p0.pos[0] + dx * t, p0.pos[1] + dy * t);
            const float press = p0.pressure + (p1.pressure - p0.pressure) * t;

            stamps.push_back(BrushStamp{
                .pos = p,
                .radius = size_ * press * 0.5f,
                .opacity = opacity_ * press,
                .color = color_
            });
        }

        return stamps;
    }

    [[nodiscard]] float size() const noexcept { return size_; }
    [[nodiscard]] Color color() const noexcept { return color_; }

private:
    float size_ = 10.0f;
    float spacing_ = 0.25f; // Step fraction of size
    float opacity_ = 1.0f;
    Color color_ = colors::black();
};

} // namespace kalpana
