#include "catch_amalgamated.hpp"
#include "gati/gati.hpp"
#include <cmath>

TEST_CASE("Gati: Clock Fixed Timestep & Alpha Interpolation", "[gati][clock]") {
    gati::Clock clock{gati::ClockConfig{.hz = 60.0f}};

    REQUIRE(clock.dt() == (1.0f / 60.0f));

    // Advance by half a dt
    clock.advance(1.0f / 120.0f);
    REQUIRE_FALSE(clock.should_step());
    REQUIRE(clock.alpha() > 0.49f);
    REQUIRE(clock.alpha() < 0.51f);

    // Advance by another half a dt -> 1 full step should be available
    clock.advance(1.0f / 120.0f);
    REQUIRE(clock.should_step());
    REQUIRE_FALSE(clock.should_step()); // Drained
}

TEST_CASE("Gati: Transform Interpolation and Hierarchy", "[gati][transform]") {
    pebble::ecs::World world;

    auto parent = world.spawn();
    auto child  = world.spawn();

    world.add<gati::Transform>(parent, {.position = pebble::math::vec2(10.0f, 0.0f)});
    world.add<gati::Transform>(child,  {.position = pebble::math::vec2(5.0f, 0.0f)});

    REQUIRE(gati::set_parent(world, child, parent));
    // Cycle rejection
    REQUIRE_FALSE(gati::set_parent(world, parent, child));

    auto wpos = gati::world_position(world, child);
    REQUIRE(wpos[0] == 15.0f);
    REQUIRE(wpos[1] == 0.0f);

    // Test render interpolation
    auto* tr = world.get<gati::Transform>(child);
    tr->prev_position = pebble::math::vec2(0.0f, 0.0f);
    tr->position      = pebble::math::vec2(10.0f, 0.0f);

    auto pose = gati::interpolated(*tr, 0.5f);
    REQUIRE(pose.position[0] == 5.0f);
    REQUIRE(pose.position[1] == 0.0f);
}

TEST_CASE("Gati: EventBus Lock-Free Channels", "[gati][event]") {
    gati::EventBus bus;

    bus.publish(gati::ContactEvent{1, 2, pebble::math::vec2(0.0f, 1.0f), 0.5f});
    bus.publish(gati::ContactEvent{3, 4, pebble::math::vec2(1.0f, 0.0f), 0.2f});

    int count = 0;
    std::size_t drained = bus.drain<gati::ContactEvent>([&](const gati::ContactEvent& ce) {
        ++count;
        if (count == 1) {
            REQUIRE(ce.a == 1);
            REQUIRE(ce.b == 2);
            REQUIRE(ce.depth == 0.5f);
        }
    });

    REQUIRE(drained == 2);
    REQUIRE(count == 2);

    // Second drain should be empty
    std::size_t empty_drain = bus.drain<gati::ContactEvent>([](const auto&) {});
    REQUIRE(empty_drain == 0);
}

TEST_CASE("Gati: Animation Sampling and State Machine", "[gati][anim]") {
    gati::Clip clip;
    gati::TrackScalar posXTrack;
    posXTrack.channel = gati::Channel::PosX;
    (void)posXTrack.curve.keys.push_back({0.0f, 0.0f, gati::Interp::Linear});
    (void)posXTrack.curve.keys.push_back({1.0f, 100.0f, gati::Interp::Linear});
    (void)clip.tracks.push_back(posXTrack);
    clip.duration = 1.0f;

    gati::Transform tr;
    clip.sample_into(tr, 0.5f);

    REQUIRE(tr.position[0] == 50.0f);
}

TEST_CASE("Gati: Game Orchestrator Loop", "[gati][game]") {
    gati::Game game{gati::ClockConfig{.hz = 60.0f}};

    auto e = game.world().spawn();
    game.world().add<gati::Transform>(e, {.position = pebble::math::vec2(0.0f, 0.0f)});

    // Update by 1/60th second
    game.update(1.0f / 60.0f);
    REQUIRE(game.clock().total_steps() == 1);
}

TEST_CASE("Gati: Reactive Collision Cue Manager", "[gati][reactive_cues]") {
    gati::EventBus bus;
    gati::ReactiveCueManager cues;

    int triggered_count = 0;
    cues.on_impact(0.2f, [&](const gati::ContactEvent& ce) {
        ++triggered_count;
        REQUIRE(ce.depth >= 0.2f);
    });

    // 1. Weak contact (below threshold) -> should not trigger
    bus.publish(gati::ContactEvent{1, 2, pebble::math::vec2(0.0f, 1.0f), 0.05f});
    cues.process_events(bus);
    REQUIRE(triggered_count == 0);

    // 2. Strong contact (above threshold) -> should trigger
    bus.publish(gati::ContactEvent{1, 2, pebble::math::vec2(0.0f, 1.0f), 0.5f});
    cues.process_events(bus);
    REQUIRE(triggered_count == 1);
}

TEST_CASE("Gati: SpatialTileStreamer fires on_evict for tiles leaving viewport", "[gati][world][streamer]") {
    gati::world::SpatialTileStreamer<320.0f, 200.0f> streamer;

    std::vector<gati::world::TileCoord> discovered, active, evicted;

    // First viewport: covers tile (0,0)
    streamer.update_viewport(
        pebble::math::vec2(160.0f, 100.0f), 320.0f, 200.0f, 0.0f,
        [&](gati::world::TileCoord c) { discovered.push_back(c); },
        [&](gati::world::TileCoord c) { active.push_back(c); },
        [&](gati::world::TileCoord c) { evicted.push_back(c); }
    );

    REQUIRE(discovered.size() >= 1);
    REQUIRE(active.size() >= 1);
    REQUIRE(evicted.empty()); // nothing to evict on first frame

    active.clear(); evicted.clear();

    // Move camera far right: tile (0,0) should be evicted
    streamer.update_viewport(
        pebble::math::vec2(2000.0f, 100.0f), 320.0f, 200.0f, 0.0f,
        [&](gati::world::TileCoord c) { discovered.push_back(c); },
        [&](gati::world::TileCoord c) { active.push_back(c); },
        [&](gati::world::TileCoord c) { evicted.push_back(c); }
    );

    REQUIRE_FALSE(evicted.empty()); // tile (0,0) should be evicted
}

TEST_CASE("Gati: SpatialTileStreamer update_viewport backward-compat (no on_evict)", "[gati][world][streamer]") {
    gati::world::SpatialTileStreamer<320.0f, 200.0f> streamer;

    int disc_count = 0, active_count = 0;
    // Old 2-callback API still compiles and works
    streamer.update_viewport(
        pebble::math::vec2(160.0f, 100.0f), 320.0f, 200.0f, 0.0f,
        [&](gati::world::TileCoord) { ++disc_count; },
        [&](gati::world::TileCoord) { ++active_count; }
    );

    REQUIRE(disc_count >= 1);
    REQUIRE(active_count >= 1);
}

