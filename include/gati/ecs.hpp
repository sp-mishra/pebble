#pragma once
// ============================================================================
// gati/ecs.hpp — ECS integration header for Gati
// ============================================================================
// Reuses pebble::ecs directly with zero-overhead policy defaults.
// ============================================================================

#include "ecs/ecs.hpp"

namespace gati {

using Entity = pebble::ecs::Entity;
inline constexpr Entity null_entity = pebble::ecs::null_entity;
using World = pebble::ecs::World;
using BasicWorld = pebble::ecs::BasicWorld<>;
using CommandBuffer = pebble::ecs::CommandBuffer;
template <typename C>
using ComponentStore = pebble::ecs::ComponentStore<C>;

inline constexpr std::uint32_t kDefaultUniverse = pebble::ecs::kDefaultUniverse;

} // namespace gati
