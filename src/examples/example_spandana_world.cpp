#include "test/example_registry.hpp"
#include "spandana/spandana.hpp"
#include "akruti/akruti.hpp"
#include "akruti/spline.hpp"
#include "gati/gati.hpp"
#include <iostream>

namespace {

using namespace pebble::spandana::edsl;
using namespace pebble::spandana::ease;

class SpandanaWorldExample : public pebble::testfw::Example {
public:
    static constexpr std::string_view name() {
        return "spandana_world_demo";
    }

    static constexpr std::string_view description() {
        return "Showcase of Spandana Universal World EDSL: Splines, CSG shapes, springs, camera shake, and physics impulses";
    }

    static constexpr std::array<std::string_view, 3> tag_data{"spandana", "gati", "animation"};
    static constexpr std::span<const std::string_view> tags() {
        return tag_data;
    }

    pebble::testfw::Result run() override {
        // Setup Gati Game Runtime
        gati::Game game{gati::ClockConfig{.hz = 60.0f}};
        auto player = game.world().spawn();
        game.world().add<gati::Transform>(player, {.position = pebble::math::vec2(0.0f, 0.0f)});

        auto* tr = game.world().get<gati::Transform>(player);
        pebble::spandana::ScreenShake2D camera;

        // Construct an Akruti Cubic Bézier trajectory
        akruti::CubicBezierCurve spline{
            .p0 = {0.0f, 0.0f},
            .p1 = {20.0f, 80.0f},
            .p2 = {80.0f, 80.0f},
            .p3 = {100.0f, 0.0f},
            .radius = 2.0f
        };

        // Construct Declarative Spandana Timeline
        pebble::spandana::Timeline timeline;
        float player_rotation = 0.0f;
        pebble::spandana::VerletCloth2D cape(6, 4.0f);

        timeline.add(
            // 1. Follow Bézier Spline trajectory with tangent orientation
            follow_path(tr->position, spline).duration(1.0f).orient_to_tangent(player_rotation).ease(in_out_quad),

            // 2. Camera shake and radial blast on impact at the end of the path
            shake_camera(camera).trauma(0.8f).duration(0.4f),
            radial_impulse().at(spline.p3).radius(80.0f).magnitude(500.0f),
            particle_burst().at(spline.p3).count(32).speed(100.0f, 250.0f).lifetime(0.4f),

            // 3. Return player position back with an elastic bounce
            tween(tr->position).to({0.0f, 0.0f}, 0.5f).ease(out_elastic)
        );

        // Step simulation for 2.0 seconds
        constexpr float dt = 1.0f / 60.0f;
        for (int frame = 0; frame < 120; ++frame) {
            timeline.update(dt);
            cape.update(tr->position, dt);
            game.update(dt);
        }

        return pebble::testfw::Result::success();
    }
};

REGISTER_EXAMPLE(SpandanaWorldExample);

} // namespace
