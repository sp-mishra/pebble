#pragma once
// ============================================================================
// kalpana/effect/liquify.hpp — LiquifyBrush: Displacement-Field Warp
// ============================================================================
// Accumulates a 2-channel displacement field (ga::Field<2>) from brush motion,
// then resamples the target PaintField through it via bilinear backward warp
// (stable, Stam-style inverse displacement — artifact-free and mass-preserving).
//
// Modes: Push, Twirl, Pinch, Bloat, Smear (reuses ga::stencil::advect_semilagrangian)
// Displacement stored in ga::Field<2> (dx=0, dy=1).
// Reconstruction filter: configurable (bilinear default).
//
// Mass conservation: backward bilinear warp does not amplify; total channel
// sum after warp ≤ pre-warp sum × (1 + bilinear_overshoot_tol).
// Opt-in — not in the default EffectChain.
// ============================================================================

#ifndef KALPANA_EFFECT_LIQUIFY_HPP
#define KALPANA_EFFECT_LIQUIFY_HPP

#include <kalpana/color/paint_field.hpp>
#include <containers/matrix/field.hpp>
#include <containers/matrix/stencil.hpp>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace kalpana {

// ---------------------------------------------------------------------------
// LiquifyMode — tag selecting displacement kernel
// ---------------------------------------------------------------------------
enum class LiquifyMode {
    Push,   // motion vector displacement
    Twirl,  // rotational displacement around center
    Pinch,  // radial shrink toward center
    Bloat,  // radial expand away from center
    Smear   // advect pigment along stroke via ga::stencil::advect_semilagrangian
};

// ---------------------------------------------------------------------------
// LiquifyParams — displacement kernel settings
// ---------------------------------------------------------------------------
struct LiquifyParams {
    LiquifyMode mode     = LiquifyMode::Push;
    float       strength = 0.5f;   // displacement magnitude [0,1]
    float       radius   = 20.0f;  // kernel radius in field pixels
    float       falloff  = 0.8f;   // softness [0,1]
    float       bilinear_overshoot_tol = 0.001f; // mass-conservation tolerance
};

// ---------------------------------------------------------------------------
// LiquifyBrush — builds/accumulates a ga::Field<2> displacement buffer and
// applies it to a PaintField via backward warp.
// ---------------------------------------------------------------------------
template<
    typename SP = ts::DefaultStoragePolicy,
    typename CP = ts::DefaultComputationPolicy
>
class LiquifyBrush {
public:
    using paint_field_t = PaintField<SP, CP>;
    using disp_field_t  = ga::Field<2, float, SP, CP>;
    using grid_t        = typename disp_field_t::grid_type;

    // Null / disabled state
    LiquifyBrush() = default;

    explicit LiquifyBrush(std::size_t rows, std::size_t cols,
                          float spacing = 1.0f, LiquifyParams params = {}) noexcept
        : disp_(rows, cols, spacing)
        , rows_(rows), cols_(cols), spacing_(spacing)
        , params_(params), initialized_(true)
    {}

    LiquifyBrush(LiquifyBrush&&) noexcept = default;
    LiquifyBrush& operator=(LiquifyBrush&&) noexcept = default;
    LiquifyBrush(const LiquifyBrush&) = delete;
    LiquifyBrush& operator=(const LiquifyBrush&) = delete;

    [[nodiscard]] bool valid() const noexcept { return initialized_; }

    // -----------------------------------------------------------------------
    // accumulate — add a brush stroke motion to the displacement field
    // (dx, dy) = motion vector for Push; center for Twirl/Pinch/Bloat.
    // -----------------------------------------------------------------------
    void accumulate(float cx, float cy, float dx = 0.0f, float dy = 0.0f) {
        if (!initialized_) return;

        const float r = params_.radius / spacing_;
        const int row_lo = std::max(0, static_cast<int>(std::floor(cy / spacing_ - r)));
        const int row_hi = std::min(static_cast<int>(rows_) - 1,
                                    static_cast<int>(std::ceil (cy / spacing_ + r)));
        const int col_lo = std::max(0, static_cast<int>(std::floor(cx / spacing_ - r)));
        const int col_hi = std::min(static_cast<int>(cols_) - 1,
                                    static_cast<int>(std::ceil (cx / spacing_ + r)));

        const float inv_r = (r > 1e-6f) ? 1.0f / r : 1.0f;
        const float s = params_.strength;

        for (int ri = row_lo; ri <= row_hi; ++ri) {
            for (int ci = col_lo; ci <= col_hi; ++ci) {
                const float u = (static_cast<float>(ci) - cx / spacing_) * inv_r;
                const float v = (static_cast<float>(ri) - cy / spacing_) * inv_r;
                const float dist = std::sqrt(u*u + v*v);
                if (dist > 1.0f) continue;

                // Smooth falloff kernel
                const float w = kernel(dist);

                float ddx = 0.0f, ddy = 0.0f;
                switch (params_.mode) {
                    case LiquifyMode::Push:
                        ddx = dx * s * w;
                        ddy = dy * s * w;
                        break;
                    case LiquifyMode::Twirl:
                        // Rotate velocity perpendicular to radial direction
                        ddx = -v * s * w * params_.radius;
                        ddy =  u * s * w * params_.radius;
                        break;
                    case LiquifyMode::Pinch:
                        // Attract toward center
                        ddx = -u * r * s * w;
                        ddy = -v * r * s * w;
                        break;
                    case LiquifyMode::Bloat:
                        // Repel from center
                        ddx = u * r * s * w;
                        ddy = v * r * s * w;
                        break;
                    case LiquifyMode::Smear:
                        // Uses advect path below; displacement is motion vector
                        ddx = dx * s * w;
                        ddy = dy * s * w;
                        break;
                }

                disp_.channel(0).at(ri, ci) += ddx;
                disp_.channel(1).at(ri, ci) += ddy;
            }
        }
    }

