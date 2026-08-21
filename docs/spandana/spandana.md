# Spandana (स्पन्दन) — Universal Visual, Motion, Shape, Effect & Physics EDSL Engine

Header-only C++23/C++26. No virtual, no macros. Contract-based static policy dispatch.
Spandana is the high-level declarative visual and motion language for Pebble, combining 2D Animation, Easing, Harmonic Springs, Inverse Kinematics, Flipbooks, Akruti CSG Shape Morphs, Akruti Splines as first-class geometric shapes, Prakriti Physics Impulses, Particle Emitters, and Node-Based Dependency Inference.

Include: `#include <spandana/spandana.hpp>`

---

## 1. Core Architecture

1. **Automatic Dependency & Parallelism Inference (`ResourceKey`)**:
   - Actions targeting different entities/components automatically run concurrently.
   - Actions targeting the same component field automatically serialize in sequence.
2. **Contract-Based Concepts**:
   - `EasingFunction`: 30+ Robert Penner mathematical easings and cubic-bezier curves.
   - `SpringSolver`: Exact closed-form analytical damped harmonic oscillator with zero numerical instability across variable $dt$.
   - `IKSolver2D`: Analytical `TwoBoneIK` limb solver and `FABRIK2D` multi-joint chain solver.
   - `Tweenable`: Concept for any interpolatable type using `pebble::math::lerp`.
3. **Generic Splines as First-Class Akruti Shapes (`akruti/spline.hpp`)**:
   - `CubicBezierCurve` and `CatmullRomSpline` implement the full `akruti::Shape` contract (`sdf(p)`, `aabb()`, `support(d)`).
   - Splines participate seamlessly in **CSG shape arithmetic** (`csg::subtract`, `csg::smooth_union`).
   - Arc-length parameterization for constant-speed evaluation.
4. **Unified Declarative World EDSL**:
   - Motion & Splines: `tween(prop).to(val)`, `spring(prop).target(val)`, `follow_path(pos, spline).orient_to_tangent(rot)`
   - Geometry & Shapes: `shape_fx(circle(10.0f)).grow(2.0f)`, `morph_shape(...)`
   - Physics: `radial_impulse().at(pos).magnitude(500.0f)`, `apply_impulse(...)`, `attach_joint(...)`
   - Particles & Effects: `particle_burst().at(pos).count(32)`, `shake_camera(cam).trauma(0.7f)`, `flipbook(sprite).play("attack")`

---

## 2. Quick Start Example

```cpp
#include <spandana/spandana.hpp>
#include <akruti/spline.hpp>

using namespace pebble::spandana::edsl;
using namespace pebble::spandana::ease;

pebble::spandana::Timeline timeline;

akruti::CubicBezierCurve trajectory{
    .p0 = {0.0f, 0.0f},
    .p1 = {20.0f, 80.0f},
    .p2 = {80.0f, 80.0f},
    .p3 = {100.0f, 0.0f},
    .radius = 1.5f
};

// Intuitive declaration: non-conflicting actions run in parallel; conflicting actions serialize automatically!
timeline.add(
    // 1. Follow Bézier Spline trajectory with tangent orientation
    follow_path(transform.position, trajectory).duration(1.2f).orient_to_tangent(transform.rotation).ease(in_out_quad),

    // 2. Camera trauma shake and particle burst upon impact at the end of the path
    shake_camera(camera).trauma(0.8f).duration(0.4f),
    particle_burst().at(trajectory.p3).count(32).speed(100.0f, 250.0f).lifetime(0.4f),
    radial_impulse().at(trajectory.p3).radius(80.0f).magnitude(600.0f),

    // 3. Return position back to origin with an elastic bounce (chains automatically!)
    tween(transform.position).to({0.0f, 0.0f}).duration(0.5f).ease(out_elastic)
);

// In your game loop:
timeline.update(dt);
```
