#pragma once
// ============================================================================
// kalpana/effect/effect_chain.hpp — Composable Effect Pipeline EDSL
// ============================================================================
// Small-buffer optimized composable effect chains with operator| and operator>>.
// ============================================================================

#include "effect.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include <variant>
#include <span>
#include <utility>

namespace kalpana {

using EffectData = std::variant<
    BlurEffect,
    DropShadowEffect,
    TintEffect,
    GlowEffect,
    InnerGlowEffect,
    NoiseEffect,
    GrainEffect,
    DisplaceEffect,
    PosterizeEffect,
    BloomEffect,
    VibranceEffect,
    SaturateEffect,
    BrightnessEffect,
    ContrastEffect,
    HueRotateEffect,
    InvertEffect,
    SepiaEffect,
    VignetteEffect,
    ChromaticAberrationEffect,
    RoundedShadowEffect,
    SpectralTintEffect,
    SpectralBloomEffect,
    PigmentGranulationEffect,
    EdgeDarkeningEffect,
    BackdropBlurEffect,
    DepthOfFieldEffect
>;

class EffectNode {
public:
    constexpr EffectNode() noexcept = default;

    template <typename T>
        requires std::constructible_from<EffectData, T>
    constexpr EffectNode(T&& data) noexcept : data_(std::forward<T>(data)) {}

    // Factory methods
    [[nodiscard]] static EffectNode blur(float radius = 5.0f) noexcept {
        return EffectNode(BlurEffect{.radius = radius});
    }

    [[nodiscard]] static EffectNode shadow(
        float blur_r = 4.0f,
        pebble::math::vec2 offset = {2.0f, 2.0f},
        Color color = {0.0f, 0.0f, 0.0f, 0.5f}) noexcept {
        return EffectNode(DropShadowEffect{.offset = offset, .blur_radius = blur_r, .color = color});
    }

    [[nodiscard]] static EffectNode tint(Color color, float intensity = 1.0f) noexcept {
        return EffectNode(TintEffect{.color = color, .intensity = intensity});
    }

    [[nodiscard]] static EffectNode glow(float radius = 3.0f, Color color = colors::white()) noexcept {
        return EffectNode(GlowEffect{.radius = radius, .color = color});
    }

    [[nodiscard]] static EffectNode inner_glow(float radius = 2.0f, Color color = colors::white()) noexcept {
        return EffectNode(InnerGlowEffect{.radius = radius, .color = color});
    }

    [[nodiscard]] static EffectNode noise(float amount = 0.1f, float scale = 1.0f) noexcept {
        return EffectNode(NoiseEffect{.amount = amount, .scale = scale});
    }

    [[nodiscard]] static EffectNode grain(float amount = 0.04f) noexcept {
        return EffectNode(GrainEffect{.amount = amount});
    }

    [[nodiscard]] static EffectNode displace(float amount = 5.0f) noexcept {
        return EffectNode(DisplaceEffect{.amount = amount});
    }

    [[nodiscard]] static EffectNode posterize(int levels = 4) noexcept {
        return EffectNode(PosterizeEffect{.levels = levels});
    }

    [[nodiscard]] static EffectNode bloom(float threshold = 0.8f, float intensity = 1.0f) noexcept {
        return EffectNode(BloomEffect{.threshold = threshold, .intensity = intensity});
    }

    [[nodiscard]] static EffectNode vibrance(float amount = 0.2f) noexcept {
        return EffectNode(VibranceEffect{.amount = amount});
    }

    [[nodiscard]] static EffectNode saturate(float amount = 1.0f) noexcept {
        return EffectNode(SaturateEffect{.amount = amount});
    }

    [[nodiscard]] static EffectNode brightness(float amount = 1.0f) noexcept {
        return EffectNode(BrightnessEffect{.amount = amount});
    }

    [[nodiscard]] static EffectNode contrast(float amount = 1.0f) noexcept {
        return EffectNode(ContrastEffect{.amount = amount});
    }

    [[nodiscard]] static EffectNode hue_rotate(float degrees = 0.0f) noexcept {
        return EffectNode(HueRotateEffect{.degrees = degrees});
    }

    [[nodiscard]] static EffectNode invert() noexcept {
        return EffectNode(InvertEffect{});
    }

