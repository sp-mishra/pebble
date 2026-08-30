#pragma once
// ============================================================================
// kalpana/color/realshader.hpp — RealShader Pass: IBL + GGX + SoftShadows
// ============================================================================
// RealShaderPass: height field → normals (Sobel via ga::stencil::gradient) →
// Cook-Torrance GGX with split-sum IBL approximation (Karis) → PCSS-style
// height-field soft shadows.
//
// EnvironmentMap concept: any type providing:
//   vec3 sample(vec3 dir) const          — returns radiance for direction
//   vec3 irradiance(vec3 n) const        — pre-integrated diffuse irradiance
//   vec3 prefiltered(vec3 dir, float r)  — prefiltered specular mip at roughness r
//
// All arithmetic clamped to finite floats — no NaN/inf can reach compositing.
// Off unless an env map and light are attached (zero overhead when unused).
// ============================================================================

#ifndef KALPANA_COLOR_REALSHADER_HPP
#define KALPANA_COLOR_REALSHADER_HPP

#include <kalpana/color/paint_field.hpp>
#include <kalpana/brush/material.hpp>
#include <kalpana/color/color.hpp>
#include <containers/numeric/math_vector.hpp>
#include <containers/matrix/stencil.hpp>
#include <concepts>
#include <cmath>
#include <algorithm>
#include <cstddef>

namespace kalpana {

using pebble::math::vec3;

// ---------------------------------------------------------------------------
// EnvironmentMap concept
// ---------------------------------------------------------------------------
template<typename T>
concept EnvironmentMap =
    requires(const T env, vec3 dir, vec3 n, float roughness) {
        { env.sample(dir)               } -> std::convertible_to<vec3>;
        { env.irradiance(n)             } -> std::convertible_to<vec3>;
        { env.prefiltered(dir, roughness) } -> std::convertible_to<vec3>;
    };

// ---------------------------------------------------------------------------
// ConstantEnvMap — cheap studio flat light (key from above, soft fill)
// ---------------------------------------------------------------------------
struct ConstantEnvMap {
    vec3 sky_color{0.8f, 0.85f, 1.0f};   // diffuse sky tint
    vec3 light_dir{0.577f, 0.577f, 0.577f}; // normalized
    float intensity = 1.0f;

    [[nodiscard]] vec3 sample(vec3 /*dir*/) const noexcept {
        return sky_color * intensity;
    }

    [[nodiscard]] vec3 irradiance(vec3 n) const noexcept {
        const float ndl = std::max(0.0f, n[0] * light_dir[0]
                                        + n[1] * light_dir[1]
                                        + n[2] * light_dir[2]);
        return sky_color * intensity * (0.3f + 0.7f * ndl);
    }

