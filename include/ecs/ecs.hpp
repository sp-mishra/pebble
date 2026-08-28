#pragma once
// ============================================================================
// ecs/ecs.hpp — High-Performance C++23 Standalone Entity-Component System
// ============================================================================
// Policy-based storage and execution, zero-virtual hot paths, zero macros,
// SparseSet and Archetype storage backends, generational handles, linear arena
// command mutations, reactive observers, entity relations, and system scheduling.
// ============================================================================

#include "entity.hpp"
#include "paged_sparse.hpp"
#include "storage_policy.hpp"
#include "component_store.hpp"
#include "command_buffer.hpp"
#include "query.hpp"
#include "observer.hpp"
#include "relation.hpp"
#include "change_detection.hpp"
#include "system.hpp"
#include "scheduler.hpp"
#include "archetype.hpp"
#include "world.hpp"

namespace pebble::ecs {

// Policy-Configured World Aliases
using DefaultWorld   = BasicWorld<SparseSetStoragePolicy, ArenaAllocPolicy, AutoSchedulerPolicy, PagedSparsePolicy>;
using ArchetypeWorld = BasicWorld<ArchetypeStoragePolicy, ArenaAllocPolicy, AutoSchedulerPolicy, PagedSparsePolicy>;
using ParallelWorld  = BasicWorld<SparseSetStoragePolicy, ArenaAllocPolicy, AutoSchedulerPolicy, PagedSparsePolicy>;

} // namespace pebble::ecs
