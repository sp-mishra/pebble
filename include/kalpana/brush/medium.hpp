#pragma once
// ============================================================================
// kalpana/brush/medium.hpp — Natural-Media Simulation: MediumSolver + Reservoir
// ============================================================================
// MediumSolver steps a PaintField per frame by calling ga::stencil / ga::iterative
// ops — kalpana writes no stencil code; it only wires coefficients from
// WaterPhysicsParams into the matrix-library grid primitives.
//
// Steps per frame (operator-split):
//   1. Reaction-diffusion of pigment weighted by water (ga::jacobi_diffuse)
//   2. Tilt-gravity advection of pigment (ga::advect_semilagrangian)
//   3. Incompressible water spread — pressure projection via ga::jacobi_pressure
//   4. Drying decay: water *= exp(-drying_rate * dt)
//   5. Granulation: sediment ∝ pigment * granulation * (paper grain < threshold)
//   6. Edge darkening: sediment accumulates at drying front (∇water sign change)
//
// PigmentReservoir<N> — N-slot bidirectional pigment pickup/deposit buffer.
//   N=1 (default) = single pigment slot (§4 design).
//   Dirty persistence and multicolor mapping are opt-in profile flags (§4b).
// ============================================================================

#ifndef KALPANA_BRUSH_MEDIUM_HPP
#define KALPANA_BRUSH_MEDIUM_HPP

#include <kalpana/color/paint_field.hpp>
#include <kalpana/color/spectral.hpp>
#include <kalpana/brush/brush_preset.hpp>
#include <containers/matrix/stencil.hpp>
#include <cmath>
#include <algorithm>
#include <array>
#include <cstddef>

namespace kalpana {
    // ---------------------------------------------------------------------------
    // MediumSolver — value type, stepped by the caller (or a gati system)
    // Pay-only-if-used: a dry/opaque brush instantiates none.
    // ---------------------------------------------------------------------------
    template <
        typename SP = ts::DefaultStoragePolicy,
        typename CP = ts::DefaultComputationPolicy>
    class MediumSolver {
    public:
        using field_t = PaintField<SP, CP>;

        // Null / disabled state — no-cost default
        MediumSolver() = default;

        explicit MediumSolver(WaterPhysicsParams params) noexcept
            : params_(params) {}

        MediumSolver(MediumSolver&&) noexcept = default;
        MediumSolver& operator=(MediumSolver&&) noexcept = default;
        MediumSolver(const MediumSolver&) = delete;
        MediumSolver& operator=(const MediumSolver&) = delete;

        [[nodiscard]] const WaterPhysicsParams& params() const noexcept { return params_; }
        void set_params(WaterPhysicsParams p) noexcept { params_ = p; }

