#pragma once
// ============================================================================
// docs/ecs/ecs.md — High-Performance C++23/26 Standalone Entity-Component System
// ============================================================================

# pebble::ecs — High-Performance C++23/26 Standalone Entity-Component System

`pebble::ecs` is a modern, cache-coherent, header-only Entity-Component System engineered for extreme performance, minimal memory footprints, and zero runtime waste. It strictly follows Pebble's zero-overhead principle: **strictly zero virtual functions, zero macros, zero RTTI, and zero heap allocation during queries**.

Include: `#include <ecs/ecs.hpp>`

---

## 1. Core Architecture & Storage Model

```
                                  ┌────────────────────────┐
                                  │      pebble::ecs       │
                                  └───────────┬────────────┘
                                              │
         ┌────────────────────────────────────┼────────────────────────────────────┐
         ▼                                    ▼                                    ▼
┌──────────────────┐                 ┌──────────────────┐                 ┌──────────────────┐
│      Entity      │                 │  ComponentStore  │                 │   CommandBuffer  │
│  • Generational  │                 │  • SparseSet/Arch│                 │  • LinearArena   │
│    Safe Handles  │                 │    ($O(1)$ Probes│                 │    Zero-Heap Ops │
│  • 32-bit Index  │                 │  • ErasedStore   │                 │  • Local Buffers │
│  • 32-bit Gen    │                 │    Zero Virtuals │                 │    Zero Contention│
└──────────────────┘                 └──────────────────┘                 └──────────────────┘
```

1. **Stale-Safe Generational Handles (`Entity`)** (`include/ecs/entity.hpp`):
   - Backed by `containers::generational_handle<entity_tag, std::uint32_t>`.
   - Comprises a 32-bit entity index and a 32-bit generation counter. Stale handles are detected in $O(1)$.
2. **Dense $O(1)$ Component Stores (`ComponentStore<C>`)** (`include/ecs/component_store.hpp`):
   - Stores components contiguously in a cache-aligned dense vector alongside sparse lookup tables.
   - Dispatched via `ErasedStore` compile-time function pointer table — **zero virtual methods, zero `std::shared_ptr` atomic refcounts**.
3. **Pluggable Policy Architecture** (`include/ecs/storage_policy.hpp`):
   - `StoragePolicy`: `SparseSetStoragePolicy` (default, dynamic composition) or `ArchetypeStoragePolicy` (columnar bulk SoA).
   - `AllocPolicy`: `ArenaAllocPolicy` (default linear arena) or `SystemAllocPolicy`.
   - `SchedulerPolicy`: `AutoSchedulerPolicy` (topological graph execution) or `ManualSchedulerPolicy`.
   - `SparsePolicy`: `PagedSparsePolicy` (on-demand 4KB page chunks) or `FlatSparsePolicy`.
4. **Auto-Lead-Store Dense Join View (`World::view`)** (`include/ecs/world.hpp`):
   - Iterates across the store containing the smallest number of active components (the *lead store*) and performs branch-free $O(1)$ direct sparse-index probes into companion component stores.
5. **Arena-Backed Command Buffer (`CommandBuffer`)** (`include/ecs/command_buffer.hpp`):
   - Records structural mutations (`spawn`, `despawn`, `add`, `remove`, `emplace`) into a `smriti::pools::LinearArena` bump allocator.
   - Zero `std::function` heap allocations. Thread-local recording with `LocalCommandBuffer`.
6. **Reactive Observers & Relations (`include/ecs/observer.hpp`, `include/ecs/relation.hpp`)**:
   - `OnAdd<C>` and `OnRemove<C>` hooks without virtual dispatch or `std::function` closures.
   - Typed entity relations (`ChildOf`, `Targets`, `MemberOf`) with cascading despawn.
7. **Topological System Scheduler (`include/ecs/scheduler.hpp`)**:
   - Analyzes `Reads<...>` and `Writes<...>` traits to order systems via `containers::topological_sort` on a `containers::LiteGraph`.

---

## 2. Quick Start Example

```cpp
#include <ecs/ecs.hpp>
#include <containers/numeric/math_vector.hpp>

struct Position { pebble::math::vec2 val; };
struct Velocity { pebble::math::vec2 val; };
struct Health   { float hp = 100.0f; };

pebble::ecs::World world;

// 1. Spawning entities and attaching components
auto player = world.spawn();
world.add<Position>(player, {.val = {100.0f, 150.0f}});
world.add<Velocity>(player, {.val = {12.0f, -4.0f}});
world.add<Health>(player, {.hp = 85.0f});

// 2. Reactive observers
world.on_add<Health>([](pebble::ecs::OnAdd<Health> ev) {
    // React to Health attachment
});

// 3. Dense join query over matching entities
world.view<Position, Velocity>([](pebble::ecs::Entity e, Position& pos, const Velocity& vel) {
    pos.val = pos.val + vel.val * 0.016f;
});

// 4. Deferred structural mutations via Arena CommandBuffer
world.view<Health>([&](pebble::ecs::Entity e, Health& h) {
    if (h.hp <= 0.0f) {
        world.commands().despawn(e);
    }
});
world.flush_commands(); // Deterministic sync point
```

---

## 3. Subsystem Integration

- **Gati Integration**: Gati binds `pebble::ecs::World` as its central state substrate, orchestrating components such as `Transform`, `MaterialComponent`, and `ElementalComponent`.
- **Spandana Integration**: Spandana timelines manipulate component properties directly via type-safe accessors and deferred command buffers.
- **Kalpana Separation**: `pebble::ecs` has **zero visual dependencies**; Kalpana extracts presentation state independently through read-only views during frame assembly.
