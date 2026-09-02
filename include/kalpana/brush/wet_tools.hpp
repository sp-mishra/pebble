#pragma once
// ============================================================================
// kalpana/brush/wet_tools.hpp — Wet / Dry / Blow Canvas Tools
// ============================================================================
// Three stateless functors over PaintField channels (not brushes that deposit
// pigment) for water-only manipulation of wet natural-media canvases.
//
//   Wet  — add clean water to re-activate dried pigment
//   Dry  — remove water, freeze pigment (blot / hair dryer)
//   Blow — directional air impulse that pushes wet pigment via semi-Lagrangian
//           advect on the next MediumSolver step
//
// Zero cost unless used.  Compose with MediumSolver::step().
// ============================================================================

#ifndef KALPANA_BRUSH_WET_TOOLS_HPP
#define KALPANA_BRUSH_WET_TOOLS_HPP

#include <kalpana/color/paint_field.hpp>
#include <kalpana/brush/stamp_shape.hpp>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace kalpana {
    // ---------------------------------------------------------------------------
    // WetTool — add clean water within a stamp footprint
    // Re-mobilizes fixed pigment (clears "dry" state) over the dab area.
    // ---------------------------------------------------------------------------
    struct WetTool {
        float flow = 0.6f; // water added per step [0,1]
        float radius = 10.0f; // dab radius (field pixels)

        template <
            typename SP = ts::DefaultStoragePolicy,
            typename CP = ts::DefaultComputationPolicy,
            StampPreset Preset = StampPreset::Airbrush
        >
        void apply(PaintField<SP, CP>& field,
                   float cx, float cy,
                   float pressure = 1.0f) const {
            if (!field.valid()) return;
            const float r = radius / field.spacing();
            const int row_lo = std::max(0, static_cast<int>(std::floor(cy / field.spacing() - r)));
            const int row_hi = std::min(static_cast<int>(field.rows()) - 1,
                                        static_cast<int>(std::ceil(cy / field.spacing() + r)));
            const int col_lo = std::max(0, static_cast<int>(std::floor(cx / field.spacing() - r)));
            const int col_hi = std::min(static_cast<int>(field.cols()) - 1,
                                        static_cast<int>(std::ceil(cx / field.spacing() + r)));

            StampShape < Preset > shape{.hardness = 0.3f};
            const float inv_r = (r > 1e-6f) ? 1.0f / r : 1.0f;

            for (int ri = row_lo; ri <= row_hi; ++ri) {
                for (int ci = col_lo; ci <= col_hi; ++ci) {
                    const float u = (static_cast<float>(ci) - cx / field.spacing()) * inv_r;
                    const float v = (static_cast<float>(ri) - cy / field.spacing()) * inv_r;
                    const float alpha = shape.sample(u, v) * pressure;
                    if (alpha < 1e-6f) continue;
                    auto& w = field.field().channel(PaintChannels::WATER).at(ri, ci);
                    w = std::clamp(w + flow * alpha, 0.0f, 1.0f);
                }
            }
        }
    };

    // ---------------------------------------------------------------------------
    // DryTool — remove water (blot / hair-dryer)
    // Multiplies water by (1 - blot); above-threshold pigment fixes.
    // ---------------------------------------------------------------------------
    struct DryTool {
        float blot = 0.8f; // fraction of water removed per step [0,1]
        float radius = 12.0f;

        template <
            typename SP = ts::DefaultStoragePolicy,
            typename CP = ts::DefaultComputationPolicy,
            StampPreset Preset = StampPreset::Flat
        >
        void apply(PaintField<SP, CP>& field,
                   float cx, float cy,
                   float pressure = 1.0f) const {
            if (!field.valid()) return;
            const float r = radius / field.spacing();
            const int row_lo = std::max(0, static_cast<int>(std::floor(cy / field.spacing() - r)));
            const int row_hi = std::min(static_cast<int>(field.rows()) - 1,
                                        static_cast<int>(std::ceil(cy / field.spacing() + r)));
            const int col_lo = std::max(0, static_cast<int>(std::floor(cx / field.spacing() - r)));
            const int col_hi = std::min(static_cast<int>(field.cols()) - 1,
                                        static_cast<int>(std::ceil(cx / field.spacing() + r)));

            StampShape < Preset > shape{.hardness = 0.9f};
            const float inv_r = (r > 1e-6f) ? 1.0f / r : 1.0f;
            const float remove_factor = blot * pressure;

            for (int ri = row_lo; ri <= row_hi; ++ri) {
                for (int ci = col_lo; ci <= col_hi; ++ci) {
                    const float u = (static_cast<float>(ci) - cx / field.spacing()) * inv_r;
                    const float v = (static_cast<float>(ri) - cy / field.spacing()) * inv_r;
                    const float alpha = shape.sample(u, v);
                    if (alpha < 1e-6f) continue;
                    auto& w = field.field().channel(PaintChannels::WATER).at(ri, ci);
                    w = std::max(0.0f, w * (1.0f - remove_factor * alpha));
                    // Fix sediment when water crosses drying threshold
                    if (w < 0.05f) {
                        // Pigment already fixed by MediumSolver drying step;
                        // here we accelerate sediment settlement
                        auto& sed = field.field().channel(PaintChannels::SEDIMENT).at(ri, ci);
                        float pigment_total = 0.0f;
                        for (std::size_t b = 0; b < spectral::kBands; ++b)
                            pigment_total += field.field().channel(PaintChannels::KM_START + b).at(ri, ci);
                        sed += pigment_total * 0.05f;
                    }
                }
            }
        }
    };

    // ---------------------------------------------------------------------------
    // BlowTool — directional air impulse pushing wet pigment
    // Injects a velocity impulse (dir * strength) into field's velocity channels
    // so the next MediumSolver::step() advects wet pigment along dir.
    // Reuses MediumSolver semi-Lagrangian advect — no new solver code.
    // ---------------------------------------------------------------------------
    struct BlowTool {
        float strength = 1.0f; // impulse magnitude (field-pixels/s)
        float radius = 20.0f; // footprint radius

        template <
            typename SP = ts::DefaultStoragePolicy,
            typename CP = ts::DefaultComputationPolicy>
        void apply(PaintField<SP, CP>& field,
                   float cx, float cy,
                   float dir_x, float dir_y, // direction (will be normalized)
                   float pressure = 1.0f) const {
            if (!field.valid()) return;

            // Normalize direction
            const float dlen = std::sqrt(dir_x * dir_x + dir_y * dir_y);
            const float ndx = (dlen > 1e-6f) ? dir_x / dlen : 0.0f;
            const float ndy = (dlen > 1e-6f) ? dir_y / dlen : 0.0f;
            const float mag = strength * pressure;

            const float r = radius / field.spacing();
            const int row_lo = std::max(0, static_cast<int>(std::floor(cy / field.spacing() - r)));
            const int row_hi = std::min(static_cast<int>(field.rows()) - 1,
                                        static_cast<int>(std::ceil(cy / field.spacing() + r)));
            const int col_lo = std::max(0, static_cast<int>(std::floor(cx / field.spacing() - r)));
            const int col_hi = std::min(static_cast<int>(field.cols()) - 1,
                                        static_cast<int>(std::ceil(cx / field.spacing() + r)));

            StampShape<StampPreset::Airbrush> shape{.hardness = 0.2f};
            const float inv_r = (r > 1e-6f) ? 1.0f / r : 1.0f;

            // Write velocity impulse into PaintField's velocity channels
            for (int ri = row_lo; ri <= row_hi; ++ri) {
                for (int ci = col_lo; ci <= col_hi; ++ci) {
                    const float u = (static_cast<float>(ci) - cx / field.spacing()) * inv_r;
                    const float v = (static_cast<float>(ri) - cy / field.spacing()) * inv_r;
                    const float alpha = shape.sample(u, v);
                    if (alpha < 1e-6f) continue;

                    const float water = field.field().channel(PaintChannels::WATER).at(ri, ci);
                    if (water < 0.05f) continue; // only blow wet areas

                    // Additive impulse (clamped to prevent runaway)
                    auto& vx = field.vel().channel(VelocityChannels::VX).at(ri, ci);
                    auto& vy = field.vel().channel(VelocityChannels::VY).at(ri, ci);
                    vx = std::clamp(vx + ndx * mag * alpha, -50.0f, 50.0f);
                    vy = std::clamp(vy + ndy * mag * alpha, -50.0f, 50.0f);
                }
            }
        }
    };
} // namespace kalpana

#endif // KALPANA_BRUSH_WET_TOOLS_HPP
