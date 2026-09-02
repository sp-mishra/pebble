#pragma once
// ============================================================================
// kalpana/brush/brush_preset.hpp — Modular Brush Configurations & Presets
// ============================================================================
// Physics & mechanics presets: water flow, wetness, paper absorption, drying,
// impasto thickness, tilt drip physics, and PBR material channel.
// ============================================================================

#include "stamp_shape.hpp"
#include "deposition.hpp"
#include "material.hpp"
#include <string_view>

namespace kalpana {
    // Physical fluid & paper mechanics parameters
    struct WaterPhysicsParams {
        float wetness = 0.0f; // [0, 1] liquid content on canvas / brush
        float water_flow = 0.0f; // [0, 1] diffusion speed into neighbor pixels
        float paper_absorption = 0.5f; // [0, 1] rate at which water is soaked into paper fibers
        float drying_rate = 0.1f; // [0, 1] evaporation rate per unit time
        float tilt_drip = 0.0f; // [0, 1] gravity directional bleed when canvas is tilted

        friend constexpr bool operator==(const WaterPhysicsParams&, const WaterPhysicsParams&) = default;
    };

    // Impasto & pigment density parameters
    struct PigmentImpastoParams {
        float loading = 1.0f; // [0, 1] pigment amount in brush reservoir
        float impasto_thickness = 0.0f; // [0, 1] height field relief for specular lighting
        float smudge_rate = 0.0f; // [0, 1] pigment pickup and drag from wet canvas
        float granulation = 0.0f; // [0, 1] sediment accumulation in micro-crevices

        friend constexpr bool operator==(const PigmentImpastoParams&, const PigmentImpastoParams&) = default;
    };

    // Comprehensive Brush Preset configuration struct
    struct BrushPreset {
        std::string_view name = "Standard Brush";
        StampPreset stamp_preset = StampPreset::Round;
        deposit::Mode deposit_mode = deposit::Mode::Default;
        float default_size = 16.0f;
        float spacing = 0.20f;
        deposit::DepositionParams deposition{};
        WaterPhysicsParams water{};
        PigmentImpastoParams impasto{};
        PaintMaterial material = PaintMaterial::preset_matte(); // NEW: PBR material

        // ── Ready-to-Use Factory Presets (original 5, unchanged) ────────────────

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
            p.material = PaintMaterial::preset_watercolor();
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
            p.material = PaintMaterial::preset_glossy_oil();
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
            p.material = PaintMaterial::preset_matte();
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
            p.material = PaintMaterial::preset_matte();
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
            p.material = PaintMaterial::preset_matte();
            return p;
        }

        // ── New presets (Kalpana Next — do not modify existing 5 above) ─────────

        static BrushPreset graphite_pencil() noexcept {
            BrushPreset p;
            p.name = "Graphite Pencil";
            p.stamp_preset = StampPreset::Flat;
            p.deposit_mode = deposit::Mode::Pencil;
            p.default_size = 6.0f;
            p.spacing = 0.12f;
            p.deposition = deposit::DepositionParams{
                .mode = deposit::Mode::Pencil,
                .flow = 0.5f,
                .buildup_rate = 0.4f,
                .grain_scale = 2.0f, // strong grain interaction
                .edge_darken = 0.0f,
                .opacity_body = deposit::WatercolorBody::Transparent
            };
            p.water = WaterPhysicsParams{.wetness = 0.0f};
            p.impasto = PigmentImpastoParams{
                .loading = 0.4f,
                .impasto_thickness = 0.0f,
                .smudge_rate = 0.1f,
                .granulation = 0.1f
            };
            p.material = PaintMaterial::preset_pencil();
            return p;
        }

