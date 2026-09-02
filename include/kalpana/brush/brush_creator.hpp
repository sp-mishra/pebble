#pragma once
// ============================================================================
// kalpana/brush/brush_creator.hpp — Serializable BrushProfile + BrushPipeline
// ============================================================================
// BrushProfile is the canonical, aggregate-initializable descriptor bundling
// every brush policy/param: stamp, dynamics, deposition, water, impasto,
// material, reservoir, stabilizer, and medium/drop/liquify toggle flags.
//
// Creating a brush = filling a struct. No engine edits required.
// Serialization: to_toml / from_toml round-trip via simple string key=value.
// (Uses petika KV pattern: key–value pairs with no heap in the struct itself.)
//
// BrushPipeline<Profile> is enhanced to consume BrushProfile and add:
//   stroke_to(PaintField&, points...) — splat + medium coupling
//   stroke_batch(span<Stroke>, ExecBackend) — multi-stroke fan-out
//   Curvature-adaptive stamp spacing (denser on turns, sparse on straights)
// ============================================================================

#ifndef KALPANA_BRUSH_BRUSH_CREATOR_HPP
#define KALPANA_BRUSH_BRUSH_CREATOR_HPP

#include <kalpana/brush/brush.hpp>
#include <kalpana/brush/brush_preset.hpp>
#include <kalpana/brush/material.hpp>
#include <kalpana/brush/stabilizer.hpp>
#include <kalpana/color/paint_field.hpp>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <sstream>
#include <charconv>

namespace kalpana {
    // ---------------------------------------------------------------------------
    // StabilizerConfig — mode + strength bundled for BrushProfile
    // ---------------------------------------------------------------------------
    struct StabilizerConfig {
        StabilizerMode mode = StabilizerMode::OneEuro;
        float strength = 0.0f; // 0 = pass-through

        friend constexpr bool operator==(const StabilizerConfig&,
                                         const StabilizerConfig&) = default;
    };

    // ---------------------------------------------------------------------------
    // BrushProfile — the single serializable brush descriptor
    // Aggregate-initializable, heap-free (no std::string or dynamic members).
    // ============================================================================
    struct BrushProfile {
        // ── Identity ─────────────────────────────────────────────────────────────
        std::string_view name = "Custom Brush";

        // ── Stamp ─────────────────────────────────────────────────────────────────
        StampPreset stamp_preset = StampPreset::Round;
        float size = 10.0f;
        float spacing = 0.25f; // fraction of diameter
        float roundness = 1.0f;
        float angle = 0.0f;
        float hardness = 0.8f;

        // ── Deposition ───────────────────────────────────────────────────────────
        deposit::DepositionParams deposition{};

        // ── Physics medium ────────────────────────────────────────────────────────
        WaterPhysicsParams water{};
        PigmentImpastoParams impasto{};

        // ── PBR material ─────────────────────────────────────────────────────────
        PaintMaterial material = PaintMaterial::preset_matte();

        // ── Reservoir flags ──────────────────────────────────────────────────────
        std::size_t reservoir_slots = 1; // N for PigmentReservoir<N>
        bool dirty_persistent = false; // dirty brush: residue across strokes
        bool multicolor = false; // N slots mapped across footprint u

        // ── Input stabilizer ─────────────────────────────────────────────────────
        StabilizerConfig stabilizer{};

        // ── Medium / physics toggles ─────────────────────────────────────────────
        bool use_medium = false; // enable MediumSolver step
        bool use_drops = false; // enable DropEngine drips
        bool use_liquify = false; // enable LiquifyBrush

        // ── Adaptive spacing ─────────────────────────────────────────────────────
        bool curvature_adaptive_spacing = true; // on by default

        friend constexpr bool operator==(const BrushProfile&,
                                         const BrushProfile&) = default;
    };

