#pragma once
// ============================================================================
// kalpana/brush/stamp_shape.hpp — Dab Alpha Mask Policies & Geometry
// ============================================================================
// Defines parametric stamp shapes (round, flat, chisel, bristle, airbrush)
// with hardness falloffs and orientation angles.
// ============================================================================

#include <cmath>
#include <cstdint>
#include <algorithm>

namespace kalpana {

enum class StampPreset : std::uint8_t {
    Round,          // soft circular falloff
    Flat,           // hard-edge circle
    Chisel,         // rectangular elongated
    Bristle,        // textured multi-fiber bristle pattern
    Airbrush        // very soft gaussian falloff
};

template <StampPreset Preset = StampPreset::Round>
struct StampShape {
    float roundness = 1.0f; // 1 = circle, <1 = ellipse / elongated
    float angle     = 0.0f; // rotation of the stamp mask in radians
    float hardness  = 0.8f; // edge sharpness (0 = pure soft gradient, 1 = hard edge)

    // Evaluates alpha in normalized dab coordinate space (u, v) in [-1, 1]²
    [[nodiscard]] float sample(float u, float v) const noexcept {
        // Rotate (u, v) by -angle
        const float cos_a = std::cos(-angle);
        const float sin_a = std::sin(-angle);
        float ru = cos_a * u - sin_a * v;
        float rv = sin_a * u + cos_a * v;

        // Apply roundness
        if (roundness > 1e-4f && roundness < 1.0f) {
            rv /= roundness;
        }

        if constexpr (Preset == StampPreset::Flat) {
            const float r2 = ru * ru + rv * rv;
            return r2 <= 1.0f ? 1.0f : 0.0f;
        } else if constexpr (Preset == StampPreset::Chisel) {
            const float au = std::fabs(ru);
            const float av = std::fabs(rv);
            auto smoothstep = [](float edge0, float edge1, float x) -> float {
                const float t = std::clamp((x - edge0) / (edge1 - edge0 + 1e-6f), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };
            if (au <= 1.0f && av <= 0.35f) {
                const float edge_u = 1.0f - smoothstep(hardness, 1.0f, au);
                const float edge_v = 1.0f - smoothstep(hardness, 1.0f, av / 0.35f);
                return edge_u * edge_v;
            }
            return 0.0f;
        } else if constexpr (Preset == StampPreset::Bristle) {
            const float dist = std::sqrt(ru * ru + rv * rv);
            if (dist > 1.0f) return 0.0f;
            // Simulated 7-fiber bristle cluster
            const float fiber = 0.5f + 0.5f * std::cos(ru * 16.0f) * std::cos(rv * 12.0f);
            const float falloff = std::clamp(1.0f - dist, 0.0f, 1.0f);
            return std::clamp(fiber * falloff * 1.5f, 0.0f, 1.0f);
        } else if constexpr (Preset == StampPreset::Airbrush) {
            const float r2 = ru * ru + rv * rv;
            if (r2 > 1.0f) return 0.0f;
            return std::exp(-3.5f * r2);
        } else {
            // Default Round
            const float dist = std::sqrt(ru * ru + rv * rv);
            if (dist > 1.0f) return 0.0f;
            if (dist <= hardness) return 1.0f;
            if (hardness >= 0.999f) return 1.0f;
            const float t = (dist - hardness) / (1.0f - hardness);
            return 1.0f - (3.0f * t * t - 2.0f * t * t * t);
        }
    }
};

} // namespace kalpana
