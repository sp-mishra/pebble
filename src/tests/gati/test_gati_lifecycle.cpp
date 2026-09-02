#include "catch_amalgamated.hpp"
#include "gati/gati.hpp"
#include <cmath>

TEST_CASE (
"Gati: ContactStateTracker Enter, Stay and Exit Lifecycle"
,
"[gati][lifecycle]"
)
 {
    gati::EventBus bus;
    gati::ContactStateTracker tracker;

    // Frame 1: Publish collision between entity 1 and entity 2
    bus.publish(gati::ContactEvent{1, 2, pebble::math::vec2(0.0f, 1.0f), 0.5f});
    tracker.update(bus);

    int enters = 0, stays = 0, exits = 0;
    bus.drain<gati::ContactPhaseEvent>([&](const gati::ContactPhaseEvent& cpe) {
        if (cpe.phase == gati::ContactPhase::Enter) ++enters;
        if (cpe.phase == gati::ContactPhase::Stay)  ++stays;
        if (cpe.phase == gati::ContactPhase::Exit)  ++exits;
    });

    REQUIRE(enters == 1);
    REQUIRE(stays == 0);
    REQUIRE(exits == 0);

    // Frame 2: Collision persists -> Stay
    bus.publish(gati::ContactEvent{1, 2, pebble::math::vec2(0.0f, 1.0f), 0.4f});
    tracker.update(bus);

    enters = stays = exits = 0;
    bus.drain<gati::ContactPhaseEvent>([&](const gati::ContactPhaseEvent& cpe) {
        if (cpe.phase == gati::ContactPhase::Enter) ++enters;
        if (cpe.phase == gati::ContactPhase::Stay)  ++stays;
        if (cpe.phase == gati::ContactPhase::Exit)  ++exits;
    });

    REQUIRE(enters == 0);
    REQUIRE(stays == 1);
    REQUIRE(exits == 0);

    // Frame 3: Collision separates -> Exit
    tracker.update(bus);

    enters = stays = exits = 0;
    bus.drain<gati::ContactPhaseEvent>([&](const gati::ContactPhaseEvent& cpe) {
        if (cpe.phase == gati::ContactPhase::Enter) ++enters;
        if (cpe.phase == gati::ContactPhase::Stay)  ++stays;
        if (cpe.phase == gati::ContactPhase::Exit)  ++exits;
    });

    REQUIRE(enters == 0);
    REQUIRE(stays == 0);
    REQUIRE(exits == 1);
}
