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
#include <cstddef>

namespace kalpana::deposit {
    enum class Mode : std::uint8_t {
        Default, // direct opacity overlay
        Watercolor, // edge darkening, pigment granulation, flow into valleys
        Marker, // semi-transparent buildup, saturates quickly
        Pastel, // dry textured deposit, interacts with paper grain
        Oil, // thick opaque impasto with directional smear
        Ink, // high contrast, bleeds at edges
        Pencil // grain-dependent, pressure-sensitive lightness
    };

    // Opacity/translucency axis for body colors (replaces watercolor-only semantics)
    enum class WatercolorBody : std::uint8_t {
        Transparent, // default — layers show through
        Semi, // mid opacity (e.g. semi-opaque watercolor wash)
        Opaque // fully opaque body color (gouache, oils)
    };

    struct DepositionParams {
        Mode mode = Mode::Default;
        float flow = 1.0f; // how much pigment is released per dab
        float buildup_rate = 0.8f; // saturation rate
        float grain_scale = 1.0f; // interaction strength with paper texture
        float edge_darken = 0.0f; // watercolor fringe accumulation coefficient
        WatercolorBody opacity_body = WatercolorBody::Transparent; // NEW: body opacity axis

        // Maps surface state, dab mask, stylus pressure, and paper grain to deposited opacity
        [[nodiscard]] float compute_opacity(
            float existing_coverage, float stamp_alpha,
            float pressure, float grain_value) const noexcept {
            const float dab = stamp_alpha * flow * pressure;
            if (dab <= 0.0f) return existing_coverage;

            switch (mode) {
            case Mode::Watercolor: {
                const float grain_mod = 1.0f + (grain_value - 0.5f) * grain_scale * 0.5f;
                const float fringe = (stamp_alpha > 0.1f && stamp_alpha < 0.85f) ? (1.0f + edge_darken * 0.4f) : 1.0f;
                // Opaque body: floor keeps coverage from dropping (gouache-like)
                const float body_floor = (opacity_body == WatercolorBody::Opaque)
                                             ? 0.85f
                                             : (opacity_body == WatercolorBody::Semi)
                                             ? 0.4f
                                             : 0.0f;
                const float added = dab * grain_mod * fringe;
                const float result = existing_coverage + added * (1.0f - existing_coverage * buildup_rate);
                return std::clamp(std::max(result, stamp_alpha * body_floor), 0.0f, 1.0f);
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

        // deposit_to_field — apply this deposition policy to a single cell of any
        // field type that exposes a .channel(ch).at(row, col) mutable float ref.
        // Writes into pigment channel `km_ch` (KM accumulator) and optionally
        // adjusts height via `height_ch`.  Avoids including PaintField here
        // (would be circular); callers include paint_field.hpp themselves.
        template <typename FieldT>
        void deposit_to_field(
            FieldT& field,
            std::size_t row,
            std::size_t col,
            std::size_t km_ch, // which KM band to write
            std::size_t height_ch, // HEIGHT channel index
            float stamp_alpha, // dab mask value at this cell [0,1]
            float pressure, // stylus pressure [0,1]
            float grain, // paper grain value at cell [0,1]
            float loading, // pigment loading from PigmentImpastoParams
            float impasto_thickness // height contribution
        ) const noexcept {
            float& km = field.channel(km_ch).at(row, col);
            const float new_km = compute_opacity(km, stamp_alpha * loading, pressure, grain);
            km = new_km;

            if (impasto_thickness > 0.0f) {
                float& h = field.channel(height_ch).at(row, col);
                h = std::min(1.0f, h + stamp_alpha * impasto_thickness * pressure);
            }
        }
    };
} // namespace kalpana::deposit