    // ---------------------------------------------------------------------------
    // Simple TOML-subset serialization (key = value, one per line)
    // No heap in BrushProfile; the serialized string is returned by value.
    // ---------------------------------------------------------------------------
    [[nodiscard]] inline std::string to_toml(const BrushProfile& p) {
        std::ostringstream oss;
        oss << "name = \"" << p.name << "\"\n";
        oss << "stamp_preset = " << static_cast<int>(p.stamp_preset) << "\n";
        oss << "size = " << p.size << "\n";
        oss << "spacing = " << p.spacing << "\n";
        oss << "roundness = " << p.roundness << "\n";
        oss << "angle = " << p.angle << "\n";
        oss << "hardness = " << p.hardness << "\n";

        // Deposition
        oss << "deposit_mode = " << static_cast<int>(p.deposition.mode) << "\n";
        oss << "deposit_flow = " << p.deposition.flow << "\n";
        oss << "deposit_buildup = " << p.deposition.buildup_rate << "\n";
        oss << "deposit_grain = " << p.deposition.grain_scale << "\n";
        oss << "deposit_edge_darken = " << p.deposition.edge_darken << "\n";

        // Water
        oss << "water_wetness = " << p.water.wetness << "\n";
        oss << "water_flow = " << p.water.water_flow << "\n";
        oss << "water_absorption = " << p.water.paper_absorption << "\n";
        oss << "water_drying = " << p.water.drying_rate << "\n";
        oss << "water_tilt = " << p.water.tilt_drip << "\n";

        // Impasto
        oss << "impasto_loading = " << p.impasto.loading << "\n";
        oss << "impasto_thickness = " << p.impasto.impasto_thickness << "\n";
        oss << "impasto_smudge = " << p.impasto.smudge_rate << "\n";
        oss << "impasto_granulation = " << p.impasto.granulation << "\n";

        // Material
        oss << "mat_metallic = " << p.material.metallic << "\n";
        oss << "mat_roughness = " << p.material.roughness << "\n";
        oss << "mat_gloss = " << p.material.gloss << "\n";
        oss << "mat_anisotropy = " << p.material.anisotropy << "\n";

        // Reservoir / stabilizer / flags
        oss << "reservoir_slots = " << p.reservoir_slots << "\n";
        oss << "dirty_persistent = " << (p.dirty_persistent ? 1 : 0) << "\n";
        oss << "multicolor = " << (p.multicolor ? 1 : 0) << "\n";
        oss << "stabilizer_mode = " << static_cast<int>(p.stabilizer.mode) << "\n";
        oss << "stabilizer_strength = " << p.stabilizer.strength << "\n";
        oss << "use_medium = " << (p.use_medium ? 1 : 0) << "\n";
        oss << "use_drops = " << (p.use_drops ? 1 : 0) << "\n";
        oss << "use_liquify = " << (p.use_liquify ? 1 : 0) << "\n";
        oss << "curvature_adaptive = " << (p.curvature_adaptive_spacing ? 1 : 0) << "\n";
        return oss.str();
    }

