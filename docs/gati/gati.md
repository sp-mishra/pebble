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

## 2. Rigid Dynamics, Sequential Impulse & Multi-Body Architecture
- **Unified Simulation Facade (`gati::Simulation`)**: Single zero-configuration entry point orchestrating the 9-phase physics loop.
- **Sequential Impulse Solver (`gati::SequentialImpulseSolver`)**: Warm-started accumulated normal & friction impulses with Baumgarte stabilization.
- **Contact Manifold Cache (`gati::ContactCache`)**: Skips narrowphase queries for resting/stacked bodies; retains separating axes.
- **Island Partitioning & Sleeping (`gati::UnionFindIslands`)**: Partitions contact graphs with union-find, sleeping stable islands for 5–20× speedup on scenes with resting bodies.
- **Continuum Two-Way Coupling (`gati::DynamicCouplingBridge`)**: Coupled fluid buoyancy, drag, and boundary penalties with Prakriti.
- **Plug-and-Play `SimConfig`**: Fully concept-constrained compile-time policy composition with `::with_*` overrides.

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