    [[nodiscard]] static EffectNode sepia(float intensity = 1.0f) noexcept {
        return EffectNode(SepiaEffect{.intensity = intensity});
    }

    [[nodiscard]] static EffectNode vignette(float radius = 0.8f, float softness = 0.3f) noexcept {
        return EffectNode(VignetteEffect{.radius = radius, .softness = softness});
    }

    [[nodiscard]] static EffectNode chromatic_aberration(float offset = 2.0f) noexcept {
        return EffectNode(ChromaticAberrationEffect{.offset = offset});
    }

    [[nodiscard]] static EffectNode rounded_shadow(float corner_radius = 12.0f, float shadow_blur = 8.0f) noexcept {
        return EffectNode(RoundedShadowEffect{.corner_radius = corner_radius, .shadow_blur = shadow_blur});
    }

    // Spectral specific
    [[nodiscard]] static EffectNode spectral_tint(spectral::SpectralColor pigment, float intensity = 1.0f) noexcept {
        return EffectNode(SpectralTintEffect{.pigment = pigment, .intensity = intensity});
    }

    [[nodiscard]] static EffectNode spectral_bloom(float threshold = 0.8f) noexcept {
        return EffectNode(SpectralBloomEffect{.threshold = threshold});
    }

    [[nodiscard]] static EffectNode pigment_granulation(float scale = 1.0f, float amount = 0.3f) noexcept {
        return EffectNode(PigmentGranulationEffect{.scale = scale, .amount = amount});
    }

    [[nodiscard]] static EffectNode edge_darkening(float width = 2.0f, float intensity = 0.5f) noexcept {
        return EffectNode(EdgeDarkeningEffect{.width = width, .intensity = intensity});
    }

    [[nodiscard]] static EffectNode backdrop_blur(float radius = 20.0f) noexcept {
        return EffectNode(BackdropBlurEffect{.radius = radius});
    }

    [[nodiscard]] static EffectNode dof(pebble::math::vec2 focus_point = {0.0f, 0.0f},
                                        float focal_range = 100.0f,
                                        float max_blur_radius = 8.0f) noexcept {
        return EffectNode(DepthOfFieldEffect{.focus_point = focus_point,
                                             .focal_range = focal_range,
                                             .max_blur_radius = max_blur_radius});
    }

    [[nodiscard]] const EffectData& data() const noexcept { return data_; }
    [[nodiscard]] EffectData& data() noexcept { return data_; }

