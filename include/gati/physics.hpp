#pragma once
// ============================================================================
// gati/physics.hpp — Prakriti Dynamics Bridge (guarded GATI_HAS_PRAKRITI)
// ============================================================================
// Maps Gati entities to Prakriti particles and synchronizes physics state.
// ============================================================================

#include "math.hpp"
#include "ecs.hpp"
#include "transform.hpp"
#include "system.hpp"

#if defined(GATI_ENABLE_PRAKRITI) && __has_include("prakriti/prakriti.hpp")
#define GATI_HAS_PRAKRITI 1
#include "prakriti/prakriti.hpp"

namespace gati {

// Component: which Prakriti particle backs this entity
struct BodyRef {
    std::uint32_t particle;
};

class Physics {
public:
    explicit Physics(prakriti::WorldConfig cfg = {}) : world_(cfg) {
        default_material_ = world_.materials().add(prakriti::MaterialParams{});
    }

    [[nodiscard]] BodyRef add_dynamic(Vec2 pos, Scalar mass = 1.0f) {
        const auto i = world_.particles().add(
            {.position = pos, .mass = mass, .material = default_material_});
        return BodyRef{static_cast<std::uint32_t>(i)};
    }

    [[nodiscard]] BodyRef add_static(Vec2 pos) {
        return add_dynamic(pos, Scalar(0));
    }

    [[nodiscard]] Vec2 position(BodyRef b) {
        return world_.particles().pos_v(b.particle);
    }

    void set_dt(Scalar dt) {
        world_.config().dt = dt;
    }

    void step() {
        world_.step();
    }

    [[nodiscard]] prakriti::World<>& prakriti() noexcept {
        return world_;
    }

private:
    prakriti::World<> world_;
    prakriti::MaterialId default_material_{};
};

struct PhysicsSyncSystem {
    Physics* physics = nullptr;

    void run(World& w, StepContext ctx) {
        if (!physics) return;
        physics->set_dt(ctx.dt);
        auto& pw = physics->prakriti();
        auto& P = pw.particles();

        // 1. Sync Gati ECS input velocities / forces to Prakriti before step
        w.view<BodyRef, Transform>([&](Entity e, BodyRef& b, Transform& tr) {
            tr.checkpoint();
            if (b.particle < P.size()) {
                if (auto* mat = w.get<MaterialComponent>(e)) {
                    P.temperature[b.particle] = mat->temperature;
                }
            }
        });

        // 2. Step Prakriti Continuum & Multiphysics World
        physics->step();

        // 3. Write back simulated positions, velocities, and phase states to Gati ECS
        w.view<BodyRef, Transform>([&](Entity e, BodyRef& b, Transform& tr) {
            if (b.particle < P.size()) {
                tr.position = physics->position(b);
                if (auto* mat = w.get<MaterialComponent>(e)) {
                    mat->temperature = P.temperature[b.particle];
                }
            }
        });
    }
};

} // namespace gati
#endif // GATI_HAS_PRAKRITI
