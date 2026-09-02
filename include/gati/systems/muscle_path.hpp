#pragma once
// ============================================================================
// gati/systems/muscle_path.hpp — path/via-point muscle length updater.
// ============================================================================

#include "../components/muscle_controller.hpp"
#include "../ecs.hpp"
#include "../math.hpp"
#include "../system.hpp"
#include <containers/dynamic/SmallVector.hpp>

#if __has_include("prakriti/state/muscle_store.hpp")
#include "prakriti/state/muscle_store.hpp"
#endif

#include <algorithm>
#include <cstdint>

namespace gati {
    struct PathPoint {
        Vec2 position{};
    };

    struct MusclePath {
        containers::dynamic::SmallVector<PathPoint, 8 * sizeof(PathPoint)> via_points{};
        Scalar cached_path_length = 0.0f;
    };

#if __has_include("prakriti/state/muscle_store.hpp")
    template <typename Cfg = prakriti::MuscleConstraintCfg<>>
    struct PathUpdateSystem {
        prakriti::MuscleStore<Cfg>* muscle_store = nullptr;

        void run(World& world, StepContext ctx) {
            if (!muscle_store) return;
            world.par_view<MuscleController, MusclePath>(ctx.executor,
                                                         [this](Entity, MuscleController& ctrl, MusclePath& path) {
                                                             if (ctrl.prakriti_constraint_id >= muscle_store->size())
                                                                 return;
                                                             if (path.via_points.size() < 2) return;

                                                             Scalar len = 0.0f;
                                                             for (std::size_t i = 1; i < path.via_points.size(); ++i) {
                                                                 const Vec2 d = path.via_points[i].position - path.
                                                                     via_points[i - 1].position;
                                                                 len += pebble::math::length(d);
                                                             }
                                                             path.cached_path_length = len;
                                                             muscle_store->rest_length[ctrl.prakriti_constraint_id] =
                                                                 std::max(len, 1e-4f);
                                                         });
        }
    };
#endif
} // namespace gati
