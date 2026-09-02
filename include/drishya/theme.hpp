#pragma once
// ============================================================================
// drishya/theme.hpp — constexpr design tokens
// ----------------------------------------------------------------------------
// A Theme is a plain constexpr token bundle: colors, spacing ramp, radii, and a
// type scale. Themes are values — a widget reads tokens from whichever theme it
// is handed. Light and dark presets ship built in, plus a dataviz-aligned chart
// palette for AI/ML dashboards. Colors reuse kalpana::Color (linear RGBA).
//
// Tokens are stored as packed 0xAARRGGBB so they cross the Painter boundary
// (which speaks packed ARGB via set_color) without conversion churn; a helper
// yields kalpana::Color when a widget needs the linear form directly.
// ============================================================================

#include "kalpana/kalpana.hpp"

#include <array>
#include <cstdint>

namespace pebble::drishya::theme {
    using Argb = std::uint32_t;

    [[nodiscard]] constexpr kalpana::Color to_color(Argb v) noexcept {
        const float a = static_cast<float>((v >> 24) & 0xFF) / 255.0f;
        const float r = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
        const float g = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
        const float b = static_cast<float>(v & 0xFF) / 255.0f;
        return kalpana::Color(r, g, b, a);
    }

    // Spacing ramp (px) — a small geometric scale used for padding/gaps.
    struct Spacing {
        float xs = 4.0f;
        float sm = 8.0f;
        float md = 16.0f;
        float lg = 24.0f;
        float xl = 32.0f;
    };

    // Corner radii (px).
    struct Radii {
        float none = 0.0f;
        float sm = 4.0f;
        float md = 8.0f;
        float lg = 16.0f;
        float pill = 9999.0f;
    };

    // Type scale (font sizes, px).
    struct TypeScale {
        float caption = 12.0f;
        float body = 14.0f;
        float subtitle = 16.0f;
        float title = 20.0f;
        float headline = 28.0f;
        float display = 40.0f;
    };

    // Semantic color roles.
    struct Palette {
        Argb background = 0xFF101418;
        Argb surface = 0xFF171C22;
        Argb surface_alt = 0xFF1F262E;
        Argb border = 0xFF2A333D;
        Argb text = 0xFFE6EDF3;
        Argb text_muted = 0xFF9BA7B4;
        Argb primary = 0xFF3B82F6;
        Argb primary_hover = 0xFF60A5FA;
        Argb success = 0xFF22C55E;
        Argb warning = 0xFFF59E0B;
        Argb danger = 0xFFEF4444;
        Argb focus_ring = 0xFF93C5FD;
    };

    // Categorical dataviz palette (8 hues) for charts/sparklines — chosen for
    // perceptual separation on dark backgrounds.
    struct ChartColors {
        std::array<Argb, 8> series{
            0xFF60A5FA, 0xFF34D399, 0xFFFBBF24, 0xFFF87171,
            0xFFA78BFA, 0xFF22D3EE, 0xFFF472B6, 0xFF9CA3AF,
        };

        [[nodiscard]] constexpr Argb at(std::size_t i) const noexcept {
            return series[i % series.size()];
        }
    };

    struct Theme {
        Palette color{};
        Spacing spacing{};
        Radii radii{};
        TypeScale type{};
        ChartColors chart{};
    };

    // --- presets ---------------------------------------------------------------
    [[nodiscard]] constexpr Theme dark() noexcept { return Theme{}; }

    [[nodiscard]] constexpr Theme light() noexcept {
        Theme t{};
        t.color = Palette{
            /*background*/ 0xFFF7F9FC,
            /*surface*/ 0xFFFFFFFF,
            /*surface_alt*/ 0xFFEEF2F7,
            /*border*/ 0xFFD5DCE5,
            /*text*/ 0xFF1A2029,
            /*text_muted*/ 0xFF5B6673,
            /*primary*/ 0xFF2563EB,
            /*primary_hover*/ 0xFF1D4ED8,
            /*success*/ 0xFF16A34A,
            /*warning*/ 0xFFD97706,
            /*danger*/ 0xFFDC2626,
            /*focus_ring*/ 0xFF3B82F6,
        };
        return t;
    }
} // namespace pebble::drishya::theme
