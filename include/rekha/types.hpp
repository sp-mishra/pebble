#pragma once

#include "akruti/math.hpp"
#include "kalpana/color/color.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rekha {

using Scalar = float;
using Vec2 = akruti::Vec2<Scalar>;
using Color = kalpana::Color;

struct Range {
    Scalar min = 0.0f;
    Scalar max = 1.0f;

    [[nodiscard]] constexpr Scalar span() const noexcept {
        return max - min;
    }

    [[nodiscard]] constexpr bool degenerate() const noexcept {
        return !(max > min);
    }

    constexpr void include(Scalar v) noexcept {
        min = std::min(min, v);
        max = std::max(max, v);
    }
};

struct Margin {
    Scalar left = 56.0f;
    Scalar right = 20.0f;
    Scalar top = 20.0f;
    Scalar bottom = 44.0f;
};

struct StrokeStyle {
    Color color = kalpana::colors::black();
    Scalar width = 1.5f;
};

struct MarkerStyle {
    Color color = kalpana::colors::blue();
    Scalar radius = 3.0f;
};

struct Rect {
    Scalar x = 0.0f;
    Scalar y = 0.0f;
    Scalar w = 0.0f;
    Scalar h = 0.0f;
};

struct Viewport {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    Margin margin{};

    [[nodiscard]] Rect plot_rect() const noexcept {
        const Scalar pw = std::max<Scalar>(1.0f, static_cast<Scalar>(width) - margin.left - margin.right);
        const Scalar ph = std::max<Scalar>(1.0f, static_cast<Scalar>(height) - margin.top - margin.bottom);
        return Rect{margin.left, margin.top, pw, ph};
    }
};

struct Palette {
    std::array<Color, 8> colors{
        kalpana::colors::blue(),
        kalpana::colors::red(),
        kalpana::colors::green(),
        kalpana::colors::yellow(),
        kalpana::colors::coral(),
        kalpana::colors::cyan(),
        kalpana::colors::magenta(),
        kalpana::colors::white()
    };

    [[nodiscard]] Color pick(std::size_t i) const noexcept {
        return colors[i % colors.size()];
    }
};

} // namespace rekha

