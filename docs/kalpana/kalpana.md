# Kalpana (कल्पना) — 2D Graphics Language, Spectral Color Science & Procedural Fills

Header-only C++23/C++26. Zero virtual dispatch, no macros. Concept-monomorphized, policy-configurable storage.
Kalpana is Pebble's unified **2D graphics language**, featuring **Kubelka-Munk subtractive spectral pigment mixing**, Rebelle-inspired physics brushes, composable `EffectChain` EDSL, layer compositing with extensible combiners, procedural fills (paper textures, marble, grain, wood), geometry modifiers (`offset`, `roughen`, `smooth`, `simplify`), and a declarative scene authoring API.

Include: `#include <kalpana/kalpana.hpp>`

---

## 1. Core Modules

| Module | Location | Description |
|---|---|---|
| **Color Science** | `kalpana/color/` | 16-band Kubelka–Munk mixing, `SpectralColor`, `SpectralGradient`, `spectral_bloom`, `pigments::*` catalog & extensible registry, HSL & OkLab conversions. |
| **Brush Engine** | `kalpana/brush/` | Physics-driven spectral brushes with Rebelle-inspired parameters (water flow, drying, impasto), `DynamicsBinding` (pressure, tilt, velocity), and customizable stamp shapes. |
| **Effect Pipeline** | `kalpana/effect/` | Composable `EffectChain` with `operator\|` and `operator>>`, SmallVector-backed, SBO type-erased `EffectNode`. |
| **Layer Compositor** | `kalpana/layer/` | `LayerStack`, blend isolation, per-layer effects, and extensible `LayerCombiner` (Spectral, Porter-Duff, Photoshop, Wet Diffusion). |
| **Procedural Fills** | `kalpana/fill/` | Simplex, FBM, Worley, Turbulence noise generators, and procedural fills (paper texture, marble, wood, canvas, grain). |
| **Geometry Modifiers** | `kalpana/geom/` | Path modifiers (`offset`, `roughen`, `smooth`, `simplify`, `warp`, `dash`), shape builders (`circle`, `rect`, `star`, `arc`), and Akruti CSG. |
| **Scene EDSL** | `kalpana/edsl/` | Declarative fluent authoring API (`shape()`, `text()`, `NodeBuilder`, `TextBuilder`, `operator<<`). |
| **Backends** | `kalpana/backend/` | `capture_backend` (headless verification), `sokol_backend` (GPU Metal/OpenGL), `notcurses_backend` (terminal). |

---

## 2. Quick Start: Declarative Scene EDSL

```cpp
#include <kalpana/kalpana.hpp>

using namespace kalpana;
using namespace kalpana::edsl;

Scene scene;
scene.clear_color(colors::white());

// 1. Declarative vector shape with procedural fill and effect chain
scene
<< shape(round_rect(20.0f, 20.0f, 300.0f, 150.0f, 16.0f, 16.0f))
       .fill(ProceduralFill::watercolor_paper(0.8f, 0.4f))
       .stroke(colors::black(), 2.0f)
       .effect(shadow(8.0f) | blur(2.0f))

// 2. Text node with spectral color mixing
<< text("Kalpana 2.0")
       .fill(pigments::cerulean_blue())
       .font_size(28.0f)
       .position(40.0f, 80.0f)
       .effect(glow(3.0f));

// 3. Render through headless canvas
DefaultCanvas canvas(400, 200);
canvas.render(scene);
auto pixels = canvas.snapshot();
```

---

## 3. Physical Spectral Brush & Rebelle Presets

```cpp
#include <kalpana/kalpana.hpp>

using namespace kalpana;

// Configure spectral brush with watercolor wash preset
SpectralBrush brush;
brush.apply_preset(BrushPreset::watercolor_wash())
     .pigment(pigments::cadmium_yellow());

// Dynamic pressure-to-size curve
brush.size_dyn() = DynamicsBinding{
    .source = DynamicsSource::Pressure,
    .lo = 8.0f,
    .hi = 32.0f,
    .curve = 0.8f // Ease-out
};

// Emit dabs along stroke trajectory
BrushPoint p0{.pos = {10.0f, 50.0f}, .pressure = 0.4f};
BrushPoint p1{.pos = {200.0f, 50.0f}, .pressure = 0.9f};

auto stamps = brush.stroke_segment(p0, p1);
```

---

## 4. Geometry Modifiers & Vector Pipelines

```cpp
#include <kalpana/kalpana.hpp>

using namespace kalpana;

// Pipe contour modifications into path
Path hand_drawn_card = rect(10.0f, 10.0f, 180.0f, 100.0f)
    | roughen(1.5f, 0.8f)
    | smooth(2)
    | offset(2.0f);
```

---

## 5. Extensibility Points

- **Custom Spectral Pigments**: Register custom curves via `pigments::register_custom_pigment_curve(name, fn)`.
- **Custom Color Spaces**: Implement `color_space_type` concept or specialize `color_space_converter<CS>`.
- **Custom Procedural Noise**: Pass any struct satisfying `noise::noise_generator` to `ProceduralFill::custom_noise(gen)`.
- **Custom Layer Combiners**: Pass custom combination functions to `LayerCombiner::custom(fn)` or `layer.combiner(...)`.
- **Configurable Storage Policy**: `BasicPath`, `BasicLayer`, `BasicLayerStack`, and `BasicEffectChain` support custom container template parameters (defaults to `containers::dynamic::SmallVector`).
