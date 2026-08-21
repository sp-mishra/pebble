#pragma once
// ============================================================================
// kalpana/effect/effect.hpp — Abstracted Graphic Effects (Blur, Drop Shadow, Tint)
// ============================================================================

#include "../color/color.hpp"
#include "containers/numeric/math_vector.hpp"

namespace kalpana {

enum class EffectKind : std::uint8_t {
    Blur,
    DropShadow,
    Tint,
    ColorFilter
};

struct BlurEffect {
    float radius = 5.0f;
};

struct DropShadowEffect {
    pebble::math::vec2 offset{2.0f, 2.0f};
    float              blur_radius = 4.0f;
    Color              color{0.0f, 0.0f, 0.0f, 0.5f};
};

struct TintEffect {
    Color color = colors::white();
    float intensity = 1.0f;
};

struct Effect {
    EffectKind kind = EffectKind::Blur;
    union {
        BlurEffect       blur;
        DropShadowEffect shadow;
        TintEffect       tint;
    };

    constexpr Effect() noexcept : kind(EffectKind::Blur), blur{5.0f} {}
    constexpr explicit Effect(BlurEffect b) noexcept : kind(EffectKind::Blur), blur(b) {}
    constexpr explicit Effect(DropShadowEffect s) noexcept : kind(EffectKind::DropShadow), shadow(s) {}
    constexpr explicit Effect(TintEffect t) noexcept : kind(EffectKind::Tint), tint(t) {}
};

} // namespace kalpana
