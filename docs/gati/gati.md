# Gati — Header-Only Realtime Game Runtime

Header-only C++23/C++26. No virtual, no macros. Concept-based static dispatch, zero-overhead policy composition. Sits **above** `pebble::ecs`, `akruti` (geometry) and `prakriti` (dynamics) and binds them into a deterministic game loop — entities, systems, animation, joints, input, and a fixed-step scheduler with render interpolation.

Include: `#include <gati/gati.hpp>`

---

## 1. Overview & Architecture

Gati (गति — "motion / gait") turns geometry and physics libraries into a playable engine. Akruti answers *where* and *how far*; Prakriti decides *what happens* to matter; **Gati decides *when* and *to whom*** — it owns the game clock, entity orchestration, system schedule, and presentation interpolation.

```
Game code                    scenes, gameplay rules, rendering (consumes interpolated Transforms)
      │
      v
Gati systems                 AnimationSystem · PhysicsSyncSystem · CollisionSystem · JointSystem
      │
      v
Scheduler + ECS + Events     Clock (fixed-step + alpha) · World (pebble::ecs) · RingBuffer queues · Pravaha
      │
      ├───────────────┬───────────────────────────────────────────────
      v               v
Prakriti bridge     Akruti bridge          dynamics (particles/edges/solvers) · geometry (SDF/GJK/CCD)
      │               │
      v               v
Prakriti            Akruti                 the two independent engines (unaware of Gati)
```

---

## 2. Direct Pebble Math & Geometry Integration

Gati completely avoids math duplication or conversion layers. It uses `pebble::math` types directly:
- `gati::Vec2` = `pebble::math::vec2`
- `gati::Mat2` = `pebble::math::mat2`
- `gati::AABB` = `pebble::math::aabb2`

When `GATI_ENABLE_AKRUTI` is active, Gati reuses Akruti for:
- **Broadphase + Narrowphase Collision**: `AABBTree` fat-margin spatial indexing with `akruti::gjk_overlap` and `akruti::epa` contact manifold generation.
- **Rich 2D Primitives in `ShapeRef`**: `akruti::Circle`, `akruti::Box`, `akruti::Capsule`, `akruti::OrientedBox` (OBB), `akruti::Triangle`, `akruti::RoundedBox`, `akruti::Sector` (FOV cones), `akruti::ConvexPoly<8>`, `akruti::ChainShape<16>` (terrain/tilemaps), and `akruti::GridSDF<16, 16>` (discrete raster SDFs).
- **Continuous Collision Detection (CCD)**: `gati::sweep_test()` utilizing `akruti::time_of_impact()` to prevent tunneling for fast-moving projectiles.
- **Raycasting**: `gati::raycast()` broadphase acceleration delegating exact intersection to `akruti::raycast()`.
- **Contact Lifecycle Management (`ContactStateTracker`)**: Dispatches stateful `ContactPhase::Enter`, `ContactPhase::Stay`, and `ContactPhase::Exit` events across frames.
- **Continuous Material Reactions**: Automatic thermal diffusion, contact fusion, and brittle fracture evaluation during fixed steps.

---

## 3. Quick Start

```cpp
#include <gati/gati.hpp>
#define GATI_ENABLE_PRAKRITI
#define GATI_ENABLE_AKRUTI

using namespace gati;

Game game;

Entity player = game.world().spawn();
game.world().add<Transform>(player, {.position = pebble::math::vec2(0.0f, 10.0f)});
game.world().add<ShapeRef>(player, {.shape = akruti::Circle{{0.0f, 0.0f}, 1.0f}});

while (running) {
    game.update(real_dt);                  // Runs 0..N fixed steps, flushes deferred commands
    Scalar alpha = game.alpha();           // [0, 1) presentation blend factor
    
    // Render using interpolated pose
    game.world().view<Transform>([&](Entity, Transform& tr) {
        Pose p = interpolated(tr, alpha);
        draw_sprite(p.position, p.angle);
    });
}
```

