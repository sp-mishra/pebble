# Rekha (रेखा) — Backend-Agnostic Plotting and Graph Visualization

Header-only C++23/C++26 plotting library built for zero-runtime-waste integration with Pebble.
`rekha` uses policy-based static dispatch (no virtual functions) and can target multiple rendering backends. Default
shipping backend: `KalpanaBackend`.

Include: `#include <rekha/rekha.hpp>`

---

## What Rekha Provides

- Major 2D statistical plots:
    - `LinePlot`
    - `AreaPlot`
    - `StepPlot`
    - `StemPlot`
    - `ScatterPlot`
    - `BubblePlot`
    - `ErrorBarPlot`
    - `BarPlot`
    - `HistogramPlot`
    - `HeatmapPlot`
    - `PiePlot` / donut via `inner_radius`
- Graph drawing:
    - `GraphPlot`
    - Force-directed spring layout (`ForceDirectedLayout`) with tunable policies.
- Backend abstraction via `PlotBackend` concept.
- Basic legend overlay via `Figure::legend(true)`.
- Multi-panel subplot layout via `Figure::subplots(rows, cols)` and `select_subplot(row, col)`.
- Shared-axis mode via `Figure::share_axes(share_x, share_y)`.
- Per-subplot annotations via `Figure::annotate(x, y, text, color)`.
- Arrow annotations via `Figure::annotate_arrow(x, y, text_x, text_y, text, color)`.
- Legend placement control via `Figure::legend_position(...)`.
- Auto legend placement via `Figure::legend_auto(true)` (chooses a lower-density corner).
- Constrained spacing controls: `Figure::constrained_layout(true)` and `Figure::subplot_gap(x_gap, y_gap)`.
- Tick label controls on `Axes`: `x_ticks`, `y_ticks`, `x_precision`, `y_precision`, `show_tick_labels`, `x_percent`,
  `y_percent`.
- Reusable theme presets via `Figure::theme(...)`:
    - `Figure::theme_dark_neon()`
    - `Figure::theme_scientific_light()`
    - `Figure::theme_finance_dark()`

`src/app/pebble_rekha.cpp` uses `theme_dark_neon()` as the default dashboard style.

Tick labels are generated from inferred data ranges (not normalized 0..1 axis fractions), so labels follow plot values
directly.

- Kalpana integration:
    - translates plots to `kalpana::Scene` nodes,
    - optional software rasterization through `kalpana::DefaultCanvas`.
- Gati integration:
    - `GraphLayoutRuntime` uses `gati::Clock` fixed-step updates to animate convergence deterministically.

---

## Core Design

- **Backend agnostic**: any backend satisfying `PlotBackend` works.
- **Policy based**: layout behavior customized via `ForceSpringPolicy` template.
- **Header only**: all logic in `include/rekha/`.
- **No virtual dispatch**: concept constraints + compile-time monomorphization.
- **Pay-for-what-you-use**: no graph layout/runtime cost unless graph APIs are instantiated.

---

## Quick Example

```cpp
#include <rekha/rekha.hpp>

int main() {
    rekha::XYSeries trend("latency");
    trend.add(1.0f, 2.1f).add(2.0f, 1.7f).add(3.0f, 1.4f);

    rekha::Figure fig;
    fig.axes({"build", "ms", 5})
       .add(rekha::LinePlot{trend});

    rekha::KalpanaBackend backend;
    fig.render(backend);

    auto pixels = backend.rasterize(); // ARGB8888 frame
    return pixels.empty() ? 1 : 0;
}
```

---

## Public API Map

- `rekha/types.hpp`
    - `Scalar`, `Vec2`, `Range`, `Margin`, `Viewport`, styles.
- `rekha/series.hpp`
    - `XYSeries`, `Graph`, `Edge`.
- `rekha/scales.hpp`
    - `LinearScale`, `Log10Scale`.
- `rekha/graph.hpp`
    - `ForceSpringPolicy`, `LayoutConfig`, `ForceDirectedLayout`, `GraphLayoutRuntime`.
- `rekha/backend.hpp`
    - `PlotBackend` concept, `KalpanaBackend`.
- `rekha/plot.hpp`
    - `Figure`, `Axes`, subplot/annotation/legend controls, and plot variants (`LinePlot`, `AreaPlot`, `StepPlot`,
      `StemPlot`, `ScatterPlot`, `BubblePlot`, `ErrorBarPlot`, `BarPlot`, `HistogramPlot`, `HeatmapPlot`, `PiePlot`,
      `GraphPlot`).
- `rekha/rekha.hpp`
    - umbrella include.

---

## Extension Points

1. Add a backend by implementing the `PlotBackend` concept methods.
2. Add new plot variants by extending the `Plot` variant and drawing visitor.
3. Customize force layout dynamics by supplying a custom policy to `ForceDirectedLayout<Policy>`.

---

## Testing

Coverage entry point:

- `src/tests/rekha/test_rekha.cpp`

Test cases validate:

- mixed-plot dispatch to backend primitives,
- force-directed layout constraints,
- Kalpana rasterization path.

---

## Example Runner

`src/examples/exemain.cpp` now accepts:

- `pebble rekha capture`
- `pebble rekha sokol`
- `pebble rekha notcurses`

Dedicated executable is also available:

- `pebble_rekha_demo capture`
- `pebble_rekha_demo sokol`
- `pebble_rekha_demo notcurses`

Windowed showcase target:

- `pebble_rekha`

Keyboard controls:

- `1` toggle signal subplot
- `2` toggle graph subplot
- `3` toggle heatmap subplot
- `4` toggle distribution subplot
- `0` enable all subplots
- `L` toggle legend visibility
- `A` toggle legend auto-placement
- `R` reset graph layout
- `ESC` quit

If no backend is passed, default is selected in this order:

- `notcurses` (when enabled)
- `sokol` (when enabled)
- `capture` (fallback)

Sokol/Notcurses options are available only when enabled via CMake flags:

- `-DPEBBLE_ENABLE_KALPANA_SOKOL_BACKEND=ON/OFF`
- `-DPEBBLE_ENABLE_KALPANA_NOTCURSES_BACKEND=ON/OFF`