    friend constexpr bool operator==(const EffectNode&, const EffectNode&) = default;

private:
    EffectData data_{BlurEffect{}};
};

// Convenience free functions for effect creation
[[nodiscard]] inline EffectNode blur(float radius = 5.0f) noexcept { return EffectNode::blur(radius); }
[[nodiscard]] inline EffectNode shadow(float blur_r = 4.0f, pebble::math::vec2 offset = {2.0f, 2.0f}, Color color = {0.0f, 0.0f, 0.0f, 0.5f}) noexcept { return EffectNode::shadow(blur_r, offset, color); }
[[nodiscard]] inline EffectNode tint(Color color, float intensity = 1.0f) noexcept { return EffectNode::tint(color, intensity); }
[[nodiscard]] inline EffectNode glow(float radius = 3.0f, Color color = colors::white()) noexcept { return EffectNode::glow(radius, color); }
[[nodiscard]] inline EffectNode inner_glow(float radius = 2.0f, Color color = colors::white()) noexcept { return EffectNode::inner_glow(radius, color); }
[[nodiscard]] inline EffectNode noise_effect(float amount = 0.1f, float scale = 1.0f) noexcept { return EffectNode::noise(amount, scale); }
[[nodiscard]] inline EffectNode grain(float amount = 0.04f) noexcept { return EffectNode::grain(amount); }
[[nodiscard]] inline EffectNode displace(float amount = 5.0f) noexcept { return EffectNode::displace(amount); }
[[nodiscard]] inline EffectNode posterize(int levels = 4) noexcept { return EffectNode::posterize(levels); }
[[nodiscard]] inline EffectNode bloom(float threshold = 0.8f, float intensity = 1.0f) noexcept { return EffectNode::bloom(threshold, intensity); }
[[nodiscard]] inline EffectNode vibrance(float amount = 0.2f) noexcept { return EffectNode::vibrance(amount); }
[[nodiscard]] inline EffectNode saturate(float amount = 1.0f) noexcept { return EffectNode::saturate(amount); }
[[nodiscard]] inline EffectNode brightness(float amount = 1.0f) noexcept { return EffectNode::brightness(amount); }
[[nodiscard]] inline EffectNode contrast(float amount = 1.0f) noexcept { return EffectNode::contrast(amount); }
[[nodiscard]] inline EffectNode hue_rotate(float degrees = 0.0f) noexcept { return EffectNode::hue_rotate(degrees); }
[[nodiscard]] inline EffectNode invert() noexcept { return EffectNode::invert(); }
[[nodiscard]] inline EffectNode sepia(float intensity = 1.0f) noexcept { return EffectNode::sepia(intensity); }
[[nodiscard]] inline EffectNode vignette(float radius = 0.8f, float softness = 0.3f) noexcept { return EffectNode::vignette(radius, softness); }
[[nodiscard]] inline EffectNode chromatic_aberration(float offset = 2.0f) noexcept { return EffectNode::chromatic_aberration(offset); }
[[nodiscard]] inline EffectNode rounded_shadow(float corner_radius = 12.0f, float shadow_blur = 8.0f) noexcept { return EffectNode::rounded_shadow(corner_radius, shadow_blur); }
[[nodiscard]] inline EffectNode spectral_tint(spectral::SpectralColor pigment, float intensity = 1.0f) noexcept { return EffectNode::spectral_tint(pigment, intensity); }
[[nodiscard]] inline EffectNode spectral_bloom(float threshold = 0.8f) noexcept { return EffectNode::spectral_bloom(threshold); }
[[nodiscard]] inline EffectNode pigment_granulation(float scale = 1.0f, float amount = 0.3f) noexcept { return EffectNode::pigment_granulation(scale, amount); }
[[nodiscard]] inline EffectNode edge_darkening(float width = 2.0f, float intensity = 0.5f) noexcept { return EffectNode::edge_darkening(width, intensity); }
[[nodiscard]] inline EffectNode backdrop_blur(float radius = 20.0f) noexcept { return EffectNode::backdrop_blur(radius); }

// Configurable Effect Chain with SmallVector storage policy
template <std::size_t InlineBytes = 256>
class BasicEffectChain {
public:
    using container_type = containers::dynamic::SmallVector<EffectNode, InlineBytes>;

    BasicEffectChain() = default;

    BasicEffectChain(EffectNode node) {
        nodes_.push_back(std::move(node));
    }

    BasicEffectChain& add(EffectNode node) {
        nodes_.push_back(std::move(node));
        return *this;
    }

    [[nodiscard]] BasicEffectChain operator|(EffectNode node) const {
        BasicEffectChain copy = *this;
        copy.add(std::move(node));
        return copy;
    }

    [[nodiscard]] BasicEffectChain operator|(const BasicEffectChain& other) const {
        BasicEffectChain copy = *this;
        for (const auto& n : other.nodes()) {
            copy.add(n);
        }
        return copy;
    }

    [[nodiscard]] std::span<const EffectNode> nodes() const noexcept {
        return std::span<const EffectNode>(nodes_.data(), nodes_.size());
    }

    [[nodiscard]] bool empty() const noexcept { return nodes_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    void clear() noexcept { nodes_.clear(); }

    friend bool operator==(const BasicEffectChain&, const BasicEffectChain&) = default;

private:
    container_type nodes_;
};

// Default alias
using EffectChain = BasicEffectChain<256>;

// Free operator| for chaining two EffectNodes
[[nodiscard]] inline EffectChain operator|(EffectNode a, EffectNode b) {
    EffectChain chain(std::move(a));
    chain.add(std::move(b));
    return chain;
}

// Ranges-style operator>>
[[nodiscard]] inline EffectChain operator>>(EffectNode a, EffectNode b) {
    return std::move(a) | std::move(b);
}

[[nodiscard]] inline EffectChain operator>>(EffectChain a, EffectNode b) {
    return std::move(a) | std::move(b);
}

} // namespace kalpana
