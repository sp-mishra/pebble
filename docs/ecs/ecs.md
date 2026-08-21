# pebble::ecs — High-Performance C++23 Standalone Entity-Component System

`pebble::ecs` is a modern, cache-coherent, header-only Entity-Component System engineered for extreme performance, minimal memory footprints, and zero runtime waste. It strictly follows Pebble's zero-overhead principle: **no virtual functions on hot paths, no RTTI, no macros, and zero heap allocation during queries**.

Include: `#include <ecs/ecs.hpp>`

---

## 1. Core Architecture

- **`Entity`** (`ecs/entity.hpp`): Stale-safe `containers::generational_handle<entity_tag, std::uint32_t>`. Uniquely identifies an entity and immediately invalidates upon despawn.
- **`ComponentStore<C>`** (`ecs/component_store.hpp`): Backed by `sparseset::SparseSet<std::uint32_t, C>`. Provides:
  - $O(1)$ component lookup, branch-free membership testing.
  - Contiguous, dense array layout for cache-friendly iterations and SIMD sweeps.
- **`World`** (`ecs/world.hpp`): Manages entity lifecycles, component stores, and query execution.
  - `w.view<A, B...>()`: Iterates the store with the smallest number of components and performs $O(1)$ sparse probes into the other stores.
  - `w.par_view<A, B...>(executor, fn)`: Chunks the iteration across Pravaha threads for multi-core parallelism.
- **`CommandBuffer`** (`ecs/command_buffer.hpp`): Thread-safe recording of structural mutations (`spawn`, `despawn`, `add`, `remove`) during system execution or parallel views, flushed at safe sync points.

---

## 2. Quick Start

```cpp
#include <ecs/ecs.hpp>
#include <containers/numeric/math_vector.hpp>

struct Position { pebble::math::vec2 val; };
struct Velocity { pebble::math::vec2 val; };

pebble::ecs::World world;

// 1. Spawning and adding components
auto e1 = world.spawn();
world.add(e1, Position{pebble::math::vec2(0.0f, 10.0f)});
world.add(e1, Velocity{pebble::math::vec2(1.0f, 0.0f)});

// 2. Dense join query
world.view<Position, Velocity>([](pebble::ecs::Entity e, Position& pos, const Velocity& vel) {
    pos.val = pos.val + vel.val * 0.016f;
});
```