        // -----------------------------------------------------------------------
        // step(field, dt, tilt_x, tilt_y, granulation) — advance one frame
        //
        // tilt_x/tilt_y: gravity direction vector (canvas tilt) in field-space units/s.
        // granulation: per-stroke granulation coefficient [0,1] for sediment settle.
        //
        // All physics ops delegate to ga::stencil on field.field() channels.
        // No stencil code is implemented here.
        // -----------------------------------------------------------------------
        void step(field_t& field, float dt,
                  float tilt_x = 0.0f, float tilt_y = 0.0f,
                  float granulation = 0.0f) const {
            if (!field.valid() || dt <= 0.0f) return;

            const float D_w = params_.water_flow;
            const float drying_k = params_.drying_rate;
            const float absorption = params_.paper_absorption;
            const float tilt_scale = params_.tilt_drip;

            auto& ga_field = field.field();
            auto& vel = field.vel();
            const std::size_t rows = field.rows();
            const std::size_t cols = field.cols();

            // ── 1. Pigment diffusion weighted by water content ──────────────────
            // D_pigment = D_w * water_at_cell; approximated as uniform D_w * avg_wet.
            // Full cell-wise weighting would require per-cell stencil — use uniform
            // approximation consistent with the continuum model.
            if (D_w > 0.0f) {
                for (std::size_t b = 0; b < spectral::kBands; ++b) {
                    ga_field.diffuse(PaintChannels::KM_START + b,
                                     D_w,
                                     dt,
                                     /*n_sweeps=*/4);
                }
            }

            // ── 2. Tilt-gravity advection of pigment + water ─────────────────────
            // Inject tilt velocity into the velocity channels, then advect.
            if (tilt_scale > 0.0f && (std::fabs(tilt_x) > 1e-6f || std::fabs(tilt_y) > 1e-6f)) {
                const float vx = tilt_x * tilt_scale;
                const float vy = tilt_y * tilt_scale;

                // Fill velocity channels (uniform tilt field)
                for (std::size_t ri = 0; ri < rows; ++ri) {
                    for (std::size_t ci = 0; ci < cols; ++ci) {
                        vel.channel(VelocityChannels::VX).at(ri, ci) = vx;
                        vel.channel(VelocityChannels::VY).at(ri, ci) = vy;
                    }
                }

                // Advect pigment channels via semi-Lagrangian (Stam stable fluids)
                // We use ga_field's advect method, passing VX/VY of the vel field.
                // Since advect() operates on ga_field's own velocity channels, we
                // copy tilt velocity into ga_field then advect, then restore.
                // Alternatively, call stencil directly per channel:
                auto& vx_grid = vel.channel(VelocityChannels::VX);
                auto& vy_grid = vel.channel(VelocityChannels::VY);

                for (std::size_t b = 0; b < spectral::kBands; ++b) {
                    ga_field.channel(PaintChannels::KM_START + b) =
                        ga::advect_semilagrangian<float, SP, CP, ga::NeumannBC>(
                            ga_field.channel(PaintChannels::KM_START + b),
                            vx_grid, vy_grid, dt);
                }
                // Also advect water
                ga_field.channel(PaintChannels::WATER) =
                    ga::advect_semilagrangian<float, SP, CP, ga::NeumannBC>(
                        ga_field.channel(PaintChannels::WATER),
                        vx_grid, vy_grid, dt);
            }

            // ── 3. Incompressible water spread (pressure projection) ─────────────
            // Only if wetness significant enough to pool
            if (D_w > 0.0f && params_.wetness > 0.1f) {
                // Pressure projection on water channel: treat water as scalar velocity proxy.
                // Full 2D pressure solve requires velocity field; we use Jacobi diffusion
                // on water as the pooling approximation (Laplacian smoothing → level-set).
                ga_field.diffuse(PaintChannels::WATER, absorption, dt, /*sweeps=*/2);
            }

            // ── 4. Drying decay: water *= exp(-drying_rate * dt) ─────────────────
            if (drying_k > 0.0f) {
                const float decay = std::exp(-drying_k * dt);
                for (std::size_t ri = 0; ri < rows; ++ri)
                    for (std::size_t ci = 0; ci < cols; ++ci)
                        ga_field.channel(PaintChannels::WATER).at(ri, ci) *= decay;
            }

            // ── 5. Granulation: sediment settles proportional to pigment mass ─────
            if (granulation > 0.0f) {
                for (std::size_t ri = 0; ri < rows; ++ri) {
                    for (std::size_t ci = 0; ci < cols; ++ci) {
                        // Sediment settles where water is drying (water < 0.3)
                        const float water = ga_field.channel(PaintChannels::WATER).at(ri, ci);
                        if (water < 0.3f) {
                            float pigment_total = 0.0f;
                            for (std::size_t b = 0; b < spectral::kBands; ++b)
                                pigment_total += ga_field.channel(PaintChannels::KM_START + b).at(ri, ci);
                            const float settle = pigment_total * granulation * dt * 0.1f;
                            ga_field.channel(PaintChannels::SEDIMENT).at(ri, ci) += settle;
                        }
                    }
                }
            }

            // ── 6. Edge darkening — capillary backrun at drying front ────────────
            // Sediment accumulates where ∇water changes sign (outer drying fringe).
            // Approximate: cells where water is between 0.05 and 0.25 (transition zone).
            {
                auto [gx, gy] = ga_field.template grad_ch<ga::NeumannBC>(PaintChannels::WATER);
                for (std::size_t ri = 0; ri < rows; ++ri) {
                    for (std::size_t ci = 0; ci < cols; ++ci) {
                        const float water = ga_field.channel(PaintChannels::WATER).at(ri, ci);
                        const float grad_mag = std::sqrt(gx.at(ri, ci) * gx.at(ri, ci)
                            + gy.at(ri, ci) * gy.at(ri, ci));
                        // Drying front: high gradient, low water
                        if (water > 0.05f && water < 0.25f && grad_mag > 0.05f) {
                            float pigment_total = 0.0f;
                            for (std::size_t b = 0; b < spectral::kBands; ++b)
                                pigment_total += ga_field.channel(PaintChannels::KM_START + b).at(ri, ci);
                            ga_field.channel(PaintChannels::SEDIMENT).at(ri, ci) +=
                                pigment_total * grad_mag * dt * 0.05f;
                        }
                    }
                }
            }
        }

