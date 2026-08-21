#pragma once
// ============================================================================
// ecs/entity.hpp — Generational Entity Handle for pebble::ecs
// ============================================================================
// Stale-safe phantom-typed entity ID.
// index: addresses the entity slot in stores
// generation: detects stale handles after reuse
// Zero heap allocation, trivially copyable, constexpr-constructible.
// ============================================================================

#include "containers/handle/generational_handle.hpp"
#include <cstdint>

namespace pebble::ecs {

struct entity_tag {};

// Stable entity identifier
using Entity = containers::generational_handle<entity_tag, std::uint32_t>;

// Sentinel null entity handle
inline constexpr Entity null_entity = containers::null_handle<entity_tag, std::uint32_t>;

// Default universe capacity for SparseSets
inline constexpr std::uint32_t kDefaultUniverse = 1u << 16;

} // namespace pebble::ecs
