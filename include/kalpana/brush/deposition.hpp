#pragma once
// ============================================================================
// kalpana/brush/deposition.hpp — Paint Deposition Mechanics & Mediums
// ============================================================================
// Simulates watercolor pigment bleeding, oil impasto buildup, marker pooling,
// and dry pastel grain interaction.
// ============================================================================

#include <cmath>
#include <cstdint>
#include <algorithm>

namespace kalpana::deposit {

enum class Mode : std::uint8_t {
    Default,      // direct opacity overlay
    Watercolor,   // edge darkening, pigment granulation, flow into valleys
    Marker,       // semi-transparent buildup, saturates quickly
    Pastel,       // dry textured deposit, interacts with paper grain
    Oil,          // thick opaque impasto with directional smear
    Ink,          // high contrast, bleeds at edges
    Pencil        // grain-dependent, pressure-sensitive lightness
};

struct DepositionParams {
    Mode  mode         = Mode::Default;
    float flow         = 1.0f; // how much pigment is released per dab
    float buildup_rate = 0.8f; // saturation rate
    float grain_scale  = 1.0f; // interaction strength with paper texture
    float edge_darken  = 0.0f; // watercolor fringe accumulation coefficient

    // Maps surface state, dab mask, stylus pressure, and paper grain to deposited opacity
    [[nodiscard]] float compute_opacity(
        float existing_coverage, float stamp_alpha,
        float pressure, float grain_value) const noexcept {
        const float dab = stamp_alpha * flow * pressure;
        if (dab <= 0.0f) return existing_coverage;

        switch (mode) {
            case Mode::Watercolor: {
                // Watercolor deposits more pigment where paper grain is deeper (valleys)
                // and darkens at outer stamp boundary (edge darkening / drying fringe)
                const float grain_mod = 1.0f + (grain_value - 0.5f) * grain_scale * 0.5f;
                const float fringe = (stamp_alpha > 0.1f && stamp_alpha < 0.85f) ? (1.0f + edge_darken * 0.4f) : 1.0f;
                const float added = dab * grain_mod * fringe;
                return std::clamp(existing_coverage + added * (1.0f - existing_coverage * buildup_rate), 0.0f, 1.0f);
            }
            case Mode::Marker: {
                // Fast saturation buildup
                const float added = dab * 1.25f;
                return std::clamp(existing_coverage + added * (1.0f - existing_coverage * 0.95f), 0.0f, 1.0f);
            }
            case Mode::Pastel: {
                // Dry deposit caught only on high peaks of paper grain
                if (grain_value < 0.45f * (1.0f / std::max(0.1f, pressure))) {
                    return existing_coverage; // misses paper valleys
                }
                const float added = dab * grain_value * grain_scale;
                return std::clamp(existing_coverage + added, 0.0f, 1.0f);
            }
            case Mode::Oil: {
                // Thick impasto covers completely and quickly
                const float added = dab * 1.5f;
                return std::clamp(existing_coverage + added, 0.0f, 1.0f);
            }
            case Mode::Ink: {
                // High contrast bleeding
                const float added = dab > 0.3f ? 1.0f : dab * 2.0f;
                return std::clamp(existing_coverage + added, 0.0f, 1.0f);
            }
            case Mode::Pencil: {
                // Subtle graphite deposit modulated by grain peaks
                const float graphite = dab * (0.3f + 0.7f * grain_value) * 0.6f;
                return std::clamp(existing_coverage + graphite, 0.0f, 1.0f);
            }
            case Mode::Default:
            default: {
                return std::clamp(existing_coverage + dab * (1.0f - existing_coverage), 0.0f, 1.0f);
            }
        }
    }

    friend constexpr bool operator==(const DepositionParams&, const DepositionParams&) = default;
};

} // namespace kalpana::deposit