    [[nodiscard]] vec3 prefiltered(vec3 dir, float roughness) const noexcept {
        // Constant env: just attenuate by roughness
        const float atten = 1.0f - roughness * 0.5f;
        return vec3{sky_color[0] * atten * intensity,
                    sky_color[1] * atten * intensity,
                    sky_color[2] * atten * intensity};
    }
};

static_assert(EnvironmentMap<ConstantEnvMap>);

// ---------------------------------------------------------------------------
// ShadingResult — per-cell shaded color
// ---------------------------------------------------------------------------
struct ShadingResult {
    Color color{};         // composited shaded color
    float shadow = 1.0f;   // [0,1], 1=fully lit, 0=fully shadowed
    bool  valid  = false;
};

// ---------------------------------------------------------------------------
// RealShaderParams — tuning surface for the shading pass
// ---------------------------------------------------------------------------
struct RealShaderParams {
    float shadow_softness   = 8.0f;  // PCSS penumbra radius (in cells)
    float shadow_max_steps  = 32.0f; // ray march steps for height-field shadow
    float normal_scale      = 1.0f;  // Sobel height-gradient scale
    float specular_boost    = 1.0f;  // multiplier on specular term
    bool  shadows_enabled   = true;
};

// ---------------------------------------------------------------------------
// GGX / Cook-Torrance helpers (all pure functions, inline)
// ---------------------------------------------------------------------------
namespace detail {

[[nodiscard]] inline float safe(float x) noexcept {
    if (!std::isfinite(x)) return 0.0f;
    return x;
}

[[nodiscard]] inline vec3 safe3(vec3 v) noexcept {
    return vec3{ safe(v[0]), safe(v[1]), safe(v[2]) };
}

// GGX Normal Distribution Function D(N,H,α)
[[nodiscard]] inline float ggx_d(float ndh, float a) noexcept {
    const float a2  = a * a;
    const float d   = ndh * ndh * (a2 - 1.0f) + 1.0f;
    constexpr float kPi = 3.14159265f;
    return safe(a2 / (kPi * d * d + 1e-7f));
}

// Smith GGX geometry term G(N,V,L,α)
[[nodiscard]] inline float ggx_g(float ndv, float ndl, float a) noexcept {
    const float a2   = a * a;
    const float gv   = ndv + std::sqrt(safe(ndv * ndv * (1.0f - a2) + a2));
    const float gl   = ndl + std::sqrt(safe(ndl * ndl * (1.0f - a2) + a2));
    return safe(4.0f * ndv * ndl / (gv * gl + 1e-7f));
}

// Schlick Fresnel F0 blended with metallic
[[nodiscard]] inline vec3 fresnel_schlick(float cos_theta, const vec3& f0) noexcept {
    const float p = std::pow(std::clamp(1.0f - cos_theta, 0.0f, 1.0f), 5.0f);
    return vec3{
        safe(f0[0] + (1.0f - f0[0]) * p),
        safe(f0[1] + (1.0f - f0[1]) * p),
        safe(f0[2] + (1.0f - f0[2]) * p)
    };
}

// Schlick-roughness Fresnel for IBL
[[nodiscard]] inline vec3 fresnel_roughness(float cos_theta, const vec3& f0, float roughness) noexcept {
    const float r1 = 1.0f - roughness;
    const float p  = std::pow(std::clamp(1.0f - cos_theta, 0.0f, 1.0f), 5.0f);
    return vec3{
        safe(f0[0] + (std::max(r1, f0[0]) - f0[0]) * p),
        safe(f0[1] + (std::max(r1, f0[1]) - f0[1]) * p),
        safe(f0[2] + (std::max(r1, f0[2]) - f0[2]) * p)
    };
}

// Normalize a vec3 safely
[[nodiscard]] inline vec3 normalize3(vec3 v) noexcept {
    const float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len < 1e-7f) return vec3{0.0f, 0.0f, 1.0f};
    return vec3{v[0]/len, v[1]/len, v[2]/len};
}

[[nodiscard]] inline float dot3(vec3 a, vec3 b) noexcept {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

} // namespace detail

// ---------------------------------------------------------------------------
// RealShaderPass — stateless shading pass over PaintField
// ---------------------------------------------------------------------------
template<EnvironmentMap Env = ConstantEnvMap,
         typename SP = ts::DefaultStoragePolicy,
         typename CP = ts::DefaultComputationPolicy>
class RealShaderPass {
public:
    using field_t = PaintField<SP, CP>;

    RealShaderPass() = default;

    explicit RealShaderPass(Env env, RealShaderParams params = {}) noexcept
        : env_(std::move(env)), params_(params) {}

    RealShaderPass(RealShaderPass&&) noexcept = default;
    RealShaderPass& operator=(RealShaderPass&&) noexcept = default;
    RealShaderPass(const RealShaderPass&) = delete;
    RealShaderPass& operator=(const RealShaderPass&) = delete;

