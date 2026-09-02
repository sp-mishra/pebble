#pragma once
// ============================================================================
// kalpana/color/color_space.hpp — Extensible Color Spaces & Perceptual Interpolation
// ============================================================================
// Concept-driven color space conversion architecture supporting HSL, OkLab,
// and seamless user extensions without modifying core library files.
// ============================================================================

#include "color.hpp"
#include <concepts>
#include <cmath>
#include <algorithm>

namespace kalpana::color_space {
    // ── Built-in Color Space Types ───────────────────────────────────────────────

    struct HSL {
        float h = 0.0f; // [0, 360)
        float s = 0.0f; // [0, 1]
        float l = 0.0f; // [0, 1]
        float a = 1.0f; // [0, 1]

        friend constexpr bool operator==(const HSL&, const HSL&) = default;
    };

    struct OkLab {
        float L = 0.0f; // Perceived lightness [0, 1]
        float a = 0.0f; // Green-red axis ~[-0.4, 0.4]
        float b = 0.0f; // Blue-yellow axis ~[-0.4, 0.4]
        float alpha = 1.0f; // [0, 1]

        friend constexpr bool operator==(const OkLab&, const OkLab&) = default;
    };

    struct OkLCh {
        float L = 0.0f; // Perceived lightness [0, 1]
        float C = 0.0f; // Chroma [0, ~0.4]
        float h = 0.0f; // Hue in radians [0, 2pi)
        float alpha = 1.0f; // [0, 1]

        friend constexpr bool operator==(const OkLCh&, const OkLCh&) = default;
    };

    [[nodiscard]] inline OkLCh to_oklch(const OkLab& lab) noexcept {
        const float C = std::sqrt(lab.a * lab.a + lab.b * lab.b);
        float h = std::atan2(lab.b, lab.a);
        if (h < 0.0f) h += 6.28318530717958647692f;
        return OkLCh{.L = lab.L, .C = C, .h = h, .alpha = lab.alpha};
    }

    [[nodiscard]] inline OkLab from_oklch(const OkLCh& lch) noexcept {
        return OkLab{
            .L = lch.L,
            .a = lch.C * std::cos(lch.h),
            .b = lch.C * std::sin(lch.h),
            .alpha = lch.alpha
        };
    }

    // ── Color Space Concept & Customization Point ────────────────────────────────

    // Default template converter (can be specialized by users for custom color spaces)
    template <typename CS>
    struct color_space_converter;

    // Concept satisfied by any color space type providing to_color/from_color
    template <typename CS>
    concept color_space_type = requires(const CS& cs, const Color& c) {
            { to_color(cs) } -> std::same_as<Color>;
            { from_color<CS>(c) } -> std::same_as<CS>;
        } || requires(const CS& cs, const Color& c) {
            { color_space_converter<CS>::to_color(cs) } -> std::same_as<Color>;
            { color_space_converter<CS>::from_color(c) } -> std::same_as<CS>;
        };

    // ── HSL Conversions ─────────────────────────────────────────────────────────

    [[nodiscard]] inline HSL to_hsl(const Color& c) noexcept {
        const float r = kalpana::detail::clamp01(c.r);
        const float g = kalpana::detail::clamp01(c.g);
        const float b = kalpana::detail::clamp01(c.b);

        const float max_v = std::max({r, g, b});
        const float min_v = std::min({r, g, b});
        const float delta = max_v - min_v;

        HSL out{.h = 0.0f, .s = 0.0f, .l = (max_v + min_v) * 0.5f, .a = c.a};

        if (delta > 1e-6f) {
            out.s = (out.l > 0.5f) ? (delta / (2.0f - max_v - min_v)) : (delta / (max_v + min_v));
            if (max_v == r) {
                out.h = (g - b) / delta + (g < b ? 6.0f : 0.0f);
            }
            else if (max_v == g) {
                out.h = (b - r) / delta + 2.0f;
            }
            else {
                out.h = (r - g) / delta + 4.0f;
            }
            out.h *= 60.0f;
        }
        return out;
    }

