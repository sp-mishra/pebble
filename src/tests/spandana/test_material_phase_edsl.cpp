#include "catch_amalgamated.hpp"
#include "gati/gati.hpp"
#include "gati/material.hpp"
#include "gati/material_reaction.hpp"
#include "spandana/spandana.hpp"

TEST_CASE("Spandana & Gati: Ice Melting into Water and Boiling to Gas", "[spandana][material][phase]") {
    pebble::ecs::World world;

    auto ice_entity = world.spawn();
    world.add<gati::Transform>(ice_entity, {.position = {0.0f, 0.0f}});
    world.add<gati::MaterialComponent>(ice_entity, gati::MaterialComponent::Ice());

    auto* mat = world.get<gati::MaterialComponent>(ice_entity);
    REQUIRE(mat->temperature == -10.0f);
    REQUIRE(mat->phase_fractions.solid() > 0.9f);
    REQUIRE(mat->phase_fractions.liquid() < 0.1f);

    // 1. Heat ice up above 0 C (e.g. 50 C)
    mat->update_thermodynamics(60.0f, 1.0f); // T = 50 C
    REQUIRE(mat->temperature == 50.0f);
    REQUIRE(mat->phase_fractions.liquid() > 0.8f);
    REQUIRE(mat->phase_fractions.solid() < 0.1f);

    // 2. Heat water up above 100 C (e.g. 150 C) -> turns into gas
    mat->update_thermodynamics(100.0f, 1.0f); // T = 150 C
    REQUIRE(mat->temperature == 150.0f);
    REQUIRE(mat->phase_fractions.gas() > 0.8f);
}

TEST_CASE("Spandana & Gati: Brittle Glass Shatter on High-Velocity Collision", "[spandana][material][shatter]") {
    pebble::ecs::World world;

    auto glass_entity = world.spawn();
    world.add<gati::Transform>(glass_entity, {.position = {0.0f, 0.0f}});
    world.add<gati::MaterialComponent>(glass_entity, gati::MaterialComponent::Glass());

    auto steel_entity = world.spawn();
    world.add<gati::Transform>(steel_entity, {.position = {1.0f, 0.0f}});
    world.add<gati::MaterialComponent>(steel_entity, gati::MaterialComponent::Steel());

    gati::ContactEvent ce{
        .a = glass_entity,
        .b = steel_entity,
        .normal = {1.0f, 0.0f},
        .point = {0.5f, 0.0f},
        .depth = 5.0f // High penetration / impact depth
    };

    gati::MaterialReactionSystem::evaluate_reactions(world, ce);

    // Glass should fracture and despawn from world
    REQUIRE_FALSE(world.alive(glass_entity));
    // Dynamic glass shards should now be spawned
    REQUIRE(world.entity_count() >= 3);
}

TEST_CASE("Spandana & Gati: Molten Material Fusion / Welding on Contact", "[spandana][material][fusion]") {
    pebble::ecs::World world;

    auto lava_a = world.spawn();
    world.add<gati::Transform>(lava_a, {.position = {0.0f, 0.0f}});
    world.add<gati::MaterialComponent>(lava_a, gati::MaterialComponent::Lava());

    auto lava_b = world.spawn();
    world.add<gati::Transform>(lava_b, {.position = {10.0f, 0.0f}});
    world.add<gati::MaterialComponent>(lava_b, gati::MaterialComponent::Lava());

    gati::ContactEvent ce{
        .a = lava_a,
        .b = lava_b,
        .normal = {1.0f, 0.0f},
        .point = {5.0f, 0.0f},
        .depth = 1.0f
    };

    gati::MaterialReactionSystem::evaluate_reactions(world, ce);

    // Molten blobs should fuse into single body
    REQUIRE(world.alive(lava_a));
    REQUIRE_FALSE(world.alive(lava_b));
}

TEST_CASE("Spandana: Declarative Thermodynamics EDSL", "[spandana][edsl][thermodynamics]") {
    pebble::ecs::World world;
    pebble::spandana::Timeline timeline;

    auto ice_cube = world.spawn();
    world.add<gati::Transform>(ice_cube, {.position = {10.0f, 10.0f}});

    timeline.add(
        pebble::spandana::edsl::set_material(world, ice_cube, gati::MaterialComponent::Ice()),
        pebble::spandana::edsl::apply_heat(world).at({10.0f, 10.0f}).temperature(400.0f).radius(50.0f).duration(0.2f)
    );

    // Initial state
    auto* mat = world.get<gati::MaterialComponent>(ice_cube);
    REQUIRE(mat != nullptr);
    REQUIRE(mat->temperature == -10.0f);

    // Step timeline with heat source
    timeline.update(0.1f);
    REQUIRE(mat->temperature > -10.0f);

    timeline.update(0.1f);
    REQUIRE(mat->temperature > 20.0f);
    REQUIRE(mat->phase_fractions.liquid() > 0.5f);
}
