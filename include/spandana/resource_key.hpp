#pragma once
// ============================================================================
// spandana/resource_key.hpp — Resource Key for Dependency & Parallelism Inference
// ============================================================================
// Identifies targeted component fields to detect conflicts vs parallelism.
// ============================================================================

#include <cstdint>
#include <functional>

namespace pebble::spandana {
    struct ResourceKey {
        std::uint32_t entity_id = 0;
        std::uint32_t component_id = 0;
        std::uint32_t property_offset = 0;

        [[nodiscard]] constexpr bool operator==(const ResourceKey&) const noexcept = default;
    };

    // Sentinel global resource for camera / world effects
    inline constexpr ResourceKey kWorldResource{0xFFFFFFFF, 0xFFFFFFFF, 0};
    inline constexpr ResourceKey kCameraResource{0xFFFFFFFE, 0xFFFFFFFF, 0};
} // namespace pebble::spandana

namespace std {
    template <>
    struct hash<pebble::spandana::ResourceKey> {
        std::size_t operator()(const pebble::spandana::ResourceKey& k) const noexcept {
            return (std::hash<std::uint32_t>{}(k.entity_id) ^
                    (std::hash<std::uint32_t>{}(k.component_id) << 1)) ^
                (std::hash<std::uint32_t>{}(k.property_offset) << 2);
        }
    };
} // namespace std
