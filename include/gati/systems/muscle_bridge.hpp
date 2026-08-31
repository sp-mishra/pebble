#pragma once
// ============================================================================
// gati/systems/muscle_bridge.hpp — copy ECS activations to Prakriti muscle SoA.
// ============================================================================

#include "../components/muscle_controller.hpp"
#include "../ecs.hpp"
#include "../system.hpp"

#if __has_include("prakriti/state/muscle_store.hpp")
#include "prakriti/state/muscle_store.hpp"

namespace gati {

template <typename Cfg = prakriti::MuscleConstraintCfg<>>
struct MuscleBridgeSystem {
    prakriti::MuscleStore<Cfg>* muscle_store = nullptr;

    void run(World& world, StepContext ctx) {
        if (!muscle_store) return;
        world.par_view<MuscleController>(ctx.executor, [this](Entity, MuscleController& ctrl) {
            if (ctrl.prakriti_constraint_id >= muscle_store->size()) return;
            muscle_store->activation[ctrl.prakriti_constraint_id] = ctrl.activation;
        });
    }
};

} // namespace gati
#endif
