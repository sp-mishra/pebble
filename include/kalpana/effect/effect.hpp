#pragma once
// ============================================================================
// kalpana/effect/effect.hpp — Value-Type Effect Parameter Specifications
// ============================================================================
// Zero-virtual, lightweight value structs describing graphical filter parameters.
// Composed via EffectChain in kalpana/effect/effect_chain.hpp.
// ============================================================================

#include "../color/color.hpp"
#include "../color/spectral.hpp"
#include "containers/numeric/math_vector.hpp"

namespace kalpana {
    struct BlurEffect {
        float radius = 5.0f;
        friend constexpr bool operator==(const BlurEffect&, const BlurEffect&) = default;
    };

    struct DropShadowEffect {
        pebble::math::vec2 offset{2.0f, 2.0f};
        float blur_radius = 4.0f;
        Color color{0.0f, 0.0f, 0.0f, 0.5f};
        friend constexpr bool operator==(const DropShadowEffect&, const DropShadowEffect&) = default;
    };

    struct TintEffect {
        Color color = colors::white();
        float intensity = 1.0f;
        friend constexpr bool operator==(const TintEffect&, const TintEffect&) = default;
    };

    struct GlowEffect {
        float radius = 3.0f;
        Color color = colors::white();
        friend constexpr bool operator==(const GlowEffect&, const GlowEffect&) = default;
    };

    struct InnerGlowEffect {
        float radius = 2.0f;
        Color color = colors::white();
        friend constexpr bool operator==(const InnerGlowEffect&, const InnerGlowEffect&) = default;
    };

    struct NoiseEffect {
        float amount = 0.1f;
        float scale = 1.0f;
        friend constexpr bool operator==(const NoiseEffect&, const NoiseEffect&) = default;
    };

    struct GrainEffect {
        float amount = 0.04f;
        friend constexpr bool operator==(const GrainEffect&, const GrainEffect&) = default;
    };

    struct DisplaceEffect {
        float amount = 5.0f;
        friend constexpr bool operator==(const DisplaceEffect&, const DisplaceEffect&) = default;
    };

    struct PosterizeEffect {
        int levels = 4;
        friend constexpr bool operator==(const PosterizeEffect&, const PosterizeEffect&) = default;
    };

    struct BloomEffect {
        float threshold = 0.8f;
        float intensity = 1.0f;
        friend constexpr bool operator==(const BloomEffect&, const BloomEffect&) = default;
    };

    struct VibranceEffect {
        float amount = 0.2f;
        friend constexpr bool operator==(const VibranceEffect&, const VibranceEffect&) = default;
    };

    struct SaturateEffect {
        float amount = 1.0f;
        friend constexpr bool operator==(const SaturateEffect&, const SaturateEffect&) = default;
    };

    struct BrightnessEffect {
        float amount = 1.0f;
        friend constexpr bool operator==(const BrightnessEffect&, const BrightnessEffect&) = default;
    };

    struct ContrastEffect {
        float amount = 1.0f;
        friend constexpr bool operator==(const ContrastEffect&, const ContrastEffect&) = default;
    };

    struct HueRotateEffect {
        float degrees = 0.0f;
        friend constexpr bool operator==(const HueRotateEffect&, const HueRotateEffect&) = default;
    };

    struct InvertEffect {
        friend constexpr bool operator==(const InvertEffect&, const InvertEffect&) = default;
    };

    struct SepiaEffect {
        float intensity = 1.0f;
        friend constexpr bool operator==(const SepiaEffect&, const SepiaEffect&) = default;
    };

    struct VignetteEffect {
        float radius = 0.8f;
        float softness = 0.3f;
        friend constexpr bool operator==(const VignetteEffect&, const VignetteEffect&) = default;
    };

    struct ChromaticAberrationEffect {
        float offset = 2.0f;
        friend constexpr bool operator==(const ChromaticAberrationEffect&, const ChromaticAberrationEffect&) = default;
    };

    struct RoundedShadowEffect {
        float corner_radius = 12.0f;
        float shadow_blur = 8.0f;
        friend constexpr bool operator==(const RoundedShadowEffect&, const RoundedShadowEffect&) = default;
    };

    // ── Spectral Color Science Specific Effects ─────────────────────────────────

    struct SpectralTintEffect {
        spectral::SpectralColor pigment{};
        float intensity = 1.0f;
        friend constexpr bool operator==(const SpectralTintEffect&, const SpectralTintEffect&) = default;
    };

    struct SpectralBloomEffect {
        float threshold = 0.8f;
        friend constexpr bool operator==(const SpectralBloomEffect&, const SpectralBloomEffect&) = default;
    };

    struct PigmentGranulationEffect {
        float scale = 1.0f;
        float amount = 0.3f;
        friend constexpr bool operator==(const PigmentGranulationEffect&, const PigmentGranulationEffect&) = default;
    };

    struct EdgeDarkeningEffect {
        float width = 2.0f;
        float intensity = 0.5f;
        friend constexpr bool operator==(const EdgeDarkeningEffect&, const EdgeDarkeningEffect&) = default;
    };

    struct BackdropBlurEffect {
        float radius = 20.0f;
        friend constexpr bool operator==(const BackdropBlurEffect&, const BackdropBlurEffect&) = default;
    };

    struct DepthOfFieldEffect {
        pebble::math::vec2 focus_point{0.0f, 0.0f};
        float focal_range = 100.0f;
        float max_blur_radius = 8.0f;
        friend constexpr bool operator==(const DepthOfFieldEffect&, const DepthOfFieldEffect&) = default;
    };
} // namespace kalpana
