#pragma once
// ============================================================================
// prakriti/constraints/muscle.hpp — policy-configurable XPBD muscle constraint.
// ============================================================================

#include "fatigue_policies.hpp"
#include "fiber_models.hpp"
#include "tendon_models.hpp"
#include "../core/config.hpp"

namespace prakriti {
    struct ViscousDamping {
        static Scalar ratio(Scalar) noexcept { return Scalar(0.05f); }
    };

    template <typename FatiguePolicy = NoFatigue,
              typename FiberModel = HillTypeFiber,
              typename TendonModel = NonlinearTendon,
              typename DampingPolicy = ViscousDamping>
    struct MuscleConstraintCfg {
        using fatigue = FatiguePolicy;
        using fiber = FiberModel;
        using tendon = TendonModel;
        using damping = DampingPolicy;
    };

    template <typename Cfg = MuscleConstraintCfg<>>
    struct MuscleConstraint {
        Index origin = kInvalidIndex;
        Index insertion = kInvalidIndex;

        Scalar rest_length = Scalar(0.1f);
        Scalar tendon_slack_length = Scalar(0.02f);
        Scalar max_isometric_force = Scalar(100.0f);
        Scalar activation = Scalar(0);
        Scalar pennation_angle = Scalar(0);
        Scalar optimal_fiber_length = Scalar(0.08f);

        Scalar lambda_accum = Scalar(0);

        [[no_unique_address]] typename Cfg::fatigue::State fatigue_state{};
    };
} // namespace prakriti

