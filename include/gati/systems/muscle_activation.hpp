#pragma once
// ============================================================================
// gati/systems/muscle_activation.hpp — first-order activation ODE integration.
// ============================================================================

#include "../components/muscle_controller.hpp"
#include "../ecs.hpp"
#include "../system.hpp"

namespace gati {

struct MuscleExcitation {
    Scalar value = 0.0f;
};

struct MuscleActivationSystem {
    void run(World& world, StepContext ctx) {
        world.par_view<MuscleController, MuscleExcitation>(
            ctx.executor,
            [dt = ctx.dt](Entity, MuscleController& ctrl, MuscleExcitation& ex) {
                ctrl.neural_step(ex.value, dt);
            }
        );
    }
};

} // namespace gati
