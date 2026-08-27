#pragma once
// ============================================================================
// kalpana/fill/procedural.hpp — Procedural Fills & Extensible Texture Generators
// ============================================================================
// Supports paper texture, marble, wood grain, canvas, parchment, and custom
// user-defined noise algorithms.
// ============================================================================

#include "../color/color.hpp"
#include "noise.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <cmath>

namespace kalpana {

class ProceduralFill {
public:
    enum class Kind : std::uint8_t {
        PaperTexture,
        WatercolorPaper,
        Marble,
        Wood,
        Grain,
        Noise,
        Canvas,
        Linen,
        Parchment,
        Custom
    };

    Kind  kind   = Kind::PaperTexture;
    float scale  = 1.0f;             // spatial scale
    float amount = 0.5f;             // blend strength [0, 1]
    Color tint   = colors::white();  // tint color

    ProceduralFill() = default;

    // Factory constructors
    static ProceduralFill paper_texture(float scale = 1.0f, float amount = 0.3f) noexcept {
        ProceduralFill pf;
        pf.kind = Kind::PaperTexture;
        pf.scale = scale;
        pf.amount = amount;
        return pf;
    }

    static ProceduralFill watercolor_paper(float scale = 0.8f, float amount = 0.4f) noexcept {
        ProceduralFill pf;
        pf.kind = Kind::WatercolorPaper;
        pf.scale = scale;
        pf.amount = amount;
        return pf;
    }

    static ProceduralFill marble(float scale = 1.0f, Color tint = colors::white()) noexcept {
        ProceduralFill pf;
        pf.kind = Kind::Marble;
        pf.scale = scale;
        pf.amount = 0.7f;
        pf.tint = tint;
        return pf;
    }

    static ProceduralFill wood(float scale = 1.0f, Color tint = {0.55f, 0.35f, 0.17f}) noexcept {
        ProceduralFill pf;
        pf.kind = Kind::Wood;
        pf.scale = scale;
        pf.amount = 0.8f;
        pf.tint = tint;
        return pf;
    }

    static ProceduralFill grain(float amount = 0.04f) noexcept {
        ProceduralFill pf;
        pf.kind = Kind::Grain;
        pf.scale = 10.0f;
        pf.amount = amount;
        return pf;
    }

    static ProceduralFill canvas_texture(float scale = 1.0f, Color tint = colors::white()) noexcept {
        ProceduralFill pf;
        pf.kind = Kind::Canvas;
        pf.scale = scale;
        pf.amount = 0.35f;
        pf.tint = tint;
        return pf;
    }

    static ProceduralFill parchment(float scale = 1.0f, float age = 0.5f) noexcept {
        ProceduralFill pf;
        pf.kind = Kind::Parchment;
        pf.scale = scale;
        pf.amount = age;
        pf.tint = Color{0.92f, 0.85f, 0.70f};
        return pf;
    }

    // Plug-and-play custom user noise generator
    template <noise::noise_generator G>
    static ProceduralFill custom_noise(G generator, float scale = 1.0f, float amount = 0.5f, Color tint = colors::white()) {
        ProceduralFill pf;
        pf.kind = Kind::Custom;
        pf.scale = scale;
        pf.amount = amount;
        pf.tint = tint;
        pf.custom_eval_ = [gen = std::move(generator)](float x, float y) -> float {
            return gen.evaluate(x, y);
        };
        return pf;
    }

    static ProceduralFill custom_function(std::function<float(float, float)> fn, float scale = 1.0f, float amount = 0.5f, Color tint = colors::white()) {
        ProceduralFill pf;
        pf.kind = Kind::Custom;
        pf.scale = scale;
        pf.amount = amount;
        pf.tint = tint;
        pf.custom_eval_ = std::move(fn);
        return pf;
    }

