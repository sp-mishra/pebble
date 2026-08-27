# Akruti — Header-Only 2D Shape, Geometry & Simulation Engine

Header-only C++23/C++26. No virtual, no macros. Concept-based static dispatch. `#include <akruti/akruti.hpp>`.

## Table of Contents
1. [Overview](#1-overview)
2. [Quick Start](#2-quick-start)
3. [Architecture](#3-architecture)
4. [Core Math](#4-core-math)
5. [The Shape Concept](#5-the-shape-concept)
6. [Primitives Catalog](#6-primitives-catalog)
7. [High-Performance Narrowphase & SAT](#7-high-performance-narrowphase--sat)
8. [Google Highway SIMD Acceleration](#8-google-highway-simd-acceleration)
9. [Advanced CSG & Expression EDSL](#9-advanced-csg--expression-edsl)
10. [Continuous Collision (CCD)](#10-continuous-collision-ccd)
11. [Fracture & Tear (Khanda)](#11-fracture--tear-khanda)
12. [Joint Frames](#12-joint-frames)
13. [Scene Layer, Collision Layers & Pravaha](#13-scene-layer-collision-layers--pravaha)
14. [Zero Overhead Guarantees](#14-zero-overhead-guarantees)

---

## 1. Overview

Akruti (आकृति — "shape/form") is a state-of-the-art 2D shape, geometry, and collision system for Pebble, engineered for **60–240 FPS real-time games** and **100k+ body physical simulations**:
- **Unified World-Space Transform (`TransformedShape<S>`)**: Zero-cost geometric adapter satisfying `Shape` with zero trig overhead on axis-aligned shapes.
- **Type-Erased `ShapeStore`**: Direct matrix indexed dispatch without `std::visit` or dynamic allocations.
- **Analytic 2D Fast-Paths & SAT**: $O(1)$ Circle-Circle, Circle-Capsule, Circle-Box, Circle-OBB, Circle-Triangle, Circle-RoundedBox, Circle-Sector, Segment-Circle, Segment-Box, Capsule-Triangle, Triangle-Triangle, and OBB-OBB Separating Axis Theorem (SAT).
- **Minkowski Portal Refinement (MPR)**: Fast 4–8 iteration distance oracle and continuous collision detection.
- **Fortune's Sweep-Line Voronoi & CDT**: $O(n \log n)$ Voronoi shatter and Constrained Delaunay Triangulation with native hole support.
- **Dynamic `SpatialHash` Broadphase**: Morton Z-order indexing with counting-sort contiguous cell construction.
- **Google Highway SIMD Acceleration**: Multi-lane vectorized batch point membership, packet raycasting, and vertex dot sweeps.
- **Zero-Heap CSG**: C++23 expression template EDSL (`operator|`, `operator-`, `operator&`) + Flat AST Arena storage.

---

## 2. Quick Start

```cpp
#include <akruti/akruti.hpp>
using namespace akruti;

// 1. SAT 2-Point Contact Manifold
OrientedBox a{{0, 0}, {1, 1}, Mat2<Scalar>::rotation(0.0f)};
OrientedBox b{{0, 1.8f}, {1, 1}, Mat2<Scalar>::rotation(0.0f)};
Manifold m = collide_obb_obb(a, b);
// m.hit == true, m.depth == 0.2, m.points.size() == 2 (stable stacking!)

// 2. Zero-Heap Inlined Expression CSG (C++23)
using namespace akruti::expr;
Circle c{{0, 0}, 1.0f};
Box    box{{0.5f, 0}, {0.5f, 0.5f}};
auto shape = c - box; // subtracted!
Scalar d = shape.sdf({0.5f, 0}); // > 0 (carved out, zero heap allocation)

// 3. Fast Sphere-Trace Raycast
RayHit h = raycast(c, Vec2<Scalar>{-5, 0}, Vec2<Scalar>{1, 0});
// h.hit == true, h.t == 4.0, h.normal == {-1, 0}
```

---

## 3. Architecture

```
Scene Layer (akruti::scene)    SoA Batches · LayerMasks · AABBTree BVH · Pravaha Task DAG
      │
      v
Narrowphase & SAT              SAT 2-point manifolds · O(1) Circle/Box/Capsule · Warm GJK
      │
      v
Fracture (Khanda) · CCD        Voronoi · EarClip · 2nd Moment Inertia · Conservative Adv
      │
      v
Advanced CSG                   Inlined Expression EDSL · Flat Arena AST · Dynamic CSG
      │
      v
Primitives & SIMD              Highway Vectorized Sweeps · OBB, Triangle, RoundedBox, Sector
      │
      v
Core Math (pebble::math)       vec2, mat2 (static_tensor), aabb2, cross, perp, dot, length
```

---

## 4. Core Math & AABB (pebble::math Integration)

Akruti directly reuses Pebble's unified tensor math engine and AABB geometry:
- `pebble::math::vec2`: Stack-allocated, zero-heap, constexpr $2$-element vector built on `static_tensor<float, ..., 2>`.
- `pebble::math::mat2`: $2 \times 2$ transform matrix built on `static_tensor<float, ..., 2, 2>`.
- `pebble::math::aabb2` & `aabb3`: Pebble's canonical Axis-Aligned Bounding Box (`lo`, `hi`) supporting fattening, containment, overlaps, surface area, clamping, and merging.
- `containers::AABBTree`: Dynamic Bounding-Volume Hierarchy (BVH) natively indexed by Pebble `aabb2` and `aabb3`.
- Geometric functions: `pebble::math::cross` (2D scalar cross), `pebble::math::perp` (90° CCW normal), `dot`, `distance`, `normalize`.



Each primitive satisfies `Shape` with exact `.sdf()`, `.aabb()`, and `.support()`:
- `Circle`: $\{ \text{center}, \text{radius} \}$
- `Segment`: $\{ a, b \}$
- `Capsule`: $\{ a, b, \text{radius} \}$
- `Box`: $\{ \text{center}, \text{half} \}$ (axis-aligned)
- `OrientedBox` (OBB): $\{ \text{center}, \text{half}, \text{rotation} \}$
- `Triangle`: $\{ a, b, c \}$ (exact barycentric distance)
- `RoundedBox`: $\{ \text{center}, \text{half}, \text{radius} \}$
- `Sector` / `Arc`: $\{ \text{center}, \text{radius}, \text{half\_angle}, \text{rotation} \}$ (sensor / FOV cones)
- `HalfPlane`: $\{ \text{normal}, \text{point} \}$
- `ConvexPoly<N>`: Vertices in inline `containers::static_vector<Vec, N>`
- `RoundedPoly<N>`: Convex polygon inflated by corner radius $r$
- `ChainShape<N>`: Polyline / edge-loop with ghost-vertex elimination and radius support
- `GridSDF<W, H>`: Sampled 2D discrete SDF grid with bilinear interpolation

---

## 5. High-Performance Narrowphase & SAT

`akruti/narrowphase.hpp`:
- `collide_circle_circle(c1, c2)`: $O(1)$ distance formula (0 iterations).
- `collide_circle_capsule(c, cap)`: Clamped line projection.
- `collide_circle_box(c, b)`: Clamped AABB quadrant projection.
- `collide_capsule_capsule(cap1, cap2)`: Segment-segment distance with parallel incident edge clipping for **2-point contact manifolds**.
- `collide_capsule_obb(cap, obb)`: Multi-point endpoint & midpoint projection against local OBB frame.
- `collide_obb_obb(a, b)` / `collide_box_box(a, b)`: 2D Separating Axis Theorem (SAT) with incident-reference edge clipping to produce **2-point contact manifolds** with contact normal and individual penetration depths.
- `collide_gjk_warm_started(a, b, &cache)`: Reuses previous frame separating axis to warm-start GJK/EPA.

---

## 6. Advanced CSG & Expression EDSL

`akruti/csg.hpp`:
- **Expression Templates (`akruti::expr`)**:
  - `(a | b)`: Union
  - `(a & b)`: Intersection
  - `(a - b)`: Difference
  - `csg_shell(shape, thickness)`: Hollowed shell ($|d| - t$)
  - `csg_offset(shape, r)`: Inflate / Deflate ($d - r$)
  - `normal_auto_diff(expr, p)`: Symbolic / dual-step automatic gradient surface normal calculation
- **Flat Arena CSG (`FlatCsgTree`)**: Cache-contiguous array of AST nodes optionally backed by Smriti `LinearArena`.
- **Extended Dynamic Operators**: `ChamferUnion`, `Morph`, `SmoothUnion`, `Transform`.

---

## 7. Fracture & Tear (Khanda)

`akruti/khanda.hpp`:
- `EarClipTriangulator`: Ear-clipping triangulation with automatic hole-bridging.
- `shard_mass_props`: Exact computation of area, centroid, and **polar moment of inertia** about centroid ($J = \iint (x^2+y^2)dA$) with parallel-axis shifting.
- `poisson_disk_sites`: Bridson Poisson disk sampling with impact-site bias.
- `fracture_voronoi`: End-to-end shard generation, filtering, triangulation, and convex decomposition.

---

## 8. Scene Layer & Pravaha Parallelism

`akruti/scene/`:
- `ShapeBatch<Prim>`: SoA cache-friendly column storage.
- `Scene`: Manages per-primitive batches, `LayerMask` collision filtering, and dynamic `containers::AABBTree` index.
- `ParallelExecutor`: Leverages `pebble::pravaha` for task-graph chunked parallel execution when enabled (`AKRUTI_ENABLE_PRAVAHA`).
- Bulk operations: `broadphase_pairs`, `bulk_narrowphase`, `bulk_point_inside`, `bulk_raycast`, `bulk_nearest_shape`, `bulk_sdf_field`.

---

## 9. Dynamic Rigid Bodies & 2-Way Continuum Coupling

`akruti/body.hpp`:
- `DynamicBody<Shape>`: Encapsulates dynamic 6-DOF (2D translation + rotation) motion with mass, moment of inertia, and linear/angular velocity.
- Integrates with Prakriti continuum particle solvers: particles exert continuous contact reaction forces and hydrodynamic pressure impulses against `DynamicBody` surfaces, enabling floating, sinking, buoyancy, and mechanical deflection.
- World-space evaluation of `sdf(p)`, `aabb()`, `support(d)`, and continuous collision detection (CCD) `raycast(ray_start, ray_dir)` for any underlying Akruti Shape (including compound CSG and deformed shapes).
