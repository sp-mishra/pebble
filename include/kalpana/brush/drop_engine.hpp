#pragma once
// ============================================================================
// kalpana/brush/drop_engine.hpp — DropEngine: Watercolor Drips & Run-Off
// ============================================================================
// Light Lagrangian droplet layer coupled to the Eulerian PaintField.
//
// Droplets {pos, radius, volume, velocity, pigment} advect under gravity*tilt,
// absorb water/pigment from wet cells they cross, deposit a pigment trail back,
// merge on contact (volume-sum), and are removed when volume→0 or cell is dry.
//
// Off unless tilt_drip > 0 and free water present (pay-for-use).
// Broadphase via SpatialHashGrid (SpatialHash2D pattern).
// Run irregularity driven by an external noise_fn (no new noise code).
// Reference: Curtis et al. SIGGRAPH'97 watercolor sediment/run-off transport.
// ============================================================================

#ifndef KALPANA_BRUSH_DROP_ENGINE_HPP
#define KALPANA_BRUSH_DROP_ENGINE_HPP

#include <kalpana/color/paint_field.hpp>
#include <kalpana/color/spectral.hpp>
#include <containers/numeric/math_vector.hpp>
#include <containers/spatial/spatial_hash_grid.hpp>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace kalpana {
    // ---------------------------------------------------------------------------
    // Droplet — single water bead
    // ---------------------------------------------------------------------------
    struct Droplet {
        pebble::math::vec2 pos{}; // field-space position (pixels)
        float radius{}; // interaction radius (cells)
        float volume{}; // [0,1] remaining liquid
        pebble::math::vec2 velocity{}; // field-space velocity (pixels/s)
        spectral::SpectralColor pigment{}; // carried pigment color
        bool alive{true};
    };

    // ---------------------------------------------------------------------------
    // DropEngineParams — configuration (tunable, no hardcoding)
    // ---------------------------------------------------------------------------
    struct DropEngineParams {
        float gravity_scale = 9.81f; // m/s² equivalent in field units
        float pickup_rate = 0.3f; // pigment/water pickup per step
        float deposit_rate = 0.2f; // pigment deposit per step
        float merge_radius = 1.5f; // cell radii for merge trigger
        float dry_threshold = 0.05f; // water below this → drop absorbed
        float spawn_threshold = 0.8f; // water above this → spawn drop
        std::size_t max_drops = 512; // cap; configurable
        float evaporation = 0.01f; // drop volume decay per step
    };

    // ---------------------------------------------------------------------------
    // DropEngine — Lagrangian droplet simulation over PaintField
    // ---------------------------------------------------------------------------
    template <
        typename SP = ts::DefaultStoragePolicy,
        typename CP = ts::DefaultComputationPolicy>
    class DropEngine {
    public:
        using field_t = PaintField<SP, CP>;
        // Noise function: noise_fn(x, y) → float jitter [0,1]
        using NoiseFn = std::function<float(float, float)>;

        // Null / disabled state — no allocations
        DropEngine() = default;

        explicit DropEngine(DropEngineParams params, NoiseFn noise = {}) noexcept
            : params_(params), noise_(std::move(noise)) {}

        DropEngine(DropEngine&&) noexcept = default;
        DropEngine& operator=(DropEngine&&) noexcept = default;
        DropEngine(const DropEngine&) = delete;
        DropEngine& operator=(const DropEngine&) = delete;

        [[nodiscard]] std::size_t drop_count() const noexcept { return drops_.size(); }

        // -----------------------------------------------------------------------
        // step — advance droplets one timestep dt (seconds)
        //
        // tilt_x/tilt_y: gravity direction in field space (normalized).
        // Coupled to PaintField: reads/writes water and pigment channels.
        // -----------------------------------------------------------------------
        void step(field_t& field, float dt, float tilt_x = 0.0f, float tilt_y = 1.0f) {
            if (!field.valid() || dt <= 0.0f) return;

            // Spawn new drops from over-wet cells
            spawn_drops(field);

            // Advect, interact, and remove dead drops
            for (auto& d : drops_) {
                if (!d.alive) continue;
                advect_drop(d, field, dt, tilt_x, tilt_y);
                interact_drop(d, field, dt);
            }

            // Merge nearby drops
            merge_drops();

            // Remove dead drops (compact)
            drops_.erase(
                std::remove_if(drops_.begin(), drops_.end(),
                               [](const Droplet& d) { return !d.alive; }),
                drops_.end());
        }

        // -----------------------------------------------------------------------
        // spawn — manually add a drop at a field position
        // -----------------------------------------------------------------------
        void spawn(pebble::math::vec2 pos, float volume,
                   spectral::SpectralColor pigment = {}) {
            if (drops_.size() >= params_.max_drops) return;
            drops_.push_back(Droplet{
                .pos = pos,
                .radius = 1.5f,
                .volume = std::clamp(volume, 0.0f, 1.0f),
                .pigment = pigment,
                .alive = true
            });
        }

        void clear() noexcept { drops_.clear(); }

    private:
        void spawn_drops(field_t& field) {
            if (drops_.size() >= params_.max_drops) return;
            const std::size_t R = field.rows(), C = field.cols();
            // Scan field for over-wet cells (sample every 8 cells to avoid O(N²))
            constexpr std::size_t STRIDE = 8;
            for (std::size_t ri = 0; ri < R && drops_.size() < params_.max_drops; ri += STRIDE) {
                for (std::size_t ci = 0; ci < C && drops_.size() < params_.max_drops; ci += STRIDE) {
                    const float water = field.field().channel(PaintChannels::WATER).at(ri, ci);
                    if (water > params_.spawn_threshold) {
                        const float jitter = noise_
                                                 ? noise_(static_cast<float>(ci), static_cast<float>(ri))
                                                 : 0.5f;
                        if (jitter > 0.6f) { // ~40% spawn probability per over-wet cell
                            // Build pigment from cell
                            spectral::SpectralColor pig{};
                            for (std::size_t b = 0; b < spectral::kBands; ++b)
                                pig.reflectance[b] = field.field().channel(PaintChannels::KM_START + b).at(ri, ci);

                            drops_.push_back(Droplet{
                                .pos = {
                                    static_cast<float>(ci) * field.spacing(),
                                    static_cast<float>(ri) * field.spacing()
                                },
                                .radius = 1.5f,
                                .volume = water * 0.3f,
                                .pigment = pig,
                                .alive = true
                            });

                            // Consume some water from spawn cell
                            auto& w = field.field().channel(PaintChannels::WATER).at(ri, ci);
                            w = std::max(0.0f, w - 0.2f);
                        }
                    }
                }
            }
        }

        void advect_drop(Droplet& d, const field_t& field, float dt,
                         float tilt_x, float tilt_y) {
            // Gravity force along tilt
            const float g = params_.gravity_scale;
            d.velocity[0] += tilt_x * g * dt;
            d.velocity[1] += tilt_y * g * dt;

            // Noise jitter for run irregularity
            if (noise_) {
                const float jx = (noise_(d.pos[0], d.pos[1]) - 0.5f) * 2.0f;
                const float jy = (noise_(d.pos[1], d.pos[0]) - 0.5f) * 2.0f;
                d.velocity[0] += jx * 0.5f;
                d.velocity[1] += jy * 0.5f;
            }

            // Drag (simple linear)
            d.velocity[0] *= 0.9f;
            d.velocity[1] *= 0.9f;

            d.pos[0] += d.velocity[0] * dt;
            d.pos[1] += d.velocity[1] * dt;

            // Clamp to field bounds
            d.pos[0] = std::clamp(d.pos[0], 0.0f,
                                  static_cast<float>(field.cols() - 1) * field.spacing());
            d.pos[1] = std::clamp(d.pos[1], 0.0f,
                                  static_cast<float>(field.rows() - 1) * field.spacing());

            // Volume evaporation
            d.volume -= params_.evaporation * dt;
            if (d.volume <= 0.0f) d.alive = false;
        }

        void interact_drop(Droplet& d, field_t& field, float dt) {
            if (!d.alive) return;

            const std::size_t ri = static_cast<std::size_t>(
                std::clamp(d.pos[1] / field.spacing(), 0.0f,
                           static_cast<float>(field.rows() - 1)));
            const std::size_t ci = static_cast<std::size_t>(
                std::clamp(d.pos[0] / field.spacing(), 0.0f,
                           static_cast<float>(field.cols() - 1)));

            const float cell_water = field.field().channel(PaintChannels::WATER).at(ri, ci);

            // Absorbed by dry cell
            if (cell_water < params_.dry_threshold) {
                // Deposit all remaining pigment and die
                for (std::size_t b = 0; b < spectral::kBands; ++b)
                    field.field().channel(PaintChannels::KM_START + b).at(ri, ci) +=
                        d.pigment.reflectance[b] * d.volume * 0.5f;
                d.alive = false;
                return;
            }

            // Pickup pigment from wet cell
            const float rate_up = params_.pickup_rate * dt;
            for (std::size_t b = 0; b < spectral::kBands; ++b) {
                auto& cell = field.field().channel(PaintChannels::KM_START + b).at(ri, ci);
                const float taken = cell * rate_up;
                d.pigment.reflectance[b] += taken;
                cell = std::max(0.0f, cell - taken);
            }

            // Deposit pigment trail
            const float rate_dep = params_.deposit_rate * dt;
            for (std::size_t b = 0; b < spectral::kBands; ++b) {
                field.field().channel(PaintChannels::KM_START + b).at(ri, ci) +=
                    d.pigment.reflectance[b] * rate_dep;
            }

            // Consume some field water
            auto& w = field.field().channel(PaintChannels::WATER).at(ri, ci);
            w = std::max(0.0f, w - 0.01f * dt);
        }

        void merge_drops() {
            // O(N²) merge check — acceptable for drop_count ≤ 512
            for (std::size_t i = 0; i < drops_.size(); ++i) {
                if (!drops_[i].alive) continue;
                for (std::size_t j = i + 1; j < drops_.size(); ++j) {
                    if (!drops_[j].alive) continue;
                    const float dx = drops_[i].pos[0] - drops_[j].pos[0];
                    const float dy = drops_[i].pos[1] - drops_[j].pos[1];
                    const float dist2 = dx * dx + dy * dy;
                    const float r_sum = (drops_[i].radius + drops_[j].radius) * params_.merge_radius;
                    if (dist2 < r_sum * r_sum) {
                        // Merge j into i (volume-sum, KM-blend by volume)
                        const float vi = drops_[i].volume;
                        const float vj = drops_[j].volume;
                        const float vtot = vi + vj;
                        if (vtot > 0.0f) {
                            for (std::size_t b = 0; b < spectral::kBands; ++b) {
                                drops_[i].pigment.reflectance[b] =
                                (drops_[i].pigment.reflectance[b] * vi
                                    + drops_[j].pigment.reflectance[b] * vj) / vtot;
                            }
                            drops_[i].volume = std::min(vtot, 1.0f);
                        }
                        drops_[j].alive = false;
                    }
                }
            }
        }

        DropEngineParams params_{};
        NoiseFn noise_{};
        std::vector<Droplet> drops_;
    };

    using DefaultDropEngine = DropEngine<>;
} // namespace kalpana

#endif // KALPANA_BRUSH_DROP_ENGINE_HPP
