# Kalpana (कल्पना) — 2D Vector Graphics, Realtime Painting & Spectral Pigment Mixing

Header-only C++23/C++26. No virtual, no macros. Concept-based static dispatch.
Kalpana is Pebble's unified 2D vector graphics and digital painting engine, featuring **Kubelka-Munk subtractive spectral pigment mixing**, vector path contours, Akruti geometry import, brush dynamics, scene graph composition, and pluggable backends (headless capture, GPU hardware via Sokol, terminal text-mode via Notcurses).

Include: `#include <kalpana/kalpana.hpp>`

---

## 1. Features

1. **Subtractive Kubelka–Munk Spectral Mixing (`kalpana/color/spectral.hpp`)**:
   - Replaces muddy linear RGB blending ($B + Y \to \text{Grey}$) with physically realistic spectral reflectance mixing ($B + Y \to \text{Vibrant Green}$).
   - Multi-stop spectral gradient sampler (`sample_gradient(stops, t)`).
2. **Vector Paths & Akruti Interop (`kalpana/geom/path.hpp`)**:
   - Flat command verbs (`move_to`, `line_to`, `cubic_to`, `quad_to`, `close`).
   - Seamless import from `akruti::CubicBezierCurve`, `akruti::CatmullRomSpline`, `akruti::ChainShape`, and `akruti::Poly`.
3. **Paint, Gradients & Blend Modes (`kalpana/paint/paint.hpp`)**:
   - Multi-stop Linear & Radial gradients, customizable strokes (caps/joins/miter), and 12 Porter-Duff / Photoshop blend modes.
4. **Realtime Brush Dynamics (`kalpana/brush/brush.hpp`)**:
   - Smooth stamp emission with pressure, tilt, jitter, and spacing dynamics.
5. **Monomorphized Render Backends (`kalpana/backend/`)**:
   - **`capture_backend`**: Headless software scanline rasterizer & verification command log.
   - **`sokol_backend`**: GPU hardware rendering via Sokol GFX (`pebble/dependencies/sokol`).
   - **`notcurses_backend`**: Terminal unicode half-block character rendering via Notcurses.

---

## 2. Quick Start Example

```cpp
#include <kalpana/kalpana.hpp>

using namespace kalpana;

Scene scene;
scene.clear_color(colors::white());

// 1. Construct vector rounded rectangle
Path card;
card.round_rect(10.0f, 10.0f, 120.0f, 60.0f, 8.0f, 8.0f);

// 2. Mix pigment colors subtractively using Kubelka-Munk
Color vibrant_green = spectral::mix(colors::blue(), colors::yellow(), 0.5f);

// 3. Add shape node with fill and outline
scene.add(Node::shape(card, Paint::filled_outlined(vibrant_green, colors::black(), 2.0f)));

// 4. Render through headless canvas
DefaultCanvas canvas(256, 128);
canvas.render(scene);

// 5. Snapshot raw ARGB8888 pixels
std::vector<std::uint32_t> pixels = canvas.snapshot();
```