        [[nodiscard]] bool active() const noexcept {
            return params_.wetness > 0.0f || params_.water_flow > 0.0f;
        }

    private:
        WaterPhysicsParams params_{};
    };

    // ---------------------------------------------------------------------------
    // PigmentReservoir<N> — N-slot bidirectional pigment pickup/deposit buffer
    //
    // N=1 (default): single slot, equivalent to original smudge_sample behavior.
    // Dirty persistence: reservoir not reset between strokes; decays by clean_rate.
    // Multicolor: N slots mapped across stamp footprint u-coordinate.
    // ---------------------------------------------------------------------------
    template <std::size_t N = 1>
    struct PigmentReservoir {
        static_assert(N >= 1 && N <= 16, "Reservoir slot count must be 1..16");

        struct Slot {
            spectral::SpectralColor pigment{};
            float volume = 0.0f; // [0,1] loaded pigment mass
        };

        std::array<Slot, N> slots{};
        bool dirty_enabled = false; // persist residue across strokes
        float clean_rate = 1.0f; // fraction cleaned per explicit rinse [0,1]

        // -----------------------------------------------------------------------
        // pickup — draw pigment from field cell into reservoir slot
        // Returns the mixed color after pickup.
        // -----------------------------------------------------------------------
        [[nodiscard]] spectral::SpectralColor pickup(
            std::size_t slot_idx,
            const spectral::SpectralColor& field_pigment,
            float field_water,
            float pickup_rate) noexcept {
            auto& s = slots[slot_idx % N];
            const float rate = std::clamp(pickup_rate * field_water, 0.0f, 1.0f);
            if (rate < 1e-6f) return s.pigment;
            s.pigment = s.pigment.mix_km(field_pigment, rate);
            s.volume = std::clamp(s.volume + rate * 0.5f, 0.0f, 1.0f);
            return s.pigment;
        }

        // -----------------------------------------------------------------------
        // deposit — blend reservoir pigment into field cell, return deposited color
        // -----------------------------------------------------------------------
        [[nodiscard]] spectral::SpectralColor deposit(
            std::size_t slot_idx,
            const spectral::SpectralColor& field_pigment,
            float deposit_strength) noexcept {
            const auto& s = slots[slot_idx % N];
            if (s.volume < 1e-6f) return field_pigment;
            const float d = std::clamp(deposit_strength * s.volume, 0.0f, 1.0f);
            return field_pigment.mix_km(s.pigment, d);
        }

        // -----------------------------------------------------------------------
        // slot_for_u — map stamp u-coordinate [0,1] to slot index for multicolor
        // -----------------------------------------------------------------------
        [[nodiscard]] constexpr std::size_t slot_for_u(float u) const noexcept {
            if constexpr (N == 1) return 0;
            const std::size_t idx = static_cast<std::size_t>(std::clamp(u, 0.0f, 1.0f) * float(N));
            return std::min(idx, N - 1);
        }

        // -----------------------------------------------------------------------
        // rinse — clear reservoir (dirty brush cleanup)
        // -----------------------------------------------------------------------
        void rinse() noexcept {
            for (auto& s : slots) {
                s.volume *= (1.0f - clean_rate);
                if (s.volume < 1e-4f) {
                    s.volume = 0.0f;
                    s.pigment = {};
                }
            }
        }

        // Reset fully (between strokes when dirty_enabled=false)
        void reset() noexcept {
            if (!dirty_enabled) {
                for (auto& s : slots) { s = {}; }
            }
        }
    };

    // Default single-slot reservoir alias
    using DefaultReservoir = PigmentReservoir<1>;

    // Default solver alias
    using DefaultMediumSolver = MediumSolver<>;
} // namespace kalpana

#endif // KALPANA_BRUSH_MEDIUM_HPP