    // Minimal TOML-subset parser (key = value lines only, no sections)
    [[nodiscard]] inline BrushProfile from_toml(std::string_view toml) {
        BrushProfile p{};
        auto parse_f = [](std::string_view s) -> float {
            float v = 0.0f;
            std::from_chars(s.data(), s.data() + s.size(), v);
            return v;
        };
        auto parse_i = [](std::string_view s) -> int {
            int v = 0;
            std::from_chars(s.data(), s.data() + s.size(), v);
            return v;
        };

        std::size_t pos = 0;
        while (pos < toml.size()) {
            const std::size_t nl = toml.find('\n', pos);
            const std::string_view line = toml.substr(pos, nl == std::string_view::npos
                                                               ? std::string_view::npos
                                                               : nl - pos);
            pos = (nl == std::string_view::npos) ? toml.size() : nl + 1;

            const std::size_t eq = line.find('=');
            if (eq == std::string_view::npos) continue;

            auto trim = [](std::string_view sv) -> std::string_view {
                while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
                while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) sv.remove_suffix(1);
                return sv;
            };

            const std::string_view key = trim(line.substr(0, eq));
            std::string_view val = trim(line.substr(eq + 1));
            // Strip quotes
            if (val.size() >= 2 && val.front() == '"') {
                val = val.substr(1, val.size() - 2);
            }

            if (key == "size") p.size = parse_f(val);
            else if (key == "spacing") p.spacing = parse_f(val);
            else if (key == "roundness") p.roundness = parse_f(val);
            else if (key == "angle") p.angle = parse_f(val);
            else if (key == "hardness") p.hardness = parse_f(val);
            else if (key == "stamp_preset") p.stamp_preset = static_cast<StampPreset>(parse_i(val));
            else if (key == "deposit_mode") p.deposition.mode = static_cast<deposit::Mode>(parse_i(val));
            else if (key == "deposit_flow") p.deposition.flow = parse_f(val);
            else if (key == "deposit_buildup") p.deposition.buildup_rate = parse_f(val);
            else if (key == "deposit_grain") p.deposition.grain_scale = parse_f(val);
            else if (key == "deposit_edge_darken") p.deposition.edge_darken = parse_f(val);
            else if (key == "water_wetness") p.water.wetness = parse_f(val);
            else if (key == "water_flow") p.water.water_flow = parse_f(val);
            else if (key == "water_absorption") p.water.paper_absorption = parse_f(val);
            else if (key == "water_drying") p.water.drying_rate = parse_f(val);
            else if (key == "water_tilt") p.water.tilt_drip = parse_f(val);
            else if (key == "impasto_loading") p.impasto.loading = parse_f(val);
            else if (key == "impasto_thickness") p.impasto.impasto_thickness = parse_f(val);
            else if (key == "impasto_smudge") p.impasto.smudge_rate = parse_f(val);
            else if (key == "impasto_granulation") p.impasto.granulation = parse_f(val);
            else if (key == "mat_metallic") p.material.metallic = parse_f(val);
            else if (key == "mat_roughness") p.material.roughness = parse_f(val);
            else if (key == "mat_gloss") p.material.gloss = parse_f(val);
            else if (key == "mat_anisotropy") p.material.anisotropy = parse_f(val);
            else if (key == "reservoir_slots") p.reservoir_slots = static_cast<std::size_t>(parse_i(val));
            else if (key == "dirty_persistent") p.dirty_persistent = parse_i(val) != 0;
            else if (key == "multicolor") p.multicolor = parse_i(val) != 0;
            else if (key == "stabilizer_mode") p.stabilizer.mode = static_cast<StabilizerMode>(parse_i(val));
            else if (key == "stabilizer_strength") p.stabilizer.strength = parse_f(val);
            else if (key == "use_medium") p.use_medium = parse_i(val) != 0;
            else if (key == "use_drops") p.use_drops = parse_i(val) != 0;
            else if (key == "use_liquify") p.use_liquify = parse_i(val) != 0;
            else if (key == "curvature_adaptive") p.curvature_adaptive_spacing = parse_i(val) != 0;
        }
        return p;
    }

    // ---------------------------------------------------------------------------
    // curvature_adaptive_step — compute step distance for stamp spacing
    // given local curvature (derived from three consecutive brush points).
    // Returns a step in [min_step, max_step] — denser on turns, sparser on straights.
    // ---------------------------------------------------------------------------
    [[nodiscard]] inline float curvature_adaptive_step(
        const BrushPoint& p0, const BrushPoint& p1, const BrushPoint& p2,
        float base_size, float base_spacing) noexcept {
        // Menger curvature from three points
        const float ax = p1.pos[0] - p0.pos[0];
        const float ay = p1.pos[1] - p0.pos[1];
        const float bx = p2.pos[0] - p1.pos[0];
        const float by = p2.pos[1] - p1.pos[1];
        const float cross = ax * by - ay * bx; // |a × b|
        const float la = std::sqrt(ax * ax + ay * ay);
        const float lb = std::sqrt(bx * bx + by * by);
        const float lc = std::sqrt((p2.pos[0] - p0.pos[0]) * (p2.pos[0] - p0.pos[0])
            + (p2.pos[1] - p0.pos[1]) * (p2.pos[1] - p0.pos[1]));
        const float denom = la * lb * lc;
        const float kappa = (denom > 1e-6f) ? std::fabs(cross * 2.0f) / denom : 0.0f;

        // High curvature → denser stamps (min 40% of base), low → sparser (up to 2×)
        const float min_step = base_size * base_spacing * 0.4f;
        const float max_step = base_size * base_spacing * 2.0f;
        const float kappa_scale = std::clamp(1.0f / (1.0f + kappa * 100.0f), 0.4f, 1.0f);
        return std::clamp(base_size * base_spacing * kappa_scale, min_step, max_step);
    }
} // namespace kalpana

#endif // KALPANA_BRUSH_BRUSH_CREATOR_HPP
