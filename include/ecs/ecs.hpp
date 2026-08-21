#pragma once
// ============================================================================
// ecs/ecs.hpp — High-Performance C++23 Standalone Entity-Component System
// ============================================================================
// Zero-virtual hot paths, SparseSet-backed component stores, generation-safe
// handles, deferred CommandBuffer mutations, and Pravaha multi-threaded queries.
// ============================================================================

#include "entity.hpp"
#include "component_store.hpp"
#include "command_buffer.hpp"
#include "world.hpp"