        static BrushPreset metallic_paint() noexcept {
            BrushPreset p;
            p.name = "Metallic Paint";
            p.stamp_preset = StampPreset::Bristle;
            p.deposit_mode = deposit::Mode::Oil;
            p.default_size = 20.0f;
            p.spacing = 0.10f;
            p.deposition = deposit::DepositionParams{
                .mode = deposit::Mode::Oil,
                .flow = 1.0f,
                .buildup_rate = 0.9f,
                .grain_scale = 0.1f,
                .edge_darken = 0.0f,
                .opacity_body = deposit::WatercolorBody::Opaque
            };
            p.water = WaterPhysicsParams{.wetness = 0.0f};
            p.impasto = PigmentImpastoParams{
                .loading = 1.0f,
                .impasto_thickness = 0.7f, // high relief → strong specular
                .smudge_rate = 0.3f,
                .granulation = 0.0f
            };
            p.material = PaintMaterial::preset_metallic();
            return p;
        }

        static BrushPreset rough_feather() noexcept {
            BrushPreset p;
            p.name = "Rough Feather";
            p.stamp_preset = StampPreset::Airbrush;
            p.deposit_mode = deposit::Mode::Default;
            p.default_size = 28.0f;
            p.spacing = 0.20f;
            p.deposition = deposit::DepositionParams{
                .mode = deposit::Mode::Default,
                .flow = 0.4f,
                .buildup_rate = 0.35f,
                .grain_scale = 1.5f,
                .edge_darken = 0.1f,
                .opacity_body = deposit::WatercolorBody::Transparent
            };
            p.water = WaterPhysicsParams{.wetness = 0.1f};
            p.impasto = PigmentImpastoParams{
                .loading = 0.5f,
                .impasto_thickness = 0.0f,
                .smudge_rate = 0.0f,
                .granulation = 0.4f
            };
            p.material = PaintMaterial::preset_feather();
            return p;
        }

        static BrushPreset express_oils() noexcept {
            BrushPreset p;
            p.name = "Express Oils";
            p.stamp_preset = StampPreset::Flat;
            p.deposit_mode = deposit::Mode::Oil;
            p.default_size = 22.0f;
            p.spacing = 0.30f; // wide spacing for speed (swept dab, §9)
            p.deposition = deposit::DepositionParams{
                .mode = deposit::Mode::Oil,
                .flow = 1.0f,
                .buildup_rate = 1.0f,
                .grain_scale = 0.2f,
                .edge_darken = 0.0f,
                .opacity_body = deposit::WatercolorBody::Opaque
            };
            p.water = WaterPhysicsParams{.wetness = 0.0f};
            p.impasto = PigmentImpastoParams{
                .loading = 1.0f,
                .impasto_thickness = 0.4f,
                .smudge_rate = 0.7f, // dirty reservoir on (§4b)
                .granulation = 0.05f
            };
            p.material = PaintMaterial::preset_glossy_oil();
            return p;
        }

        static BrushPreset gouache() noexcept {
            BrushPreset p;
            p.name = "Gouache";
            p.stamp_preset = StampPreset::Round;
            p.deposit_mode = deposit::Mode::Watercolor;
            p.default_size = 18.0f;
            p.spacing = 0.18f;
            p.deposition = deposit::DepositionParams{
                .mode = deposit::Mode::Watercolor,
                .flow = 0.9f,
                .buildup_rate = 0.85f,
                .grain_scale = 0.5f,
                .edge_darken = 0.1f,
                .opacity_body = deposit::WatercolorBody::Opaque // opaque body color
            };
            p.water = WaterPhysicsParams{
                .wetness = 0.5f,
                .water_flow = 0.3f,
                .paper_absorption = 0.6f,
                .drying_rate = 0.2f, // fast dry
                .tilt_drip = 0.0f
            };
            p.impasto = PigmentImpastoParams{
                .loading = 0.95f,
                .impasto_thickness = 0.05f,
                .smudge_rate = 0.1f,
                .granulation = 0.2f
            };
            p.material = PaintMaterial::preset_gouache();
            return p;
        }
    };
} // namespace kalpana
