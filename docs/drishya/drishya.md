# Drishya (दृश्य) — Backend-Agnostic Retained-Mode Widget & UI Engine

Header-only C++23. No virtual dispatch, no macros, no RTTI, zero heap on hot paths. Drishya composes UI from
concept-satisfying value types, lays them out with `akruti::layout`, routes pointer/keyboard input, animates layout
changes with `spandana` springs, and paints through any `Painter` adapter. `[[no_unique_address]]` policies mean you pay
only for the widgets, backends, and motion you actually instantiate. One widget vocabulary serves both AI/ML dashboards
and game HUDs.

Include: `#include <drishya/drishya.hpp>`

---

## Table of Contents

1. [Design Principles](#1-design-principles)
2. [Architecture](#2-architecture)
3. [The Widget & Painter Concepts](#3-the-widget--painter-concepts)
4. [Type-Erasure: `AnyWidgetT`](#4-type-erasure-anywidgett)
5. [The Retained Tree: Handles & Dirty Tracking](#5-the-retained-tree-handles--dirty-tracking)
6. [Layout Bridge & Size Units](#6-layout-bridge--size-units)
7. [Reactive State: Signals & Binding](#7-reactive-state-signals--binding)
8. [Input Routing](#8-input-routing)
9. [Reflow Motion](#9-reflow-motion)
10. [The Painter & Backends](#10-the-painter--backends)
11. [Widget Catalog](#11-widget-catalog)
12. [Authoring a Custom Widget](#12-authoring-a-custom-widget)
13. [Composition: Fluent Builder & EDSL](#13-composition-fluent-builder--edsl)
14. [Tree Operations](#14-tree-operations)
15. [Theming](#15-theming)
16. [Testing: Headless Snapshot](#16-testing-headless-snapshot)
17. [Examples](#17-examples)

---

## 1. Design Principles

- **Concept-based, monomorphic.** A widget is any value type modeling the `Widget<W, Metrics>` and `PaintableWith<W, P>`
  concepts — no base class, no vtable, no registration. A `Button` painting to two backends is two monomorphized
  functions with no shared virtual dispatch.
- **Reuse, don't reinvent.** Geometry is `akruti::layout::{Rect2D, Size2D, Bounds2D, Edges}`; colors are packed
  `0xAARRGGBB` `std::uint32_t`; reactive cells are `containers::reactive::Signal`; motion is `spandana`. Drishya
  introduces no parallel vocabularies.
- **Backend-agnostic.** Drishya never talks to a GPU/terminal/window. It emits an immediate-mode drawing vocabulary
  against a `Painter` concept and pulls input from a host-supplied `InputFrame`. `KalpanaPainter` is the reference
  adapter; the same widget code drives a headless capture canvas, a GPU canvas, or a terminal cell grid.
- **Pay for what you use.** Empty policies (`NullMotion`) are stored `[[no_unique_address]]` and cost zero bytes. Static
  trees carry no per-node heap; type-erasure and reflow springs are opt-in.
- **Retained.** Widgets persist across frames in a flat SoA arena; only dirty subtrees re-solve, matching
  `akruti::layout`'s incremental model.

**Non-goals.** Drishya does not own a window/GL context, a font rasterizer, or an OS event loop — it consumes them
through concepts. It does not simulate physics or run a clock; it reads a host-supplied frame `dt`.

---

## 2. Architecture

`App<Metrics, Painter, Motion, InlineBytes>` owns a frame's worth of state: the retained tree, the layout bridge, the
input router, and the reflow motion policy. The `(Metrics, Painter)` pair is fixed once — it determines the erased
widget type — and `Motion` defaults to `NullMotion` (snap).

```cpp
template <typename Metrics, typename Painter_, typename Motion = NullMotion,
          std::size_t InlineBytes = 512>
    requires ITextMetrics<Metrics> && Painter<Painter_> && ReflowMotion<Motion>
class App;

using DefaultApp = App<MonospaceMetrics, DefaultPainter>;   // headless, snap reflow
```

The public surface:

| Method                                                  | Effect                                                                                                                           |
|:--------------------------------------------------------|:---------------------------------------------------------------------------------------------------------------------------------|
| `set_root(w) -> NodeId`                                 | Replace the root subtree; marks the tree structurally dirty.                                                                     |
| `add_child(parent, w) -> NodeId`                        | Append a child; marks the tree structurally dirty.                                                                               |
| `remove(NodeId)`                                        | Remove a node and its subtree.                                                                                                   |
| `set_viewport(Rect2D)`                                  | Set the solve viewport; marks geometry dirty if it changed.                                                                      |
| `solve()`                                               | (Re)layout only if dirty. Structural change → full rebuild + solve; geometry/style change → incremental solve; no-op when clean. |
| `pump(InputFrame, scale=1) -> RouteResult`              | Solve, then route the input frame to widgets.                                                                                    |
| `tick(dt) -> bool`                                      | Advance reflow motion; returns `true` while still animating (`NullMotion` always `false`).                                       |
| `paint(Painter&)`                                       | Solve, then walk the tree pre-order painting each widget at its motion-adjusted rect.                                            |
| `clear()`                                               | Reset tree, focus, and motion.                                                                                                   |
| `invalidate_style(NodeId)` / `invalidate_paint(NodeId)` | Mark a node stale for the next solve/paint.                                                                                      |
| `tree()` / `layout()` / `router()` / `motion()`         | Subsystem accessors.                                                                                                             |

A typical frame drives the subsystems in order:

```cpp
void frame(float dt, const InputFrame& in, DefaultPainter& painter) {
    app.pump(in);            // route pointer/keys → widget.on_event
    app.tick(dt);            // advance reflow springs
    app.solve();             // (re)layout dirty subtrees
    painter.begin_frame();
    app.paint(painter);      // emit draw commands at resolved rects
    painter.present();       // hand the scene to the backend canvas
}
```

`pump()` and `paint()` each call `solve()` internally, so an explicit `solve()` is only needed when you want the
resolved geometry between those steps.

---

## 3. The Widget & Painter Concepts

A widget models three hooks, decomposed into named concepts (all in `drishya/widget_concept.hpp`):

```cpp
template <typename W, typename Metrics>
concept MeasurableWith = ITextMetrics<Metrics> &&
    requires(const W& w, MeasureCtxT<Metrics> mc) {
        { w.measure(mc) } -> std::convertible_to<Size2D>;   // intrinsic size
    };

template <typename W>
concept Styled = requires(const W& w) {
    { w.style() } -> std::convertible_to<LayoutStyle>;       // akruti layout style
};

template <typename W>
concept EventHandler = requires(W& w, EventCtx ec) {
    { w.on_event(ec) } -> std::convertible_to<EventResult>;  // input already hit-tested
};

template <typename W, typename Metrics>
concept Widget = MeasurableWith<W, Metrics> && Styled<W> && EventHandler<W>;
```

`measure`/`style` feed the layout solver directly (no translation layer). Painting is checked separately against the
concrete backend so it stays monomorphic:

```cpp
template <typename W, typename P>
concept PaintableWith = Painter<P> && requires(const W& w, P& painter, Rect2D box) {
    { w.paint(painter, box) };
};
```

**Context types.** `MeasureCtxT<Metrics>` carries a `const Metrics&` (a compile-time `ITextMetrics`, no virtual) plus a
display `scale`. `EventCtx` carries the immutable `InputFrame`, the widget's resolved `box`, the active `clip`, and
`scale`, with helpers `pointer_inside()` and `local_pointer()` — routing has already resolved the hit, so widgets never
re-run hit tests.

**Input.** Drishya owns its `InputFrame` (pointer position/delta, held/previous button bitmasks, wheel, a span of
`KeyEvent`, committed UTF-8 `text`) because `gati` models input as abstract axes only. Edge detection is built in via
`pressed()`/`released()`/`held()`.

**Event result.**

```cpp
enum class EventResult : std::uint8_t {
    Ignored = 0,        // not handled; keep bubbling
    Consumed = 1,       // handled; stop bubbling
    CapturePointer = 2, // handled; route subsequent pointer input here
    ReleasePointer = 3, // handled; release a prior pointer capture
};
```

The `Painter` concept is an immediate-mode drawing vocabulary; the adapter translates it to whatever the backend needs:

```cpp
template <typename P>
concept Painter = requires(P& p, const P& cp, Rect2D rect, Bounds2D clip,
                           Vec2 a, Vec2 b, float r, float w, std::string_view s, float fs) {
    { p.push_clip(clip) }; { p.pop_clip() };
    { p.fill_rect(rect) }; { p.stroke_rect(rect, w) }; { p.round_rect(rect, r) };
    { p.line(a, b, w) };
    { p.text(s, a, fs) };
    { cp.measure_text(s, fs) } -> std::convertible_to<Size2D>;
};

template <typename P>                       // most painters also expose a draw color
concept ColorPainter = Painter<P> && requires(P& p, std::uint32_t argb) { { p.set_color(argb) }; };
```

Widgets guard `set_color` with `if constexpr (ColorPainter<P>)`, so they remain valid against a color-less painter.

---

## 4. Type-Erasure: `AnyWidgetT`

Widgets are stored in the tree type-erased through `AnyWidgetT<Metrics, Painter, InlineBytes = 512>`. It holds any value
satisfying `Widget<W, Metrics>` and `PaintableWith<W, Painter>` inline in a fixed byte buffer and dispatches through a
`static constexpr` free-function vtable — no virtual, no RTTI, no heap for widgets that fit. This mirrors
`spandana::BasicAction`'s erasure pattern.

- The vtable holds `{measure, style, on_event, paint, move_construct, destroy}`, each a stateless lambda.
- The holder is **move-only**. Construction `static_assert`s that the widget fits `InlineBytes`, is not over-aligned,
  and is nothrow-move-constructible.
- `InlineBytes` defaults to **512**: every stock widget embeds a full `LayoutStyle` (~336 B) plus its own state, so the
  largest (`TextField`, `Button`) land near 480 B. A project using only small custom widgets can instantiate a narrower
  `AnyWidgetT<..., 128>` to shrink each tree node.

The `(Metrics, Painter)` pair is a template parameter because `measure()` takes a `MeasureCtxT<Metrics>` and `paint()`
takes a `Painter&`. Pick one pair per app; `DefaultApp` uses `MonospaceMetrics` + `DefaultPainter`.

---

## 5. The Retained Tree: Handles & Dirty Tracking

`WidgetTree<Metrics, Painter, InlineBytes>` is a flat structure-of-arrays arena addressed by a contiguous `NodeId`
(`std::uint32_t`; `kInvalidNode = 0xFFFFFFFF`). Links are `first_child` / `next_sibling` / `parent` indices — the same
contiguous-index addressing `akruti`'s layout engine uses, so the layout bridge maps a `NodeId` onto a layout node with
no hashing.

`containers::NAryTree` is deliberately *not* used: it heap-allocates a node per element. The arena stays flat and reuses
`containers` only where they fit:

- `containers::slot_map<NodeId, WidgetHandle>` mints **stable, generation-checked handles** (`WidgetHandle`) the app
  holds across rebuilds. A raw `NodeId` is a slot index; a `WidgetHandle` survives churn — `make_handle(id)`,
  `resolve(h) -> NodeId`, `alive(h)`.
- `sparseset::SparseSet<NodeId, std::uint8_t>` is the `O(1)` **dirty set**.

Dirty bits are merged, not overwritten:

```cpp
enum DirtyBits : std::uint8_t {
    kDirtyNone   = 0,
    kDirtyLayout = 1u << 0, // style/measure changed → relayout subtree
    kDirtyPaint  = 1u << 1, // visual-only change → repaint, geometry stable
    kDirtyTree   = 1u << 2, // structural change (insert/remove/move)
};
```

Traversal helpers: `walk(id, fn)` / `walk(fn)` (pre-order DFS, `fn(NodeId, depth)`), `for_each_child(id, fn)`, plus
`widget(id)`, `parent(id)`, `first_child(id)`, `next_sibling(id)`, and `valid(id)`. `drain_dirty(fn)` consumes and
clears the dirty set once per frame. `set_root` and `add_child` mark the appropriate bits automatically; `remove` frees
the subtree iteratively (no recursion-depth risk) and reclaims handles.

---

## 6. Layout Bridge & Size Units

`LayoutBridge<Metrics, Painter, InlineBytes>` translates the retained tree into an `akruti::layout::Engine` solve and
exposes each widget's resolved rect back by `NodeId`.

- **Rebuild** (on any structural change): author an `akruti::layout::LayoutTree` in widget pre-order, stamping each
  layout node's `user_tag` with its source `NodeId`, then `bake`. A `NodeId ↔ layout-index` map is reconstructed from
  the baked `user_tag` column.
- **Incremental** (style-only change): `update_style(tree, id)` re-pushes a widget's `LayoutStyle` and marks the
  affected node dirty, so `solve_incremental(viewport)` reprocesses only that subtree.
- **Text metrics** reach the solver via `akruti::layout::make_text_measure(metrics)` — a zero-alloc trampoline over the
  host's `ITextMetrics` (the same object the painter measures with).

Resolved geometry is addressed by `NodeId`: `rect(id) -> Rect2D`, `clip(id) -> Bounds2D`, and `hit(x, y) -> NodeId`.

**Size units** live in `akruti::layout::SizeSpec` (used directly, no wrapper):

| Constructor            | Meaning                                                               |
|:-----------------------|:----------------------------------------------------------------------|
| `SizeSpec::Px(v)`      | Absolute pixels.                                                      |
| `SizeSpec::Percent(v)` | Percent of the parent content box.                                    |
| `SizeSpec::Fr(v)`      | Fractional weight — distributes free main-axis space (CSS-grid `fr`). |
| `SizeSpec::Content`    | Intrinsic (`measure`) size.                                           |
| `SizeSpec::Aspect(r)`  | Cross axis derived from the resolved main axis.                       |
| `SizeSpec::Auto`       | Solver default.                                                       |

Bounded sizing uses `SizeSpecClamp{min, pref, max}` set through `LayoutStyle::width_clamp` /
`LayoutStyle::height_clamp`, where each field may itself be any `SizeSpec`. Alongside
`LayoutStyle::{axis, align_items, justify_content, padding, flex_grow, overflow_x, overflow_y, scroll_offset}`, this is
the full styling surface — Drishya adds no parallel style model.

```cpp
auto sidebar = w::vstack(8.0f);
sidebar.style_.width_clamp = { SizeSpec::Px(200.f), SizeSpec::Percent(20.f), SizeSpec::Px(320.f) };
content.style_.width = SizeSpec::Fr(1.0f);   // eats remaining space
```

---

## 7. Reactive State: Signals & Binding

`drishya/reactive.hpp` re-exports the generic `containers::reactive` primitives unchanged and adds UI-facing binding
helpers. Signals are generic value cells, so any subsystem can use them; Drishya only supplies the conveniences.

`Signal<T, ObserverInlineBytes = 256>`:

| Member                         | Effect                                                                          |
|:-------------------------------|:--------------------------------------------------------------------------------|
| `get()` / `operator()`         | Read the current value.                                                         |
| `set(next)`                    | Assign and always notify observers.                                             |
| `set_if_changed(next) -> bool` | Assign and notify only on inequality.                                           |
| `mutate(fn)`                   | Mutate in place, then notify.                                                   |
| `subscribe(fn) -> ObserverId`  | Register a zero-argument observer; the id survives other subscribe/unsubscribe. |
| `unsubscribe(id)`              | Remove an observer.                                                             |
| `observer_count()`             | Live observer count.                                                            |

`Computed<F>` is a memoized derived value: call `depend_on(signal)` for each dependency; a dependency change marks it
dirty and the next `get()` / `operator()` recomputes.

Binding helpers:

```cpp
// bind(source, setter): mirror the signal into a setter on every change (and once now).
Signal<int> count{0};
bind(count, [&](const int& v){ label.set_text(std::to_string(v)); });

// bind_signal(dst, src): keep dst equal to src.
```

Because widgets are value types erased in the tree (no RTTI reach-in), the retained-mode idiom for reactive UI is:
mutate the `Signal`, rebuild the affected node's widget from its new value, and mark it `kDirtyPaint`:

```cpp
Signal<float> health{1.0f};
const NodeId hp_id = app.add_child(status_id, w::health_bar(health.get()));

bind(health, [&](const float& hpv) {
    auto g = w::health_bar(hpv);
    g.style_.width = SizeSpec::Percent(100.0f);
    app.tree().widget(hp_id) = App<M, P, SpringReflow>::widget_type{std::move(g)};
    app.tree().mark_dirty(hp_id, kDirtyPaint);
});
// later: health.set(0.35f);   // took a hit → the gauge rebuilds and repaints
```

---

## 8. Input Routing

`Router<Metrics, Painter, InlineBytes>::route(tree, bridge, input, scale)` turns one `InputFrame` into `on_event()`
calls on the widgets that should see it, using the solved layout to decide who is under the pointer and who owns focus.
`App::pump()` wraps it. The pipeline:

1. **Pointer capture** takes priority. A widget that returned `CapturePointer` (a slider thumb, a drag) receives every
   subsequent pointer frame directly, bypassing hit-testing, until it returns `ReleasePointer` or dies.
2. **Hit + bubble.** Otherwise `akruti`'s `hit_test_chain` yields the node under the pointer and its ancestor chain
   (leaf → root). Delivery is leaf-first, stopping at the first handled result. A left-press that lands claims keyboard
   focus.
3. **Focus + keys.** Key/text frames go to the focused widget. `Tab` / `Shift+Tab` advance focus through leaves in
   layout order (`for_each_leaf` = tab order) — handled by the router itself, not delivered as a key.

`route` returns a `RouteResult { hovered, consumed_by, pointer_handled, key_handled }`. Focus can be driven explicitly
with `set_focus(id)`, `focus_next(bridge)`, `focus_prev(bridge)`. Scratch buffers for the chain (64) and leaf
enumeration (256) are stack-bounded — no heap on the hot path.

---

## 9. Reflow Motion

When the solver produces a new rect for a node, the `ReflowMotion` policy decides how the node travels from its previous
rect to the new one:

```cpp
template <typename M>
concept ReflowMotion = requires(M& m, NodeId id, Rect2D target, float dt) {
    { m.resolve(id, target, dt) } -> std::convertible_to<Rect2D>;
    { m.settled() } -> std::convertible_to<bool>;
};
```

- **`NullMotion`** (default) — snaps to the target instantly. Empty type, `[[no_unique_address]]`-friendly, zero cost.
  Correct for HUDs and anything that must reflect state immediately.
- **`SpringReflow`** — eases each moved node from its previous rect to the target through a `spandana::RectSpring`
  (component-wise closed-form damped spring over the 4-float rect). Springs are created lazily on the first move and
  dropped once settled, so steady-state cost is zero. They live in a `SparseSet` keyed by `NodeId`; constructible as
  `SpringReflow{stiffness, damping}` (defaults `180.0f` / `20.0f`).

```cpp
App<M, P, SpringReflow> app(metrics);   // panels glide to new rects on resize/insert/remove
```

`App::tick(dt)` calls the policy's `begin_frame()` (if present) and reports whether motion is still settling.

---

## 10. The Painter & Backends

`KalpanaPainter<Canvas, Metrics>` is the reference adapter. `kalpana` is a *retained* scene-graph renderer; the
`Painter` concept is *immediate-mode*. The adapter bridges the two: each immediate call appends a `kalpana::Node` to a
`kalpana::Scene` it owns for the current frame, and `present()` hands the finished scene to `canvas.render(scene)`.

Frame lifecycle and primitives:

```cpp
painter.begin_frame();                 // clear the scene + clip stack, reset color
painter.set_color(0xFF3B82F6u);        // packed 0xAARRGGBB
painter.fill_rect(box);
painter.stroke_rect(box, 2.0f);
painter.round_rect(box, 6.0f);
painter.line(a, b, 1.0f);
painter.push_clip(bounds); /* ... */ painter.pop_clip();
painter.text("label", pos, 14.0f);
painter.image(pixels, w, h, dst);      // ARGB8888 blit
painter.present();                     // canvas.render(scene)
```

- **Color** — a packed `0xAARRGGBB` `std::uint32_t` is converted to a linear `kalpana::Color` via `argb_to_color`.
- **Clipping** — `kalpana` has no clip API, so the adapter maintains a scissor stack drishya-side and intersects/skips
  draws against the top clip.
- **Text metrics** — `kalpana` has no text metrics, so the painter holds a `const Metrics&` and delegates `measure_text`
  to it. `MonospaceMetrics` (fixed 8 px advance, 16 px line) is the fallback for headless tests and blocky HUDs.

`DefaultPainter = KalpanaPainter<kalpana::DefaultCanvas, MonospaceMetrics>` is a capture-backed, headless-friendly
painter. Swapping `DefaultCanvas` for a GPU or terminal canvas leaves widget code unchanged.

**`DrawList`** (`painter/draw_list.hpp`) is an optional SoA command buffer: `set_color` / `push_clip` / `pop_clip` /
`fill_rect` / `stroke_rect` / `round_rect` / `line` / `text` record into parallel `std::vector`s keyed by an op stream,
and `replay(painter)` flushes them against a real `Painter`. `clear()` keeps capacity so a reused list never
re-allocates. It is a cold-path recording aid, not per-frame-critical.

---

## 11. Widget Catalog

All stock widgets live in `drishya/widgets/` (one header per family; each is independently includable). Most inherit
`widgets::WidgetBase`, which supplies trivial defaults (`style()` returning `style_`, empty `measure`, `Ignored` events,
no-op `paint`) — a widget overrides only what differs. Inheritance here is pure code reuse: dispatch stays static
through `AnyWidgetT`'s vtable.

**Containers** (`containers.hpp`) — carry layout intent and optionally a backdrop; children are separate tree nodes.
`Stack` (`vstack(pad)` / `hstack(pad)`), `Grid` (`grid(cols, pad)`), `Spacer` (`spacer()` grows, `strut(px)` fixed),
`Panel` (`panel()`) and `Card` (`card(surface, radius=8)` — paints a rounded fill), `ScrollArea` (`scroll_area()` —
clipping viewport, `overflow_y=Scroll`), `Splitter` (`hsplit(ratio)` / `vsplit(ratio)` — two `Fr`-weighted panes),
`Tabs` (`tabs(active)`). `akruti`'s `LayoutStyle` has no inter-child gap field; spacing is expressed with `spacer`/
`strut` or per-child style.

**Display** (`display.hpp`) — `Label` (`label(text)`; `.set_text/.set_color/.size`), `Icon`, `Separator`, `Badge`,
`Progress` (`value` in [0,1], `track`/`fill` colors), `Spinner`, `Tooltip`.

**Inputs** (`inputs.hpp`) — `Button` (`button(text)`; rounded label, fires `Callback on_click` on left press+release
inside the box, tracks `hovered`/`pressed`), `Toggle`, `Checkbox` (both flip on click and fire a
`BasicCallback<64> on_change`), `Slider` (drags via the capture protocol: `CapturePointer` on press, writes `value` in
`[min,max]` and fires `on_change` each move, `ReleasePointer` on release), `TextField` (single-line edit: appends
committed text, handles `Backspace`, paints a focus border), `Select` (click cycles options).

**Data / dashboards** (`data.hpp`) — `Sparkline` (`values`, `color`; a polyline auto-scaled to the box), `StatTile`
(`caption`/`value`/`delta` with `value_color`/`caption_color`/`delta_color`/`value_size`), `ListView` (**virtualized** —
`row_count`, `row_height`, `scroll_y`; only visible rows are painted, so millions of logical rows stay bounded), `Table`
(virtualized rows + header), `Chat` (`ChatMessage` list + composer, for LLM UIs).

**Game / HUD** (`game.hpp`) — `Gauge` (`value` in [0,1], `fill`/`low_color` switching below `low_threshold`;
`health_bar(v)` is a preset), `Crosshair` (`color`, `center_dot`), `RadialMenu`, `DamageNumber` (float-up with alpha
scaled by life), `NinePatch`, `WorldAnchor` (screen-projects a world position each frame).

**Stubs** (`stubs.hpp`) — ~25 concept-complete placeholders that satisfy `Widget`/`PaintableWith` (via `StubBase`) so
they compose and lay out today, awaiting full paint bodies: `Markdown`, `CodeEditor`, `TextArea`, `LogView`, `TreeView`,
`Chart`, `Heatmap`, `ImageGrid`, `ColorPicker`, `DatePicker`, `FileDrop`, `Combo`, `RangeSlider`, `NumberField`,
`RadioGroup`, `Accordion`, `Overlay`, `Modal`, `Popover`, `Drawer`, `Avatar`, `Image`, `InventoryGrid`, `Minimap`,
`DialogueBox`, `Hotbar`.

---

## 12. Authoring a Custom Widget

A new widget is any value type satisfying `Widget<W, Metrics>` and `PaintableWith<W, P>`. No base class, macro, or
registration is required — inherit `WidgetBase` purely to skip the trivial hooks, or write them all out. It must be
nothrow-move-constructible and fit the `AnyWidget` buffer.

```cpp
#include <drishya/drishya.hpp>
using namespace pebble::drishya;

// (1) Reuse WidgetBase; override only measure() + paint().
struct VuMeter : widgets::WidgetBase {
    float level = 0.0f;                       // 0..1
    std::uint32_t segments = 12;
    std::uint32_t on_color = 0xFF22C55Eu, off_color = 0x40FFFFFFu;

    explicit VuMeter(float lvl) noexcept : level(lvl) {
        style_.width = akruti::layout::SizeSpec::Px(24.0f);
    }

    template <ITextMetrics Metrics>
    Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        return Size2D{24.0f, static_cast<float>(segments) * 6.0f};
    }

    template <typename P> requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        const auto lit = static_cast<std::uint32_t>(level * segments + 0.5f);
        for (std::uint32_t i = 0; i < segments; ++i) {
            if constexpr (ColorPainter<P>) painter.set_color(i < lit ? on_color : off_color);
            painter.fill_rect(/* segment rect from box */ box);
        }
    }
};

// (2) Standalone: no base, every hook by hand.
struct Dot {
    LayoutStyle style_{};
    std::uint32_t color = 0xFFF59E0Bu;
    float diameter = 10.0f;
    LayoutStyle style() const noexcept { return style_; }
    template <ITextMetrics Metrics>
    Size2D measure(const MeasureCtxT<Metrics>&) const noexcept { return {diameter, diameter}; }
    EventResult on_event(EventCtx&) noexcept { return EventResult::Ignored; }
    template <typename P> requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(color);
        painter.round_rect(box, diameter * 0.5f);
    }
};

static_assert(Widget<VuMeter, MonospaceMetrics> && PaintableWith<VuMeter, DefaultPainter>);
static_assert(Widget<Dot, MonospaceMetrics> && PaintableWith<Dot, DefaultPainter>);
```

Both drop straight into the same `App` tree; each is monomorphized into every backend it paints to. Nothing in Drishya's
core changes.

---

## 13. Composition: Fluent Builder & EDSL

Two front ends, one widget vocabulary.

**Fluent (runtime).** `ui::root(app, w)` mounts a root and returns a `Builder`; `.child(w)` mounts a leaf and returns
`*this` for flat chaining; `.nest(w)` mounts a container child and returns a `Builder` positioned at it so you can
descend. Mounting is eager (each call reaches into `App::add_child`); this is the cold build path.

```cpp
using namespace pebble::drishya;
auto root = ui::root(app, widgets::vstack(16.f));
root.child(widgets::label("Title"))
    .child(widgets::button("OK"));
auto row = root.nest(widgets::hstack());
row.child(widgets::button("Yes")).child(widgets::button("No"));
```

**EDSL (compile-time tree).** `edsl::node(widget, child, child, ...)` nests into a `std::tuple`, so the whole hierarchy
is one value with its shape known at compile time. `mount(app)` realizes the tuple into the retained tree (type-erasing
each widget exactly as the fluent builder does — the tuple only shapes construction order, not storage). Style modifiers
compose with `operator|`, each returning the same widget by value with one `style_` field set (`pad`, `flex`, `align`,
`justify`, `width`, `height`), and the `_px` literal yields a `SizeSpec::Px`:

```cpp
using namespace pebble::drishya::edsl;
auto view = node(vstack_(16) | pad(12) | flex(1),
                 label_("Title") | align(Align::Center),
                 node(hstack_(),
                      button_("OK") | width(120_px),
                      button_("Cancel")));
view.mount(app);
```

---

## 14. Tree Operations

`drishya/ops.hpp` provides free-function algorithms over a `WidgetTree` (and, where geometry matters, a solved
`LayoutBridge`). All are non-owning and heap-free on the hot path — result sets are written into a caller-supplied span.

| Operation                           | Signature (sketch)  | Notes                                                           |
|:------------------------------------|:--------------------|:----------------------------------------------------------------|
| `visit(tree, fn)`                   | `fn(NodeId, depth)` | Pre-order walk.                                                 |
| `find(tree, pred)`                  | `-> NodeId`         | First node matching `pred`; `kInvalidNode` if none.             |
| `find_all(tree, pred, out)`         | `-> std::size_t`    | Count matches; writes up to `out.size()` (overflow detectable). |
| `subtree_size(tree, id)`            | `-> std::size_t`    | Node count in a subtree.                                        |
| `depth_of(tree, id)`                | `-> std::size_t`    | Distance from root.                                             |
| `hit(tree, bridge, x, y)`           | `-> NodeId`         | Delegates to `bridge.hit`.                                      |
| `subtree_bounds(tree, bridge, id)`  | `-> Rect2D`         | Union of solved rects.                                          |
| `measure(tree, id, metrics, scale)` | `-> Size2D`         | Intrinsic size of one widget.                                   |

---

## 15. Theming

`drishya/theme.hpp` supplies constexpr design tokens. A `Theme` bundles a `Palette` (semantic color roles), a `Spacing`
ramp, `Radii`, a `TypeScale`, and a `ChartColors` categorical palette (8 hues chosen for perceptual separation on dark
backgrounds). Tokens are packed `0xAARRGGBB` so they cross the `Painter` boundary without conversion; `to_color(argb)`
yields the linear `kalpana::Color` form. `dark()` and `light()` presets ship built in.

```cpp
constexpr auto t = theme::dark();
button.background = t.color.primary;
sparkline.color   = t.chart.at(0);
```

---

## 16. Testing: Headless Snapshot

The `DefaultPainter` renders into a `kalpana::DefaultCanvas` capture backend, giving deterministic snapshot pixels with
no GPU or terminal — regression-safe rendering tests. The pattern (matching the shipped examples):

```cpp
MonospaceMetrics metrics;
App<MonospaceMetrics, DefaultPainter> app(metrics);

app.set_root(/* ... build tree via add_child ... */);

kalpana::DefaultCanvas canvas(640, 400);
app.set_viewport(Rect2D{0.f, 0.f, 640.f, 400.f});

DefaultPainter painter(canvas, metrics);
painter.begin_frame();
painter.set_color(0xFF0B0E12u);
painter.fill_rect(Rect2D{0.f, 0.f, 640.f, 400.f});
app.paint(painter);              // solve + walk + paint
painter.present();

const std::vector<std::uint32_t> px = canvas.snapshot();   // 640*400 ARGB pixels
// assert px.size() and app.tree().node_count()
```

Concept conformance is checked at compile time: `static_assert(Widget<W, MonospaceMetrics>)`,
`static_assert(PaintableWith<W, DefaultPainter>)`.

---

## 17. Examples

Runnable, headless examples covering every path above:

- `src/examples/drishya_ml_dashboard.cpp` — KPI `StatTile`s, a loss `Sparkline`, and a `Progress` bar composed onto a
  headless canvas.
- `src/examples/drishya_game_hud.cpp` — `health_bar`/`Gauge` bound to a reactive `Signal`, a `Crosshair`, an ability
  row, pointer routing, and `SpringReflow`.
- `src/examples/drishya_custom_widget.cpp` — authoring a `VuMeter` (via `WidgetBase`) and a standalone `Dot`.
- `src/examples/drishya_terminal_dashboard.cpp` — the same vocabulary sized to a small blocky canvas: an `hsplit` of a
  status column and a virtualized `ListView`.
