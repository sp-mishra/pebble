# pebble::ecs — High-Performance C++23 Standalone Entity-Component System

`pebble::ecs` is a modern, cache-coherent, header-only Entity-Component System engineered for extreme performance, minimal memory footprints, and zero runtime waste. It strictly follows Pebble's zero-overhead principle: **no virtual functions on hot paths, no RTTI, no macros, and zero heap allocation during queries**.

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
│  • Generational  │                 │  • SparseSet     │                 │  • Structural    │
│    Safe Handles  │                 │    ($O(1)$ Probes│                 │    Deferred Ops  │
│  • 32-bit Index  │                 │  • Contiguous    │                 │  • Thread-Safe   │
│  • 32-bit Gen    │                 │    Dense Array   │                 │    Sync Flushes  │
└──────────────────┘                 └──────────────────┘                 └──────────────────┘
```

1. **Stale-Safe Generational Handles (`Entity`)** (`include/ecs/entity.hpp`):
   - Backed by `containers::generational_handle<entity_tag, std::uint32_t>`.
   - Comprises a 32-bit entity index and a 32-bit generation counter. When an entity is despawned, its generation increments, immediately rendering dangling references harmless.
2. **Dense $O(1)$ Sparse Set Component Stores (`ComponentStore<C>`)** (`include/ecs/component_store.hpp`):
   - Stores components contiguously in a cache-aligned dense vector alongside an internal sparse lookup table (`sparseset::SparseSet<std::uint32_t, C>`).
   - Guarantees $O(1)$ component insertion, erasure, and lookup without pointer indirection.
   - Dense contiguous layout ensures direct hardware prefetching and SIMD vectorization sweeps.
3. **Multi-Store Dense Join View (`World::view`)** (`include/ecs/world.hpp`):
   - Iterates across the store containing the smallest number of active components (the *lead store*) and performs branch-free $O(1)$ direct sparse-index probes into companion component stores.
   - Zero heap allocation throughout query lifetimes.
4. **Multi-Threaded Parallel Views (`World::par_view`)**:
   - Leverages `pebble::pravaha` task chunking to parallelize component processing across hardware worker threads with zero lock contention.
5. **Thread-Safe Deferred Mutations (`CommandBuffer`)** (`include/ecs/command_buffer.hpp`):
   - Records structural mutations (`spawn`, `despawn`, `add`, `remove`) during system execution or parallel views, flushed deterministically at frame sync boundaries.

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

// 2. Dense join query over matching entities
world.view<Position, Velocity>([](pebble::ecs::Entity e, Position& pos, const Velocity& vel) {
    pos.val = pos.val + vel.val * 0.016f;
});

// 3. Deferred structural mutations via CommandBuffer
pebble::ecs::CommandBuffer cmd(world);
world.view<Health>([&](pebble::ecs::Entity e, Health& h) {
    if (h.hp <= 0.0f) {
        cmd.despawn(e);
    }
});
cmd.flush(); // Deterministic sync point
```

---

## 3. Subsystem Integration

- **Gati Integration**: Gati binds `pebble::ecs::World` as its central state substrate, orchestrating components such as `Transform`, `MaterialComponent`, and `ElementalComponent`.
- **Spandana Integration**: Spandana timelines manipulate component properties directly via type-safe accessors and deferred command buffers.
- **Kalpana Separation**: `pebble::ecs` has **zero visual dependencies**; Kalpana extracts presentation state independently through read-only views during frame assembly.

