# Prakriti Muscle Constraints

`prakriti` now provides a policy-based muscle stack for active soft constraints on top of XPBD primitives.

## Headers

- `#include <prakriti/constraints/muscle.hpp>`
- `#include <prakriti/state/muscle_store.hpp>`
- `#include <prakriti/solvers/muscle.hpp>`

## Core Types

- `prakriti::MuscleConstraintCfg<Fatigue, Fiber, Tendon, Damping>`: compile-time policy bundle.
- `prakriti::MuscleConstraint<Cfg>`: single origin-insertion muscle descriptor.
- `prakriti::MuscleStore<Cfg>`: SoA storage for muscle constraints.
- `prakriti::MuscleSolver<Cfg>`: XPBD-compatible projection step using activation, tendon, and fiber force models.

## Included Models

- `HillTypeFiber`, `LinearFiber`
- `NonlinearTendon`, `LinearTendon`, `RigidTendon`
- `NoFatigue`, `ThreeCompartmentFatigue`
- Batch APIs for SoA sweeps:
    - `HillTypeFiber::compute_force_batch(...)`
    - `LinearFiber::compute_force_batch(...)`
    - `NonlinearTendon::force_batch(...)`
    - `LinearTendon::force_batch(...)`
    - `RigidTendon::force_batch(...)`

## Minimal Usage

```cpp
#include <prakriti/prakriti.hpp>

using ProductionMuscle = prakriti::MuscleConstraintCfg<
    prakriti::ThreeCompartmentFatigue,
    prakriti::HillTypeFiber,
    prakriti::NonlinearTendon,
    prakriti::ViscousDamping
>;

prakriti::ParticleStore particles;
const auto a = particles.add({.position = {0.0f, 0.0f}, .mass = 1.0f});
const auto b = particles.add({.position = {0.35f, 0.0f}, .mass = 1.0f});

prakriti::MuscleStore<ProductionMuscle> muscles;
muscles.add({
    .origin = a,
    .insertion = b,
    .rest_length = 0.25f,
    .tendon_slack_length = 0.05f,
    .max_isometric_force = 700.0f,
    .activation = 1.0f,
    .pennation_angle = 0.15f,
    .optimal_fiber_length = 0.12f,
});

prakriti::MuscleSolver<ProductionMuscle> solver;
constexpr float dt = 1.0f / 120.0f;
solver.solve_substep(muscles, particles, dt, 1.0f / (dt * dt));
```

## Performance Benchmark

Use the dedicated hidden Catch2 perf case to compare muscle solve throughput paths:

```bash
cmake --build build --target pebble_tests -j4
./build/pebble_tests "[prakriti][muscle][perf]"
```

Interpretation:

- `serial_us`: scalar-like baseline (solver chunk size forced to a huge value, no practical task splitting).
- `batch_us`: batched SoA path (contiguous spans through `compute_force_batch`/`force_batch`).
- `pravaha_us`: optional parallel path when `PRAKRITI_HAS_MUSCLE_PRAVAHA` is enabled.

Performance guidance:

- For small muscle counts, serial or coarse chunks can be faster due to lower scheduling overhead.
- For large muscle counts (thousands+), batched and Pravaha paths should reduce total solve time.
- Keep `MuscleStore::reserve(...)` aligned with expected muscle count to avoid reallocation stalls.

## Notes

- Hot-path projection math uses Pebble primitives (`ga::Vec2`, `ga::dot`, `ga::axpy`) rather than ad-hoc scalar/vector
  wrappers.
- `MuscleStore` is append-only for stable IDs consumed by Gati bridges.
- Warm-start caching can be applied through `MuscleSolver::cache_lambdas(...)` with a Kosha cache type.
- The solver is independent and can be composed in custom world pipelines without virtual dispatch.