    [[nodiscard]] inline Color from_hsl(const HSL& hsl) noexcept {
        auto hue_to_rgb = [](float p, float q, float t) -> float {
            if (t < 0.0f) t += 1.0f;
            if (t > 1.0f) t -= 1.0f;
            if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
            if (t < 1.0f / 2.0f) return q;
            if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
            return p;
        };

        const float h = std::fmod(hsl.h, 360.0f) / 360.0f;
        const float s = kalpana::detail::clamp01(hsl.s);
        const float l = kalpana::detail::clamp01(hsl.l);

        if (s <= 1e-6f) {
            return Color{l, l, l, hsl.a};
        }

        const float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
        const float p = 2.0f * l - q;

        return Color{
            hue_to_rgb(p, q, h + 1.0f / 3.0f),
            hue_to_rgb(p, q, h),
            hue_to_rgb(p, q, h - 1.0f / 3.0f),
            hsl.a
        };
    }

    // ── OkLab Conversions (Björn Ottosson's Perceptually Uniform Space) ──────────

    [[nodiscard]] inline OkLab to_oklab(const Color& c) noexcept {
        const float r = kalpana::detail::clamp01(c.r);
        const float g = kalpana::detail::clamp01(c.g);
        const float b = kalpana::detail::clamp01(c.b);

        // Linear RGB to cone response LMS
        const float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
        const float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
        const float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

        const float l_ = std::cbrt(std::max(0.0f, l));
        const float m_ = std::cbrt(std::max(0.0f, m));
        const float s_ = std::cbrt(std::max(0.0f, s));

        return OkLab{
            .L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
            .a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
            .b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
            .alpha = c.a
        };
    }

    [[nodiscard]] inline Color from_oklab(const OkLab& lab) noexcept {
        const float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
        const float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
        const float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;

        const float l = l_ * l_ * l_;
        const float m = m_ * m_ * m_;
        const float s = s_ * s_ * s_;

        return Color{
            kalpana::detail::clamp01(+4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s),
            kalpana::detail::clamp01(-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s),
            kalpana::detail::clamp01(-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s),
            lab.alpha
        };
    }

    // ── Specializations for Built-in Types ──────────────────────────────────────

    template <>
    struct color_space_converter<HSL> {
        static Color to_color(const HSL& hsl) noexcept { return from_hsl(hsl); }
        static HSL from_color(const Color& c) noexcept { return to_hsl(c); }
    };

    template <>
    struct color_space_converter<OkLab> {
        static Color to_color(const OkLab& lab) noexcept { return from_oklab(lab); }
        static OkLab from_color(const Color& c) noexcept { return to_oklab(c); }
    };

    // ── Generic Free Functions for Any Color Space ──────────────────────────────

    template <color_space_type CS>
    [[nodiscard]] inline Color to_color(const CS& cs) noexcept {
        return color_space_converter<CS>::to_color(cs);
    }

    template <color_space_type CS>
    [[nodiscard]] inline CS from_color(const Color& c) noexcept {
        return color_space_converter<CS>::from_color(c);
    }

    // Perceptually uniform interpolation in OkLab
    [[nodiscard]] inline Color lerp_oklab(const Color& c1, const Color& c2, float t) noexcept {
        const OkLab a = to_oklab(c1);
        const OkLab b = to_oklab(c2);
        const float factor = kalpana::detail::clamp01(t);

        const OkLab interpolated{
            .L = a.L + (b.L - a.L) * factor,
            .a = a.a + (b.a - a.a) * factor,
            .b = a.b + (b.b - a.b) * factor,
            .alpha = a.alpha + (b.alpha - a.alpha) * factor
        };
        return from_oklab(interpolated);
    }

    // Generic interpolation in any custom color space
    template <color_space_type CS, typename LerpFn>
    [[nodiscard]] inline Color lerp_custom(const Color& c1, const Color& c2, float t, LerpFn&& lerp_fn) {
        const CS a = from_color<CS>(c1);
        const CS b = from_color<CS>(c2);
        const CS interpolated = lerp_fn(a, b, kalpana::detail::clamp01(t));
        return to_color<CS>(interpolated);
    }
} // namespace kalpana::color_space
