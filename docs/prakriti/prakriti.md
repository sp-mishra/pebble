# Prakriti (प्रकृति) — Unified Material-State 2D Physics Engine

Header-only C++23/C++26. No virtual dispatch, no macros. Zero-overhead policy composition. macOS-first (Apple Silicon NEON, SIMD-friendly SoA columns).
Prakriti is a hybrid particle-field Lagrangian continuum dynamics simulator where macroscopic phenomena — melting, boiling, fracture, plastic bending, Navier-Stokes fluid flow, and rigid collision — **emerge naturally** from explicit thermodynamic, mechanical, and constitutive *Material Laws*.

Include: `#include <prakriti/prakriti.hpp>`

---

## Table of Contents
1. [Overview & Emergent Physics Philosophy](#1-overview--emergent-physics-philosophy)
2. [Subsystem Architecture & Layer Contracts](#2-subsystem-architecture--layer-contracts)
3. [Algorithmic Foundations & Mathematical Formulations](#3-algorithmic-foundations--mathematical-formulations)
   - [Continuous 4-Fraction Thermodynamics & Phase Transitions](#31-continuous-4-fraction-thermodynamics--phase-transitions)
   - [Constitutive Laws & Tait Equation of State (EOS)](#32-constitutive-laws--tait-equation-of-state-eos)
   - [Extended Position-Based Dynamics (XPBD) & Kosha Warm-Starting](#33-extended-position-based-dynamics-xpbd--kosha-warm-starting)
   - [Position-Based Fluids (PBF) & Interfacial Tension](#34-position-based-fluids-pbf--interfacial-tension)
   - [Graph-Laplacian Explicit Thermal Diffusion](#35-graph-laplacian-explicit-thermal-diffusion)
   - [Strain-Driven Plasticity, Fracture & Island Connectivity (Union-Find)](#36-strain-driven-plasticity-fracture--island-connectivity-union-find)
4. [State Layer & Stride-1 Split Columns (SoA)](#4-state-layer--stride-1-split-columns-soa)
5. [Master Subsystem Catalog & Public API](#5-master-subsystem-catalog--public-api)
6. [Configuration, Defaults & Performance Tuning Guide](#6-configuration-defaults--performance-tuning-guide)
   - [Default Configuration Settings](#61-default-configuration-settings)
   - [How to Optimize Further (Extreme Continuum Throughput)](#62-how-to-optimize-further-extreme-continuum-throughput)
   - [How to Improve Physical Quality & Fluid Incompressibility](#63-how-to-improve-physical-quality--fluid-incompressibility)
   - [Configuration Trade-Off Matrix](#64-configuration-trade-off-matrix)
7. [Compute Backends (Scalar, Google Highway SIMD, Pravaha)](#7-compute-backends-scalar-google-highway-simd-pravaha)
8. [Zero-to-Hero Tutorial](#8-zero-to-hero-tutorial)
   - [Step 1: Instantiating World & SIMD Backend](#step-1-instantiating-world--simd-backend)
   - [Step 2: Defining Custom Materials in Registry](#step-2-defining-custom-materials-in-registry)
   - [Step 3: Spawning Structural Bonds & Simulating Thermal Melting](#step-3-spawning-structural-bonds--simulating-thermal-melting)
   - [Step 4: Simulating PBF Fluids & Fracture Islands](#step-4-simulating-pbf-fluids--fracture-islands)
9. [Pebble Subsystem Reuse](#9-pebble-subsystem-reuse)

---

## 1. Overview & Emergent Physics Philosophy

In traditional game engines, an object's behavior is hardcoded into rigid class hierarchies (e.g. `RigidBody`, `FluidEmitter`, `DestructibleMesh`). In Prakriti, **matter is defined solely by thermodynamic and mechanical state variables**. 

```
Ice (Solid)  ──[+ Heat]──>  Water (Liquid)  ──[+ Heat]──>  Steam (Gas)
    │                             │                             │
XPBD compliant bonds          PBF density constraints       Tait Gas Expansion
& shear elasticity            & surface tension             & buoyancy forces
```

**Principle:** Geometry is merely an observable of physical state, never the state itself.

---

## 2. Subsystem Architecture & Layer Contracts

Prakriti maintains a strict downward dependency hierarchy. Solvers never define physical behavior directly — they consume coefficients handed down by the Material Law Layer:

```
Application Level              Scene setup, particle injectors, diagnostic telemetry
       │
       ▼
Material Law Layer             Thermodynamic state -> coefficients (compliance α, viscosity μ, EOS P)
       │
       ▼
Simulation Control             Substep scheduler · Morton Z-order Spatial Hash · Accumulators
       │
       ▼
Multi-Physics Solvers          Thermal diffusion · XPBD mechanics · PBF density · Damage/Plasticity
       │
       ▼
Hardware Execution Layer       Stride-1 SoA columns · Google Highway SIMD · Pravaha Task Graphs
```

---

## 3. Algorithmic Foundations & Mathematical Formulations

### 3.1 Continuous 4-Fraction Thermodynamics & Phase Transitions
Every particle carries a continuous phase fraction state vector $\vec{f} = [f_{\text{solid}}, f_{\text{plastic}}, f_{\text{liquid}}, f_{\text{gas}}]^T$ satisfying $\sum f_\phi = 1.0$.
Phase fractions are computed smoothly across transition ramps:
$$f_{\text{liquid}}(T) = \text{smoothstep}(T_{\text{melt}} - \delta, T_{\text{melt}} + \delta, T)$$
Effective physical properties are evaluated via barycentric mixture:
$$\alpha_{\text{eff}} = \sum_{\phi} f_\phi \alpha_\phi, \qquad \mu_{\text{eff}} = \sum_{\phi} f_\phi \mu_\phi$$

### 3.2 Constitutive Laws & Tait Equation of State (EOS)
Liquid and gas pressure are evaluated using the modified Tait Equation of State with an unrolled $\gamma = 7$ integer exponent:
$$P_i = B \left( \left(\frac{\rho_i}{\rho_0}\right)^\gamma - 1 \right) + R \cdot f_{\text{gas}} \cdot T_i$$
Negative pressures are clamped to prevent artificial tensile instability on fluid free surfaces.

### 3.3 Extended Position-Based Dynamics (XPBD) & Kosha Warm-Starting
Compliant distance constraints between bonded particle pairs $(a, b)$ are projected unconditionally stable at small substeps:
$$C(x) = \|x_a - x_b\| - L_0$$
$$\Delta \lambda = \frac{-C(x) - \tilde{\alpha} \lambda_{\text{accum}}}{w_a + w_b + \tilde{\alpha}}, \qquad \tilde{\alpha} = \frac{\alpha_{\text{eff}}}{\Delta t^2}$$
$$\Delta x_a = +w_a \nabla_{x_a} C \, \Delta \lambda, \qquad \Delta x_b = -w_b \nabla_{x_b} C \, \Delta \lambda$$
Accumulated Lagrange multipliers $\lambda$ are cached in `kosha::LRUCache` to warm-start subsequent frames.

### 3.4 Position-Based Fluids (PBF) & Interfacial Tension
Fluid density is evaluated using the 2D Poly6 smoothing kernel:
$$\rho_i = \sum_j m_j W_{\text{poly6}}(\|x_i - x_j\|, h)$$
Density constraint: $C_i(x) = \frac{\rho_i}{\rho_0} - 1 \le 0$.
The position correction including Monaghan-style artificial pressure $s_{\text{corr}}$ (anti-clustering) and interfacial tension is:
$$\Delta x_i = \frac{1}{\rho_0} \sum_j \left( \lambda_i + \lambda_j + s_{\text{corr}} \right) \nabla W_{\text{spiky}}(x_i - x_j, h) - n_{\text{interfacial}} (0.04 h)$$

### 3.5 Graph-Laplacian Explicit Thermal Diffusion
Heat energy $Q$ diffuses across spatial neighbors and structural bonds via the graph Laplacian:
$$\Delta T_i = \frac{k}{c_p} \sum_{j \in \mathcal{N}_i} \frac{m_j}{\rho_j} (T_j - T_i) \nabla^2 W(x_i - x_j, h) \cdot \Delta t$$
Latent heat plateaus buffer temperature changes during solid $\leftrightarrow$ liquid and liquid $\leftrightarrow$ gas transitions until phase latent energy is exhausted.

### 3.6 Strain-Driven Plasticity, Fracture & Island Connectivity (Union-Find)
- **Mechanical Strain**: $\epsilon = \frac{\|x_a - x_b\| - L_0}{L_0}$
- **Plastic Flow**: If $\epsilon > \epsilon_{\text{yield}}$, rest length mutates permanently: $L_0 \leftarrow L_0 + \gamma (\epsilon - \epsilon_{\text{yield}}) L_0$.
- **Damage Accumulation**: $\Delta D = \left( \frac{\epsilon - \epsilon_{\text{yield}}}{\epsilon_{\text{ultimate}}} \right)^\beta \Delta t$.
- **Fracture**: Structural compliance is softened by damage: $\alpha_{\text{struct}} = \frac{\alpha_{\text{base}}}{1 - D}$. When $D \ge 1.0$, the bond breaks and is removed.
- **Island Tracking**: `containers::union_find` clusters unbroken bonds into contiguous physical shards and computes shard mass properties.

---

## 4. State Layer & Stride-1 Split Columns (SoA)

Prakriti arranges particle attributes into split scalar columns rather than interleaved `struct Particle { vec2 pos, vel; ... }`. This guarantees contiguous memory access and autovectorizes effortlessly:

```
pos_x : [ x0, x1, x2, x3, x4, x5, ... ]  <-- Stride-1 float (100% SIMD lane utilization)
pos_y : [ y0, y1, y2, y3, y4, y5, ... ]
vel_x : [ vx0, vx1, vx2, vx3, ... ]
temp  : [ T0, T1, T2, T3, ... ]
phase : [ f0, f1, f2, f3, ... ]
```

---

## 5. Master Subsystem Catalog & Public API

| Header / Subsystem | Key Types & Functions | Description |
|:---|:---|:---|
| `prakriti/engine.hpp` | `World<Law, Backend>`, `.step(dt)`, `.kinetic_energy()` | Master simulation facade managing particles, edges, spatial hash, and solvers. |
| `prakriti/state/particle_store.hpp` | `ParticleStore`, `.add(ParticleDesc)`, `.pos_v(i)` | Stride-1 SoA particle columns with fast vector accessors. |
| `prakriti/state/edge_store.hpp` | `EdgeStore`, `.add(a, b, rest_len)`, `.compact()` | Bond connectivity graph storing strain, plastic rest length, and damage. |
| `prakriti/state/material_registry.hpp`| `MaterialRegistry`, `MaterialParams`, `.steel()`, `.water()` | Immutable material constants and presets. |
| `prakriti/core/spatial_hash.hpp` | `SpatialHash2D`, `.build()`, `.for_each_neighbor()` | Counting-sort uniform grid over Morton Z-order indices. |
| `prakriti/solvers/thermal.hpp` | `ThermalSolver` | Explicit graph Laplacian heat diffusion with latent transition plateaus. |
| `prakriti/solvers/xpbd.hpp` | `XpbdSolver` | Compliant distance constraint projection with warm-started multipliers. |
| `prakriti/solvers/density.hpp` | `DensitySolver` | Position-Based Fluids (PBF) incompressibility solver. |
| `prakriti/solvers/damage.hpp` | `DamageSolver`, `.track_islands(edges)` | Plastic mutation, damage accumulation, and union-find shard extraction. |

---

## 6. Configuration, Defaults & Performance Tuning Guide

### 6.1 Default Configuration Settings

| Tunable Struct | Field | Default Value | Role / Effect |
|:---|:---|:---|:---|
| `WorldConfig` | `substeps` | `8` | XPBD small-steps per frame (guarantees stiffness and numerical stability). |
| `WorldConfig` | `solver_iters` | `4` | XPBD constraint projection sweeps per substep. |
| `WorldConfig` | `cell_size` | `1.0f` | Uniform spatial hash cell edge size ($\sim 2\times$ particle radius). |
| `WorldConfig` | `clamp_negative_pressure`| `true` | Prevents artificial surface tension clump explosion in PBF fluids. |
| `FluidConfig` | `rest_density` | `1.5f` | Target kernel density at rest particle spacing. |
| `FluidConfig` | `smoothing_h` | `1.0f` | SPH Poly6 / Spiky gradient smoothing kernel radius. |
| `FluidConfig` | `relaxation_eps`| `0.01f` | Density constraint Lagrange denominator stabilizer (prevents division by zero). |
| `FluidConfig` | `scorr_k` | `1e-4f` | Monaghan anti-clustering artificial pressure strength. |
| `ThermalConfig` | `diffusivity` | `0.1f` | Global thermal diffusion scale. |

### 6.2 How to Optimize Further (Extreme Continuum Throughput)
1. **Choose `HighwayBackend`**: On macOS Apple Silicon, Google Highway executes Poly6 and Spiky SPH neighbor loops using NEON vector registers, yielding a **$4\text{–}6\times$ performance boost** over scalar code.
2. **Use `PravahaBackend` for 50k+ Particles**: Enables 4-color checkerboard domain decomposition (`parallel_for_color`), executing spatial cells across all CPU cores without mutex locks or race conditions.
3. **Tune `cell_size`**: Keep `cell_size = smoothing_h`. Setting cell size larger checks unnecessary distant cells; setting it smaller misses neighbors.

### 6.3 How to Improve Physical Quality & Fluid Incompressibility
1. **Increase `substeps` to 12–16**: Higher substep counts allow XPBD structural bonds to behave like unyielding rigid solids without rubber-band stretching.
2. **Increase `solver_iters` in `DensitySolver`**: Setting `solver_iters = 6` eliminates volume compression in deep fluid columns.
3. **Calibrate `scorr_k` and `scorr_dq`**: Fine-tune anti-clustering to maintain a crystal lattice particle spacing without surface boiling artifacts.

### 6.4 Configuration Trade-Off Matrix

| Simulation Mode | Backend | Substeps | Iters | 10k Particle Frame Time | Physical Fidelity & Incompressibility |
|:---|:---|:---:|:---:|:---:|:---:|
| **Realtime Mobile / Web** | `ScalarBackend` | 4 | 2 | **$\sim 1.8\text{ms}$** | Moderate (compressible fluids, flexible bonds) |
| **Default Balanced** | `HighwayBackend` | 8 | 4 | **$\sim 0.6\text{ms}$** | High (stiff structures, stable fluid volumes) |
| **Scientific Continuum** | `HighwayBackend` / `Pravaha` | 16 | 8 | **$\sim 2.1\text{ms}$** | Maximum (truly incompressible, brittle glass) |

---

## 7. Compute Backends (Scalar, Google Highway SIMD, Pravaha)

Prakriti parameterizes execution via the `ComputeBackend` concept:
```cpp
template <MaterialLaw Law = DefaultMaterialLaw, ComputeBackend CB = HighwayBackend>
class World;
```

| Tier | Backend | Implementation | Target Hardware |
|:---|:---|:---|:---|
| **Tier 1** | `ScalarBackend` | Plain stride-1 loops | Portable reference, microcontrollers, debug builds. |
| **Tier 2** | `HighwayBackend` | Google Highway SIMD | Apple Silicon NEON (M1/M2/M3/M4) & x86 AVX2/AVX-512 vectorization. |
| **Tier 3** | `PravahaBackend` | Pravaha Task Graphs | Multi-core thread pools with 4-color checkerboard domain decomposition. |

---

## 8. Zero-to-Hero Tutorial

### Step 1: Instantiating World & SIMD Backend
```cpp
#include <prakriti/prakriti.hpp>

using namespace prakriti;

WorldConfig config{
    .bounds = {{-50.0f, 0.0f}, {50.0f, 100.0f}}, // Bounding domain with floor at y=0
    .gravity = {0.0f, -9.81f},
    .substeps = 8,                                // 8 XPBD sub-steps per frame
    .cell_size = 1.0f                             // Spatial hash radius
};

// Monomorphized with Highway SIMD backend
World<DefaultMaterialLaw, HighwayBackend> world(config);
```

### Step 2: Defining Custom Materials in Registry
```cpp
auto& reg = world.materials();

// 1. Steel: high Young's modulus, low compliance, high melting point
auto steel = reg.add(MaterialRegistry::steel());

// 2. Custom Magma: High viscosity, extreme temperature
MaterialParams magma_params{
    .density_solid = 2800.0f,
    .density_liquid = 2600.0f,
    .melt_temp = 1200.0f,
    .boil_temp = 2500.0f,
    .latent_heat_melt = 400000.0f,
    .thermal_conductivity = 3.5f,
    .viscosity = {1e6f, 500.0f, 100.0f, 0.01f} // Viscous liquid phase
};
auto magma = reg.add(magma_params);
```

### Step 3: Spawning Structural Bonds & Simulating Thermal Melting
```cpp
auto& particles = world.particles();
auto& edges = world.edges();

// Create an anchored structural truss beam
Index p_anchor = particles.add({.position = {0.0f, 20.0f}, .mass = 0.0f, .material = steel}); // Static anchor
Index p_mid    = particles.add({.position = {2.0f, 20.0f}, .mass = 1.0f, .material = steel});
Index p_tip    = particles.add({.position = {4.0f, 20.0f}, .mass = 1.0f, .material = steel});

edges.add(p_anchor, p_mid, 2.0f);
edges.add(p_mid, p_tip, 2.0f);

// Inject extreme localized heat at mid-point -> melts beam into flowing liquid
particles.temperature(p_mid) = 1600.0f;
```

### Step 4: Simulating PBF Fluids & Fracture Islands
```cpp
// Inject 1000 water particles
auto water = reg.add(MaterialRegistry::water());
for (int i = 0; i < 1000; ++i) {
    particles.add({
        .position = {-10.0f + (i % 20) * 0.5f, 30.0f + (i / 20) * 0.5f},
        .velocity = {0.0f, -2.0f},
        .material = water
    });
}

// Execute 60 simulation frames
for (int frame = 0; frame < 60; ++frame) {
    world.step(1.0f / 60.0f);
    
    // Telemetry: measure total kinetic energy and active fracture shards
    float ke = world.kinetic_energy();
    auto islands = world.solvers().damage().track_islands(world.edges());
}
```

---

## 9. Pebble Subsystem Reuse

| Subsystem | Component Used | Purpose in Prakriti |
|:---|:---|:---|
| `pebble::math` | `math_vector.hpp` | `vec2`, `mat2`, `aabb2`, `dot`, `cross`, `length_sq`, `normalize`. |
| `kosha` | `LRUCache.hpp` | XPBD constraint Lagrange multiplier warm-starting across substeps. |
| `containers` | `union_find.hpp` | Connected-component graph partitioning during structural fracture. |
| `mem` | `LinearArena.hpp` | Per-frame spatial hash neighbor pair scratch arena. |
| `pravaha` | `pravaha.hpp` | Multi-core 4-color checkerboard domain parallelization (`PravahaBackend`). |
| `akruti` | `akruti.hpp` | SDF obstacle collision projection and continuous collision raycasting. |
| `observability` | `nadi.hpp` | Zero-overhead telemetry of kinetic energy, solver iterations, and temperature fields. |
