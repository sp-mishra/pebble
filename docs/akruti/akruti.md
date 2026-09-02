# Akruti (आकृति) — Header-Only 2D Shape, Geometry & Narrowphase Engine

Header-only C++23/C++26. No virtual dispatch, no macros. Concept-based static dispatch, zero heap allocation on hot
paths. Akruti is Pebble's high-performance 2D geometric and narrowphase collision substrate, engineered for **60–240 FPS
real-time games** and **100k+ body physical simulations**: analytic primitives, Separating Axis Theorem (SAT) 2-point
manifolds, GJK/EPA, continuous collision detection (CCD), zero-heap expression template CSG, Fortune's Voronoi fracture
(*Khanda*), dynamic rigid bodies, and Google Highway SIMD sweeps.

Include: `#include <akruti/akruti.hpp>`

---

## Table of Contents

1. [Overview & Core Design](#1-overview--core-design)
2. [Subsystem Architecture](#2-subsystem-architecture)
3. [Algorithmic Foundations & Mathematical Formulations](#3-algorithmic-foundations--mathematical-formulations)
    - [The `Shape` Concept Contract](#31-the-shape-concept-contract)
    - [Analytic Narrowphase & SAT 2-Point Contact Manifolds](#32-analytic-narrowphase--sat-2-point-contact-manifolds)
    - [GJK Overlap & EPA Penetration Depth](#33-gjk-overlap--epa-penetration-depth)
    - [Continuous Collision Detection (CCD) & Conservative Advancement](#34-continuous-collision-detection-ccd--conservative-advancement)
    - [Zero-Heap Expression Template CSG & AST Arenas](#35-zero-heap-expression-template-csg--ast-arenas)
    - [Khanda Fracture Pipeline & Exact Polar Inertia](#36-khanda-fracture-pipeline--exact-polar-inertia)
4. [Master Primitives & Complete Public API](#4-master-primitives--complete-public-api)
5. [Configuration, Defaults & Performance Tuning Guide](#5-configuration-defaults--performance-tuning-guide)
    - [Default Configuration Settings](#51-default-configuration-settings)
    - [How to Optimize Further (Extreme Narrowphase Throughput)](#52-how-to-optimize-further-extreme-narrowphase-throughput)
    - [How to Improve Geometric Quality & Anti-Tunneling Accuracy](#53-how-to-improve-geometric-quality--anti-tunneling-accuracy)
    - [Configuration Trade-Off Matrix](#54-configuration-trade-off-matrix)
6. [CSG Expression Template EDSL & AST Specifications](#6-csg-expression-template-edsl--ast-specifications)
7. [Google Highway SIMD Acceleration](#7-google-highway-simd-acceleration)
8. [Zero-to-Hero Tutorial](#8-zero-to-hero-tutorial)
    - [Step 1: Instantiating Primitives & SDF Queries](#step-1-instantiating-primitives--sdf-queries)
    - [Step 2: Zero-Heap CSG Boolean Modeling](#step-2-zero-heap-csg-boolean-modeling)
    - [Step 3: Generating SAT 2-Point Stacking Manifolds](#step-3-generating-sat-2-point-stacking-manifolds)
    - [Step 4: Continuous Collision Detection (Anti-Tunneling)](#step-4-continuous-collision-detection-anti-tunneling)
    - [Step 5: Dynamic Voronoi Fracture (
      *Khanda*) with Mass Properties](#step-5-dynamic-voronoi-fracture-khanda-with-mass-properties)
9. [Pebble Subsystem Reuse](#9-pebble-subsystem-reuse)

---

## 1. Overview & Core Design

Akruti avoids runtime virtual table lookups and polymorphic indirection through compile-time concept monomorphization:

- **Zero Heap Allocations on Hot Paths**: Point queries, raycasts, GJK/EPA, and SAT manifolds execute entirely on
  registers and stack memory.
- **Stable Multi-Point Stacking**: Fast SAT edge clipping generates 2-point contact manifolds for flat surfaces
  (OBB-OBB, Box-Box, Capsule-OBB).
- **Zero-Trig Adapters**: `TransformedShape<S>` provides translation and rotation without trigonometric re-evaluation
  for axis-aligned shapes.

---

## 2. Subsystem Architecture

```
Scene Layer (akruti::scene)    SoA Batches · LayerMasks · AABBTree BVH · Pravaha Task DAG
      │
      ▼
Narrowphase & SAT              SAT 2-point manifolds · O(1) Circle/Box/Capsule · Warm GJK
      │
      ▼
Fracture (Khanda) · CCD        Voronoi · EarClip · 2nd Moment Inertia · Conservative Adv
      │
      ▼
Advanced CSG                   Inlined Expression EDSL · Flat Arena AST · Dynamic CSG
      │
      ▼
Primitives & SIMD              Highway Vectorized Sweeps · OBB, Triangle, RoundedBox, Sector
      │
      ▼
Core Math (pebble::math)       vec2, mat2 (static_tensor), aabb2, cross, perp, dot, length
```

---

## 3. Algorithmic Foundations & Mathematical Formulations

### 3.1 The `Shape` Concept Contract

Any type `S` modeling `Shape` implements three fundamental geometric queries:

```cpp
template <typename S>
concept Shape = requires(const S& s, pebble::math::vec2 p, pebble::math::vec2 d) {
    { s.sdf(p) }      -> std::convertible_to<float>;              // Signed distance (d < 0 inside)
    { s.aabb() }      -> std::same_as<pebble::math::aabb2>;       // Conservative axis-aligned bounding box
    { s.support(d) }  -> std::convertible_to<pebble::math::vec2>; // Extreme point along direction d
    { s.centroid() }  -> std::convertible_to<pebble::math::vec2>; // Geometric centroid (mass center for uniform density)
};
```

### 3.2 Analytic Narrowphase & SAT 2-Point Contact Manifolds

For polygons $A$ and $B$, the Separating Axis Theorem (SAT) tests projection overlap along candidate face normals $n_i$:
$$\text{overlap}_i = (\max_{v \in A} v \cdot n_i - \min_{v \in A} v \cdot n_i) + (\max_{u \in B} u \cdot n_i - \min_{u \in B} u \cdot n_i) - \text{dist} (\text{proj}_A, \text{proj}_B)$$

- **Minimum Penetration**: Identifies the reference edge with minimal penetration depth $d_{\text{min}}$.
- **Incident Edge Clipping**: The incident face on the opposing body is clipped against the reference face's side
  planes, generating **2 contact points** with individual penetration depths for jitter-free resting stacks.

### 3.3 GJK Overlap & EPA Penetration Depth

- **Gilbert-Johnson-Keerthi (GJK)**: Evaluates whether the Minkowski
  difference $A \ominus B = \{ a - b \mid a \in A, b \in B \}$ contains the origin via an evolving 2-simplex.
- **Expanding Polytope Algorithm (EPA)**: If GJK finds an intersection, EPA iteratively expands the 2D polytope toward
  the Minkowski boundary to find the exact penetration depth vector $v_{\text{pen}} = d \cdot n$.

### 3.4 Continuous Collision Detection (CCD) & Conservative Advancement

To prevent fast-moving dynamic bodies from tunneling through thin obstacles, Akruti uses Conservative Advancement:
$$\Delta t_k = \frac{\max (0, \text{dist} (A (t_k), B (t_k)) - r_{\text{safe}})}{v_{\text{rel\_max}}}$$
$$t_{k+1} = t_k + \Delta t_k$$
Iterations continue until $\text{dist} < \epsilon_{\text{contact}}$ (Hit with Time-of-Impact $\tau = t_k$)
or $t_k > 1.0$ (No collision in time interval).

### 3.5 Zero-Heap Expression Template CSG & AST Arenas

CSG operations build expression trees without heap allocations:

- **Union (`a | b`)**: $\text{sdf}_{A \cup B} (p) = \min (\text{sdf}_A (p), \text{sdf}_B (p))$
- **Intersection (`a & b`)**: $\text{sdf}_{A \cap B} (p) = \max (\text{sdf}_A (p), \text{sdf}_B (p))$
- **Subtraction (`a - b`)**: $\text{sdf}_{A \setminus B} (p) = \max (\text{sdf}_A (p), -\text{sdf}_B (p))$
- **Smooth Union**: $\text{sdf}_{\text{smooth}} (a, b, k) = -\ln (e^{-k a} + e^{-k b}) / k$

### 3.6 Khanda Fracture Pipeline & Exact Polar Inertia

1. **Site Generation**: Impact-biased Poisson disk sampling generates seed sites $\{ s_1, \dots, s_N \}$.
2. **Voronoi Partitioning**: Sutherland-Hodgman clipping bounds each cell against polygon edges.
3. **Triangulation**: Ear-Clipping triangulates concave shards and bridges interior holes.
4. **Exact Mass Properties**:
    - Area: $A = \frac{1}{2} \sum (x_i y_{i+1} - x_{i+1} y_i)$
    - Centroid: $C = \frac{1}{6A} \sum (p_i + p_{i+1}) (x_i y_{i+1} - x_{i+1} y_i)$
    - Polar Moment of Inertia about Centroid:
      $$J = \frac{\rho}{12} \sum (x_i y_{i+1} - x_{i+1} y_i) \left (\|p_i - C\|^2 + (p_i - C)\cdot (p_{i+1} - C) + \|p_{i+1} - C\|^2 \right)$$

---

## 4. Master Primitives & Complete Public API

| Primitive / Query                   | Mathematical Signature                                       | Description                                                                                                                                                                                                                                                                               |
|:------------------------------------|:-------------------------------------------------------------|:------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `Circle`                            | `{vec2 center, float radius}`                                | Analytic $O(1)$ SDF ($\|p-c\| - r$) and sphere tracing.                                                                                                                                                                                                                                   |
| `Box`                               | `{vec2 center, vec2 half_extents}`                           | Axis-aligned box with branchless quadrant SDF.                                                                                                                                                                                                                                            |
| `OrientedBox`                       | `{vec2 center, vec2 half, mat2 rot}`                         | Rotated 2D OBB with SAT clipping fast-path.                                                                                                                                                                                                                                               |
| `Capsule`                           | `{vec2 a, vec2 b, float radius}`                             | Line-swept disk; distance to segment minus radius.                                                                                                                                                                                                                                        |
| `Segment`                           | `{vec2 a, vec2 b}`                                           | 1D line segment with normal derivation.                                                                                                                                                                                                                                                   |
| `Triangle`                          | `{vec2 a, vec2 b, vec2 c}`                                   | Barycentric coordinate interior distance query.                                                                                                                                                                                                                                           |
| `RoundedBox`                        | `{vec2 center, vec2 half, float r}`                          | Box inflated by smooth corner radius $r$.                                                                                                                                                                                                                                                 |
| `Sector`                            | `{vec2 center, float r, float angle, rot}`                   | Sensor FOV cone with exact boundary angle clamp.                                                                                                                                                                                                                                          |
| `ConvexPoly<N>`                     | `containers::static_vector<vec2, N>`                         | Fixed-capacity inlined convex polygon (zero heap).                                                                                                                                                                                                                                        |
| `raycast(shape, start, dir, max_t)` | $\to$ `RayHit{bool hit, float t, vec2 normal}`               | Analytic / sphere-traced ray intersection.                                                                                                                                                                                                                                                |
| `collide_obb_obb(a, b)`             | $\to$ `Manifold{bool hit, vec2 normal, float depth, points}` | 2-point contact manifold generation.                                                                                                                                                                                                                                                      |
| `closest_point(shape, p, h)`        | $\to$ `vec2`                                                 | Nearest **surface** point to `p` (SDF-gradient descent, step `h`).                                                                                                                                                                                                                        |
| `project(shape, p, h=1e-3)`         | $\to$ `vec2`                                                 | **Feasible** projection onto the solid interior ($\text{sdf}\le 0$): returns `p` unchanged when already inside, else `closest_point(shape,p,h)` on the boundary. Idempotent on feasible inputs — the clamp an optimizer wants for a 2D convex domain (see `kalpa`). `<akruti/query.hpp>`. |

---

## 5. Configuration, Defaults & Performance Tuning Guide

### 5.1 Default Configuration Settings

| Setting                    | Location       | Default Value          | Role / Effect                                                  |
|:---------------------------|:---------------|:-----------------------|:---------------------------------------------------------------|
| `gjk_max_iterations`       | `GjkConfig`    | `16`                   | Maximum simplex iterations before terminating GJK.             |
| `epa_max_iterations`       | `GjkConfig`    | `32`                   | Maximum polytope expansion steps in EPA.                       |
| `ccd_max_iterations`       | `CcdConfig`    | `10`                   | Maximum conservative advancement steps per ray sweep.          |
| `ccd_tolerance`            | `CcdConfig`    | `1e-3f` ($1\text{mm}$) | Separation distance considered a collision contact.            |
| `voronoi_relaxation_iters` | `KhandaConfig` | `2`                    | Lloyd relaxation sweeps on Poisson seeds for shard uniformity. |

### 5.2 How to Optimize Further (Extreme Narrowphase Throughput)

1. **Use Analytic Pair Dispatches**: Ensure pairs use `collide_circle_circle`, `collide_circle_box`, or
   `collide_obb_obb` rather than generic GJK/EPA. Analytic paths execute in **$4\text{–}12\text{ns}$**.
2. **Warm-Start GJK**: When generic convex polygons collide over multiple frames, pass a persistent `GjkCache` storing
   the previous separating axis normal.
3. **Use Highway SIMD for Bulk Queries**: Use `simd_point_inside_batch` to test 16 points against a shape
   simultaneously.

### 5.3 How to Improve Geometric Quality & Anti-Tunneling Accuracy

1. **Enable CCD for Fast Bodies**: For bodies where velocity $v \cdot \Delta t > \text{thickness}$, enable Conservative
   Advancement to eliminate tunneling through walls.
2. **Increase Poisson Fracture Density**: Set `num_shards = 16` with impact site bias to generate high-density
   micro-shards at bullet impact points and larger shards outward.

### 5.4 Configuration Trade-Off Matrix

| Configuration Mode   | Narrowphase Engine           |    CCD Sweeps     |    Latency per Pair     | Stacking Quality |          Anti-Tunneling          |
|:---------------------|:-----------------------------|:-----------------:|:-----------------------:|:----------------:|:--------------------------------:|
| **Fast 2D Arcade**   | Analytic + SAT only          |        OFF        |  **$\sim 8\text{ns}$**  |  High (2-point)  | Vulnerable if $v > 80\text{m/s}$ |
| **Default Balanced** | Analytic + Warm GJK          | Speculative Bound | **$\sim 25\text{ns}$**  |  High (2-point)  |    Safe for standard physics     |
| **Exact CCD Sim**    | GJK + EPA + Conservative Adv |  Full Continuous  | **$\sim 120\text{ns}$** |     Maximum      |  100% Guaranteed Zero Tunneling  |

---

## 6. CSG Expression Template EDSL & AST Specifications

Akruti provides zero-heap expression templates in `akruti::expr`:

```cpp
using namespace akruti::expr;

// Binary CSG Operators (Zero heap allocations!)
auto shape_union        = shape_a | shape_b;        // Union (min(sdf_a, sdf_b))
auto shape_intersect    = shape_a & shape_b;        // Intersection (max(sdf_a, sdf_b))
auto shape_difference   = shape_a - shape_b;        // Difference (max(sdf_a, -sdf_b))

// Smooth CSG Operators (polynomial C1-continuous blending, k = blend radius)
auto smooth_sub    = csg_smooth_subtract(shape_a, shape_b, /*k=*/0.3f);  // organic hole-punching
auto smooth_inter  = csg_smooth_intersect(shape_a, shape_b, /*k=*/0.2f); // rounded-lens intersection

// Geometric Transformers
auto shell   = csg_shell(shape_a, /*thickness=*/0.5f); // |sdf| - t
auto rounded = csg_offset(shape_a, /*radius=*/1.2f);   // sdf - r
```

---

## 7. Google Highway SIMD Acceleration

When Google Highway is present, Akruti vectorizes bulk geometric queries across NEON (Apple Silicon) and AVX2/AVX-512
(x86):

- `simd_point_inside_batch(points, shape)`: Evaluates 8–16 points simultaneously.
- `simd_raycast_packet(rays, shape)`: Computes packet intersection distances in parallel lanes.
- `simd_poly_dot_sweep(vertices, normal)`: Vectorized dot-product projection for SAT broadphase.

---

## 8. Zero-to-Hero Tutorial

### Step 1: Instantiating Primitives & SDF Queries

```cpp
#include <akruti/akruti.hpp>

using namespace akruti;

// Define analytic shapes
Circle circle{.center = {0.0f, 0.0f}, .radius = 2.0f};
OrientedBox obb{.center = {0.0f, 3.0f}, .half = {1.0f, 0.5f}, .rot = Mat2<float>::rotation(0.785f)};

// Evaluate Signed Distance Field
float dist_inside  = circle.sdf({0.0f, 0.5f}); // Negative (-1.5f) -> inside
float dist_outside = circle.sdf({0.0f, 5.0f}); // Positive (+3.0f) -> outside
```

### Step 2: Zero-Heap CSG Boolean Modeling

```cpp
using namespace akruti::expr;

// Zero-heap expression template: hollowed keyhole shape
Circle outer_disk{.center = {0.0f, 0.0f}, .radius = 3.0f};
Box    inner_slot{.center = {0.0f, 0.0f}, .half = {0.5f, 1.5f}};

auto keyhole = outer_disk - inner_slot; // Subtraction (zero allocations!)
float d = keyhole.sdf({0.0f, 0.0f});    // > 0 (carved slot is outside)
```

### Step 3: Generating SAT 2-Point Stacking Manifolds

```cpp
OrientedBox box_a{.center = {0.0f, 0.0f}, .half = {2.0f, 1.0f}, .rot = Mat2<float>::identity()};
OrientedBox box_b{.center = {0.0f, 1.9f}, .half = {2.0f, 1.0f}, .rot = Mat2<float>::identity()};

// Run 2D SAT narrowphase with incident-edge clipping
Manifold m = collide_obb_obb(box_a, box_b);
if (m.hit) {
    // m.points.size() == 2 -> 2-point manifold for stable stacking!
    // m.normal points along separation vector
}
```

### Step 4: Continuous Collision Detection (Anti-Tunneling)

```cpp
// Check if a high-speed bullet penetrates an obstacle
pebble::math::vec2 bullet_start{-10.0f, 0.0f};
pebble::math::vec2 bullet_velocity{500.0f, 0.0f}; // 500 units/frame!

RayHit hit = raycast(box_a, bullet_start, bullet_velocity.normalized(), bullet_velocity.length());
if (hit.hit) {
    float impact_time = hit.t; // Exact time of impact
    pebble::math::vec2 normal = hit.normal;
}
```

### Step 5: Dynamic Voronoi Fracture (*Khanda*) with Mass Properties

```cpp
// Shatter a rectangular glass pane at impact location
ConvexPoly<4> glass_pane = Box{{0.0f, 0.0f}, {4.0f, 6.0f}}.to_poly();
pebble::math::vec2 impact_site{1.0f, 2.0f};

// Generate 8 Voronoi shards biased toward impact site
auto shards = khanda::fracture_voronoi(glass_pane, impact_site, /*num_shards=*/8);

for (const auto& shard : shards) {
    auto props = khanda::shard_mass_props(shard.polygon, /*density=*/2500.0f);
    // props.mass, props.centroid, props.polar_inertia
}
```

---

## 9. Pebble Subsystem Reuse

| Subsystem      | Reused Module                    | Purpose in Akruti                                                     |
|:---------------|:---------------------------------|:----------------------------------------------------------------------|
| `pebble::math` | `math_vector.hpp`                | `vec2`, `mat2`, `aabb2`, `dot`, `cross`, `perp`, `normalize`.         |
| `containers`   | `AABBTree.hpp`                   | Dynamic broadphase bounding volume hierarchy.                         |
| `containers`   | `containers/tree/NAryTree.hpp`   | Authoring hierarchical layout tree representation (`LayoutTree`).     |
| `containers`   | `containers/graph/LiteGraph.hpp` | Relative layout constraint DAG graph solver (`constraints_graph`).    |
| `containers`   | `static_vector.hpp`              | Inlined polygon vertices (`ConvexPoly<N>`) with zero heap allocation. |
| `mem`          | `LinearArena.hpp`                | Flat Arena CSG AST allocations and Voronoi clipping scratch memory.   |
| `pravaha`      | `pravaha.hpp`                    | Chunked multi-threaded parallel execution of scene queries.           |

---

## 10. `akruti::layout` — 2D Layout Engine

`akruti::layout` provides a CPU baseline 2D layout engine. It decouples high-level UI tree authoring from flat,
cache-coherent Structure-of-Arrays (SoA) layout execution:

- **Authoring Layer**: Uses `NAryTree<LayoutNode>` (`LayoutTree`) to build hierarchical UI layouts.
- **Baking & Execution**: Bakes tree hierarchies into contiguous vectors (`parent`, `child_count`, `axis`, `width`,
  `height`, `flex_grow`, `padding`, `margin`).
- **4-Phase Execution Pipeline**:
    1. **Measure Pass** (Bottom-Up): Computes intrinsic element and text content sizes (`text_measure_callback`).
    2. **Place Pass** (Top-Down): Solves Flexbox main/cross axis layout, alignments (`Align`, `Justify`), flex
       distribution, padding, and margins.
    3. **Constraints Pass**: Evaluates parent matching (`match_parent_width`, `match_parent_height`), centering, and
       relative anchor offsets via `LiteGraph`.
    4. **Clip Pass** (Top-Down): Computes hierarchy clipping bounds (`Bounds2D` intersection) for overflow modes
       (`Overflow::Clip`, `Overflow::Scroll`).
- **Spatial Hash**: Grid-based spatial hash index (`SpatialHash`) enabling $O (1)$ hit testing (`hit_test(x, y)`).
- **Phase-Aware Invalidation & Incremental Solving**: `mark_dirty(node, mask)` propagates dirty flags up ancestor paths.
  `solve_incremental(viewport)` skips unchanged subtrees, running bottom-up measurements only on dirty nodes.
- **Highway SIMD Acceleration**: Vectorized batch bounding box calculations and spatial hash point-overlap queries via
  Google Highway when available.
- **Smriti Zero-Heap Scratch Memory**: Seamlessly leverages `smriti::pools::ScopedArena` and
  `smriti::pools::LinearArena` for stack-bounded scratch allocations.
- **Snapshots & Debugging**: Built-in snapshot ring buffer (`take_snapshot`, `restore_snapshot`) and customizable debug
  overlay renderer (`DebugOverlay`).

### Additive Sizing, Traversal & Text-Metrics Extensions

The following are backward-compatible additions layered onto the existing engine (the original `SizeSpec::Auto`/`Px`/
`Percent`, `hit_test`, and the C-function-pointer `TextMeasure` hook are unchanged and byte-identical when the new
features are unused). They back the Drishya widget engine.

- **New `SizeSpec` units** (all additive; mixable inside `SizeSpecClamp` arms):
    - `SizeSpec::Fr(weight)` — flexible fraction. Splits free space on the main axis proportionally to `weight`;
      translated to `flex_grow` with a zero basis at bake time.
    - `SizeSpec::Content(min_px = 0, max_px = 0)` — sizes to intrinsic content, optionally clamped to `[min_px, max_px]`
      (`max_px <= 0` ⇒ unbounded).
    - `SizeSpec::Aspect(ratio)` — derives this axis from the resolved other axis via `width : height = ratio`. `Aspect`
      wins over `Align::Stretch`.
- **`SizeSpecClamp{min, pref, max}`** — an optional min/pref/max triple stored in `LayoutStyle::width_clamp` and
  `LayoutStyle::height_clamp`, bounding a resolved size between a min and max window. Each arm is a full `SizeSpec`, so
  units can be mixed (e.g. `min = Px(120)`, `pref = Fr(1)`, `max = Percent(50)`).
- **`Engine::hit_test_chain(x, y, std::span<uint32_t> out) -> std::size_t`** — walks the baked `parent[]` array from the
  hit leaf up to the root, filling `out` leaf-first, and returns the count written. Zero heap. `hit_test` itself is
  unchanged.
- **`Engine::for_each_leaf(Fn fn)`** — invokes `fn(node_index)` for every leaf (`child_count == 0`) in baked pre-order,
  which is the natural tab order. Zero heap.
- **`ITextMetrics` concept + `make_text_measure(T&)`** — opt-in typed text metrics. Any type satisfying `ITextMetrics`
  (a `measure(const char*, float) -> Size2D` member) can be adapted into a `TextMeasure` trampoline via
  `make_text_measure`, which captures `&metrics` as `user_data` (the metrics object must outlive the returned
  `TextMeasure`; zero allocation). The existing C-function-pointer `TextMeasure` hook still works.

### Usage Example

```cpp
#include <akruti/layout.hpp>

using namespace akruti::layout;

// 1. Author Layout Tree
LayoutTree tree;

LayoutNode container;
container.style.axis = Axis::Column;
container.style.width = SizeSpec::Px(400.0f);
container.style.height = SizeSpec::Px(600.0f);
container.style.padding = Edges{.l = 16.0f, .t = 16.0f, .r = 16.0f, .b = 16.0f};
auto root_id = tree.insert(nullptr, container);

LayoutNode flex_child;
flex_child.style.flex_grow = 1.0f;
tree.insert(root_id, flex_child);

// 2. Bake and Solve Layout
Engine engine;
engine.bake(tree);

Bounds2D viewport{{0.0f, 0.0f}, {400.0f, 600.0f}};
engine.solve(viewport);

// 3. Query Results & Hit Test
Rect2D child_rect = engine.rect[1]; // x, y, w, h
auto hit_node = engine.hit_test(100.0f, 100.0f);

// 4. True Incremental Updates
LayoutStyle mod_style = flex_child.style;
mod_style.flex_grow = 2.0f;
engine.set_style(1, mod_style);
engine.solve_incremental(viewport); // Fast path: only recomputes affected subtrees!
```


