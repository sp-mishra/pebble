#include "catch_amalgamated.hpp"
#include "spandana/destruction.hpp"
#include "spandana/blend_space.hpp"
#include "spandana/spandana.hpp"
#include "gati/gati.hpp"

TEST_CASE("Spandana: Procedural Voronoi Polygon Shattering & Mass Properties", "[spandana][destruction]") {
    akruti::Poly box_poly{
        akruti::Vec{-20.0f, -20.0f},
        akruti::Vec{20.0f, -20.0f},
        akruti::Vec{20.0f, 20.0f},
        akruti::Vec{-20.0f, 20.0f}
    };

    pebble::math::vec2 impact{0.0f, 0.0f};

    auto shards = pebble::spandana::DestructionEngine::shatter_polygon(
        box_poly, impact, /*shard_count*/ 6, /*radial_impulse_mag*/ 250.0f
    );

    REQUIRE_FALSE(shards.empty());
    REQUIRE(shards.size() >= 3);

    float total_area = 0.0f;
    for (const auto& shard : shards) {
        REQUIRE(shard.area > 0.0f);
        REQUIRE(shard.inertia_z > 0.0f);
        total_area += shard.area;

        // Radial velocity directed outward
        const float speed = std::sqrt(shard.initial_velocity[0] * shard.initial_velocity[0] +
                                      shard.initial_velocity[1] * shard.initial_velocity[1]);
        if (speed > 1e-3f) {
            REQUIRE(std::abs(speed - 250.0f) < 1.0f);
        }
    }

    // Area conservation: total shard area should equal original box area (40x40 = 1600)
    REQUIRE(std::abs(total_area - 1600.0f) < 5.0f);
}

TEST_CASE("Spandana: World Entity Shattering", "[spandana][destruction][ecs]") {
    pebble::ecs::World world;

    auto entity = world.spawn();
    world.add<gati::Transform>(entity, {.position = {0.0f, 0.0f}});

    REQUIRE(world.alive(entity));

    pebble::spandana::DestructionEngine::shatter_entity_in_world(
        world, entity, {0.0f, 0.0f}, 6, 300.0f
    );

    // Original entity should be despawned
    REQUIRE_FALSE(world.alive(entity));

    // Dynamic shard entities should now be alive in the world
    REQUIRE(world.entity_count() >= 3);
}

TEST_CASE("Spandana: Parametric 2D Directional Blend Space", "[spandana][blend_space]") {
    pebble::spandana::FlipbookClip idle_clip{.name = "idle"};
    pebble::spandana::FlipbookClip walk_fwd_clip{.name = "walk_fwd"};
    pebble::spandana::FlipbookClip walk_back_clip{.name = "walk_back"};
    pebble::spandana::FlipbookClip strafe_right_clip{.name = "strafe_right"};
    pebble::spandana::FlipbookClip strafe_left_clip{.name = "strafe_left"};

    pebble::spandana::BlendSpace2D blend_space;
    blend_space.add_sample({0.0f, 0.0f}, &idle_clip);
    blend_space.add_sample({0.0f, 1.0f}, &walk_fwd_clip);
    blend_space.add_sample({0.0f, -1.0f}, &walk_back_clip);
    blend_space.add_sample({1.0f, 0.0f}, &strafe_right_clip);
    blend_space.add_sample({-1.0f, 0.0f}, &strafe_left_clip);

    // 1. Pure forward velocity (0, 1) -> 100% walk_fwd_clip
    auto fwd_weights = blend_space.evaluate_weights({0.0f, 1.0f});
    REQUIRE(fwd_weights.size() >= 1);
    bool found_fwd = false;
    for (const auto& w : fwd_weights) {
        if (w.clip == &walk_fwd_clip) {
            REQUIRE(w.weight > 0.99f);
            found_fwd = true;
        }
    }
    REQUIRE(found_fwd);

    // 2. Diagonal velocity (1, 1) -> normalized weights summing to 1.0
    auto diag_weights = blend_space.evaluate_weights({1.0f, 1.0f});
    float sum = 0.0f;
    for (const auto& w : diag_weights) {
        sum += w.weight;
    }
    REQUIRE(std::abs(sum - 1.0f) < 1e-4f);

    // 3. Phase synchronizer
    pebble::spandana::BlendSpaceAnimator animator(&blend_space);
    animator.set_velocity({0.0f, 2.0f});
    animator.update(0.1f);
    REQUIRE(animator.phase() > 0.0f);
}
