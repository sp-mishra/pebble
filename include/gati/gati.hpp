#pragma once
// ============================================================================
// gati/gati.hpp — Umbrella Include & Game Facade for Gati Realtime Runtime
// ============================================================================
// Header-only C++23/C++26, zero virtual, concept-based static dispatch.
// Composes pebble::ecs, pebble::math, pebble::pravaha, akruti, and prakriti.
// ============================================================================

#include "math.hpp"
#include "clock.hpp"
#include "ecs.hpp"
#include "transform.hpp"
#include "system.hpp"
#include "event.hpp"
#include "input.hpp"
#include "anim.hpp"
#include "parallel.hpp"

// Optional bridges
#include "physics.hpp"
#include "collision.hpp"
#include "joint.hpp"
#include "reactive_cues.hpp"
#include "material.hpp"
#include "material_reaction.hpp"
#include "elemental.hpp"

namespace gati {

// Default ordered system schedule
struct DefaultSystems {
    AnimationSystem animation;
#if defined(GATI_HAS_PRAKRITI)
    PhysicsSyncSystem physics_sync;
#endif
#if defined(GATI_HAS_AKRUTI)
    CollisionSystem collision;
#endif
#if defined(GATI_HAS_JOINTS)
    JointSystem joints;
#endif

    void run(World& w, StepContext ctx) {
        animation.run(w, ctx);
#if defined(GATI_HAS_PRAKRITI)
        physics_sync.run(w, ctx);
#endif
#if defined(GATI_HAS_AKRUTI)
        collision.run(w, ctx);
#endif
#if defined(GATI_HAS_JOINTS)
        joints.run(w, ctx);
#endif

        // Process material reactions and environmental thermodynamics
        // Note: do not drain here so external listeners / ContactStateTracker can observe ContactEvent
        MaterialReactionSystem::step_thermodynamics(w, ctx.dt);
    }
};

// Game facade: World + fixed-step Clock + SystemStack + EventBus + LinearArena scratch + ParallelExecutor
template <typename Systems = DefaultSystems>
class Game {
public:
    explicit Game(ClockConfig clock_cfg = {}, std::uint32_t universe = kDefaultUniverse,
                  std::size_t scratch_bytes = std::size_t{1} << 20)
        : clock_(clock_cfg), world_(universe), scratch_(scratch_bytes) {}

    [[nodiscard]] World&            world()    noexcept { return world_; }
    [[nodiscard]] Clock&            clock()    noexcept { return clock_; }
    [[nodiscard]] EventBus&         events()   noexcept { return events_; }
    [[nodiscard]] Systems&          systems()  noexcept { return systems_; }
    [[nodiscard]] ParallelExecutor& executor() noexcept { return executor_; }

    // Advance by real wall-clock dt: run fixed simulation steps and flush deferred commands
    void update(Scalar real_dt) {
        clock_.advance(real_dt);
        while (clock_.should_step()) {
            const auto cp = scratch_.checkpoint();
            StepContext ctx{clock_.dt(), clock_.total_steps(), events_, scratch_, executor_};
            systems_.run(world_, ctx);
            world_.flush_commands();
            scratch_.rollback(cp);
        }
    }

    // Render interpolation factor for the current partial step
    [[nodiscard]] Scalar alpha() const noexcept {
        return clock_.alpha();
    }

private:
    Clock                       clock_;
    World                       world_;
    EventBus                    events_;
    ParallelExecutor            executor_;
    [[no_unique_address]] Systems systems_;
    smriti::pools::LinearArena  scratch_;
};

} // namespace gati