    // Evaluates procedural value at world coordinate (x, y) -> modulation factor [0, 1]
    [[nodiscard]] float evaluate(float x, float y) const noexcept {
        const float sx = (scale > 1e-4f) ? (x * 0.05f / scale) : x;
        const float sy = (scale > 1e-4f) ? (y * 0.05f / scale) : y;

        switch (kind) {
            case Kind::PaperTexture: {
                // Fibers + fine grain
                const float n1 = noise::simplex(sx * 4.0f, sy * 4.0f);
                const float n2 = noise::fbm(sx * 16.0f, sy * 16.0f, 3);
                return std::clamp(0.5f + 0.5f * (0.6f * n1 + 0.4f * n2) * amount, 0.0f, 1.0f);
            }
            case Kind::WatercolorPaper: {
                // Heavy rough cold-press paper grain
                const float n1 = noise::worley(sx * 3.0f, sy * 3.0f);
                const float n2 = noise::fbm(sx * 8.0f, sy * 8.0f, 4);
                return std::clamp(0.5f + (0.7f * n1 + 0.3f * n2 - 0.5f) * amount, 0.0f, 1.0f);
            }
            case Kind::Marble: {
                // Sinuous marble veins: sin(x + turbulence(x, y))
                const float turb = noise::turbulence(sx * 2.0f, sy * 2.0f, 5);
                const float sin_val = std::sin(sx * 3.0f + turb * 4.0f);
                return std::clamp(0.5f + 0.5f * sin_val * amount, 0.0f, 1.0f);
            }
            case Kind::Wood: {
                // Concentric wood ring pattern modulated by noise
                const float dist = std::sqrt(sx * sx + sy * sy) * 5.0f + noise::simplex(sx * 2.0f, sy * 2.0f) * 2.0f;
                const float ring = std::fmod(dist, 1.0f);
                return std::clamp(ring * amount + (1.0f - amount) * 0.5f, 0.0f, 1.0f);
            }
            case Kind::Grain: {
                // Fine high-frequency film grain
                const float g = noise::simplex(sx * 20.0f, sy * 20.0f);
                return std::clamp(0.5f + 0.5f * g * amount, 0.0f, 1.0f);
            }
            case Kind::Canvas: {
                // Cross-weave canvas texture
                const float wx = std::sin(sx * 30.0f);
                const float wy = std::sin(sy * 30.0f);
                const float weave = 0.5f + 0.25f * (wx + wy);
                return std::clamp(weave * amount + (1.0f - amount) * 0.5f, 0.0f, 1.0f);
            }
            case Kind::Linen: {
                // Fine irregular linen weave
                const float wx = std::sin(sx * 40.0f + noise::simplex(sx, sy) * 0.5f);
                const float wy = std::cos(sy * 40.0f + noise::simplex(sy, sx) * 0.5f);
                return std::clamp(0.5f + 0.25f * (wx + wy) * amount, 0.0f, 1.0f);
            }
            case Kind::Parchment: {
                // Aged cloudiness and mottled discoloration
                const float cloud = noise::fbm(sx * 1.5f, sy * 1.5f, 4);
                return std::clamp(0.5f + 0.5f * cloud * amount, 0.0f, 1.0f);
            }
            case Kind::Noise: {
                return std::clamp(0.5f + 0.5f * noise::simplex(sx, sy) * amount, 0.0f, 1.0f);
            }
            case Kind::Custom: {
                if (custom_eval_) {
                    return std::clamp(custom_eval_(x, y) * amount, 0.0f, 1.0f);
                }
                return 0.5f;
            }
        }
        return 0.5f;
    }

    // Modulates an existing base color by this procedural fill
    [[nodiscard]] Color modulate(const Color& base, float x, float y) const noexcept {
        const float val = evaluate(x, y);
        return Color{
            base.r * (1.0f - amount + amount * val * tint.r),
            base.g * (1.0f - amount + amount * val * tint.g),
            base.b * (1.0f - amount + amount * val * tint.b),
            base.a
        };
    }

    friend bool operator==(const ProceduralFill& a, const ProceduralFill& b) noexcept {
        return a.kind == b.kind && a.scale == b.scale && a.amount == b.amount && a.tint == b.tint;
    }

private:
    std::function<float(float, float)> custom_eval_{};
};

} // namespace kalpana
