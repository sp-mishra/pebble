#pragma once
// ============================================================================
// spandana/system.hpp — Spandana ECS & Gati System Integration
// ============================================================================

#include "timeline.hpp"
#include "ecs/ecs.hpp"
#include "gati/system.hpp"

namespace pebble::spandana {

// Component: holds active timelines for an entity
struct TimelineRunner {
    Timeline timeline;
};

// System: steps all active timelines in the World
struct SpandanaSystem {
    void run(pebble::ecs::World& w, gati::StepContext ctx) {
        w.view<TimelineRunner>([&](pebble::ecs::Entity, TimelineRunner& tr) {
            tr.timeline.update(ctx.dt);
        });
    }
};

} // namespace pebble::spandana
