#pragma once
// ============================================================================
// kalpana/brush/material.hpp — PBR Paint Material Channel
// ============================================================================
// PaintMaterial describes the physical surface properties of deposited paint
// volume, driving RealShaderPass (realshader.hpp) Cook-Torrance GGX BRDF.
//
// Default matte (metallic=0, roughness=0.9) is correct for watercolor / pastel.
// Oil binder → low roughness (glossy).  Metallic preset → metallic=1, rough=0.25.
// ============================================================================

#ifndef KALPANA_BRUSH_MATERIAL_HPP
#define KALPANA_BRUSH_MATERIAL_HPP

namespace kalpana {
    struct PaintMaterial {
        float metallic = 0.0f; // [0,1] 0=dielectric, 1=metal
        float roughness = 0.9f; // [0,1] GGX α² distribution width
        float gloss = 0.0f; // [0,1] surface gloss / binder sheen (additive specular tint)
        float anisotropy = 0.0f; // [0,1] anisotropic specular elongation (brush-direction)

        // Pre-built material descriptors for common paint types
        // Pre-built material descriptors for common paint types
        [[nodiscard]] static constexpr PaintMaterial preset_matte() noexcept {
            return {.metallic = 0.0f, .roughness = 0.9f, .gloss = 0.0f, .anisotropy = 0.0f};
        }

        [[nodiscard]] static constexpr PaintMaterial preset_glossy_oil() noexcept {
            return {.metallic = 0.0f, .roughness = 0.15f, .gloss = 0.6f, .anisotropy = 0.1f};
        }

        [[nodiscard]] static constexpr PaintMaterial preset_metallic() noexcept {
            return {.metallic = 1.0f, .roughness = 0.25f, .gloss = 0.0f, .anisotropy = 0.0f};
        }

        [[nodiscard]] static constexpr PaintMaterial preset_pencil() noexcept {
            return {.metallic = 0.0f, .roughness = 0.95f, .gloss = 0.0f, .anisotropy = 0.0f};
        }

        [[nodiscard]] static constexpr PaintMaterial preset_watercolor() noexcept {
            return {.metallic = 0.0f, .roughness = 0.85f, .gloss = 0.0f, .anisotropy = 0.0f};
        }

        [[nodiscard]] static constexpr PaintMaterial preset_gouache() noexcept {
            return {.metallic = 0.0f, .roughness = 0.85f, .gloss = 0.0f, .anisotropy = 0.0f};
        }

        [[nodiscard]] static constexpr PaintMaterial preset_feather() noexcept {
            return {.metallic = 0.0f, .roughness = 0.80f, .gloss = 0.05f, .anisotropy = 0.4f};
        }

        [[nodiscard]] constexpr float alpha() const noexcept {
            // GGX α = roughness² (Disney parameterization)
            return roughness * roughness;
        }

        friend constexpr bool operator==(const PaintMaterial&, const PaintMaterial&) = default;
    };
} // namespace kalpana

#endif // KALPANA_BRUSH_MATERIAL_HPP
