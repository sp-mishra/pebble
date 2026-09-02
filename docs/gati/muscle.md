# Gati Muscle Pipeline

Gati exposes muscle control as pure ECS components/systems and bridges activation into Prakriti SoA muscle storage.

## Headers

- `#include <gati/components/muscle_controller.hpp>`
- `#include <gati/systems/muscle_activation.hpp>`
- `#include <gati/systems/muscle_bridge.hpp>`
- `#include <gati/systems/muscle_path.hpp>`

## Components

- `gati::MuscleController`: activation state, activation/deactivation time constants, and target Prakriti constraint ID.
- `gati::MuscleExcitation`: neural input (`u in [0,1]`) consumed by activation integration.
- `gati::MuscleGroup`: optional view-level grouping.
- `gati::MusclePath`: optional via-point polyline with cached path length.

## Systems

- `MuscleActivationSystem`: integrates `da/dt = (u-a)/tau` each fixed tick.
- `MuscleBridgeSystem<Cfg>`: copies controller activation into `prakriti::MuscleStore<Cfg>::activation`.
- `PathUpdateSystem<Cfg>`: updates `MusclePath::cached_path_length` and writes it to `rest_length`.
- All three systems use `world.par_view(...)`, which routes through `gati::ParallelExecutor` (Pravaha-backed when
  `GATI_ENABLE_PRAVAHA` is enabled).

## Minimal Usage

```cpp
#include <gati/gati.hpp>
#include <prakriti/prakriti.hpp>

gati::World world;
prakriti::MuscleStore<> muscles;

const auto id = muscles.add({});
const auto e = world.spawn();
world.add<gati::MuscleController>(e, {
    .activation = 0.0f,
    .activation_time_const = 0.01f,
    .deactivation_time_const = 0.04f,
    .prakriti_constraint_id = id,
});
world.add<gati::MuscleExcitation>(e, {.value = 1.0f});

gati::EventBus bus;
smriti::pools::LinearArena arena(1024);
gati::ParallelExecutor exec;
gati::StepContext ctx{1.0f / 60.0f, 1, bus, arena, exec};

gati::MuscleActivationSystem activation_sys;
gati::MuscleBridgeSystem<> bridge{&muscles};
activation_sys.run(world, ctx);
bridge.run(world, ctx);
```
