#include "catch_amalgamated.hpp"
#include "ecs/ecs.hpp"
#include "containers/numeric/math_vector.hpp"

namespace {

struct Position {
    pebble::math::vec2 val{};
    bool operator==(const Position&) const = default;
};

struct Velocity {
    pebble::math::vec2 val{};
    bool operator==(const Velocity&) const = default;
};

struct Tag {
    int id = 0;
};

struct Disabled {};

struct Name {
    const char* str = nullptr;
};

} // namespace

TEST_CASE("ECS: Entity Lifecycle and Generation Safety", "[ecs][lifecycle]") {
    pebble::ecs::World world;

    auto e1 = world.spawn();
    REQUIRE(world.alive(e1));
    REQUIRE(e1.index == 1);
    REQUIRE(e1.generation == 1);

    auto e2 = world.spawn();
    REQUIRE(world.alive(e2));
    REQUIRE(e2.index == 2);

    world.despawn(e1);
    REQUIRE_FALSE(world.alive(e1));
    REQUIRE(world.alive(e2));

    // Re-spawn should recycle index 1 with incremented generation
    auto e3 = world.spawn();
    REQUIRE(world.alive(e3));
    REQUIRE(e3.index == 1);
    REQUIRE(e3.generation == 2);
    REQUIRE_FALSE(world.alive(e1)); // Stale handle remains invalid
}

TEST_CASE("ECS: Component Add, Get, Has, and Remove", "[ecs][components]") {
    pebble::ecs::World world;

    auto e = world.spawn();
    world.add(e, Position{pebble::math::vec2(10.0f, 20.0f)});
    world.add(e, Velocity{pebble::math::vec2(1.0f, -2.0f)});

    REQUIRE(world.has<Position>(e));
    REQUIRE(world.has<Velocity>(e));
    REQUIRE_FALSE(world.has<Tag>(e));

    auto* pos = world.get<Position>(e);
    REQUIRE(pos != nullptr);
    REQUIRE(pos->val[0] == 10.0f);
    REQUIRE(pos->val[1] == 20.0f);

    world.remove<Velocity>(e);
    REQUIRE_FALSE(world.has<Velocity>(e));
    REQUIRE(world.get<Velocity>(e) == nullptr);
    REQUIRE(world.has<Position>(e));
}

TEST_CASE("ECS: Multi-Component Query View", "[ecs][query]") {
    pebble::ecs::World world;

    auto e1 = world.spawn();
    world.add(e1, Position{pebble::math::vec2(1.0f, 2.0f)});
    world.add(e1, Velocity{pebble::math::vec2(0.1f, 0.2f)});

    auto e2 = world.spawn();
    world.add(e2, Position{pebble::math::vec2(3.0f, 4.0f)});

    auto e3 = world.spawn();
    world.add(e3, Position{pebble::math::vec2(5.0f, 6.0f)});
    world.add(e3, Velocity{pebble::math::vec2(0.5f, 0.6f)});
    world.add(e3, Tag{42});

    int matched = 0;
    world.view<Position, Velocity>([&](pebble::ecs::Entity e, Position& pos, Velocity& vel) {
        ++matched;
        pos.val = pos.val + vel.val;
    });

    REQUIRE(matched == 2);
    REQUIRE(world.get<Position>(e1)->val[0] == 1.1f);
    REQUIRE(world.get<Position>(e3)->val[0] == 5.5f);
    REQUIRE(world.get<Position>(e2)->val[0] == 3.0f); // Untouched
}

TEST_CASE("ECS: Rich Query Filter DSL (With, Without, Optional)", "[ecs][query_dsl]") {
    pebble::ecs::World world;

    auto e1 = world.spawn();
    world.add(e1, Position{pebble::math::vec2(1.0f, 1.0f)});
    world.add(e1, Velocity{pebble::math::vec2(1.0f, 0.0f)});
    world.add(e1, Name{"Player"});

    auto e2 = world.spawn();
    world.add(e2, Position{pebble::math::vec2(2.0f, 2.0f)});
    world.add(e2, Velocity{pebble::math::vec2(0.0f, 1.0f)});
    world.add(e2, Disabled{});

    auto e3 = world.spawn();
    world.add(e3, Position{pebble::math::vec2(3.0f, 3.0f)});
    world.add(e3, Velocity{pebble::math::vec2(-1.0f, 0.0f)});

    int matched = 0;
    world.query<pebble::ecs::With<Position, Velocity>,
                pebble::ecs::Without<Disabled>,
                pebble::ecs::Optional<Name>>(
        [&](pebble::ecs::Entity e, Position& pos, Velocity& vel, Name* name) {
            ++matched;
            if (name) {
                REQUIRE(std::string(name->str) == "Player");
            }
        });

    REQUIRE(matched == 2); // e1 and e3 (e2 is Disabled)
}

TEST_CASE("ECS: Deferred CommandBuffer Execution", "[ecs][commands]") {
    pebble::ecs::World world;

    auto e1 = world.spawn();
    world.add(e1, Position{pebble::math::vec2(1.0f, 1.0f)});

    world.view<Position>([&](pebble::ecs::Entity e, Position&) {
        world.commands().add(e, Tag{100});
        world.commands().despawn(e);
    });

    // Before flush, entity is still alive and has no tag
    REQUIRE(world.alive(e1));
    REQUIRE_FALSE(world.has<Tag>(e1));

    world.flush_commands();

    // After flush, entity has been despawned
    REQUIRE_FALSE(world.alive(e1));
}
