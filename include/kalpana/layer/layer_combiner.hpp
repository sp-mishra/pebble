#pragma once
// ============================================================================
// kalpana/layer/layer_combiner.hpp — Extensible Layer Combination Policies
// ============================================================================
// Zero-virtual policy-driven layer compositing: Kubelka-Munk spectral mixing,
// Porter-Duff alpha compositing, 12 Photoshop blend modes, physical wet-on-wet
// watercolor diffusion, and custom user combiner extensions.
// ============================================================================

#include "../color/color.hpp"
#include "../color/spectral.hpp"
#include "../paint/paint.hpp"
#include <concepts>
#include <functional>
#include <cmath>
#include <algorithm>

namespace kalpana {

// ── Layer Combination Policy Concept ─────────────────────────────────────────

template <typename P>
concept layer_combination_policy = requires(const P& p, const Color& dst, const Color& src, float opacity) {
    { p.combine(dst, src, opacity) } noexcept -> std::same_as<Color>;
};

// ── Built-in Layer Combiners ─────────────────────────────────────────────────

// 1. Physical Kubelka-Munk Subtractive Spectral Pigment Mixing
struct SpectralSubtractiveCombiner {
    [[nodiscard]] Color combine(const Color& dst, const Color& src, float opacity) const noexcept {
        if (src.a <= 1e-4f || opacity <= 1e-4f) return dst;
        if (dst.a <= 1e-4f) {
            return Color{src.r, src.g, src.b, src.a * opacity};
        }

        const float blend_t = std::clamp(src.a * opacity * 0.5f, 0.0f, 1.0f);
        const Color mixed = spectral::mix(dst, src, blend_t);
        const float out_a = std::clamp(dst.a + (1.0f - dst.a) * src.a * opacity, 0.0f, 1.0f);
        return Color{mixed.r, mixed.g, mixed.b, out_a};
    }
};

// 2. Standard Porter-Duff Source-Over Alpha Compositing
struct PorterDuffOverCombiner {
    [[nodiscard]] Color combine(const Color& dst, const Color& src, float opacity) const noexcept {
        const Color src_op{src.r, src.g, src.b, src.a * opacity};
        return src_op.over(dst);
    }
};

// 3. Photoshop / Standard Digital Blend Modes
struct PhotoshopBlendCombiner {
    BlendMode mode = BlendMode::SrcOver;

    [[nodiscard]] Color combine(const Color& dst, const Color& src, float opacity) const noexcept {
        const float sa = src.a * opacity;
        const float da = dst.a;
        if (sa <= 1e-4f) return dst;

        auto blend_ch = [&](float cb, float cs) -> float {
            switch (mode) {
                case BlendMode::Multiply:   return cb * cs;
                case BlendMode::Screen:     return cb + cs - cb * cs;
                case BlendMode::Overlay:    return cb < 0.5f ? (2.0f * cb * cs) : (1.0f - 2.0f * (1.0f - cb) * (1.0f - cs));
                case BlendMode::Darken:     return std::min(cb, cs);
                case BlendMode::Lighten:    return std::max(cb, cs);
                case BlendMode::ColorDodge: return cs >= 1.0f ? 1.0f : std::min(1.0f, cb / (1.0f - cs + 1e-5f));
                case BlendMode::ColorBurn:  return cs <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - cb) / (cs + 1e-5f));
                case BlendMode::HardLight:  return cs < 0.5f ? (2.0f * cb * cs) : (1.0f - 2.0f * (1.0f - cb) * (1.0f - cs));
                case BlendMode::SoftLight:  return cs < 0.5f ? (cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb)) : (cb + (2.0f * cs - 1.0f) * (std::sqrt(cb) - cb));
                case BlendMode::Difference: return std::fabs(cb - cs);
                case BlendMode::Exclusion:  return cb + cs - 2.0f * cb * cs;
                case BlendMode::SrcOver:
                default:                    return cs;
            }
        };

        const float r = blend_ch(dst.r, src.r);
        const float g = blend_ch(dst.g, src.g);
        const float b = blend_ch(dst.b, src.b);

        const Color blended{r, g, b, sa};
        return blended.over(dst);
    }
};

// 4. Physical Wet-on-Wet Watercolor Diffusion
struct WetDiffusionCombiner {
    float wetness_bleed = 0.5f;

    [[nodiscard]] Color combine(const Color& dst, const Color& src, float opacity) const noexcept {
        const float bleed = std::clamp(wetness_bleed * 0.5f, 0.0f, 0.5f);
        const Color km_mix = spectral::mix(dst, src, std::clamp(0.5f + bleed, 0.0f, 1.0f));
        const float out_a = std::clamp(dst.a + src.a * opacity * (1.0f - dst.a * 0.5f), 0.0f, 1.0f);
        return Color{km_mix.r, km_mix.g, km_mix.b, out_a};
    }
};

// ── Type-Erased Extensible Layer Combiner Wrapper ───────────────────────────

class LayerCombiner {
public:
    LayerCombiner() : fn_([](const Color& dst, const Color& src, float op) {
        return PorterDuffOverCombiner{}.combine(dst, src, op);
    }) {}

    template <layer_combination_policy Policy>
    LayerCombiner(Policy policy)
        : fn_([p = std::move(policy)](const Color& dst, const Color& src, float op) {
            return p.combine(dst, src, op);
        }) {}

    // Factory methods
    static LayerCombiner spectral() noexcept {
        return LayerCombiner(SpectralSubtractiveCombiner{});
    }

    static LayerCombiner source_over() noexcept {
        return LayerCombiner(PorterDuffOverCombiner{});
    }

    static LayerCombiner blend_mode(BlendMode mode) noexcept {
        return LayerCombiner(PhotoshopBlendCombiner{.mode = mode});
    }

    static LayerCombiner wet_diffusion(float bleed = 0.5f) noexcept {
        return LayerCombiner(WetDiffusionCombiner{.wetness_bleed = bleed});
    }

    template <typename CustomFn>
    static LayerCombiner custom(CustomFn&& fn) {
        LayerCombiner lc;
        lc.fn_ = std::forward<CustomFn>(fn);
        return lc;
    }

    [[nodiscard]] Color combine(const Color& dst, const Color& src, float opacity = 1.0f) const noexcept {
        if (fn_) return fn_(dst, src, opacity);
        return src.over(dst);
    }

private:
    std::function<Color(const Color&, const Color&, float)> fn_{};
};

} // namespace kalpana
