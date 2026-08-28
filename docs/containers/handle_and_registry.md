# Handles, Slot Maps, Registries & Content Stores

Pebble's associative handle and content storage subsystem (`include/containers/handle/`, `descriptor_registry.hpp`, `content_store.hpp`) provides type-safe, generational identifier abstractions, $O(1)$ memory slot mapping, hash descriptor routing, and immutable content-addressed storage.

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

When an element is deleted, the dense array is compacted by swapping the last element into the removed slot ($O(1)$ swap-and-pop), and the corresponding slot's generation counter is incremented. Old handles referencing the slot immediately become invalid.

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
    containers::slot_map<Monster, MonsterTag> monsters;

    // 1. Insert and obtain generational handle
    MonsterHandle goblin = monsters.insert({"Goblin", 50});
    MonsterHandle dragon = monsters.insert({"Dragon", 5000});

    // 2. Safe O(1) Lookup
    if (auto* m = monsters.get(goblin)) {
        std::cout << "Monster: " << m->name << ", HP: " << m->health << "\n";
    }

    // 3. Erase
    monsters.erase(goblin);

    // 4. Stale Handle Safety (Returns nullptr)
    if (monsters.get(goblin) == nullptr) {
        std::cout << "Goblin handle successfully invalidated! No use-after-free.\n";
    }

    // 5. Contiguous Dense Iteration across all live monsters
    for (const Monster& m : monsters.dense()) {
        std::cout << "Live Monster: " << m.name << "\n";
    }
}
```

### 3.2 `descriptor_registry` Named Symbol Routing
```cpp
#include "containers/descriptor_registry.hpp"
#include <iostream>

struct TextureTag {};
using TextureHandle = containers::generational_handle<TextureTag>;

int main() {
    containers::descriptor_registry<std::string, TextureTag> textures;

    // Register textures by string name
    TextureHandle h_grass = textures.register_named("tex_grass", "/assets/grass.png");
    TextureHandle h_stone = textures.register_named("tex_stone", "/assets/stone.png");

    // Lookup handle by name with FNV-1a hash
    if (auto h = textures.find_handle("tex_grass")) {
        std::cout << "Resolved Grass Path: " << *textures.get(*h) << "\n";
    }
}
```
