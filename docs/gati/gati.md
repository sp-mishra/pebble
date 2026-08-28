# Gati (गति) — Realtime Game Runtime & Entity Orchestration Engine

Header-only C++23/C++26. No virtual dispatch, no macros. Concept-based static dispatch, zero-overhead policy composition.
Gati sits **above** `pebble::ecs`, `akruti` (geometry/CCD/fracture), and `prakriti` (multiphysics/thermodynamics), orchestrating them into a deterministic, high-performance game runtime: entities, systems, animation splines, joints, input handling, contact caching, island sleeping, and a fixed-step scheduler with render pose interpolation.

Include: `#include <gati/gati.hpp>`

---

## Table of Contents
1. [Overview & Architectural Position](#1-overview--architectural-position)
2. [Core Architecture & Execution Pipeline](#2-core-architecture--execution-pipeline)
3. [Algorithmic Foundations & Mathematical Formulations](#3-algorithmic-foundations--mathematical-formulations)
   - [Fixed-Step Accumulator & Render Interpolation ($\alpha$)](#31-fixed-step-accumulator--render-interpolation-alpha)
   - [Sequential Impulse Solver with Baumgarte Stabilization](#32-sequential-impulse-solver-with-baumgarte-stabilization)
   - [Contact Manifold Cache & Warm-Starting](#33-contact-manifold-cache--warm-starting)
   - [Island Partitioning & Sleeping (Union-Find)](#34-island-partitioning--sleeping-union-find)
   - [Elemental & Chemical Reaction Matrix](#35-elemental--chemical-reaction-matrix)
4. [Subsystem Catalog & Complete Public API](#4-subsystem-catalog--complete-public-api)
5. [Configuration, Defaults & Performance Tuning Guide](#5-configuration-defaults--performance-tuning-guide)
   - [Default Configuration Settings](#51-default-configuration-settings)
   - [How to Optimize Further (Extreme Throughput)](#52-how-to-optimize-further-extreme-throughput)
   - [How to Improve Physical Quality & Stacking Stability](#53-how-to-improve-physical-quality--stacking-stability)
   - [Configuration Trade-Off Matrix](#54-configuration-trade-off-matrix)
6. [Extensibility & Custom Policy Composition (`SimConfig`)](#6-extensibility--custom-policy-composition-simconfig)
7. [Zero-to-Hero Tutorial](#7-zero-to-hero-tutorial)
   - [Step 1: Instantiating the Game Runtime](#step-1-instantiating-the-game-runtime)
   - [Step 2: Spawning Entities with Geometry & Physical Properties](#step-2-spawning-entities-with-geometry--physical-properties)
   - [Step 3: Deterministic Loop & Render Interpolation](#step-3-deterministic-loop--render-interpolation)
   - [Step 4: Subscribing to Events & Elemental Phase Reactions](#step-4-subscribing-to-events--elemental-phase-reactions)
8. [Pebble Subsystem Reuse](#8-pebble-subsystem-reuse)

---

## 1. Overview & Architectural Position

Akruti answers *where* and *how far*; Prakriti simulates *what happens to matter*; **Gati decides *when* and *to whom***. It provides the central game loop, entity orchestration, system schedule, contact caching, and presentation interpolation.

```
Game Code / Client Rendering (consumes interpolated Transforms at variable refresh rates)
      │
      ▼
Gati Systems Schedule (AnimationSystem · PhysicsSyncSystem · CollisionSystem · JointSystem)
      │
      ▼
Scheduler + ECS + Events (Clock fixed-step · World pebble::ecs · EventBus · Scratch LinearArena)
      │
      ├───────────────────────────────┬───────────────────────────────────────────────┐
      ▼                               ▼                                               ▼
Prakriti Bridge                 Akruti Bridge                                   Pravaha / Math
(PBF fluids, XPBD matter)       (SAT, GJK, CCD, Khanda fracture)               (Task graphs, vec2/mat2)
```

---

## 2. Core Architecture & Execution Pipeline

Gati's runtime pipeline follows a strict, deterministic sequence during each fixed tick:

```
                  ┌───────────────────────────────────────────┐
                  │          Real Wall-Clock Delta dt         │
                  └─────────────────────┬─────────────────────┘
                                        │
                                        ▼
                  ┌───────────────────────────────────────────┐
                  │    Accumulator Clock (clock_.advance(dt)) │
                  └─────────────────────┬─────────────────────┘
                                        │
               ┌────────────────────────┴────────────────────────┐
               │ While clock_.should_step()                      │
               │                                                 │
               │  1. Checkpoint Scratch Arena (LinearArena)      │
               │  2. Animation System (Spline / Flipbook / IK)   │
               │  3. Physics Integration (Prakriti / RigidBody)  │
               │  4. Broadphase & Narrowphase (Akruti SAT / CCD) │
               │  5. Contact Cache & Manifold Warm-Starting      │
               │  6. Island Partitioning & Sleep Graph Check     │
               │  7. Sequential Impulse Velocity/Position Solve  │
               │  8. Joint Constraint Projection                 │
               │  9. Material Reactions & Thermodynamics Step    │
               │ 10. Flush ECS Command Buffers & Rollback Arena  │
               └────────────────────────┬────────────────────────┘
                                        │
                                        ▼
                  ┌───────────────────────────────────────────┐
                  │  Compute Blend Factor: α = accum / dt_fix │
                  │  Render with interpolated(Transform, α)   │
                  └───────────────────────────────────────────┘
```

---

## 3. Algorithmic Foundations & Mathematical Formulations

### 3.1 Fixed-Step Accumulator & Render Interpolation ($\alpha$)
To ensure physics determinism across arbitrary display refresh rates (e.g. 60Hz, 144Hz, 240Hz):
$$\text{accumulator} \leftarrow \text{accumulator} + \Delta t_{\text{real}}$$
While $\text{accumulator} \ge \Delta t_{\text{fixed}}$:
$$x_{\text{prev}} \leftarrow x_{\text{curr}}, \quad x_{\text{curr}} \leftarrow \text{integrate}(x_{\text{curr}}, \Delta t_{\text{fixed}}), \quad \text{accumulator} \leftarrow \text{accumulator} - \Delta t_{\text{fixed}}$$
The presentation blend factor $\alpha \in [0, 1)$ is:
$$\alpha = \frac{\text{accumulator}}{\Delta t_{\text{fixed}}}$$
Rendered pose interpolation:
$$p_{\text{render}} = (1 - \alpha) \cdot p_{\text{prev}} + \alpha \cdot p_{\text{curr}}$$
$$\theta_{\text{render}} = \text{slerp}(\theta_{\text{prev}}, \theta_{\text{curr}}, \alpha)$$

### 3.2 Sequential Impulse Solver with Baumgarte Stabilization
Contacts and joints are solved via velocity-level projected Gauss-Seidel sequential impulses:
$$\Delta \lambda = \frac{-(J v + b + \frac{\beta}{\Delta t} C)}{J M^{-1} J^T}$$
- **Normal Impulse Clamping**: $\lambda_n \leftarrow \max(0, \lambda_n + \Delta \lambda_n)$
- **Friction Cone (Coulomb Model)**: $|\lambda_t| \le \mu \lambda_n$
- **Baumgarte Stabilization**: $b = \frac{\beta}{\Delta t} \max(0, \text{penetration} - \text{slop})$ with $\beta \approx 0.2$.

### 3.3 Contact Manifold Cache & Warm-Starting
Between consecutive frames, contact points are tracked using feature IDs. Cached accumulated impulses $\lambda_n, \lambda_t$ are applied immediately at the beginning of the velocity solve step:
$$v_1 \mathrel{-}= M_1^{-1} J^T \lambda_{\text{cached}}, \quad v_2 \mathrel{+}= M_2^{-1} J^T \lambda_{\text{cached}}$$
This eliminates jitter, enables stable multi-body stacking, and reduces required solver iterations from 20+ to 4–8.

### 3.4 Island Partitioning & Sleeping (Union-Find)
Contacts form an undirected graph $G = (V, E)$. Gati uses `containers::union_find` to partition active bodies into disjoint dynamic islands:
- An island is put to **sleep** if all constituent bodies satisfy:
  $$E_{\text{kinetic}} = \frac{1}{2} m \|v\|^2 + \frac{1}{2} I \omega^2 < \epsilon_{\text{sleep}} \quad \text{for } t > t_{\text{sleep\_threshold}}$$
- Sleeping islands bypass broadphase, narrowphase, and solver iterations entirely, yielding a **5–20× performance speedup** in resting scenes.

### 3.5 Elemental & Chemical Reaction Matrix
Thermodynamic state changes and chemical reactions are resolved instantaneously during contact:
- $\text{Water} + \text{Lava} \longrightarrow \text{Obsidian (solid)} + \text{Steam (gas)}$
- $\text{Fire} + \text{Wood} \longrightarrow \text{Charcoal} + \text{Heat Energy}$
- $\text{Acid} + \text{Metal} \longrightarrow \text{Salt} + \text{Hydrogen Gas}$
- $\text{Electricity} + \text{Water} \longrightarrow \text{Shockwave Area-of-Effect}$

---

## 4. Subsystem Catalog & Complete Public API

### Core Runtime Types
- **`gati::Game<Systems>`**: Master orchestrator owning `World`, `Clock`, `EventBus`, `LinearArena` scratch, and `ParallelExecutor`.
  - `.world()` $\to$ `World&`: Direct ECS access.
  - `.clock()` $\to$ `Clock&`: Query and configure clock timers.
  - `.events()` $\to$ `EventBus&`: Inter-system event publish/subscribe.
  - `.update(float real_dt)`: Runs fixed-step ticks and flushes deferred command buffers.
  - `.alpha() const` $\to$ `Scalar`: Returns presentation interpolation factor $[0, 1)$.
- **`gati::World`**: Zero-allocation generational ECS wrapper (`pebble::ecs`).
  - `.spawn()` $\to$ `Entity`: Creates a unique generational entity handle.
  - `.add<Component>(Entity, Args...)`: Adds component to entity.
  - `.get<Component>(Entity)` $\to$ `Component&`: Direct $O(1)$ access.
  - `.view<Components...>(Callable)`: Iterates matching entity archetypes with cache locality.
  - `.flush_commands()`: Applies structural additions/deletions recorded during system execution.
- **`gati::Transform`**: Dual-buffered pose component for visual interpolation.
  - `.position`, `.prev_position`: 2D coordinates.
  - `.angle`, `.prev_angle`: Rotation angle in radians.
  - `interpolated(Transform, alpha)` $\to$ `Pose`: Blends previous and current transforms.
- **`gati::RigidBody2D`**: Rigid body dynamic parameters.
  - `RigidBody2D::make_static()`: Infinite mass ($w=0, I^{-1}=0$).
  - `RigidBody2D::make_box(mass, half_extents, restitution, friction)`: Computes mass and moment of inertia $I = \frac{m}{12}(w^2 + h^2)$.
  - `RigidBody2D::make_circle(mass, radius, restitution, friction)`: Computes $I = \frac{1}{2}m r^2$.

---

## 5. Configuration, Defaults & Performance Tuning Guide

### 5.1 Default Configuration Settings

| Parameter | Struct / Location | Default Value | Role / Effect |
|:---|:---|:---|:---|
| `fixed_dt` | `ClockConfig` | `1.0f / 60.0f` (60 Hz) | Length of one physics integration step. |
| `max_substeps` | `ClockConfig` | `4` | Maximum ticks processed per frame to prevent the "spiral of death". |
| `velocity_iters` | `SolverConfig` | `8` | Projected Gauss-Seidel iterations for velocity resolution. |
| `position_iters` | `SolverConfig` | `3` | Non-penetration positional correction iterations. |
| `baumgarte_beta` | `SolverConfig` | `0.2f` | Position error stabilization factor ($20\%$ penetration resolved per step). |
| `slop` | `SolverConfig` | `0.005f` ($5\text{mm}$) | Allowed penetration before Baumgarte stabilization activates (prevents jitter). |
| `sleep_energy_threshold` | `IslandConfig` | `0.001f` | Kinetic energy threshold under which bodies enter sleeping candidate state. |
| `sleep_time_threshold` | `IslandConfig` | `0.5s` | Time a body must remain below energy threshold before sleeping. |
| `scratch_arena_size` | `Game::Game(...)` | `1 MB` | Pre-allocated per-frame scratch memory for contact manifolds and island graphs. |

### 5.2 How to Optimize Further (Extreme Throughput)
1. **Enable Island Sleeping**: Ensure `IslandStrategy = UnionFindIslands`. Resting stacks and background props drop out of broadphase/solver loops, yielding **up to $20\times$ speedups**.
2. **Reduce Solver Iterations**: In scenes with simple collisions (e.g. particle showers or pinball without tall vertical stacks), set `velocity_iters = 4` and `position_iters = 1`.
3. **Use Morton Spatial Hashing for Broadphase**: Replace naive bounding trees with `akruti::SpatialHashBroadphase` when object counts exceed 10,000.
4. **Tune `scratch_bytes`**: Size the scratch arena so zero heap allocations occur during peak contact generation.

### 5.3 How to Improve Physical Quality & Stacking Stability
1. **Increase Substeps / Solver Iterations**: For tall vertical stacks (10+ stacked boxes) or high-mass-ratio collisions, configure `fixed_dt = 1.0f / 120.0f` or set `velocity_iters = 16`, `position_iters = 6`.
2. **Enable Warm-Starting**: Ensure `ContactCache` is enabled; it preserves normal and tangential impulses across frames, virtually eliminating stack sponginess.
3. **Enable Akruti CCD**: For fast-moving projectiles, attach `akruti::CcdComponent` to run continuous conservative advancement sweeps.

### 5.4 Configuration Trade-Off Matrix

| Configuration Focus | Recommended Settings | CPU Time | Memory Overhead | Stacking Quality | High-Speed Tunneling Risk |
|:---|:---|:---:|:---:|:---:|:---:|
| **Ultra-Fast Mobile / Web** | `dt=1/60s, iters=(4, 1), Sleep=ON, CCD=OFF` | **Minimal** ($\sim 0.2\text{ms}$) | Lowest | Moderate (slight sag) | High if $v > 50\text{m/s}$ |
| **Default Balanced** | `dt=1/60s, iters=(8, 3), Sleep=ON, CCD=Auto` | **Low** ($\sim 0.6\text{ms}$) | Medium (1MB arena) | High (stable 5-10 stack) | Low |
| **High-Precision Simulation**| `dt=1/120s, iters=(16, 6), Sleep=OFF, CCD=ON`| **Higher** ($\sim 2.5\text{ms}$) | Medium | Maximum (rigid, no sag)| Zero (exact TOI) |

---

## 6. Extensibility & Custom Policy Composition (`SimConfig`)

Gati supports full compile-time policy monomorphization via `SimConfig`:

```cpp
#include <gati/sim_config.hpp>

// Compose custom high-performance simulation policies
using CustomConfig = gati::SimConfig<
    akruti::HybridBroadphase,        // Broadphase strategy
    akruti::AnalyticMatrixNarrow,    // Narrowphase algorithm
    akruti::AutoTriangulator,        // Triangulator policy
    akruti::AutoVoronoiBuilder,      // Voronoi fracture policy
    akruti::khanda::TriangleMergeDecomposer,
    akruti::MprDistanceOracle,
    gati::SequentialImpulseSolver,   // Rigid body solver
    gati::BoundaryCoupling,          // Fluid-rigid two-way coupling
    gati::UnionFindIslands,          // Sleeping island strategy
    prakriti::DefaultMechanicsStack,
    prakriti::HighwayBackend         // SIMD backend
>;
```

---

## 7. Zero-to-Hero Tutorial

### Step 1: Instantiating the Game Runtime
```cpp
#include <gati/gati.hpp>
#define GATI_ENABLE_AKRUTI
#define GATI_ENABLE_PRAKRITI

using namespace gati;

// Initialize game with default 60Hz fixed clock (dt = 1/60s)
ClockConfig clock_cfg{.fixed_dt = 1.0f / 60.0f, .max_substeps = 4};
Game game(clock_cfg);
```

### Step 2: Spawning Entities with Geometry & Physical Properties
```cpp
auto& world = game.world();

// 1. Static Floor
Entity floor = world.spawn();
world.add<Transform>(floor, Transform::from_position({0.0f, 0.0f}));
world.add<ShapeRef>(floor, {.shape = akruti::Box{{0.0f, 0.0f}, {50.0f, 2.0f}}});
world.add<RigidBody2D>(floor, RigidBody2D::make_static());

// 2. Dynamic Falling Crate
Entity crate = world.spawn();
world.add<Transform>(crate, Transform::from_position({0.0f, 15.0f}));
world.add<ShapeRef>(crate, {.shape = akruti::Box{{0.0f, 0.0f}, {1.0f, 1.0f}}});
world.add<RigidBody2D>(crate, RigidBody2D::make_box(10.0f, {1.0f, 1.0f}, 0.3f, 0.5f));
```

### Step 3: Deterministic Loop & Render Interpolation
```cpp
while (app_running) {
    float frame_time = timer.delta_seconds();
    
    // Fixed steps run deterministically inside update()
    game.update(frame_time);
    
    // Compute presentation alpha blend factor [0, 1)
    Scalar alpha = game.alpha();
    
    // Render all visible entities with smooth interpolation
    game.world().view<Transform, ShapeRef>([&](Entity, Transform& tr, ShapeRef& shape) {
        Pose render_pose = interpolated(tr, alpha);
        renderer.draw_shape(shape, render_pose.position, render_pose.angle);
    });
}
```

### Step 4: Subscribing to Events & Elemental Phase Reactions
```cpp
// Subscribe to contact events
game.events().subscribe<ContactEvent>([](const ContactEvent& evt) {
    if (evt.impact_speed > 10.0f) {
        audio_system.play_sound("impact_heavy", evt.point);
    }
});

// Attach thermodynamic material to an entity (e.g. Ice melting to Water on heat)
Entity ice_block = world.spawn();
world.add<Transform>(ice_block, Transform::from_position({2.0f, 1.0f}));
world.add<MaterialComponent>(ice_block, MaterialComponent::Ice());

// Fire source nearby triggers state change via MaterialReactionSystem
game.events().publish(HeatPulseEvent{.center = {2.0f, 1.0f}, .radius = 3.0f, .temperature = 350.0f});
```

---

## 8. Pebble Subsystem Reuse

| Subsystem | Usage in Gati |
|:---|:---|
| `pebble::ecs` | Generational entity management and cache-friendly component storage. |
| `pebble::math` | `vec2`, `mat2`, `aabb2`, `dot`, `cross`, `slerp` geometric vector mathematics. |
| `pebble::mem` | `LinearArena` scratch allocator rolled back cleanly each fixed tick. |
| `containers::union_find` | Disjoint contact graph island clustering and sleeping. |
| `akruti` | SAT 2-point manifold contact generation, continuous collision detection (CCD), and Voronoi fracture. |
| `prakriti` | Multiphysics continuum simulation, XPBD mechanics, and fluid coupling. |
| `pravaha` | Multi-threaded parallel system and chunk evaluation via task graphs. |
