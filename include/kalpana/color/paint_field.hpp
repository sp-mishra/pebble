#pragma once
// ============================================================================
// kalpana/color/paint_field.hpp — Multi-channel Eulerian Paint Field Substrate
// ============================================================================
// PaintField is a typed wrapper over ga::Field<PaintChannels::COUNT, float, SP, CP>
// (containers/matrix/field.hpp). It owns the raster substrate for natural-media
// simulation — distinct from kalpana's vector-node layer stack.
//
// Channels (SoA, stride-1, tensor-backed):
//   [0..15] pigment_km  — per-cell KM reflectance accumulator (16 spectral bands)
//   [16]    water       — free liquid content [0,1]
//   [17]    height      — impasto relief for normals/specular
//   [18]    sediment    — granulation deposit trapped in paper valleys
//   [19]    binder      — oil/gum medium fraction (gloss + dry speed)
//
// Velocity channels for internal advection live in a companion ga::Field<2>.
// All grid calculus (diffusion, advection, pressure) is delegated to
// ga::stencil / ga::iterative — no stencil code written here.
// ============================================================================

#ifndef KALPANA_COLOR_PAINT_FIELD_HPP
#define KALPANA_COLOR_PAINT_FIELD_HPP

#include <containers/matrix/field.hpp>
#include <kalpana/color/spectral.hpp>
#include <kalpana/brush/stamp_shape.hpp>
#include <cstddef>
#include <algorithm>
#include <cmath>
#include <optional>

namespace kalpana {
    // ---------------------------------------------------------------------------
    // Channel index constants
    // ---------------------------------------------------------------------------
    struct PaintChannels {
        static constexpr std::size_t KM_START = 0;
        static constexpr std::size_t KM_END = 16; // exclusive; bands [0..15]
        static constexpr std::size_t WATER = 16;
        static constexpr std::size_t HEIGHT = 17;
        static constexpr std::size_t SEDIMENT = 18;
        static constexpr std::size_t BINDER = 19;
        static constexpr std::size_t COUNT = 20;
    };

    // Velocity field for internal advection (separate, pay-for-use)
    struct VelocityChannels {
        static constexpr std::size_t VX = 0;
        static constexpr std::size_t VY = 1;
        static constexpr std::size_t COUNT = 2;
    };

    // ---------------------------------------------------------------------------
    // PaintField<SP, CP> — typed wrapper over ga::Field<PaintChannels::COUNT>
    // ---------------------------------------------------------------------------
    template <
        typename SP = ts::DefaultStoragePolicy,
        typename CP = ts::DefaultComputationPolicy>
    class PaintField {
    public:
        using field_type = ga::Field<PaintChannels::COUNT, float, SP, CP>;
        using velfield_type = ga::Field<VelocityChannels::COUNT, float, SP, CP>;
        using grid_type = typename field_type::grid_type;

        // Null / disabled state — zero cost, no allocations
        PaintField() = default;

        PaintField(std::size_t rows, std::size_t cols, float spacing = 1.0f)
            : rows_(rows), cols_(cols), spacing_(spacing)
              , initialized_(true) {
            field_.emplace(rows, cols, spacing);
            vel_.emplace(rows, cols, spacing);
        }

        PaintField(PaintField&&) noexcept = default;
        PaintField& operator=(PaintField&&) noexcept = default;
        PaintField(const PaintField&) = delete;
        PaintField& operator=(const PaintField&) = delete;

