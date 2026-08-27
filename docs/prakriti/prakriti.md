# Prakriti — Unified Material-State 2D Physics Engine

Header-only C++23/C++26. No virtual, no macros. Zero-overhead policy composition. macOS-first (Apple Silicon,
SIMD-friendly SoA). `#include <prakriti/prakriti.hpp>`.

## Table of Contents
1. [Overview](#1-overview)
2. [Quick Start](#2-quick-start)
3. [Architecture](#3-architecture)
4. [Core Math](#4-core-math)
5. [State Layer (SoA)](#5-state-layer-soa)
6. [Material Law Layer](#6-material-law-layer)
7. [Spatial Hash](#7-spatial-hash)
8. [Solvers](#8-solvers)
   - [Algorithms Used](#algorithms-used)
9. [Simulation Loop](#9-simulation-loop)
10. [Mathematical Formulations](#10-mathematical-formulations)
11. [Extensibility](#11-extensibility)
12. [Pebble Subsystem Reuse](#12-pebble-subsystem-reuse)

---

## 1. Overview

Prakriti is a hybrid particle-field continuum simulator. Matter is a set of Lagrangian material
particles carrying thermodynamic, mechanical, and chemical state. Macroscopic behaviour — shatter,
bend, melt, flow, expand, solidify — is an **emergent** result of explicit constitutive *Material
Laws*, not hardcoded object classes. XPBD is one numerical solver among several, decoupled from
material identity.

**Principle:** geometry is an observable of physical state, never the state itself.

2D first; the layer contracts are dimension-agnostic, so a 3D extension changes only the math
primitives (`vec2`→`vec3`, `mat2`→`mat3`) and the spatial-hash cell dimensionality.

## 2. Quick Start

```cpp
#include <prakriti/prakriti.hpp>
#include <containers/numeric/math_vector.hpp>
using namespace prakriti;

WorldConfig cfg;
cfg.bounds = {{-50.0f, 0.0f}, {50.0f, 100.0f}};          // floor at y = 0
World<> w(cfg);                                           // default material law + solver stack

auto steel = w.materials().add(MaterialRegistry::steel());
Index a = w.particles().add({.position = {0.0f, 40.0f}, .mass = 0, .material = steel}); // anchor
Index b = w.particles().add({.position = {1.0f, 40.0f}, .material = steel});
w.edges().add(a, b, 1.0f);                                // rigid bond, rest length 1

for (int frame = 0; frame < 120; ++frame) w.step();
```

`World<>` is fully defaulted and works out of the box. `w.kinetic_energy()` exposes a diagnostic
scalar for external telemetry (e.g. NADI).

## 3. Architecture

Strict downward dependency. Solvers never define physics — they consume coefficients handed down by
the Material Law Layer, keeping numerical method orthogonal to material behaviour.

```
Application            scene setup, exporters
      │
      v
Material Law Layer     state -> coefficients (compliance α, viscosity μ, target ρ, yield ε, EOS P)
      │
      v
Simulation Control     substep scheduler · spatial hash · accumulators   (engine.hpp)
      │
      v
Multi-Physics Solvers  thermal · XPBD mechanics · density/pressure · damage/plasticity
      │
      v
Hardware Layer         SoA columns · chunk-friendly loops (Google Highway SIMD / Pravaha task graphs)
```

## 4. Core Math

Prakriti directly reuses Pebble's core stack-allocated static tensor linear algebra primitives from
`<containers/numeric/math_vector.hpp>`:

| Type | Surface |
|------|---------|
| `pebble::math::vec2` | `+ - * /`, `dot`, `cross`, `length`/`length_sq`, `perp`, `normalize` (safe, no NaN) |
| `pebble::math::mat2` | `*` (mat·mat, mat·vec via `mul`), `determinant`, `transpose`, `inverse`, `rotation2d` |
| `pebble::math::aabb2` | `contains`, `overlaps`, `expand`, `clamp`, `center`, `extent` |

`core/config.hpp` centralises every tunable in structs (`WorldConfig`, `FluidConfig`, `ThermalConfig`,
`ObstacleConfig`, `JointConfig`) — no magic numbers in solver bodies. `using Scalar = float`.

## 5. State Layer (SoA)

Pure Structure-of-Arrays: one contiguous column per attribute, index == entity id. Vec2-valued
state is stored as **split scalar columns** (`pos_x[]`/`pos_y[]` separately), not an interleaved
`vector<vec2>` — stride-1 float columns autovectorize and feed the SIMD backends directly, with no shuffle.

- **`MaterialRegistry`** (`state/material_registry.hpp`) — immutable per-material constants
  (`MaterialParams`): densities, heat capacity/conductivity, melt/boil, latent heats, yield/ultimate
  strain, Young's modulus, per-phase compliance `alpha[4]` and viscosity `visc[4]`, EOS constants.
  Presets: `steel()`, `water()`.
- **`ParticleStore`** (`state/particle_store.hpp`) — runtime columns: `pos_x/pos_y`,
  `pred_x/pred_y`, `vel_x/vel_y`, `inv_mass`, `temperature`, `internal_energy`, `pressure`,
  `density`, four phase fractions, `damage`, `material`. `add(ParticleDesc)` takes `pebble::math::vec2`
  position/velocity and unpacks into columns; `mass == 0` ⇒ static (infinite mass).
  `pos_v(i)`/`pred_v(i)`/`vel_v(i)` return a `pebble::math::vec2` value for irregular solvers.
- **`EdgeStore`** (`state/edge_store.hpp`) — structural bond graph: `a`, `b`, `rest_len` (plastically
  mutable), `strain`, `damage`, `active`. `compact()` removes fractured edges.

## 6. Material Law Layer

- **`phase.hpp`** — continuous 4-fraction model `{solid, plastic, liquid, gas}` summing to 1.
  `phase_from_temperature()` maps temperature through smooth melt/boil/sublimation ramps; `phase_blend()` is the
  barycentric mix used for all effective coefficients.
- **`eos.hpp`** — `tait_pressure()`: `P = B((ρ/ρ₀)^γ − 1) + R·f_gas·T`, optional negative clamp for
  free surfaces.
- **`constitutive.hpp`** — the `MaterialLaw` concept + `DefaultMaterialLaw`:
  `effective_compliance`, `effective_viscosity`, `structural_alpha` (α/(1−D)), `target_density`
  (gas lowers it). Static polymorphism — `World` is templated on the law.

## 7. Spatial Hash

`core/spatial_hash.hpp` — uniform grid over predicted positions. Cell-list built by counting sort into
contiguous arrays for cache-coherent iteration; rebuilt each frame. `for_each_neighbor(px, py, radius, fn)`
scans the 3×3 cell block (radius ≤ cell size).

## 8. Solvers

Decoupled policies satisfying `PhysicsSolver`. Each reads material-derived coefficients and mutates
state columns via a `SolverContext<Law>`.

| Solver | Role |
|--------|------|
| `ThermalSolver` (`thermal.hpp`) | graph-Laplacian heat diffusion; latent-heat plateaus; recomputes phase fractions |
| `XpbdSolver` (`xpbd.hpp`) | compliant distance-constraint projection on edges; compliance from material law, inflated by damage |
| `DensitySolver` (`density.hpp`) | Position-Based Fluids density projection on liquid particles; anti-clustering scorr; EOS pressure |
| `DamageSolver` (`damage.hpp`) | strain→damage accumulation; plastic rest-length mutation; fracture (D≥1) + compaction; connected-component **islands** via `containers/union_find.hpp` (`track_islands`, `rebuild_islands`) |
| `JointSolver` (`joint.hpp`) | XPBD positional joint projection over `akruti::Joint` (distance/revolute/prismatic/weld). Opt-in (`akruti`-guarded) |
| `ObstacleSolver` (`obstacle.hpp`) | static/kinematic obstacle collision projection on predicted positions over `akruti::Shape` (restitution + friction) |

SPH kernels (2D poly6 / spiky gradient) in `solvers/kernels.hpp`. `JointSolver` and `ObstacleSolver` are opt-in — not in the default `World<>` stack.

---

## Algorithms Used

| Concern | Algorithm | Where |
|---|---|---|
| Mechanics | XPBD — extended position-based dynamics, compliant distance constraints, small-step substepping | `solvers/xpbd.hpp` |
| Fluids | Position-Based Fluids (Macklin & Müller 2013) density projection + `scorr` anti-clustering | `solvers/density.hpp` |
| SPH kernels | poly6 density kernel, spiky gradient kernel (2D) | `solvers/kernels.hpp` |
| Heat | Graph-Laplacian explicit diffusion + latent-heat plateau buffering | `solvers/thermal.hpp` |
| Damage/fracture | Strain-driven damage accumulation, plastic rest-length mutation, `D≥1` fracture | `solvers/damage.hpp` |
| Connectivity | Union-find connected components (fracture islands) | `solvers/damage.hpp` + `containers/union_find.hpp` |
| Broad-phase | Uniform-grid spatial hash built by counting sort; 3×3 cell-block neighbor scan | `core/spatial_hash.hpp` |
| Pressure/EOS | Tait equation of state `P = B((ρ/ρ₀)^γ − 1) + R·f_gas·T` | `material/eos.hpp` |
| Phase | 4-fraction barycentric phase blend from temperature ramps | `material/phase.hpp` |
| Obstacles | Akruti SDF shape non-penetration projection + Coulomb friction & restitution | `solvers/obstacle.hpp` |
| Joints | XPBD positional joint constraints (akruti-driven) | `solvers/joint.hpp` |
| Column sweeps | 3 interchangeable backends: scalar autovectorized / Google Highway SIMD / Pravaha task graph | `compute/` |

---

## 9. Simulation Loop

`World::step()` runs `substeps` iterations of `dt_sub = dt / substeps` (small-steps XPBD for
stiffness/stability). Each substep:

1. **External accumulation** — `v += gravity·dt_sub`; inject ambient/emitter heat.
2. **Thermal + phase** — diffusion pass; recompute phase fractions and thermodynamic quenching (e.g. Magma + Water $\implies$ Obsidian).
3. **Predict motion** — `x_pred = x + v·dt_sub`.
4. **Build neighborhoods** — 2D Morton Z-order spatial hash over `x_pred`.
5. **Mechanics solve loop** (`solver_iters`) — XPBD with `kosha::LRUCache` warm-starting → PBF density with interfacial tension → obstacle CCD continuous collision detection.
6. **Damage + plasticity** — strain → damage; mutate `L0`; fracture/relink.
7. **Velocity update + commit** — `v = (x_pred − x)/dt_sub`, viscous damping from `μ_eff`; `x = x_pred`.

Anti-tunneling is achieved by substepping plus speculative Continuous Collision Detection (CCD) ray sweeps on obstacle SDFs.

## 10. Mathematical Formulations

- **Phase blend:** `α_eff = Σ f_φ α_φ`, `μ_eff = Σ f_φ μ_φ`, `Σ f_φ = 1`.
- **EOS:** `P_i = B((ρ_i/ρ₀)^γ − 1) + R·f_gas·T_i` (unrolled $\gamma=7$ integer powers).
- **XPBD distance:** `Δλ = (−C − α̃λ)/(w_a + w_b + α̃)`, `Δx_a = +w_a ∇C Δλ`, `α̃ = compliance/dt²` with Kosha LRU warm-starting.
- **PBF density:** `C_i = ρ_i/ρ₀ − 1`, `λ_i = −C_i/(Σ|∇C|² + ε)`, `Δx_i = (1/ρ₀) Σ(λ_i+λ_j+scorr)∇W - n_inter (0.04 h)`.
- **Damage:** `ε = (‖x_a−x_b‖ − L0)/L0`, `ΔD = ((ε−ε_y)/ε_u)^β` for `ε > ε_y`, `α_struct = α_base/(1−D)`,
  `D ≥ 1 ⇒ fracture`.
- **Thermal:** `ΔT_i = (k/c)·Σ_j w_ij (T_j − T_i)·dt`; latent heat buffers ΔE at transition plateaus.

## 11. Extensibility

| Add | How |
|-----|-----|
| Material | register `MaterialParams` (e.g. `steel`, `water`, `dry_ice`, `magma`, `obsidian`) — zero code change |
| Material law | implement the `MaterialLaw` concept — template argument to `World` |
| Solver | implement `PhysicsSolver` — add to a `SolverStack<...>` tuple |
| Compute backend | implement the `ComputeBackend` concept — second template argument to `World` |
| 3D | swap `vec2`→`vec3`, `mat2`→`mat3`, grid 2D→3D; layer contracts unchanged |

### ComputeBackend — Swappable Execution Tiers

`compute/compute_backend.hpp` defines the `ComputeBackend` concept: six stride-1 column primitives
(`axpy_const_masked`, `predict`, `sub_scale`, `mul_col`, `copy`, `clamp`) that express every
**uniform per-particle sweep** in the substep:

```cpp
template <MaterialLaw Law = DefaultMaterialLaw, ComputeBackend CB = ScalarBackend> class World;
```

Three interchangeable tiers, all producing identical physics (parity-tested to 1e-4):

| Tier | Backend | Header | Description |
|------|---------|--------|-------------|
| 1 | `ScalarBackend` (default) | `compute/scalar_backend.hpp` | Plain stride-1 loops over split columns; autovectorizes; zero dependencies. |
| 2 | `HighwayBackend` | `compute/highway_backend.hpp` | Google Highway portable SIMD (NEON on Apple Silicon, AVX2/AVX-512 on x86) with `simd_sph_poly6`. |
| 3 | `PravahaBackend` | `compute/pravaha_backend.hpp` | Pravaha parallel task graph execution across CPU cores for large particle batches. |

## 12. Pebble Subsystem Reuse

| Concern | Reused Pebble Subsystem |
|---|---|
| **Linear Algebra** | `pebble::math` (`containers/numeric/math_vector.hpp`) — `vec2`, `mat2`, `aabb2`, `dot`, `cross`, `distance`, `normalize`, etc. |
| **Cache (Kosha)** | `kosha::LRUCache` (`containers/cache/kosha.hpp`) — active contact manifold cache and XPBD multiplier warm-starting. |
| **Lock-Free Containers** | `include/containers/lockfree/` (`RingBuffer`, `MPMCQueue`, `AtomicStack`) — thread synchronization and event streams. |
| **Connected Components** | `containers::union_find` (`containers/union_find.hpp`) — used in `DamageSolver` island tracking. |
| **Rigid Shapes & SDFs** | `akruti` (`akruti/akruti.hpp`) — used in `ObstacleSolver` for rigid boundary contact and CCD ray sweeps. |
| **Joint Kinematics** | `akruti::Joint` (`akruti/joint.hpp`) — used in `JointSolver` for positional XPBD constraint projection. |
| **Multi-Core Tasking** | `pravaha` (`pravaha/pravaha.hpp`) — 4-color checkerboard domain partitioning and parallel batching. |
| **Vector Engine (Kalpana)** | `kalpana::InstancedParticlePipeline` (`kalpana/backend/instanced_pipeline.hpp`) — single draw-call GPU instancing. |
| **Telemetry** | `observability/nadi.hpp` — easily consumes `w.kinetic_energy()` and state columns. |