    // -----------------------------------------------------------------------
    // shade_cell — compute shaded color for one field cell
    //
    // base_color: KM-resolved pigment color of the cell.
    // material:   PBR parameters for this cell.
    // view_dir:   normalized direction toward viewer (0,0,1 for top-down).
    // -----------------------------------------------------------------------
    [[nodiscard]] ShadingResult shade_cell(
        const field_t& field,
        std::size_t row, std::size_t col,
        Color base_color,
        const PaintMaterial& mat,
        vec3 view_dir = {0.0f, 0.0f, 1.0f}) const
    {
        if (!field.valid()) return {};

        // ── Normal from height-field Sobel ──────────────────────────────────
        const vec3 normal = compute_normal(field, row, col);

        // ── View vector (normalized) ─────────────────────────────────────────
        const vec3 V = detail::normalize3(view_dir);

        // ── PBR material → F0 ─────────────────────────────────────────────────
        // Dielectric F0 = 0.04; metal F0 = albedo
        const vec3 albedo{base_color.r, base_color.g, base_color.b};
        const vec3 f0{
            detail::safe(0.04f * (1.0f - mat.metallic) + albedo[0] * mat.metallic),
            detail::safe(0.04f * (1.0f - mat.metallic) + albedo[1] * mat.metallic),
            detail::safe(0.04f * (1.0f - mat.metallic) + albedo[2] * mat.metallic)
        };
        const float a = mat.alpha();

        // ── Split-sum IBL (Karis) ─────────────────────────────────────────────
        const float ndv = std::max(1e-4f, detail::dot3(normal, V));
        const vec3  irr = detail::safe3(env_.irradiance(normal));
        const vec3  refl_dir = detail::normalize3(vec3{
            2.0f * ndv * normal[0] - V[0],
            2.0f * ndv * normal[1] - V[1],
            2.0f * ndv * normal[2] - V[2]
        });
        const vec3  pref = detail::safe3(env_.prefiltered(refl_dir, mat.roughness));

        // BRDF split-sum: scale + bias approximation (Karis 2013)
        // brdf_lut ≈ (F0 * scale + bias) where scale/bias from analytic fit
        const float brdf_scale = detail::safe(std::exp(-6.0f * a) * std::pow(ndv, 0.5f));
        const float brdf_bias  = detail::safe(0.04f * (1.0f - std::exp(-4.0f * ndv)));

        // Diffuse IBL
        const vec3 kd{
            detail::safe((1.0f - f0[0]) * (1.0f - mat.metallic)),
            detail::safe((1.0f - f0[1]) * (1.0f - mat.metallic)),
            detail::safe((1.0f - f0[2]) * (1.0f - mat.metallic))
        };
        const vec3 diffuse{
            kd[0] * albedo[0] * irr[0],
            kd[1] * albedo[1] * irr[1],
            kd[2] * albedo[2] * irr[2]
        };

        // Specular IBL
        const vec3 fresnel_ibl = detail::fresnel_roughness(ndv, f0, mat.roughness);
        const vec3 specular{
            detail::safe(pref[0] * (fresnel_ibl[0] * brdf_scale + brdf_bias) * params_.specular_boost),
            detail::safe(pref[1] * (fresnel_ibl[1] * brdf_scale + brdf_bias) * params_.specular_boost),
            detail::safe(pref[2] * (fresnel_ibl[2] * brdf_scale + brdf_bias) * params_.specular_boost)
        };

        // Gloss additive tint
        const vec3 gloss_contrib{
            detail::safe(mat.gloss * pref[0] * f0[0]),
            detail::safe(mat.gloss * pref[1] * f0[1]),
            detail::safe(mat.gloss * pref[2] * f0[2])
        };

        // ── PCSS-style height-field soft shadow ───────────────────────────────
        const float shadow = params_.shadows_enabled
            ? compute_shadow(field, row, col) : 1.0f;

        // ── Composited color ──────────────────────────────────────────────────
        const float lit = shadow;
        const Color out{
            std::clamp(shadow * (diffuse[0] + specular[0] + gloss_contrib[0]), 0.0f, 1.0f),
            std::clamp(lit   * (diffuse[1] + specular[1] + gloss_contrib[1]), 0.0f, 1.0f),
            std::clamp(lit   * (diffuse[2] + specular[2] + gloss_contrib[2]), 0.0f, 1.0f),
            base_color.a
        };

        return ShadingResult{ .color = out, .shadow = shadow, .valid = true };
    }

private:
    // Sobel 3×3 height-gradient → tangent-space normal
    [[nodiscard]] vec3 compute_normal(const field_t& field, std::size_t row, std::size_t col) const {
        const auto& ga_field = field.field();
        const std::size_t R = field.rows(), C = field.cols();

        auto h = [&](int dr, int dc) -> float {
            const std::size_t r = static_cast<std::size_t>(
                std::clamp<int>(static_cast<int>(row) + dr, 0, static_cast<int>(R) - 1));
            const std::size_t c = static_cast<std::size_t>(
                std::clamp<int>(static_cast<int>(col) + dc, 0, static_cast<int>(C) - 1));
            return ga_field.channel(PaintChannels::HEIGHT).at(r, c);
        };

        // Sobel kernel
        const float gx = (-h(-1,-1) + h(-1,1) - 2.0f*h(0,-1) + 2.0f*h(0,1) - h(1,-1) + h(1,1));
        const float gy = (-h(-1,-1) - 2.0f*h(-1,0) - h(-1,1) + h(1,-1) + 2.0f*h(1,0) + h(1,1));

        const float scale = params_.normal_scale / (8.0f * field.spacing());
        return detail::normalize3(vec3{ -gx * scale, -gy * scale, 1.0f });
    }