        [[nodiscard]] bool valid() const noexcept { return initialized_; }
        [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
        [[nodiscard]] std::size_t cols() const noexcept { return cols_; }
        [[nodiscard]] float spacing() const noexcept { return spacing_; }

        [[nodiscard]] field_type& field() noexcept { return *field_; }
        [[nodiscard]] const field_type& field() const noexcept { return *field_; }
        [[nodiscard]] velfield_type& vel() noexcept { return *vel_; }
        [[nodiscard]] const velfield_type& vel() const noexcept { return *vel_; }

        // -----------------------------------------------------------------------
        // SplatParams — deposition parameters for a single dab
        // Decoupled from BrushStamp so paint_field.hpp has no brush dependency loop.
        // -----------------------------------------------------------------------
        struct SplatParams {
            float cx{}; // center x in field-space pixels
            float cy{}; // center y in field-space pixels
            float radius{}; // dab radius in field-space pixels
            float opacity{}; // dab opacity [0,1]
            float loading{}; // pigment mass coefficient [0,1]
            float water_add{}; // water deposited per cell [0,1]
            float height_add{}; // impasto relief per cell [0,1]
            float granulation{}; // sediment fraction [0,1]
            float angle{}; // stamp rotation (radians)
            float roundness{}; // stamp roundness
            float hardness{}; // stamp edge hardness
        };

        // Default grain function (constant 0.5 — no paper texture)
        struct FlatGrain {
            [[nodiscard]] constexpr float operator()(std::size_t, std::size_t) const noexcept {
                return 0.5f;
            }
        };

        // -----------------------------------------------------------------------
        // splat — rasterize a dab into field channels
        //
        // StampPolicy provides the alpha mask sample(u,v).
        // grain_fn is a callable: float(size_t row, size_t col) → paper grain [0,1].
        // Only cells within the dab's AABB are touched — O(marks × cells_touched).
        // Mass-conserving additive deposit (no channel exceeds its physical range).
        // -----------------------------------------------------------------------
        template <
            StampPreset Preset = StampPreset::Round,
            typename GrainFn = FlatGrain>
        void splat(
            const SplatParams& params,
            const spectral::SpectralColor& pigment,
            GrainFn grain_fn = {}) {
            if (!initialized_) return;

            const float r_cells = params.radius / spacing_;
            if (r_cells < 1e-6f) return;

            const int row_lo = std::max(0,
                                        static_cast<int>(std::floor(params.cy / spacing_ - r_cells)));
            const int row_hi = std::min(static_cast<int>(rows_) - 1,
                                        static_cast<int>(std::ceil(params.cy / spacing_ + r_cells)));
            const int col_lo = std::max(0,
                                        static_cast<int>(std::floor(params.cx / spacing_ - r_cells)));
            const int col_hi = std::min(static_cast<int>(cols_) - 1,
                                        static_cast<int>(std::ceil(params.cx / spacing_ + r_cells)));

            StampShape < Preset > shape{
                .roundness = params.roundness,
                .angle = params.angle,
                .hardness = params.hardness
            };
            const float inv_r = 1.0f / r_cells;

            for (int ri = row_lo; ri <= row_hi; ++ri) {
                for (int ci = col_lo; ci <= col_hi; ++ci) {
                    const float u = (static_cast<float>(ci) - params.cx / spacing_) * inv_r;
                    const float v = (static_cast<float>(ri) - params.cy / spacing_) * inv_r;
                    const float alpha = shape.sample(u, v) * params.opacity;
                    if (alpha < 1e-6f) continue;

                    const float grain = grain_fn(
                        static_cast<std::size_t>(ri),
                        static_cast<std::size_t>(ci));
                    const float deposited = alpha * params.loading;

                    // Pigment KM reflectance accumulation (additive)
                    for (std::size_t b = 0; b < spectral::kBands; ++b) {
                        field_->channel(PaintChannels::KM_START + b).at(ri, ci) +=
                            deposited * pigment.reflectance[b];
                    }

                    // Water
                    auto& w = field_->channel(PaintChannels::WATER).at(ri, ci);
                    w = std::clamp(w + alpha * params.water_add, 0.0f, 1.0f);

                    // Height (impasto relief)
                    auto& h = field_->channel(PaintChannels::HEIGHT).at(ri, ci);
                    h = std::clamp(h + alpha * params.height_add, 0.0f, 1.0f);

                    // Sediment — trapped in paper valleys (low grain)
                    field_->channel(PaintChannels::SEDIMENT).at(ri, ci) +=
                        deposited * params.granulation * (1.0f - grain);
                }
            }
        }

        // -----------------------------------------------------------------------
        // sample — read per-cell paint state at integer grid cell (row, col)
        // -----------------------------------------------------------------------
        struct CellSample {
            float km[spectral::kBands]{};
            float water{};
            float height{};
            float sediment{};
            float binder{};
        };

        [[nodiscard]] CellSample sample(std::size_t row, std::size_t col) const {
            if (!initialized_) return {};
            CellSample s;
            for (std::size_t b = 0; b < spectral::kBands; ++b)
                s.km[b] = field_->channel(PaintChannels::KM_START + b).at(row, col);
            s.water = field_->channel(PaintChannels::WATER).at(row, col);
            s.height = field_->channel(PaintChannels::HEIGHT).at(row, col);
            s.sediment = field_->channel(PaintChannels::SEDIMENT).at(row, col);
            s.binder = field_->channel(PaintChannels::BINDER).at(row, col);
            return s;
        }

        // -----------------------------------------------------------------------
        // resolve_color — KM reflectance accumulated in cell → RGB
        // Empty cell resolves to canvas_color.
        // -----------------------------------------------------------------------
        [[nodiscard]] Color resolve_color(std::size_t row, std::size_t col,
                                          Color canvas_color = colors::white()) const {
            if (!initialized_) return canvas_color;

            spectral::Spectrum refl{};
            float total = 0.0f;
            for (std::size_t b = 0; b < spectral::kBands; ++b) {
                refl[b] = field_->channel(PaintChannels::KM_START + b).at(row, col);
                total += refl[b];
            }
            if (total < 1e-6f) return canvas_color;

            // Normalize accumulated reflectance to [0,1] per band
            for (auto& r : refl) r = std::min(r, 1.0f);

            spectral::SpectralColor sc{refl};
            return sc.to_color();
        }

        // -----------------------------------------------------------------------
        // total_mass — sum of all pigment KM accumulation (Neumann BC conserves)
        // -----------------------------------------------------------------------
        [[nodiscard]] float total_mass() const {
            if (!initialized_) return 0.0f;
            float s = 0.0f;
            for (std::size_t b = 0; b < spectral::kBands; ++b)
                s += field_->mass(PaintChannels::KM_START + b);
            return s;
        }

        [[nodiscard]] float water_mass() const {
            if (!initialized_) return 0.0f;
            return field_->mass(PaintChannels::WATER);
        }

    private:
        std::optional<field_type> field_;
        std::optional<velfield_type> vel_;
        std::size_t rows_{0};
        std::size_t cols_{0};
        float spacing_{1.0f};
        bool initialized_{false};
    };

    // Default alias
    using DefaultPaintField = PaintField<>;
} // namespace kalpana

#endif // KALPANA_COLOR_PAINT_FIELD_HPP
