#pragma once
// ============================================================================
// kalpana/color/color.hpp — Linear-Light RGBA & sRGB Rgba8 Color Primitive
// ============================================================================
// Zero-virtual, constexpr-enabled color representation in linear-light float space
// with exact sRGB 8-bit conversions, hex parsing, alpha compositing, and blending.
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kalpana {

struct Rgba8 {
    std::uint8_t r{0}, g{0}, b{0}, a{255};

    friend constexpr bool operator==(const Rgba8&, const Rgba8&) = default;
};

// Linear-light RGBA color. Channels in [0, 1].
struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    constexpr Color() noexcept = default;
    constexpr Color(float red, float green, float blue, float alpha = 1.0f) noexcept
        : r(red), g(green), b(blue), a(alpha) {}

    // sRGB Transfer Functions
    [[nodiscard]] static float srgb_to_linear(float c) noexcept {
        return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }

    [[nodiscard]] static float linear_to_srgb(float c) noexcept {
        return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
    }

    // Hex parsing (0xRRGGBBAA or 0xRRGGBB)
    [[nodiscard]] static Color from_hex(std::uint32_t hex) noexcept {
        const float r8 = float((hex >> 24) & 0xFF) / 255.0f;
        const float g8 = float((hex >> 16) & 0xFF) / 255.0f;
        const float b8 = float((hex >> 8)  & 0xFF) / 255.0f;
        const float a8 = float(hex & 0xFF)         / 255.0f;
        return Color{srgb_to_linear(r8), srgb_to_linear(g8), srgb_to_linear(b8), a8};
    }

    [[nodiscard]] static Color from_srgb8(Rgba8 s) noexcept {
        return Color{
            srgb_to_linear(float(s.r) / 255.0f),
            srgb_to_linear(float(s.g) / 255.0f),
            srgb_to_linear(float(s.b) / 255.0f),
            float(s.a) / 255.0f
        };
    }

    [[nodiscard]] Rgba8 to_srgb8() const noexcept {
        auto clamp8 = [](float val) -> std::uint8_t {
            return static_cast<std::uint8_t>(std::clamp(val * 255.0f + 0.5f, 0.0f, 255.0f));
        };
        return Rgba8{
            clamp8(linear_to_srgb(r)),
            clamp8(linear_to_srgb(g)),
            clamp8(linear_to_srgb(b)),
            clamp8(a)
        };
    }

    // ARGB8888 packed integer: (A << 24) | (R << 16) | (G << 8) | B
    [[nodiscard]] std::uint32_t to_argb8888() const noexcept {
        const Rgba8 s = to_srgb8();
        return (std::uint32_t(s.a) << 24) | (std::uint32_t(s.r) << 16) |
               (std::uint32_t(s.g) << 8)  | std::uint32_t(s.b);
    }

    // Linear RGB interpolation
    [[nodiscard]] Color lerp(const Color& o, float t) const noexcept {
        return Color{
            r + (o.r - r) * t,
            g + (o.g - g) * t,
            b + (o.b - b) * t,
            a + (o.a - a) * t
        };
    }

    // Porter-Duff Source-Over alpha compositing
    [[nodiscard]] Color over(const Color& dst) const noexcept {
        const float out_a = a + dst.a * (1.0f - a);
        if (out_a <= 1e-6f) return Color{0.0f, 0.0f, 0.0f, 0.0f};
        const float inv_a = 1.0f / out_a;
        return Color{
            (r * a + dst.r * dst.a * (1.0f - a)) * inv_a,
            (g * a + dst.g * dst.a * (1.0f - a)) * inv_a,
            (b * a + dst.b * dst.a * (1.0f - a)) * inv_a,
            out_a
        };
    }

    [[nodiscard]] Color with_alpha(float new_alpha) const noexcept {
        return Color{r, g, b, new_alpha};
    }

    friend constexpr bool operator==(const Color&, const Color&) = default;
};

namespace colors {
    inline constexpr Color transparent() noexcept { return Color{0.0f, 0.0f, 0.0f, 0.0f}; }
    inline constexpr Color black()       noexcept { return Color{0.0f, 0.0f, 0.0f, 1.0f}; }
    inline constexpr Color white()       noexcept { return Color{1.0f, 1.0f, 1.0f, 1.0f}; }
    inline constexpr Color red()         noexcept { return Color{1.0f, 0.0f, 0.0f, 1.0f}; }
    inline constexpr Color green()       noexcept { return Color{0.0f, 1.0f, 0.0f, 1.0f}; }
    inline constexpr Color blue()        noexcept { return Color{0.0f, 0.0f, 1.0f, 1.0f}; }
    inline constexpr Color yellow()      noexcept { return Color{1.0f, 1.0f, 0.0f, 1.0f}; }
    inline constexpr Color cyan()        noexcept { return Color{0.0f, 1.0f, 1.0f, 1.0f}; }
    inline constexpr Color magenta()     noexcept { return Color{1.0f, 0.0f, 1.0f, 1.0f}; }
    inline constexpr Color coral()       noexcept { return Color{1.0f, 0.5f, 0.31f, 1.0f}; }
} // namespace colors

namespace detail {
    [[nodiscard]] constexpr float clamp01(float v) noexcept {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
} // namespace detail

} // namespace kalpana
