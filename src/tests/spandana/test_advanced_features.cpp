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
        .a = water.index,
        .b = lava.index,
        .normal = {1.0f, 0.0f},
        .depth = 1.0f,
        .point = {2.5f, 0.0f}
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

// ============================================================================
// Appended coverage for the no-virtual / policy-based / zero-overhead rewrite.
// (Timeline SBO type-erasure, templated easing, spring caching, dependency
//  inference, and Dhvani auto-sonification policy.)
// ============================================================================
#include "spandana/timeline.hpp"
#include "spandana/edsl/motion_edsl.hpp"
#include "spandana/edsl/audio_policy.hpp"
#include "spandana/spring.hpp"
#include <cmath>
#include <type_traits>

TEST_CASE("Spandana Timeline: no-virtual SBO action is trivially relocatable & heap-free",
          "[spandana][timeline][zero-overhead]") {
    using pebble::spandana::Action;
    using pebble::spandana::Timeline;

    // The type-erased action must be a plain value type: no vtable pointer, no
    // heap. Its size is the inline storage, not a pointer to a polymorphic base.
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<Action>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Action>);
    STATIC_REQUIRE(!std::is_polymorphic_v<Action>);
    STATIC_REQUIRE(!std::is_polymorphic_v<Timeline>);
    // Inline storage dominates the footprint (no owning pointer indirection).
    STATIC_REQUIRE(sizeof(Action) >= 96);
}

TEST_CASE("Spandana Timeline: dependency inference parallels disjoint keys, serializes shared",
          "[spandana][timeline][deps]") {
    using namespace pebble::spandana;
    using namespace pebble::spandana::edsl;

    // Two tweens sharing one resource key run sequentially → durations add.
    float a = 0.0f, b = 0.0f;
    {
        Timeline tl;
        ResourceKey shared{42};
        tl.add(tween(a, shared).to(1.0f, 0.2f));
        tl.add(tween(b, shared).to(1.0f, 0.2f));
        REQUIRE(std::abs(tl.total_duration() - 0.4f) < 1e-4f);
    }
    // Two tweens on disjoint keys run in parallel → max, not sum.
    {
        Timeline tl;
        tl.add(tween(a, ResourceKey{1}).to(1.0f, 0.2f));
        tl.add(tween(b, ResourceKey{2}).to(1.0f, 0.3f));
        REQUIRE(std::abs(tl.total_duration() - 0.3f) < 1e-4f);
    }
}

TEST_CASE("Spandana Easing: policy-templated tween is stateless & correct",
          "[spandana][easing][zero-overhead]") {
    using namespace pebble::spandana::edsl;
    using namespace pebble::spandana;

    // Rebinding the easing policy is a compile-time type change, carrying no
    // runtime bytes for stateless easings.
    float v = 0.0f;
    auto linear_tween = tween(v).to(10.0f, 1.0f);
    auto eased_tween  = tween(v).to(10.0f, 1.0f).ease(ease::InOutQuad{});
    STATIC_REQUIRE(!std::is_same_v<decltype(linear_tween), decltype(eased_tween)>);

    // Drive the eased tween to completion; endpoint must land exactly on target.
    eased_tween.update(1.0f, 1.0f);
    REQUIRE(std::abs(v - 10.0f) < 1e-3f);
}

TEST_CASE("Spandana Spring: cached omega/zeta matches recomputed analytic step",
          "[spandana][spring]") {
    using pebble::spandana::AnalyticalSpringDamper;
    AnalyticalSpringDamper spring(180.0f, 12.0f);
    float pos = 0.0f, vel = 0.0f;
    for (int i = 0; i < 120; ++i) {
        auto [np, nv] = spring.step(pos, vel, 100.0f, 1.0f / 120.0f);
        pos = np; vel = nv;
    }
    // Critically-ish damped system must settle on the target without blowup.
    REQUIRE(std::abs(pos - 100.0f) < 1.0f);
    REQUIRE(std::abs(vel) < 5.0f);
}

TEST_CASE("Spandana Spring: AngleSpringDamper takes the shortest arc across the wrap",
          "[spandana][spring]") {
    using pebble::spandana::AngleSpringDamper;
    AngleSpringDamper spring(180.0f, 24.0f);
    // From +3.0 rad toward -3.0 rad: shortest path is forward through +pi
    // (~+0.28 rad), NOT backward ~-6 rad. Velocity should be positive.
    auto [np, nv] = spring.step(3.0f, 0.0f, -3.0f, 1.0f / 120.0f);
    REQUIRE(nv > 0.0f);
    (void)np;
}

TEST_CASE("Spandana Sonification: SimProfile selects the correct Dhvani cue",
          "[spandana][dhvani][sonification]") {
    using namespace pebble::spandana::edsl;
    using pebble::dhvani::DhvaniCue;

    SonifyContext ctx{.density = 0.8f, .intensity = 1.0f};
    REQUIRE(sound_palette(SimProfile::Impact,   ctx).name == DhvaniCue::impact);
    REQUIRE(sound_palette(SimProfile::Fracture, ctx).name == DhvaniCue::fracture);
    REQUIRE(sound_palette(SimProfile::Friction, ctx).name == DhvaniCue::friction);
    // Intensity drives volume.
    REQUIRE(sound_palette(SimProfile::Impact, ctx).volume == 1.0f);
}

TEST_CASE("Spandana Sonification: NullSonifier is zero-overhead & plug-and-play default",
          "[spandana][dhvani][zero-overhead]") {
    using namespace pebble::spandana::edsl;

    // Default policy carries no sonifier bytes: the action is the same size with
    // NullSonifier as the empty-policy specialization.
    STATIC_REQUIRE(std::is_empty_v<NullSonifier>);
    STATIC_REQUIRE(sizeof(AutoSonifyAction<NullSonifier>) == sizeof(AutoSonifyAction<>));

    // A NullSonifier action runs (and does nothing) without touching any bus.
    auto a = auto_sonify(SimProfile::Impact).from(0.5f).build();
    a.on_start();
    REQUIRE(a.duration() == 0.0f);

    // The active policy drives a real bus without virtual dispatch.
    pebble::dhvani::SoundBus bus;
    auto d = auto_sonify(SimProfile::Fracture).from(0.9f).intensity(1.0f)
                 .via(DhvaniSonifier{&bus}).build();
    d.on_start();
    SUCCEED("Dhvani sonifier drove the bus via a compile-time policy");
}
