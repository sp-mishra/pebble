# Pebble Verse: N-Body Planetary Continuum Engine (`src/app/pebble_verse.cpp`)

**Pebble Verse** is Pebble's unified flagship interactive celestial simulation and graphical sandbox. Built in modern C++23 with zero virtual dispatch, it brings together N-body gravitational dynamics, relativistic astrophysics, continuum thermodynamics, fluid hydrodynamics, fracture mechanics, and procedural rendering into a single interactive cosmos.

---

## 1. High-Level Architecture & Subsystem Integration

```
                                  PEBBLE VERSE ARCHITECTURE
                                  
  ┌────────────────────────────────────────────────────────────────────────────────────────┐
  │                            PRESENTATION & DUAL BACKENDS                                │
  │   - Sokol GFX Instanced Pipeline (Apple Metal / OpenGL Core 3.3 / WebGPU)              │
  │   - Headless Terminal Text Mode (`--terminal` / `--cli`)                               │
  │   - Pure Compute Benchmark Mode (`--benchmark` / `-b`)                                 │
  └──────────────────────────────────────────┬─────────────────────────────────────────────┘
                                             │
  ┌──────────────────────────────────────────┴─────────────────────────────────────────────┐
  │                           ORCHESTRATION & SIMULATION ENGINE                            │
  │                                                                                        │
  │  Gati Fixed-Step Scheduler (DT = 0.016s) ── NADI Real-Time Microsecond Sparkline HUD   │
  │                                                                                        │
  │  1. Spatial Tile Streamer (gati::world::SpatialTileStreamer)                           │
  │     - Viewport culling & discovery: 1024px sectors + 256px margins                     │
  │     - SplitMix64 coordinate hashing & Petika / Glaze JSON background serialization     │
  │                                                                                        │
  │  2. Barnes-Hut O(N log N) Gravitational Solver (containers::spatial::BarnesHutTree)    │
  │     - Unrolled 4-way child stack push + Fast Reciprocal Square Root (1/sqrt(r²+ε²))    │
  │     - Parallel force sweep via persistent Pravaha worker thread pool                   │
  │     - Macro-Sector Multipole Collective Gravity for out-of-view dormant sectors        │
  │                                                                                        │
  │  3. Relativistic Astrophysics & Prakriti Celestial Physics                             │
  │     - 2.5PN Gravitational Wave Radiation Reaction (Inspiral orbital decay)             │
  │     - Lense-Thirring Spin Precession & Relativistic Doppler Beaming on Polar Jets      │
  │     - Eggleton Roche Lobe Overflow & SPH Gaseous Tidal Stripping                       │
  │     - Multi-Phase Supernova Remnant (SNR) Blast Waves (Sedov-Taylor to Snowplow)       │
  │     - Hawking Radiation Quantum Evaporation (dM/dt ∝ -1/M²)                           │
  │     - Morgan-Keenan (MK) Stellar Spectral Classification & Nuclear Fusion              │
  │                                                                                        │
  │  4. Contact & Collision Resolution                                                     │
  │     - O(N) SpatialHashGrid with 32-bit Morton Z-Order cache locality                   │
  │     - Inelastic impact thermodynamics (kinetic energy → heat conversion)               │
  │     - Akruti Khanda Voronoi Fracture & Contact Binary Dumbbell Coalescence             │
  │                                                                                        │
  │  5. Visuals, Color Science, & Polish                                                   │
  │     - Kalpana 16-band Kubelka-Munk & Planckian Blackbody Color (800K → 40,000K)        │
  │     - Perceptually linear OkLab gradient interpolation for incandescent accretion      │
  │     - Subtle Einstein gravitational lensing halos & ISCO accretion rings               │
  │     - Spandana non-linear camera trauma screen-shakes (Trauma² * Perlin noise)         │
  └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Integrated Pebble Libraries & Roles

| Library / Header | Namespace | Core Function in `pebble_verse.cpp` |
|:---|:---|:---|
| [`containers/spatial/barnes_hut.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/containers/spatial/barnes_hut.hpp) | `containers::spatial` | Hierarchical $O(N \log N)$ tree gravity with fast reciprocal square roots. |
| [`containers/spatial/spatial_hash_grid.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/containers/spatial/spatial_hash_grid.hpp) | `containers::spatial` | $O(N)$ broadphase collision grid with Morton Z-order cache locality. |
| [`containers/dynamic/soa_vector.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/containers/dynamic/soa_vector.hpp) | `containers::dynamic` | Structure-of-Arrays SIMD unrolled Velocity-Verlet stepping. |
| [`prakriti/material/celestial.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/prakriti/material/celestial.hpp) | `prakriti::celestial` | Stellar evolution, Roche Lobe stripping, SNR blast physics, and thermodynamics. |
| [`prakriti/celestial/sector_*.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/prakriti/celestial/sector_cache_manager.hpp) | `prakriti::celestial` | Infinite universe chunking, multipole macro-nodes, and active sector caching. |
| [`gati/world/spatial_tile_streamer.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/gati/world/spatial_tile_streamer.hpp) | `gati::world` | 2D camera viewport tile paging with discovery/cull callbacks. |
| [`gati/systems/celestial_system.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/gati/systems/celestial_system.hpp) | `gati::systems` | High-level celestial orchestration system with NADI pulse telemetry. |
| [`kalpana/kalpana.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/kalpana/kalpana.hpp) | `kalpana` | Thermal blackbody radiance, OkLab color grading, and instanced GPU rendering. |
| [`spandana/spandana.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/spandana/spandana.hpp) | `spandana` | Non-linear Perlin camera trauma screen-shakes and harmonic spring cameras. |
| [`pravaha/pravaha.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/pravaha/pravaha.hpp) | `pravaha` | Multi-threaded task graphs and persistent worker pool parallel sweeps. |
| [`petika/async_persistence_worker.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/petika/async_persistence_worker.hpp) | `petika` | SPSC circular ring buffer for non-blocking background disk serialization. |
| [`observability/nadi.hpp`](file:///Users/I501980/opt/Code/Personal/pebble/include/observability/nadi.hpp) | `pebble::utils::nadi` | Microsecond execution timing and live 100-frame compute sparklines. |

---

## 3. Mathematical & Physical Formulations

### 3.1 Barnes-Hut Hierarchical Gravity with Fast Reciprocal Math
For opening angle criterion $\frac{s}{d} < \theta$:
$$\mathbf{F}_{ij} = G \frac{m_i M_j}{(r^2 + \epsilon^2)^{3/2}} \mathbf{r}_{ij} = G m_i M_j \left(\frac{1}{\sqrt{r^2 + \epsilon^2}}\right)^3 \mathbf{r}_{ij}$$

### 3.2 2.5PN Gravitational Radiation Reaction Drag
For binary compact objects (black holes / neutron stars), gravitational wave emission accelerates orbital decay:
$$a_{\text{drag}} = \frac{32}{5} \frac{G^3 m_1 m_2 (m_1 + m_2)}{c^5 r^4}$$

### 3.3 Eggleton Analytical Roche Lobe & $L_1$ Mass Transfer
The effective Roche Lobe radius $r_L$ for mass ratio $q = M_1 / M_2$:
$$r_L(q, a) = a \frac{0.49 q^{2/3}}{0.6 q^{2/3} + \ln(1 + q^{1/3})}$$
When stellar radius $R > r_L$, gas streams through the $L_1$ Lagrange point at sound speed:
$$\dot{M} = \rho_{\text{atm}} c_s w_{L_1}^2 \left(\frac{R - r_L}{R}\right)^{3/2}$$

### 3.4 Multi-Phase Supernova Remnant (SNR) Blast Expansion
- **Adiabatic Sedov-Taylor Stage ($t < t_{\text{trans}}$)**:
  $$R(t) = 1.15 \left(\frac{E}{\rho_0}\right)^{1/5} t^{2/5}, \quad v_{\text{shock}} = \frac{0.40 R}{t}, \quad \text{compression} = 4.0$$
- **Radiative Snowplow Stage ($t \ge t_{\text{trans}}$)**:
  $$R(t) \propto t^{1/4}, \quad v_{\text{shock}} = \frac{0.25 R}{t}, \quad \text{compression} = 12.0$$

### 3.5 Lense-Thirring Precession & Relativistic Doppler Beaming
Polar relativistic jets precess due to frame-dragging:
$$\mathbf{n}_{\text{jet}}(t) = \mathbf{n}_0 + \mathbf{u} \sin(\omega_{\text{prec}} t) \theta_{\text{cone}}$$
The beamed intensity is boosted by Doppler factor $\delta$:
$$\delta = \frac{1}{\gamma (1 - \beta \cos \theta)}, \quad I_{\text{obs}} = I_0 \, \delta^3$$

---

## 4. Interactive Controls & Keybindings

| Key / Input | Action |
|:---|:---|
| **Left Click + Drag** | Pan cosmological camera across infinite 2D universe. |
| **Mouse Wheel** | Smooth logarithmic cosmic zoom ($0.25\times$ to $3.5\times$). |
| **Left Click (Hold)** | Active Gravity Vortex attracting all surrounding matter. |
| **`H` / `C`** | Heat Ray ($+800\,\text{K/s}$) / Freeze Ray ($-600\,\text{K/s}$) at mouse cursor. |
| **`B`** | Spawn Counter-Orbiting Binary Black Hole pair with 2.5PN gravitational wave inspiral. |
| **`P`** | Spawn High-Spin Millisecond Pulsar with precessing relativistic polar jets. |
| **`S`** | Spawn Glowing Protostar with 28-body Keplerian protoplanetary dust disk. |
| **`1` / `2` / `3`** | Switch Spectral Modes: Optical True-Color / Thermal Infrared / Relativistic X-Ray. |
| **`Tab`** | Toggle NADI Live Microsecond Compute Sparkline Telemetry HUD. |
| **`Space`** | Pause / Resume simulation stepping. |

---

## 5. Execution Modes

```bash
# 1. Interactive Graphical Window (Metal / OpenGL)
./build/src/app/pebble_verse

# 2. Headless Terminal ASCII Mode
./build/src/app/pebble_verse --terminal

# 3. Pure Compute 1,000-Tick Throughput Benchmark
./build/src/app/pebble_verse --benchmark
```
