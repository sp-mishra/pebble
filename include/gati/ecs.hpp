#pragma once
// ============================================================================
// gati/ecs.hpp — ECS integration header for Gati
// ============================================================================
// Reuses pebble::ecs directly.
// ============================================================================

#include "ecs/ecs.hpp"

namespace gati {

using Entity = pebble::ecs::Entity;
inline constexpr Entity null_entity = pebble::ecs::null_entity;
using World = pebble::ecs::World;
using CommandBuffer = pebble::ecs::CommandBuffer;
template <typename C>
using ComponentStore = pebble::ecs::ComponentStore<C>;

inline constexpr std::uint32_t kDefaultUniverse = pebble::ecs::kDefaultUniverse;

} // namespace gati
