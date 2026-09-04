# Handles, Slot Maps, Registries & Content Stores

Pebble's associative handle and content storage subsystem (`include/containers/handle/`, `descriptor_registry.hpp`,
`content_store.hpp`) provides type-safe, generational identifier abstractions, $O (1)$ memory slot mapping, hash
descriptor routing, and immutable content-addressed storage.

---

## 1. Architectural Overview & Component Hierarchy

```
                          ASSOCIATIVE & HANDLE SUBSYSTEM
                          
  ┌────────────────────────────────────────────────────────────────────────────────────────┐
  │                           APPLICATIONS & ENGINES (ECS / Gati)                          │
  │     - Entity IDs & Component Handles                                                   │
  │     - Stale Pointer Safety & Generational Recycling                                    │
  │     - Content-Addressed Blob Storage & Asset Management                                │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                               CONTAINERS & REGISTRIES                                  │
  │                                                                                        │
  │   1. generational_handle<PhantomTag>                                                   │
  │      - 32-bit Index + 32-bit Generation counter packed into 64-bit integer              │
  │      - Phantom-typed at compile time to prevent accidental handle mixing               │
  │                                                                                        │
  │   2. slot_map<T, HandleTag>                                                            │
  │      - O(1) insertion, O(1) deletion, O(1) lookup                                      │
  │      - Cache-friendly contiguous dense array + generational redirection table          │
  │      - Stale-handle detection: accessing an erased handle returns std::nullopt         │
  │                                                                                        │
  │   3. descriptor_registry<T, PhantomTag>                                                │
  │      - FNV-1a 64-bit name hash to generational handle routing                          │
  │      - Fast string / symbol lookup returning type-safe handles                         │
  │                                                                                        │
  │   4. content_store                                                                     │
  │      - SHA-256 content-addressed immutable blob storage                                │
  │      - Memory-mapped file caching via utils::setu                                      │
  └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Memory Layout & Algorithmic Mechanics

### 2.1 Generational Slot Map Dual-Table Architecture

```
   Handles (64-bit: index + gen)
      Handle(idx=0, gen=1) ───┐
      Handle(idx=1, gen=3) ───┼──────────┐
                              │          │
   Slots (Indirection Table)  ▼          ▼
   ┌──────────────────────┬──────────┬──────────┬──────────┐
   │ slot_index           │ 0        │ 1        │ 2 (free) │
   │ generation           │ 1        │ 3        │ 4        │
   │ dense_index          │ 1        │ 0        │ -        │
   └──────────────────────┴──────────┴──────────┴──────────┘
                              ▲          ▲
                              │          │
   Dense (Contiguous RAM)     │          │
   ┌──────────────────────┬───┴──────┬───┴──────┐
   │ Payload T            │ Item B   │ Item A   │  <-- 100% Contiguous Iteration!
   │ Slot Back-Index      │ 1        │ 0        │
   └──────────────────────┴──────────┴──────────┘
```

When an element is deleted, the dense array is compacted by swapping the last element into the removed slot ($O (1)$
swap-and-pop), and the corresponding slot's generation counter is incremented. Old handles referencing the slot
immediately become invalid.

---

## 3. End-to-End API Examples

### 3.1 `generational_handle` & `slot_map`

```cpp
#include "containers/handle/generational_handle.hpp"
#include "containers/associative/slot_map.hpp"
#include <iostream>
#include <string>

struct MonsterTag {};
using MonsterHandle = containers::generational_handle<MonsterTag>;

struct Monster {
    std::string name;
    int health;
};

int main() {
    containers::slot_map<Monster, MonsterHandle> monsters;
    monsters.reserve(1000); // Bulk reservation support

    // 1. Insert and obtain generational handle
    MonsterHandle goblin = monsters.insert(Monster{"Goblin", 50});
    MonsterHandle dragon = monsters.insert(Monster{"Dragon", 5000});

    // 2. Safe O(1) Lookup
    if (auto* m = monsters.find(goblin)) {
        std::cout << "Monster: " << m->name << ", HP: " << m->health << "\n";
    }

    // 3. Erase
    monsters.erase(goblin);

    // 4. Stale Handle Safety (Returns nullptr)
    if (monsters.find(goblin) == nullptr) {
        std::cout << "Goblin handle successfully invalidated! No use-after-free.\n";
    }

    // 5. Dense Iteration across all live monsters
    for (auto ref : monsters) {
        std::cout << "Live Monster: " << ref.value.name << "\n";
    }
}
```

#### Memory Preallocation Guidelines: `slot_map::reserve`
- `slot_map::reserve(std::size_t cap)` preallocates the underlying `free_list_` index vector capacity to eliminate memory reallocations during high-frequency bulk insertion cycles.
- The underlying `slots_` container utilizes chunked `std::deque` storage to maintain stable memory addresses across insertions.

### 3.2 `descriptor_registry` Named & Stable ID Symbol Routing

`descriptor_registry` leverages a single-pass dual index (`SparseSet` for $O(1)$ cache-dense numeric lookups + hash table for string lookups):

```cpp
#include "containers/descriptor_registry.hpp"
#include <iostream>

enum class TextureCategory : std::uint32_t { Terrain, Character };

struct TextureDesc {
    static constexpr std::uint32_t stable_id = 10;
    static constexpr std::uint64_t name_hash = containers::desc_name_hash("tex_grass");
    static constexpr TextureCategory category = TextureCategory::Terrain;

    std::string path;
};

int main() {
    containers::descriptor_registry<TextureDesc> textures(1024); // Reserve universe capacity

    // Register descriptor (single-pass lookup & in-place update)
    TextureDesc grass{.path = "/assets/grass.png"};
    containers::descriptor_handle h_grass = textures.register_desc(grass);

    // Lookup by stable ID via O(1) branch-free SparseSet
    if (const TextureDesc* d = textures.find(10)) {
        std::cout << "Resolved Grass Path by ID: " << d->path << "\n";
    }

    // Lookup by name hash
    if (const TextureDesc* d = textures.find_by_name(containers::desc_name_hash("tex_grass"))) {
        std::cout << "Resolved Grass Path by Name: " << d->path << "\n";
    }

    // Query all by category
    auto terrain_list = textures.by_category(TextureCategory::Terrain);
    std::cout << "Found " << terrain_list.size() << " terrain textures.\n";
}
```

