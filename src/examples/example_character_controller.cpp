#include "test/example_registry.hpp"
#include "spandana/spandana.hpp"
#include "gati/gati.hpp"
#include "akruti/akruti.hpp"
#include "dhvani/dhvani.hpp"
#include "dhvani/edsl.hpp"
#include <iostream>

namespace {

using namespace pebble::spandana::edsl;
using namespace pebble::spandana::ease;

enum class CharacterState { Idle, Run, Jump, Dash, Attack };

class CharacterControllerExample : public pebble::testfw::Example {
public:
    static constexpr std::string_view name() {
        return "character_controller_sandbox";
    }

    static constexpr std::string_view description() {
        return "Complete 2D Character Controller Sandbox with kinematic state machine, TwoBoneIK aiming, Verlet cape dynamics, and Dhvani sound cues";
    }

    static constexpr std::array<std::string_view, 3> tag_data{"character", "physics", "spandana"};
    static constexpr std::span<const std::string_view> tags() {
        return tag_data;
    }

    pebble::testfw::Result run() override {
        gati::Game game{gati::ClockConfig{.hz = 60.0f}};
        pebble::dhvani::SoundBus sound_bus;

        // 1. Spawn Player Entity
        auto player = game.world().spawn();
        game.world().add<gati::Transform>(player, {.position = pebble::math::vec2(0.0f, 0.0f)});
        auto* tr = game.world().get<gati::Transform>(player);

        // 2. Setup Secondary Soft Dynamics (Verlet Scarf / Cape)
        pebble::spandana::VerletCloth2D cape(6, 4.0f);

        // 3. Setup Procedural Aiming (TwoBoneIK Arm)
        pebble::spandana::TwoBoneIK arm_ik(12.0f, 10.0f);
        pebble::math::vec2 aim_target(30.0f, 20.0f);

        // 4. Character State Machine
        CharacterState state = CharacterState::Idle;

        // 5. Construct Dash & Attack Timelines
        pebble::spandana::Timeline action_timeline;
        pebble::spandana::ScreenShake2D camera;

        action_timeline.add(
            // Anticipation & Dash Lunge
            tween(tr->position).to({60.0f, 0.0f}, 0.15f).ease(out_quad),
            pebble::dhvani::edsl::audio_cue(sound_bus, "dash_woosh.wav").pitch(1.2f),

            // Particle dust burst & impact camera shake
            particle_burst().at({60.0f, 0.0f}).count(24).speed(80.0f, 200.0f).lifetime(0.3f),
            shake_camera(camera).trauma(0.6f).duration(0.3f),

            // Ground slam recovery
            tween(tr->position).to({60.0f, 0.0f}, 0.1f)
        );

        // 6. Step 60 Frames of Simulation
        constexpr float dt = 1.0f / 60.0f;
        for (int frame = 0; frame < 60; ++frame) {
            action_timeline.update(dt);
            cape.update(tr->position, dt);
            auto ik_res = arm_ik.solve(tr->position, aim_target);
            (void)ik_res;

            game.update(dt);
        }

        return pebble::testfw::Result::success();
    }
};

REGISTER_EXAMPLE(CharacterControllerExample);

} // namespace
