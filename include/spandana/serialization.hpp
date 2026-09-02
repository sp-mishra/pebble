#pragma once
// ============================================================================
// spandana/serialization.hpp — Glaze JSON Serialization for Animation & Materials
// ============================================================================
// Ultra-fast, reflection-free Glaze JSON serializer for Spandana animation data,
// blend spaces, and physical material parameters.
// ============================================================================

#include "blend_space.hpp"
#include "gati/material.hpp"
#include <glaze/glaze.hpp>
#include <string>
#include <vector>

namespace pebble::spandana::io {
    struct MaterialDataDTO {
        float rest_density = 1000.0f;
        float melt_temp = 0.0f;
        float boil_temp = 100.0f;
        float conductivity = 0.5f;
        float temperature = 20.0f;
    };

    struct BlendSampleDTO {
        float x = 0.0f;
        float y = 0.0f;
        std::string clip_name;
    };

    struct BlendSpaceDTO {
        std::vector<BlendSampleDTO> samples;
    };

    // Converts MaterialComponent to DTO for Glaze JSON serialization
    [[nodiscard]] inline std::string serialize_material_json(const gati::MaterialComponent& mat) {
        MaterialDataDTO dto{
            .rest_density = mat.params.rest_density,
            .melt_temp = mat.params.melt_temp,
            .boil_temp = mat.params.boil_temp,
            .conductivity = mat.params.conductivity,
            .temperature = mat.temperature
        };
        return glz::write_json(dto).value_or("{}");
    }

    // Deserializes MaterialComponent from Glaze JSON
    [[nodiscard]] inline gati::MaterialComponent deserialize_material_json(const std::string& json_str) {
        MaterialDataDTO dto;
        auto ec = glz::read_json(dto, json_str);
        gati::MaterialComponent mat;
        if (!ec) {
            mat.params.rest_density = dto.rest_density;
            mat.params.melt_temp = dto.melt_temp;
            mat.params.boil_temp = dto.boil_temp;
            mat.params.conductivity = dto.conductivity;
            mat.temperature = dto.temperature;
            mat.phase_fractions = prakriti::phase_from_temperature(mat.temperature, mat.params);
        }
        return mat;
    }

    // Converts BlendSpace2D to DTO for Glaze JSON serialization
    [[nodiscard]] inline std::string serialize_blend_space_json(const BlendSpaceDTO& space_dto) {
        return glz::write_json(space_dto).value_or("{}");
    }

    [[nodiscard]] inline BlendSpaceDTO deserialize_blend_space_json(const std::string& json_str) {
        BlendSpaceDTO dto;
        auto ec = glz::read_json(dto, json_str);
        (void)ec;
        return dto;
    }
} // namespace pebble::spandana::io