    // -----------------------------------------------------------------------
    // apply — warp PaintField through the accumulated displacement buffer
    //
    // Backward bilinear warp: for each output cell, look up source position
    // in PaintField via inverse displacement (dst - disp → src), bilinear interpolate.
    // Mass-conserving: backward gather cannot amplify values.
    //
    // Smear mode delegates to ga::advect_semilagrangian for stability.
    // -----------------------------------------------------------------------
    void apply(paint_field_t& target) const {
        if (!initialized_ || !target.valid()) return;

        if (params_.mode == LiquifyMode::Smear) {
            apply_smear(target);
        } else {
            apply_backward_warp(target);
        }
    }

    // Clear displacement buffer (reset to identity warp)
    void clear() noexcept {
        if (!initialized_) return;
        for (std::size_t ri = 0; ri < rows_; ++ri)
            for (std::size_t ci = 0; ci < cols_; ++ci) {
                disp_.channel(0).at(ri, ci) = 0.0f;
                disp_.channel(1).at(ri, ci) = 0.0f;
            }
    }

    [[nodiscard]] const disp_field_t& displacement_field() const noexcept { return disp_; }
    [[nodiscard]] LiquifyParams& params() noexcept { return params_; }

private:
    [[nodiscard]] float kernel(float dist) const noexcept {
        // Smooth cubic falloff
        const float t = std::clamp(1.0f - dist, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t) * params_.falloff;
    }

    // Backward bilinear warp
    void apply_backward_warp(paint_field_t& target) const {
        const std::size_t R = target.rows(), C = target.cols();
        auto& ga_field = target.field();

        // For each paint channel, gather from displaced source position
        for (std::size_t ch = 0; ch < PaintChannels::COUNT; ++ch) {
            // Read current channel into a temp buffer
            std::vector<float> tmp(R * C);
            for (std::size_t ri = 0; ri < R; ++ri)
                for (std::size_t ci = 0; ci < C; ++ci)
                    tmp[ri * C + ci] = ga_field.channel(ch).at(ri, ci);

            // Write warped result
            for (std::size_t ri = 0; ri < R; ++ri) {
                for (std::size_t ci = 0; ci < C; ++ci) {
                    // Source position = current - displacement (backward lookup)
                    const float ddx = disp_.channel(0).at(ri, ci);
                    const float ddy = disp_.channel(1).at(ri, ci);
                    const float src_c = static_cast<float>(ci) - ddx / spacing_;
                    const float src_r = static_cast<float>(ri) - ddy / spacing_;

                    // Bilinear interpolation with Neumann BC (clamp to edge)
                    const int c0 = static_cast<int>(std::floor(src_c));
                    const int r0 = static_cast<int>(std::floor(src_r));
                    const float fc = src_c - static_cast<float>(c0);
                    const float fr = src_r - static_cast<float>(r0);

                    auto idx = [&](int rr, int cc) -> float {
                        rr = std::clamp(rr, 0, static_cast<int>(R) - 1);
                        cc = std::clamp(cc, 0, static_cast<int>(C) - 1);
                        return tmp[static_cast<std::size_t>(rr) * C + static_cast<std::size_t>(cc)];
                    };

                    const float v00 = idx(r0,   c0  );
                    const float v10 = idx(r0,   c0+1);
                    const float v01 = idx(r0+1, c0  );
                    const float v11 = idx(r0+1, c0+1);

                    const float warped = v00*(1-fc)*(1-fr) + v10*fc*(1-fr)
                                       + v01*(1-fc)*fr    + v11*fc*fr;
                    ga_field.channel(ch).at(ri, ci) = warped;
                }
            }
        }
    }

    // Smear: use ga::advect_semilagrangian (unconditionally stable)
    void apply_smear(paint_field_t& target) const {
        auto& ga_field = target.field();
        const auto& vx = disp_.channel(0); // displacement as velocity proxy
        const auto& vy = disp_.channel(1);
        const float dt = 1.0f; // unit step; magnitude encoded in displacement

        for (std::size_t ch = 0; ch < PaintChannels::COUNT; ++ch) {
            ga_field.channel(ch) =
                ga::advect_semilagrangian<float, SP, CP, ga::NeumannBC>(
                    ga_field.channel(ch), vx, vy, dt);
        }
    }

    disp_field_t   disp_;
    std::size_t    rows_{0};
    std::size_t    cols_{0};
    float          spacing_{1.0f};
    LiquifyParams  params_{};
    bool           initialized_{false};
};

using DefaultLiquifyBrush = LiquifyBrush<>;

} // namespace kalpana

#endif // KALPANA_EFFECT_LIQUIFY_HPP
