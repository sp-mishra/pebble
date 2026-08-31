# Kalpana (कल्पना) — 2D Graphics Language, Spectral Color Science & Natural-Media Painting Engine

Header-only C++23. Zero virtual dispatch, no macros. Concept-monomorphized, policy-configurable storage.
Kalpana is Pebble's unified **2D graphics language and natural-media painting engine**, featuring **Kubelka-Munk subtractive spectral pigment mixing**, natural-media physics brushes (wet-on-wet diffusion, watercolor, oil impasto), multi-channel Eulerian paint field substrate (`PaintField`), Cook-Torrance GGX/IBL PBR shading (`RealShaderPass`), composable `EffectChain` EDSL, layer compositing with extensible combiners, procedural fills (paper textures, marble, grain, wood), geometry modifiers (`offset`, `roughen`, `smooth`, `simplify`), and a declarative scene authoring API.

Include: `#include <kalpana/kalpana.hpp>`

---

## Table of Contents
1. [Overview & Core Philosophy](#1-overview--core-philosophy)
2. [Subsystem Architecture](#2-subsystem-architecture)
3. [Algorithmic Foundations & Mathematical Formulations](#3-algorithmic-foundations--mathematical-formulations)
   - [Kubelka-Munk Subtractive Spectral Pigment Mixing](#31-kubelka-munk-subtractive-spectral-pigment-mixing)
   - [Blackbody Thermal Radiation & Planckian Locus](#32-blackbody-thermal-radiation--planckian-locus)
   - [Color Space Transformations (RGB $\leftrightarrow$ HSL $\leftrightarrow$ OkLab)](#33-color-space-transformations-rgb-leftrightarrow-hsl-leftrightarrow-oklab)
   - [Physics Brush Deposition, Water Flow & Drying](#34-physics-brush-deposition-water-flow--drying)
   - [Procedural Noise Generators & Material Fills](#35-procedural-noise-generators--material-fills)
   - [Geometric Path Modifiers & Algorithmic Warping](#36-geometric-path-modifiers--algorithmic-warping)
4. [Master Subsystem Catalog & Public API](#4-master-subsystem-catalog--public-api)
5. [Configuration, Defaults & Performance Tuning Guide](#5-configuration-defaults--performance-tuning-guide)
   - [Default Configuration Settings](#51-default-configuration-settings)
   - [How to Optimize Further (Extreme Vector Throughput)](#52-how-to-optimize-further-extreme-vector-throughput)
   - [How to Improve Visual Quality & Spectral Fidelity](#53-how-to-improve-visual-quality--spectral-fidelity)
   - [Configuration Trade-Off Matrix](#54-configuration-trade-off-matrix)
6. [Declarative Scene & Brush EDSL Reference](#6-declarative-scene--brush-edsl-reference)
7. [Backends & Rendering Pipeline](#7-backends--rendering-pipeline)
8. [Zero-to-Hero Tutorial](#8-zero-to-hero-tutorial)
   - [Step 1: Setting up Scene & Spectral Palette](#step-1-setting-up-scene--spectral-palette)
   - [Step 2: Vector Paths with Fluent Modifiers](#step-2-vector-paths-with-fluent-modifiers)
   - [Step 3: Physics Brush Strokes & Dynamic Pressure](#step-3-physics-brush-strokes--dynamic-pressure)
   - [Step 4: Composing Post-Processing Effect Chains & Rendering](#step-4-composing-post-processing-effect-chains--rendering)
9. [Extensibility & Custom Policies](#9-extensibility--custom-policies)

---

## 1. Overview & Core Philosophy

Standard computer graphics blend colors in non-physical sRGB space, turning vibrant primaries (e.g. Yellow + Blue) into dull, muddy dark gray. Kalpana solves this with **16-band Kubelka-Munk spectral reflectance modeling**, reproducing the physical reality of real-world oil paints, watercolors, and inks.

```
+-----------------------------------------------------------------------------------+
| RGB Additive (Muddy)    : Yellow {1, 1, 0} + Blue {0, 0, 1}    --> Gray {0.5, 0.5, 0.5} |
| Kubelka-Munk Spectral   : Cadmium Yellow ⊗ Cerulean Blue      --> Brilliant Rich Green  |
+-----------------------------------------------------------------------------------+
```

---

## 2. Subsystem Architecture

```
Scene EDSL (kalpana::edsl)       shape(), text(), NodeBuilder, TextBuilder, operator<<
       │
       ▼
Layer Stack & Compositing        LayerStack, Blend Modes, LayerCombiner (Spectral / Wet Diffusion)
       │
       ▼
Natural-Media Physics Layer      PaintField (20-ch Eulerian substrate), MediumSolver, DropEngine,
│  (Kalpana Next)                WetTool / DryTool / BlowTool, LiquifyBrush
       │
       ▼
Vector Paths & Brushes           BasicPath, PathModifiers (roughen, smooth), SpectralBrush
│                                BrushProfile (Brush Creator), StrokeStabilizer,
│                                PigmentReservoir<N>, curvature_adaptive_step
       │
       ▼
Procedural Fills & Effects       ProceduralFill (watercolor, marble, wood), EffectChain (shadow | blur)
       │
       ▼
Spectral Color Science           16-band Kubelka-Munk, Pigment Catalog, OkLab, SpectralGradient
       │
       ▼
PBR Shading (Kalpana Next)       RealShaderPass (GGX NDF, Smith G, Schlick F, Karis split-sum IBL),
│                                PaintMaterial (metallic, roughness, gloss, anisotropy), SoftShadows
       │
       ▼
Monomorphized Backends           capture_backend (headless), sokol_backend (GPU w/ GGX), notcurses (terminal)
```

---

## 3. Algorithmic Foundations & Mathematical Formulations

### 3.1 Kubelka-Munk Subtractive Spectral Pigment Mixing
Each pigment is defined across 16 discrete spectral wavelengths $\lambda \in [380\text{nm}, 730\text{nm}]$ by absorption coefficients $K(\lambda)$ and scattering coefficients $S(\lambda)$.
For a mixture of $N$ pigments with concentrations $c_i$ ($\sum c_i = 1$):
$$K_{\text{mix}}(\lambda) = \sum_{i=1}^N c_i K_i(\lambda), \qquad S_{\text{mix}}(\lambda) = \sum_{i=1}^N c_i S_i(\lambda)$$
The spectral reflectance $R(\lambda)$ of an infinitely thick layer is given by:
$$\frac{K_{\text{mix}}(\lambda)}{S_{\text{mix}}(\lambda)} = \frac{(1 - R(\lambda))^2}{2 R(\lambda)}$$
Solving for $R(\lambda)$:
$$R(\lambda) = 1 + \frac{K_{\text{mix}}}{S_{\text{mix}}} - \sqrt{\left(\frac{K_{\text{mix}}}{S_{\text{mix}}}\right)^2 + 2\left(\frac{K_{\text{mix}}}{S_{\text{mix}}}\right)}$$
Reflectances $R(\lambda)$ are converted to standard CIE XYZ and linear RGB using standard CIE $1931$ 2-degree observer color matching integrals.

### 3.2 Blackbody Thermal Radiation & Planckian Locus
`kalpana::blackbody` computes physical thermal emission spectra and RGB colors directly from thermodynamic temperature $T \in [500\text{K}, 40000\text{K}]$ using Planck's radiation law and CIE color matching:
$$B(\lambda, T) = \frac{2 h c^2}{\lambda^5} \frac{1}{e^{\frac{h c}{\lambda k_B T}} - 1}$$
- `temperature_to_rgb(kelvin)`: Evaluates chromaticity along the Planckian locus.
- `apply_thermal_glow(base_col, celsius)`: Blends material pigments with glowing incandescence and radiant bloom halos under extreme temperatures (e.g. heated rock $\to$ molten magma $\to$ incandescent stellar plasma).

### 3.3 Color Space Transformations (RGB $\leftrightarrow$ HSL $\leftrightarrow$ OkLab)
Kalpana provides analytic conversions to perceptually uniform **OkLab** space:
$$\begin{bmatrix} L \\ M \\ S \end{bmatrix} = M_1 \begin{bmatrix} R_{\text{lin}} \\ G_{\text{lin}} \\ B_{\text{lin}} \end{bmatrix}, \quad l = L^{1/3}, \quad m = M^{1/3}, \quad s = S^{1/3}$$
$$\begin{bmatrix} L^* \\ a \\ b \end{bmatrix} = M_2 \begin{bmatrix} l \\ m \\ s \end{bmatrix}$$
Perceptual distance is Euclidean in OkLab: $\Delta E_{\text{Ok}} = \sqrt{(\Delta L^*)^2 + (\Delta a)^2 + (\Delta b)^2}$.

### 3.4 Physics Brush Deposition, Water Flow & Drying
A brush stroke discretizes trajectory $(p_0, p_1)$ into spaced circular/elliptical stamps:
$$\text{spacing} = \text{diameter} \times \text{step\_ratio}$$
Dynamic property scaling from input signals ($s \in \{\text{Pressure}, \text{Tilt}, \text{Velocity}\}$):
$$v_{\text{eff}} = v_{\text{lo}} + (v_{\text{hi}} - v_{\text{lo}}) \cdot s^{\gamma}$$
- **Water Diffusion** (Eulerian, `MediumSolver`): Pigment diffuses via Jacobi sweeps on `ga::Field<20>`:
  $$\Delta \text{pigment} = D_w \cdot \nabla^2 \text{pigment} \cdot \Delta t$$
- **Semi-Lagrangian Advection** (Stam 1999, `ga::stencil::advect_semilagrangian`): Unconditionally stable advect for wet pigment under tilt and velocity impulse.
- **Evaporation / Drying**: Water content decays exponentially: $W(t) = W_0 e^{-k_{\text{dry}} t}$.
- **Sediment Settlement**: Granulation particles fix in paper valleys when $W < 0.3$.
- **Edge Darkening**: Drying fringe at waterline where $\nabla W \in [0.05, 0.25]$.
- **Curvature-Adaptive Spacing** (`curvature_adaptive_step`): Menger curvature $\kappa = 2|a \times b| / (|a||b||c|)$ maps to step in $[0.4\times, 2\times]$ base_spacing.

### 3.7 Cook-Torrance GGX PBR Shading (`RealShaderPass`)
Height-field normals computed from $3\times3$ Sobel on `PaintField::HEIGHT` channel. Shading uses:
- **GGX NDF**: $D(h) = \alpha^2 / (\pi (\mathbf{n}\cdot\mathbf{h})^2 (\alpha^2 - 1) + 1)^2$
- **Smith Geometry**: $G(\mathbf{n}, \mathbf{v}, \mathbf{l}) = G_1(\mathbf{n}, \mathbf{v}) \cdot G_1(\mathbf{n}, \mathbf{l})$, $k = \alpha/2$
- **Schlick Fresnel**: $F(\mathbf{h}, \mathbf{v}) = F_0 + (1-F_0)(1-\mathbf{h}\cdot\mathbf{v})^5$, $F_0 = \text{lerp}(0.04, \text{albedo}, \text{metallic})$
- **Karis Split-Sum IBL** (2013 approximation): $\int L_i \cdot f_r \approx L_\text{prefiltered}(r, \alpha) \cdot (F_0 \cdot \text{scale} + \text{bias})$
- **PCSS Soft Shadows**: `ShadowRayConfig::max_steps` samples along shadow ray with `shadow_softness` penumbra.

### 3.5 Procedural Noise Generators & Material Fills
All procedural fills satisfy `noise::noise_generator`:
- **Simplex Noise**: 2D skew simplex grid evaluation with polynomial radial decay $(r^2 - \|d\|^2)^4$.
- **Fractal Brownian Motion (FBM)**:
  $$\text{FBM}(p) = \sum_{o=0}^{O-1} \gamma^o \cdot \text{noise}(2^o p)$$
- **Worley Cellular Noise**: Evaluates distance $F_1(p)$ to nearest Poisson cell center in a $3\times 3$ neighborhood.
- **Procedural Marble / Wood**:
  $$\text{Marble}(x, y) = \sin(x + a \cdot \text{FBM}(x, y))$$
  $$\text{Wood}(x, y) = \text{fract}\left(b \cdot \sqrt{x^2 + y^2} + c \cdot \text{Turbulence}(x, y)\right)$$

### 3.6 Geometric Path Modifiers & Algorithmic Warping
- **Ramer-Douglas-Peucker (RDP) Simplification**: Recursively prunes vertices within perpendicular tolerance $\epsilon$.
- **Chaikin Subdivision / Catmull-Rom Smoothing**: Replaces corner vertices with quadratic B-spline control points:
  $$Q = \frac{3}{4} P_i + \frac{1}{4} P_{i+1}, \qquad R = \frac{1}{4} P_i + \frac{3}{4} P_{i+1}$$
- **Contour Roughening**: Displaces vertices along outward normal $n_i$:
  $$p'_i = p_i + n_i \cdot \text{FBM}(p_i) \cdot \text{amplitude}$$

RDP simplify, Chaikin smooth, roughen, warp, and dash are **stylistic** modifiers — Kalpana-owned.
Geometric operations are **not** duplicated here: `offset()` / `outline()` / `expand()` and the
boolean CSG builders (`unite`, `subtract`, `intersect`) delegate to Akruti's polygon algorithms
(`akruti::poly_ops` — `offset_polygon` with miter/round/bevel `JoinStyle`, `union_polygon`,
`subtract_polygon`, and `clip_polygon`). Kalpana owns only the join-style intent; Akruti owns the
shape math (single owner: Shape/Space).

---

## 4. Master Subsystem Catalog & Public API

| Module | Types & APIs | Description |
|:---|:---|:---|
| **Color Science** | `SpectralColor`, `SpectralGradient`, `pigments::*`, `colors::*` | 16-band absorption/scattering spectrums, subtractive mixing `.mix(other, ratio)`, RGB/OkLab conversions. `SpectralGradient` holds up to 8 stops via `SmallVector<GradientStop, 8>` — zero heap for ≤8 stops. |
| **Paint Substrate** | `PaintField<SP,CP>`, `PaintChannels`, `VelocityChannels` | 20-channel Eulerian raster grid backed by `ga::Field<20>`. Channels: [0..15] KM reflectance, [16] water, [17] height, [18] sediment, [19] binder. `splat<Preset>(SplatParams, SpectralColor)` rasterizes a dab; `resolve_color()` converts accumulated KM to RGB. |
| **Natural Media Physics** | `MediumSolver<SP,CP>`, `PigmentReservoir<N>` | Six-phase watercolor/oil solver: Jacobi diffusion, semi-Lagrangian advection, pooling, drying, sediment settlement, edge darkening. `PigmentReservoir<N>` bidirectional pickup/deposit with N-slot multicolor and dirty-brush mode. |
| **Wet Tools** | `WetTool`, `DryTool`, `BlowTool` | Stateless canvas tools: add water (re-mobilize pigment), blot/dry, inject velocity impulse for next `MediumSolver::step`. |
| **Watercolor Drops** | `DropEngine<SP,CP>`, `DropEngineParams`, `Droplet` | Particle-based drip simulation. Spawn, advect (gravity + tilt), deposit pigment, merge overlapping droplets. Cap: `max_drops = 512`. |
| **Liquify** | `LiquifyBrush<SP,CP>`, `LiquifyMode`, `LiquifyParams` | Displacement-field warp (Push/Twirl/Pinch/Bloat/Smear). Backward bilinear warp is mass-conserving; Smear delegates to `ga::advect_semilagrangian`. |
| **PBR Material** | `PaintMaterial` | `{metallic, roughness, gloss, anisotropy}` + presets: `preset_matte()`, `preset_glossy_oil()`, `preset_metallic()`, `preset_pencil()`, `preset_watercolor()`, `preset_gouache()`, `preset_feather()`. Disney α = roughness². |
| **PBR Shading** | `RealShaderPass<Env,SP,CP>`, `ConstantEnvMap`, `RealShaderParams` | Cook-Torrance GGX NDF + Smith G + Schlick F + Karis split-sum IBL. Height→normal via 3×3 Sobel. PCSS soft shadows. `shade_cell()` returns `ShadingResult{shaded, specular_intensity}`. |
| **Brush Engine** | `SpectralBrush`, `BrushProfile`, `DynamicsBinding`, `StampPreset` | Pressure/tilt response, curvature-adaptive spacing, `stroke_to(PaintField)` field emission path. |
| **Brush Creator** | `BrushProfile`, `to_toml()`, `from_toml()`, `curvature_adaptive_step()` | Serializable aggregate descriptor. TOML round-trip via `std::from_chars`. Menger curvature for spacing. |
| **Input Stabilizer** | `StrokeStabilizer<Mode>`, `OneEuroFilter`, `PullLagFilter`, `CatmullRomFilter<N>` | `OneEuro` (Casiez CHI 2012): adaptive cutoff per signal derivative. `PullLag`: rope physics. `CatmullRom`: Catmull-Rom midpoint. |
| **Brush Presets** | `BrushPreset::watercolor_wash/oil_impasto/ink_pen/dry_pastel/soft_airbrush` (original 5) | Physics physics & mechanics presets: wetness, diffusion, absorption, drying, impasto, tilt drip. |
| **New Presets** | `BrushPreset::graphite_pencil/metallic_paint/rough_feather/express_oils/gouache` | Kalpana Next presets. Each carries `PaintMaterial` and `WatercolorBody` opacity axis. |
| **Deposition** | `DepositionParams`, `WatercolorBody`, `deposit::Mode` | `WatercolorBody::{Transparent,Semi,Opaque}` drives opacity floor in `compute_opacity()`. `deposit_to_field<FieldT>()` writes KM + height in a single cell without circular header dependency. |
| **Effects** | `EffectChain`, `shadow()`, `blur()`, `glow()`, `bloom()`, `dof()` | Small-Buffer Optimized pipeline combiners via `operator\|` and `operator>>`. |
| **Procedural Fills** | `ProceduralFill::watercolor_paper()`, `marble()`, `wood()` | Noise-driven parametric vector fill shaders. |
| **Path Modifiers** | `roughen()`, `smooth()`, `simplify()`, `offset()`, `warp()`, `BasicPath::from_svg()` | Non-destructive operator pipe modifiers. |
| **Scene EDSL** | `Scene`, `shape()`, `text()`, `NodeBuilder`, `operator<<` | Fluent declarative canvas scene builder. |
| **GPU Pipeline** | `InstancedParticlePipeline`, `GPUInstanceData` | Single-call instanced draw (Metal/Sokol). `GPUInstanceData` now carries `height, normal[3], metallic, roughness` for GGX fragment shader. Fast path (metallic=0, roughness=1) identical to prior shader. |

---

## 5. Configuration, Defaults & Performance Tuning Guide

### 5.1 Default Configuration Settings

| Tunable Parameter | Location | Default Value | Role / Effect |
|:---|:---|:---|:---|
| `brush_step_ratio` | `SpectralBrush` | `0.1f` ($10\%$ diameter) | Distance between consecutive dab stamps. |
| `water_diffusion_rate` | `WaterPhysicsParams`| `0.25f` | Speed of lateral pigment bleed into wet canvas. |
| `evaporation_rate` | `WaterPhysicsParams`| `0.05f` | Rate at which wet paper dries to fixed pigment. |
| `fbm_octaves` | `ProceduralFill` | `4` | Number of fractal noise frequencies evaluated. |
| `fbm_lacunarity` | `ProceduralFill` | `2.0f` | Frequency multiplier per octave. |
| `fbm_gain` | `ProceduralFill` | `0.5f` | Amplitude decay per octave. |
| `path_sbo_capacity` | `BasicPath` | `16 vertices` | Inline vertices stored on stack before touching heap. |

### 5.2 How to Optimize Further (Extreme Vector Throughput)
1. **Increase `brush_step_ratio`**: Setting `step_ratio = 0.25f` reduces stamp count by $60\%$ with minimal visual difference on opaque paints.
2. **Apply `simplify(tolerance)` before Path Modifiers**: Run RDP simplification on imported SVG/curves before applying expensive multi-pass smoothing.
3. **Use `SmallVector` SBO**: For short paths (e.g. glyphs or UI buttons with $\le 16$ vertices), `BasicPath` executes with zero heap allocation.
4. **Use Instanced GPU Pipeline**: When rendering tens of thousands of procedural particles or brush stamps, use `kalpana::InstancedParticlePipeline` (Metal/OpenGL instanced draw call).

### 5.3 How to Improve Visual Quality & Spectral Fidelity
1. **Use True Pigment Combinations**: Mix `pigments::cadmium_yellow()` with `pigments::ultramarine_blue()` or `pigments::phthalo_green()` rather than sRGB hex codes.
2. **Enable Multi-Octave Paper Textures**: Use `ProceduralFill::watercolor_paper(granularity=0.85f, roughness=0.4f)` to simulate high-end rough cotton paper fiber absorption.
3. **Lower `brush_step_ratio` to `0.05f`**: Generates continuous, artifact-free smooth strokes on high-DPI displays.

### 5.4 Configuration Trade-Off Matrix

| Configuration Profile | Key Settings | Frame Render Time | Memory Overhead | Visual Quality |
|:---|:---|:---:|:---:|:---:|
| **Realtime Vector UI / HUD** | `step=0.25, SBO=16, FBM=2 octaves` | **$< 0.1\text{ms}$** | Minimal (Stack) | Crisp vectors, fast procedural fills |
| **Interactive Painting Tool** | `step=0.10, MediumSolver=ON, FBM=4` | **$\sim 1.2\text{ms}$** | Medium | Physical watercolor diffusion, realistic impasto |
| **High-Fidelity Offline Art** | `step=0.04, 16-band spectral, FBM=6, RealShader=ON` | **$\sim 8.0\text{ms}$** | Higher | Museum-grade KM pigment physics + GGX PBR shading |

### 5.5 Natural-Media Performance Budgets

| Operation | Grid | Budget | Notes |
|:---|:---|:---|:---|
| `MediumSolver::step()` | 256×256 | ≤ 16 ms | 60fps real-time target |
| `PaintField::splat<Round>()` | any | ≤ 0.05 ms/dab | AABB-bounded; O(radius²) |
| `RealShaderPass::shade_cell()` | per-cell | ≤ 0.002 ms | ~65k cells/frame budget |
| `DropEngine::step()` | 512 drops | ≤ 2 ms | Includes spawn + advect + merge |
| `LiquifyBrush::apply()` | 256×256 | ≤ 8 ms | Backward bilinear warp |
| `StrokeStabilizer::apply()` | per-point | < 1 µs | OneEuro adaptive filter |

---

## 6. Declarative Scene & Brush EDSL Reference

### Scene Fluent Builder Syntax
```cpp
using namespace kalpana::edsl;

Scene scene;
scene
<< shape(round_rect(10.0f, 10.0f, 200.0f, 100.0f, 8.0f, 8.0f))
       .fill(ProceduralFill::marble(0.5f, 0.2f))
       .stroke(colors::black(), 1.5f)
       .effect(shadow(8.0f) | blur(1.0f))

<< text("Spectral Graphics")
       .position(30.0f, 60.0f)
       .font_size(24.0f)
       .fill(pigments::cadmium_red())
       .effect(glow(5.0f));
```

### Path Modifier Piping Syntax
```cpp
Path stylized_contour = circle({100.0f, 100.0f}, 50.0f)
    | roughen(1.5f, 0.4f)  // FBM edge distortion
    | smooth(2)            // Chaikin quadratic corner subdivision
    | offset(2.0f);        // Outward contour inflation
```

---

## 7. Backends & Rendering Pipeline

| Canvas Type | Underlying Backend | Target Platform | Use Case |
|:---|:---|:---|:---|
| `DefaultCanvas` | `capture_backend` | Headless CPU | Unit tests, snapshot regression, deterministic pixel capture. |
| `SokolCanvas` | `sokol_backend` | Metal / Vulkan / WebGL | 60–240 FPS GPU accelerated hardware rendering. |
| `TerminalCanvas` | `notcurses_backend` | ANSI / Notcurses Terminal | High-performance CLI dashboards and text-mode games. |

Backend APIs can be enabled/disabled at configure time:
- `-DPEBBLE_ENABLE_KALPANA_SOKOL_BACKEND=ON/OFF`
- `-DPEBBLE_ENABLE_KALPANA_NOTCURSES_BACKEND=ON/OFF`

When a flag is `OFF`, the corresponding backend type alias is not exported from `kalpana/kalpana.hpp`.

---

## 8. Zero-to-Hero Tutorial

### Step 1: Setting up Scene & Spectral Palette
```cpp
#include <kalpana/kalpana.hpp>

using namespace kalpana;
using namespace kalpana::edsl;

Scene scene;
scene.clear_color(colors::white());

// Physical spectral pigments (Kubelka-Munk subtractive mixing)
auto yellow = pigments::cadmium_yellow();
auto blue   = pigments::cerulean_blue();
SpectralColor green = yellow.mix(blue, 0.5f); // Yields true physical green!
```

### Step 2: Vector Paths with Fluent Modifiers
```cpp
// Construct a textured card with hand-drawn organic edges
Path organic_card = rect(20.0f, 20.0f, 300.0f, 180.0f)
    | roughen(2.0f, 0.5f)    // FBM displacement
    | smooth(2)              // Chaikin quadratic subdivision
    | offset(1.5f);          // Outward contour inflation
```

### Step 3: Physics Brush Strokes & Dynamic Pressure
```cpp
SpectralBrush brush;
brush.apply_preset(BrushPreset::watercolor_wash())
     .pigment(pigments::ultramarine_blue());

// Pressure-to-radius dynamics binding
brush.size_dyn() = DynamicsBinding{
    .source = DynamicsSource::Pressure,
    .lo = 4.0f,
    .hi = 24.0f,
    .curve = 0.7f // Ease-out
};

// Generate stamps along a stylus stroke
BrushPoint start{.pos = {30.0f, 50.0f}, .pressure = 0.2f};
BrushPoint end  {.pos = {280.0f, 50.0f}, .pressure = 0.95f};
auto stroke_stamps = brush.stroke_segment(start, end);
```

### Step 4: Composing Post-Processing Effect Chains & Rendering
```cpp
// Add shapes to scene with declarative styling and effects
scene
<< shape(organic_card)
       .fill(ProceduralFill::watercolor_paper(0.85f, 0.35f))
       .stroke(colors::black(), 2.0f)
       .effect(shadow(10.0f, {4.0f, 4.0f}) | blur(1.5f))

<< text("Kalpana Spectral Vector Engine")
       .position(50.0f, 100.0f)
       .font_size(22.0f)
       .fill(green)
       .effect(glow(4.0f, colors::gold()));

// Render through headless canvas
DefaultCanvas canvas(400, 250);
canvas.render(scene);
auto frame_buffer = canvas.snapshot();
```

---

## 9. Extensibility & Custom Policies

- **Custom Spectral Pigments**: Register custom 16-band absorption/scattering curves via `pigments::register_custom_pigment_curve(name, fn)`.
- **Custom Color Spaces**: Specialize `color_space_converter<MyColorSpace>` to plug in proprietary formats.
- **Custom Procedural Noise**: Implement `float operator()(vec2 p) const` to inject custom shaders into `ProceduralFill::custom_noise(gen)`.
- **Configurable Storage Policy**: `BasicPath<Allocator>`, `BasicLayerStack<Alloc>`, and `BasicEffectChain<N>` support custom Smriti allocators and `SmallVector` inline capacities.

---

## 10. Thermal Blackbody Radiation & Perceptual OkLab Gradients

### 10.1 Planckian Incandescence & Stellar Radiance
Kalpana computes physically grounded blackbody thermal emission across temperatures from $800\,\text{K}$ (deep red incandescence) to $40,000\,\text{K}$ (supergiant blue-white):

```cpp
#include "kalpana/kalpana.hpp"
#include "kalpana/color/blackbody.hpp"
#include <iostream>

int main() {
    // 1. Evaluate stellar temperature color (Planckian locus)
    kalpana::Color sun_surface = kalpana::blackbody::temperature_to_rgb(5778.0f);   // 5,778K G2V Sun
    kalpana::Color blue_giant = kalpana::blackbody::temperature_to_rgb(25000.0f);  // 25,000K O-Type Star

    // 2. Perceptually Linear OkLab Blackbody Gradient Interpolation
    // Prevents muddy desaturation when interpolating between incandescent core and cool corona
    kalpana::OkLab oklab_core = kalpana::to_oklab(blue_giant);
    kalpana::OkLab oklab_rim  = kalpana::to_oklab(sun_surface);
    
    kalpana::OkLab blended = kalpana::lerp(oklab_core, oklab_rim, 0.5f);
    kalpana::Color srgb_blended = kalpana::to_srgb(blended);

    std::cout << "Blended RGB: (" << srgb_blended.r << ", " << srgb_blended.g << ", " << srgb_blended.b << ")\n";
}
```

---

## 11. Non-Negotiable Contracts

These invariants are enforced by design and must be preserved across all future contributions.

| # | Contract | Rationale |
|:---|:---|:---|
| 1 | **Zero virtual dispatch** — no `virtual` keyword anywhere in Kalpana | Enables full inlining, devirtualization, and LTO; critical for <1µs per-stamp latency |
| 2 | **Header-only** — all types live in `include/kalpana/`; no compiled `.cpp` translation units | Consumers include once, link nothing; policy monomorphization requires source visibility |
| 3 | **No macros** — no `#define` for logic, constants, or code generation | Macros break namespacing, tooling, and C++23 module compatibility |
| 4 | **C++23 minimum** — `std::expected`, CTAD, deducing-this, `[[nodiscard]]`, structured bindings | Older standards cannot express the zero-cost policy template patterns used throughout |
| 5 | **Pay-for-what-you-use** — every heavy subsystem (`MediumSolver`, `RealShaderPass`, `DropEngine`) is opt-in via separate include | Default `kalpana.hpp` compiles in <0.3s; full stack <1.2s |
| 6 | **No heap in hot paths** — `PaintField`, `StrokeStabilizer`, `DropEngine` must not allocate inside `step()` / `apply()` / `splat()` | Prevents GC pauses and fragmentation under 60fps interactive painting |
| 7 | **Existing tests unmodified** — new tests appended only; no changes to pre-existing test bodies | Prevents silent behavior regressions from refactors |
| 8 | **No external downloads** — zero new third-party libraries; all physics, PBR, and noise built from Pebble primitives | Supply-chain integrity and hermetic builds |