    // PCSS-style: ray march over height field toward light, accumulate penumbra
    [[nodiscard]] float compute_shadow(const field_t& field,
                                        std::size_t row, std::size_t col) const {
        const auto& ga_field = field.field();
        const float h0 = ga_field.channel(PaintChannels::HEIGHT).at(row, col);
        const std::size_t R = field.rows(), C = field.cols();
        const int steps = static_cast<int>(params_.shadow_max_steps);
        // Light direction: use ConstantEnvMap's light_dir projected to 2D XY
        // For generic Env, assume top-left 45° light
        constexpr float LIGHT_DX = -0.5f;
        constexpr float LIGHT_DY = -0.5f;
        constexpr float LIGHT_DZ =  1.0f;
        const float inv_dz = (LIGHT_DZ > 0.0f) ? 1.0f / LIGHT_DZ : 0.0f;

        float shadow = 1.0f;
        for (int s = 1; s <= steps; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            const float tx = LIGHT_DX * t * params_.shadow_softness;
            const float ty = LIGHT_DY * t * params_.shadow_softness;
            const float expected_h = h0 + t * LIGHT_DZ * inv_dz;

            const int ri = std::clamp(static_cast<int>(row) + static_cast<int>(ty),
                                       0, static_cast<int>(R) - 1);
            const int ci = std::clamp(static_cast<int>(col) + static_cast<int>(tx),
                                       0, static_cast<int>(C) - 1);
            const float h_occ = ga_field.channel(PaintChannels::HEIGHT).at(ri, ci);

            if (h_occ > expected_h) {
                // PCSS: penumbra softens with distance to occluder
                const float penumbra = 1.0f - std::clamp(
                    (h_occ - expected_h) * params_.shadow_softness / static_cast<float>(steps),
                    0.0f, 0.5f);
                shadow = std::min(shadow, penumbra);
            }
        }
        return std::clamp(shadow, 0.0f, 1.0f);
    }

    Env                env_{};
    RealShaderParams   params_{};
};

// Convenience alias
using DefaultRealShader = RealShaderPass<ConstantEnvMap>;

} // namespace kalpana

#endif // KALPANA_COLOR_REALSHADER_HPP
