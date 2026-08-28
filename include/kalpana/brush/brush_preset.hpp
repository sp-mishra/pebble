#pragma once
// ============================================================================
// kalpana/brush/brush_preset.hpp — Rebelle-Inspired Modular Brush Configurations
// ============================================================================
// Physics & mechanics presets: water flow, wetness, paper absorption, drying rate,
// impasto thickness, and tilt drip physics.
// ============================================================================

#include "stamp_shape.hpp"
#include "dynamics.hpp"
#include "deposition.hpp"

namespace kalpana {

// Physical fluid & paper mechanics parameters (Rebelle-inspired)
struct WaterPhysicsParams {
    float wetness          = 0.0f; // [0, 1] liquid content on canvas / brush
    float water_flow       = 0.0f; // [0, 1] diffusion speed into neighbor pixels
    float paper_absorption = 0.5f; // [0, 1] rate at which water is soaked into paper fibers
    float drying_rate      = 0.1f; // [0, 1] evaporation rate per unit time
    float tilt_drip        = 0.0f; // [0, 1] gravity directional bleed when canvas is tilted

    friend constexpr bool operator==(const WaterPhysicsParams&, const WaterPhysicsParams&) = default;
};

// Impasto & pigment density parameters
struct PigmentImpastoParams {
    float loading           = 1.0f; // [0, 1] pigment amount in brush reservoir
    float impasto_thickness = 0.0f; // [0, 1] height field relief for specular lighting
    float smudge_rate       = 0.0f; // [0, 1] pigment pickup and drag from wet canvas
    float granulation       = 0.0f; // [0, 1] sediment accumulation in micro-crevices

    friend constexpr bool operator==(const PigmentImpastoParams&, const PigmentImpastoParams&) = default;
};

// Comprehensive Brush Preset configuration struct
struct BrushPreset {
    std::string_view     name             = "Standard Brush";
    StampPreset          stamp_preset     = StampPreset::Round;
    deposit::Mode        deposit_mode     = deposit::Mode::Default;
    float                default_size     = 16.0f;
    float                spacing          = 0.20f;
    deposit::DepositionParams deposition{};
    WaterPhysicsParams   water{};
    PigmentImpastoParams impasto{};

    // ── Ready-to-Use Factory Presets ────────────────────────────────────────

    static BrushPreset watercolor_wash() noexcept {
        BrushPreset p;
        p.name = "Watercolor Wash";
        p.stamp_preset = StampPreset::Round;
        p.deposit_mode = deposit::Mode::Watercolor;
        p.default_size = 32.0f;
        p.spacing = 0.15f;
        p.deposition = deposit::DepositionParams{
            .mode = deposit::Mode::Watercolor,
            .flow = 0.7f,
            .buildup_rate = 0.6f,
            .grain_scale = 1.2f,
            .edge_darken = 0.8f
        };
        p.water = WaterPhysicsParams{
            .wetness = 0.9f,
            .water_flow = 0.75f,
            .paper_absorption = 0.4f,
            .drying_rate = 0.05f,
            .tilt_drip = 0.3f
        };
        p.impasto = PigmentImpastoParams{
            .loading = 0.8f,
            .impasto_thickness = 0.0f,
            .smudge_rate = 0.4f,
            .granulation = 0.7f
        };
        return p;
    }

    static BrushPreset oil_impasto() noexcept {
        BrushPreset p;
        p.name = "Oil Impasto";
        p.stamp_preset = StampPreset::Bristle;
        p.deposit_mode = deposit::Mode::Oil;
        p.default_size = 24.0f;
        p.spacing = 0.10f;
        p.deposition = deposit::DepositionParams{
            .mode = deposit::Mode::Oil,
            .flow = 1.0f,
            .buildup_rate = 0.95f,
            .grain_scale = 0.4f,
            .edge_darken = 0.0f
        };
        p.water = WaterPhysicsParams{.wetness = 0.0f};
        p.impasto = PigmentImpastoParams{
            .loading = 1.0f,
            .impasto_thickness = 0.85f,
            .smudge_rate = 0.6f,
            .granulation = 0.1f
        };
        return p;
    }

    static BrushPreset ink_pen() noexcept {
        BrushPreset p;
        p.name = "Calligraphy Ink Pen";
        p.stamp_preset = StampPreset::Chisel;
        p.deposit_mode = deposit::Mode::Ink;
        p.default_size = 8.0f;
        p.spacing = 0.08f;
        p.deposition = deposit::DepositionParams{
            .mode = deposit::Mode::Ink,
            .flow = 1.0f,
            .buildup_rate = 1.0f,
            .grain_scale = 0.2f,
            .edge_darken = 0.3f
        };
        p.water = WaterPhysicsParams{.wetness = 0.3f};
        p.impasto = PigmentImpastoParams{.loading = 1.0f};
        return p;
    }

    static BrushPreset dry_pastel() noexcept {
        BrushPreset p;
        p.name = "Dry Pastel Stick";
        p.stamp_preset = StampPreset::Flat;
        p.deposit_mode = deposit::Mode::Pastel;
        p.default_size = 20.0f;
        p.spacing = 0.25f;
        p.deposition = deposit::DepositionParams{
            .mode = deposit::Mode::Pastel,
            .flow = 0.6f,
            .buildup_rate = 0.5f,
            .grain_scale = 1.8f,
            .edge_darken = 0.0f
        };
        p.water = WaterPhysicsParams{.wetness = 0.0f};
        p.impasto = PigmentImpastoParams{.granulation = 0.9f};
        return p;
    }

    static BrushPreset soft_airbrush() noexcept {
        BrushPreset p;
        p.name = "Soft Airbrush";
        p.stamp_preset = StampPreset::Airbrush;
        p.deposit_mode = deposit::Mode::Default;
        p.default_size = 40.0f;
        p.spacing = 0.12f;
        p.deposition = deposit::DepositionParams{
            .mode = deposit::Mode::Default,
            .flow = 0.3f,
            .buildup_rate = 0.4f,
            .grain_scale = 0.0f,
            .edge_darken = 0.0f
        };
        p.water = WaterPhysicsParams{.wetness = 0.0f};
        return p;
    }
};

} // namespace kalpana
