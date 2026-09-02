#include "catch_amalgamated.hpp"
#include "dhvani/dhvani.hpp"
#include "dhvani/spatial.hpp"
#include "dhvani/edsl.hpp"
#include "spandana/spandana.hpp"

TEST_CASE (
"Dhvani: Basic SoundBus Queue and Drain"
,
"[dhvani][soundbus]"
)
 {
    pebble::dhvani::SoundBus bus;

    bus.play("jump.wav", 0.8f, 1.1f);
    bus.play("slash.wav", 1.0f, 1.0f);

    REQUIRE(bus.pending_count() == 2);

    int count = 0;
    bus.drain([&](const pebble::dhvani::SoundCue& cue) {
        ++count;
        if (count == 1) {
            REQUIRE(cue.name == "jump.wav");
            REQUIRE(cue.volume == 0.8f);
            REQUIRE(cue.pitch == 1.1f);
        }
    });

    REQUIRE(count == 2);
    REQUIRE(bus.pending_count() == 0);
}

TEST_CASE (
"Dhvani: 2D Spatial Audio Attenuation & Stereo Panning"
,
"[dhvani][spatial]"
)
 {
    pebble::dhvani::AudioListener2D listener{
        .position = {0.0f, 0.0f},
        .forward = {0.0f, 1.0f},
        .max_distance = 500.0f,
        .ref_distance = 50.0f
    };

    // 1. Emitter at listener position
    auto center_out = pebble::dhvani::compute_spatial_audio({0.0f, 0.0f}, listener, 1.0f);
    REQUIRE(center_out.attenuation == 1.0f);
    REQUIRE(center_out.pan == 0.0f);

    // 2. Emitter to the right (+X)
    auto right_out = pebble::dhvani::compute_spatial_audio({100.0f, 0.0f}, listener, 1.0f);
    REQUIRE(right_out.pan > 0.5f);
    REQUIRE(right_out.volume_right > right_out.volume_left);

    // 3. Emitter to the left (-X)
    auto left_out = pebble::dhvani::compute_spatial_audio({-100.0f, 0.0f}, listener, 1.0f);
    REQUIRE(left_out.pan < -0.5f);
    REQUIRE(left_out.volume_left > left_out.volume_right);

    // 4. Emitter beyond max distance
    auto far_out = pebble::dhvani::compute_spatial_audio({600.0f, 0.0f}, listener, 1.0f);
    REQUIRE(far_out.attenuation == 0.0f);
}

TEST_CASE (
"Dhvani: Spandana Timeline EDSL Audio Cues"
,
"[dhvani][edsl]"
)
 {
    pebble::dhvani::SoundBus sound_bus;
    pebble::spandana::Timeline timeline;

    float dummy = 0.0f;
    pebble::spandana::ResourceKey key{1, 1, 0};

    timeline.add(
        pebble::spandana::edsl::tween(dummy, key).to(10.0f, 0.2f),
        pebble::dhvani::edsl::audio_cue(sound_bus, "explosion.wav").volume(0.9f).pitch(1.0f)
    );

    REQUIRE(sound_bus.pending_count() == 0);

    // Step timeline
    timeline.update(0.1f);
    REQUIRE(sound_bus.pending_count() == 1);
}

#include "gati/reactive_cues.hpp"
#include "gati/transform.hpp"

TEST_CASE (
"Dhvani: Gati AudioEmitter Component & SpatialAudioSystem Dispatch"
,
"[dhvani][gati][ecs]"
)
 {
    pebble::ecs::World world;
    pebble::dhvani::SoundBus sound_bus;
    gati::SpatialAudioSystem audio_sys;

    // 1. Spawn AudioListener entity
    auto listener_ent = world.spawn();
    world.add<gati::AudioListener>(listener_ent, {
        .listener = pebble::dhvani::AudioListener2D{
            .position = {0.0f, 0.0f},
            .forward = {0.0f, 1.0f},
            .max_distance = 600.0f,
            .ref_distance = 50.0f
        }
    });

    // 2. Spawn AudioEmitter entity at (100, 0)
    auto emitter_ent = world.spawn();
    world.add<gati::Transform>(emitter_ent, {.position = {100.0f, 0.0f}});
    world.add<gati::AudioEmitter>(emitter_ent, {
        .name = "laser_blast.wav",
        .volume = 1.0f,
        .pitch = 1.0f,
        .trigger_play = true,
        .is_spatial = true
    });

    gati::EventBus bus;
    smriti::pools::LinearArena scratch(1024);
    gati::ParallelExecutor executor;
    gati::StepContext ctx{1.0f / 60.0f, 0, bus, scratch, executor};

    // Step spatial audio system
    audio_sys.run(world, ctx, sound_bus);

    REQUIRE(sound_bus.pending_count() == 1);

    // Verify cue properties
    sound_bus.drain([](const pebble::dhvani::SoundCue& cue) {
        REQUIRE(cue.name == "laser_blast.wav");
        REQUIRE(cue.is_spatial);
        REQUIRE(cue.volume > 0.0f);
    });

    // Trigger should have been reset
    auto* ae = world.get<gati::AudioEmitter>(emitter_ent);
    REQUIRE_FALSE(ae->trigger_play);
}

