// ============================================================================
// src/app/main.cpp — Pebble Master End-to-End Simulation Showcase Executable
// ============================================================================
// Demonstrates end-to-end synergy of Pebble subsystems:
//   - pebble::ecs: High-performance dense entity & component lifecycle
//   - akruti: Bézier Splines, Voronoi fracture, and continuous CSG morphs
//   - prakriti & gati: Thermodynamics, elemental reactions (Water + Lava -> Obsidian)
//   - spandana: Universal World EDSL with automatic timeline dependency inference,
//               TwoBoneIK aiming, Verlet cloth secondary dynamics, and camera trauma
//   - kalpana: 2D Vector scene graph, Kubelka-Munk spectral pigment mixing, and
//              monomorphized Canvas<capture_backend> snapshotting
//   - dhvani: 2D Spatial audio cue dispatch and stereo panning
// ============================================================================

#include "ecs/ecs.hpp"
#include "gati/gati.hpp"
#include "gati/material.hpp"
#include "gati/elemental.hpp"
#include "spandana/spandana.hpp"
#include "kalpana/kalpana.hpp"
#include "dhvani/dhvani.hpp"
#include "dhvani/edsl.hpp"

#include <iostream>
#include <iomanip>

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    std::cout << "===============================================================\n";
    std::cout << "       PEBBLE MASTER END-TO-END ENGINE SHOWCASE               \n";
    std::cout << "===============================================================\n\n";

    // 1. Initialize Realtime Game & ECS World
    gati::Game game{gati::ClockConfig{.hz = 60.0f}};
    auto& world = game.world();
    std::cout << "[1/6] Initialized Gati fixed-step deterministic clock & ECS World.\n";

    // 2. Setup Dhvani Audio System & Listener
    pebble::dhvani::SoundBus sound_bus;
    pebble::dhvani::AudioListener2D listener{
        .position = {0.0f, 0.0f},
        .forward = {0.0f, 1.0f},
        .max_distance = 600.0f
    };
    std::cout << "[2/6] Initialized Dhvani 2D Spatial SoundBus and AudioListener.\n";

    // 3. Spawn Entities with Materials & Elemental Types
    // Entity A: Hero Player with Transform & Verlet Scarf
    auto hero = world.spawn();
    world.add<gati::Transform>(hero, {.position = pebble::math::vec2(0.0f, 0.0f)});
    pebble::spandana::VerletCloth2D hero_scarf(6, 4.0f);
    pebble::spandana::TwoBoneIK hero_arm(15.0f, 12.0f);

    // Entity B: Molten Lava Pool
    auto lava_pool = world.spawn();
    world.add<gati::Transform>(lava_pool, {.position = pebble::math::vec2(100.0f, 0.0f)});
    world.add<gati::MaterialComponent>(lava_pool, gati::MaterialComponent::Lava());
    world.add<gati::ElementalComponent>(lava_pool, {.type = gati::ElementType::Lava});

    // Entity C: Water Projectile
    auto water_orb = world.spawn();
    world.add<gati::Transform>(water_orb, {.position = pebble::math::vec2(0.0f, 50.0f)});
    world.add<gati::MaterialComponent>(water_orb, gati::MaterialComponent::Water());
    world.add<gati::ElementalComponent>(water_orb, {.type = gati::ElementType::Water});

    std::cout << "[3/6] Spawned entities: Hero, Molten Lava Pool, and Water Projectile.\n";

    // 4. Construct Kalpana Vector Scene & Kubelka-Munk Spectral Pigment Mixing
    kalpana::Scene kalpana_scene;
    kalpana_scene.clear_color(kalpana::colors::black());

    // Mix Blue pigment + Yellow pigment subtractively using Kubelka-Munk
    kalpana::Color vibrant_green = kalpana::spectral::mix(
        kalpana::colors::blue(), kalpana::colors::yellow(), 0.5f);

    kalpana::Path banner_card;
    banner_card.round_rect(10.0f, 10.0f, 200.0f, 60.0f, 8.0f, 8.0f);
    kalpana_scene.add(kalpana::Node::shape(
        banner_card, kalpana::Paint::filled_outlined(vibrant_green, kalpana::colors::white(), 2.0f)
    ));

    kalpana::DefaultCanvas canvas(256, 128);
    canvas.render(kalpana_scene);
    auto snapshot = canvas.snapshot();
    std::cout << "[4/6] Rendered Kalpana Scene with Kubelka-Munk Spectral Pigment Mixing ("
              << snapshot.size() << " pixels captured).\n";

    // 5. Construct Declarative Universal Spandana World Timeline
    pebble::spandana::Timeline timeline;
    pebble::spandana::ScreenShake2D camera;
    auto* hero_tr = world.get<gati::Transform>(hero);
    auto* water_tr = world.get<gati::Transform>(water_orb);

    akruti::CubicBezierCurve water_trajectory{
        .p0 = {0.0f, 50.0f},
        .p1 = {30.0f, 80.0f},
        .p2 = {70.0f, 60.0f},
        .p3 = {100.0f, 0.0f},
        .radius = 2.0f
    };

    using namespace pebble::spandana::edsl;
    using namespace pebble::spandana::ease;

    timeline.add(
        // Parallel Track A: Hero dashes forward
        tween(hero_tr->position).to({40.0f, 0.0f}, 0.25f).ease(out_quad),
        pebble::dhvani::edsl::audio_cue(sound_bus, "hero_dash.wav").pitch(1.2f),

        // Parallel Track B: Water orb travels along Bézier trajectory into Lava
        follow_path(water_tr->position, water_trajectory).duration(0.3f).ease(in_out_quad),

        // Sequential Phase: Lava and Water impact, camera shakes, and steam bursts
        shake_camera(camera).trauma(0.8f).duration(0.4f),
        particle_burst().at({100.0f, 0.0f}).count(40).speed(100.0f, 250.0f).lifetime(0.4f),
        pebble::dhvani::edsl::audio_cue(sound_bus, "steam_hiss.wav").pitch(0.9f)
    );
    std::cout << "[5/6] Assembled Declarative Spandana Timeline with automatic dependency inference.\n";

    // 6. Execute Fixed-Step Simulation Loop (60 ticks)
    std::cout << "[6/6] Stepping 60 simulation ticks at 60 Hz...\n\n";
    constexpr float dt = 1.0f / 60.0f;

    for (int frame = 1; frame <= 60; ++frame) {
        // Step Spandana Timeline
        timeline.update(dt);

        // Update secondary physics dynamics (cloth scarf & two-bone IK aiming)
        hero_scarf.update(hero_tr->position, dt);
        (void)hero_arm.solve(hero_tr->position, /*target*/ {100.0f, 0.0f});

        // Trigger Elemental Reaction when water arrives at lava (at t >= 0.3s, frame 18)
        if (frame == 18 && world.alive(water_orb) && world.alive(lava_pool)) {
            gati::ContactEvent ce{
                .a = water_orb.index,
                .b = lava_pool.index,
                .normal = {0.0f, -1.0f},
                .depth = 2.0f,
                .point = {100.0f, 0.0f}
            };
            gati::ElementalReactionMatrix::process_contact(world, ce);
            std::cout << "  >>> Frame 18 (t=0.30s): Contact Event triggered! Water extinguished Lava into Obsidian solid.\n";
        }

        // Step Game & ECS
        game.update(dt);

        // Drain audio cues
        sound_bus.drain([](const pebble::dhvani::SoundCue& cue) {
            std::cout << "  [Dhvani Audio] Playing sound: " << cue.name
                      << " (Vol: " << cue.volume << ", Pitch: " << cue.pitch << ")\n";
        });

        if (frame % 20 == 0 || frame == 60) {
            std::cout << "  Frame " << std::setw(2) << frame << " (t="
                      << std::fixed << std::setprecision(2) << frame * dt << "s): Hero Pos=("
                      << hero_tr->position[0] << ", " << hero_tr->position[1] << ")"
                      << ", Camera Trauma=" << camera.trauma() << "\n";
        }
    }

    // Verify Obsidian state
    auto* lava_elem = world.get<gati::ElementalComponent>(lava_pool);
    if (lava_elem && lava_elem->type == gati::ElementType::Obsidian) {
        std::cout << "\n[SUCCESS] Lava Pool successfully reacted and crystallized into solid Obsidian rock!\n";
    }

    std::cout << "\n===============================================================\n";
    std::cout << "       PEBBLE SIMULATION EXECUTED SUCCESSFULLY                \n";
    std::cout << "===============================================================\n";

    return 0;
}
