# Spandana (स्पन्दन) — Universal Visual, Motion, Shape, Effect & Physics EDSL Engine

Header-only C++23/C++26. No virtual, no macros. Contract-based static policy dispatch.
Spandana is the high-level declarative visual, physical, and motion language for Pebble, combining 2D Animation, Easing, Harmonic Springs, Inverse Kinematics, Flipbooks, Akruti CSG Shape Morphs, Akruti Splines as first-class geometric shapes, Prakriti Physics Impulses, Procedural Voronoi Destruction, Parametric 2D Directional Blend Spaces, Material Thermodynamics & Phase Transitions (Solid/Liquid/Gas/Plasma), Particle Emitters, and Node-Based Dependency Inference.

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
4. **Procedural 2D Destruction & Voronoi Shattering (`spandana/destruction.hpp`)**:
   - Slices entities into dynamic Voronoi shards upon impact, computing exact area, centroids, and polar moment of inertia $I_z$, with radial impulse velocities.
5. **Parametric 2D Directional Blend Spaces (`spandana/blend_space.hpp`)**:
   - `BlendSpace2D` maps velocity vectors $(v_x, v_y)$ to weighted multi-clip animation samples (Walk, Run, Strafe, Turn) with footstep phase synchronization.
6. **Continuous Material Thermodynamics & Phase Changes (`gati/material.hpp`, `spandana/edsl/material_edsl.hpp`)**:
   - Continuous 4-fraction phase model (`solid`, `plastic`, `liquid`, `gas`).
   - Dynamic melting, boiling, burning, contact thermal diffusion, molten welding/fusion, and brittle fracture on collision.
7. **Elemental & Chemical Reaction Matrix (`gati/elemental.hpp`)**:
   - Automated elemental resolution: Water + Lava $\to$ Obsidian + Steam, Fire + Wood $\to$ Ignition, Acid + Metal $\to$ Corrosion, Electricity + Water $\to$ Shockwaves.
8. **2D Skeletal Hierarchy & Linear Blend Skinning (`spandana/skeleton.hpp`)**:
   - Hierarchical bone forward kinematics (FK) and linear blend skinning (LBS) for mesh contours.
9. **Glaze JSON Serialization (`spandana/serialization.hpp`)**:
   - Ultra-fast reflection-free JSON serialization/deserialization for materials and animation configurations.
10. **Unified Declarative World EDSL**:
   - Motion & Splines: `tween(prop).to(val)`, `spring(prop).target(val)`, `follow_path(pos, spline).orient_to_tangent(rot)`
   - Geometry & Shapes: `shape_fx(circle(10.0f)).grow(2.0f)`, `morph_shape(...)`
   - Physics & Destruction: `radial_impulse().at(pos).magnitude(500.0f)`, `shatter_entity(e).at(impact).shards(8)`
   - Thermodynamics & Materials: `set_material(e, Ice()).temperature(-15)`, `apply_heat().at(pos).temperature(500)`
   - Particles & Effects: `particle_burst().at(pos).count(32)`, `shake_camera(cam).trauma(0.7f)`, `flipbook(sprite).play("attack")`

---

## 2. Quick Start Example

```cpp
#include <spandana/spandana.hpp>
#include <gati/material.hpp>

using namespace pebble::spandana::edsl;
using namespace pebble::spandana::ease;

pebble::spandana::Timeline timeline;

// Intuitive declaration: non-conflicting actions run in parallel; conflicting actions serialize automatically!
timeline.add(
    // 1. Assign Ice material to entity
    set_material(world, obstacle_entity, gati::MaterialComponent::Ice()).temperature(-10.0f),

    // 2. Apply heat source in the world -> heats ice above 0°C (melts into liquid) and 100°C (vaporizes to gas)
    apply_heat(world).at({50.0f, 0.0f}).temperature(350.0f).radius(80.0f).duration(1.0f),

    // 3. Shake camera and burst steam particles
    shake_camera(camera).trauma(0.6f).duration(0.3f),
    particle_burst().at({50.0f, 0.0f}).count(40).speed(80.0f, 200.0f).lifetime(0.5f)
);

// In your game loop:
timeline.update(dt);
```
