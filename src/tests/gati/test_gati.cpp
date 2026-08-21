#include <catch_amalgamated.hpp>
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
