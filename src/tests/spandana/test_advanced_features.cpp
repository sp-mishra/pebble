#include "catch_amalgamated.hpp"
#include "gati/gati.hpp"
#include "gati/elemental.hpp"
#include "spandana/skeleton.hpp"
#include "spandana/serialization.hpp"

TEST_CASE("Gati Elemental: Water and Lava Contact Fuses to Obsidian with Steam", "[gati][elemental]") {
    pebble::ecs::World world;

    auto water = world.spawn();
    world.add<gati::Transform>(water, {.position = {0.0f, 0.0f}});
    world.add<gati::MaterialComponent>(water, gati::MaterialComponent::Water());
    world.add<gati::ElementalComponent>(water, {.type = gati::ElementType::Water});

    auto lava = world.spawn();
    world.add<gati::Transform>(lava, {.position = {5.0f, 0.0f}});
    world.add<gati::MaterialComponent>(lava, gati::MaterialComponent::Lava());
    world.add<gati::ElementalComponent>(lava, {.type = gati::ElementType::Lava});

    gati::ContactEvent ce{
        .a = water,
        .b = lava,
        .normal = {1.0f, 0.0f},
        .point = {2.5f, 0.0f},
        .depth = 1.0f
    };

    gati::ElementalReactionMatrix::process_contact(world, ce);

    // Water should be consumed
    REQUIRE_FALSE(world.alive(water));

    // Lava should transform to Obsidian solid
    REQUIRE(world.alive(lava));
    auto* elem = world.get<gati::ElementalComponent>(lava);
    REQUIRE(elem != nullptr);
    REQUIRE(elem->type == gati::ElementType::Obsidian);

    auto* mat = world.get<gati::MaterialComponent>(lava);
    REQUIRE(mat->phase_fractions.solid() > 0.8f);
}

TEST_CASE("Spandana Skeleton: 2D Bone Hierarchy Forward Kinematics & Skinning", "[spandana][skeleton]") {
    pebble::spandana::Skeleton2D skeleton;

    // Bone 0: Root at origin
    int root = skeleton.add_bone("root", -1, {.position = {0.0f, 0.0f}, .rotation = 0.0f}, 20.0f);
    // Bone 1: Arm attached to root with +20 X offset
    int arm  = skeleton.add_bone("arm", root, {.position = {20.0f, 0.0f}, .rotation = 0.0f}, 20.0f);

    skeleton.update_fk();
    REQUIRE(skeleton.bones()[arm].world_transform.position[0] == 20.0f);

    // Rotate root bone by 90 degrees (pi / 2 rad)
    constexpr float kPiOver2 = 1.57079632679f;
    skeleton.set_bone_rotation(root, kPiOver2);
    skeleton.update_fk();

    // Arm should now be at (0, 20)
    const auto& arm_pos = skeleton.bones()[arm].world_transform.position;
    REQUIRE(std::abs(arm_pos[0]) < 1e-4f);
    REQUIRE(std::abs(arm_pos[1] - 20.0f) < 1e-4f);

    // Linear Blend Skinning Test
    pebble::spandana::SkinnedVertex2D vert{
        .bind_pos = {0.0f, 0.0f}
    };
    (void)vert.weights.push_back({.bone_index = static_cast<std::uint32_t>(arm), .weight = 1.0f});

    auto skinned = skeleton.skin_vertex(vert);
    REQUIRE(std::abs(skinned[0]) < 1e-4f);
    REQUIRE(std::abs(skinned[1] - 20.0f) < 1e-4f);
}

TEST_CASE("Spandana Serialization: Glaze JSON Material & Animation Round-Trip", "[spandana][serialization]") {
    auto ice = gati::MaterialComponent::Ice();
    ice.temperature = -25.0f;

    // Serialize to Glaze JSON
    std::string json = pebble::spandana::io::serialize_material_json(ice);
    REQUIRE_FALSE(json.empty());
    REQUIRE(json.find("-25") != std::string::npos);

    // Deserialize
    auto deserialized = pebble::spandana::io::deserialize_material_json(json);
    REQUIRE(deserialized.temperature == -25.0f);
    REQUIRE(deserialized.phase_fractions.solid() > 0.9f);
}
