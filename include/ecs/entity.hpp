#pragma once
// ============================================================================
// ecs/entity.hpp — Generational Entity Handle for pebble::ecs
// ============================================================================
// Stale-safe phantom-typed entity ID.
// index: addresses the entity slot in stores
// generation: detects stale handles after reuse
// Zero heap allocation, trivially copyable, constexpr-constructible.
// No virtual methods, no macros.
// ============================================================================

#include "containers/handle/generational_handle.hpp"
#include <cstdint>
#include <type_traits>

namespace pebble::ecs {
    struct entity_tag {};

    // Stable entity identifier
    using Entity = containers::generational_handle<entity_tag, std::uint32_t>;

    // Sentinel null entity handle
    inline constexpr Entity null_entity = containers::null_handle<entity_tag, std::uint32_t>;

    // Default universe capacity for SparseSets / World slots
    inline constexpr std::uint32_t kDefaultUniverse = 1u << 16;

    static_assert(std::is_trivially_copyable_v<Entity>, "Entity must be trivially copyable");
    static_assert(!std::is_polymorphic_v<Entity>, "Entity must have zero virtual functions");
} // namespace pebble::ecs
