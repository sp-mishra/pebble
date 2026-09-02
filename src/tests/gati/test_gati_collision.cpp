#define GATI_ENABLE_AKRUTI 1
#include "catch_amalgamated.hpp"
#include "gati/gati.hpp"
#include <cmath>

TEST_CASE (
"Gati: Collision with Akruti ChainShape and GridSDF"
,
"[gati][collision]"
)
 {
    pebble::ecs::World world;
    gati::CollisionSystem col_sys;
    gati::EventBus bus;
    smriti::pools::LinearArena scratch{1024 * 1024};
    gati::ParallelExecutor executor;
    gati::StepContext ctx{1.0f / 60.0f, 0, bus, scratch, executor};

    // 1. Spawn terrain chain (horizontal line at y = 0)
    auto terrain = world.spawn();
    akruti::ChainShape<16> chain;
    (void)chain.verts.push_back({-20.0f, 0.0f});
    (void)chain.verts.push_back({20.0f, 0.0f});
    chain.radius = 0.5f;

    world.add<gati::Transform>(terrain, {.position = pebble::math::vec2(0.0f, 0.0f)});
    world.add<gati::ShapeRef>(terrain, {.shape = chain});

    // 2. Spawn a circle overlapping the chain at (0, 0.5) with radius 1.0 (overlaps down to y = -0.5)
    auto circle_ent = world.spawn();
    world.add<gati::Transform>(circle_ent, {.position = pebble::math::vec2(0.0f, 0.5f)});
    world.add<gati::ShapeRef>(circle_ent, {.shape = akruti::Circle{{0.0f, 0.0f}, 1.0f}});

    int contacts = 0;
    // Step collision system directly
    col_sys.run(world, ctx);

    // Should emit contact event
    bus.drain<gati::ContactEvent>([&](const gati::ContactEvent& ce) {
        ++contacts;
        REQUIRE(ce.depth > 0.0f);
    });

    REQUIRE(contacts >= 1);
}

TEST_CASE (
"Gati: Policy-Based TensorBroadphase with 250 Entities"
,
"[gati][collision][tensor]"
)
 {
    pebble::ecs::World world;
    gati::CollisionSystem col_sys;
    gati::EventBus bus;
    smriti::pools::LinearArena scratch{1024 * 1024};
    gati::ParallelExecutor executor;
    gati::StepContext ctx{1.0f / 60.0f, 0, bus, scratch, executor};

    // Spawn 250 overlapping entity pairs (triggers TensorBroadphase threshold > 200)
    for (int i = 0; i < 250; ++i) {
        auto e = world.spawn();
        float x = float(i % 10) * 10.0f;
        float y = float(i / 10) * 10.0f;
        world.add<gati::Transform>(e, {.position = pebble::math::vec2(x, y)});
        world.add<gati::ShapeRef>(e, {.shape = akruti::Circle{{0.0f, 0.0f}, 6.0f}});
    }

    int contacts = 0;
    col_sys.run(world, ctx);

    bus.drain<gati::ContactEvent>([&](const gati::ContactEvent& ce) {
        ++contacts;
        REQUIRE(ce.depth > 0.0f);
    });

    // 250 densely packed circles with radius 6 in a 10x10 spacing must produce contacts
    REQUIRE(contacts > 0);
}

