#include "catch_amalgamated.hpp"
#include "spandana/spandana.hpp"
#include <cmath>

TEST_CASE("Spandana: Easing Functions", "[spandana][easing]") {
    using namespace pebble::spandana::ease;

    REQUIRE(linear(0.0f) == 0.0f);
    REQUIRE(linear(1.0f) == 1.0f);
    REQUIRE(linear(0.5f) == 0.5f);

    REQUIRE(in_quad(0.5f) == 0.25f);
    REQUIRE(out_quad(0.5f) == 0.75f);

    REQUIRE(out_bounce(0.0f) == 0.0f);
    REQUIRE(std::abs(out_bounce(1.0f) - 1.0f) < 1e-4f);
}

TEST_CASE("Spandana: Analytical Spring Damper Stability", "[spandana][spring]") {
    pebble::spandana::AnalyticalSpringDamper spring(180.0f, 12.0f);

    float pos = 0.0f;
    float vel = 0.0f;
    const float target = 100.0f;

    // Step across multiple frames
    for (int i = 0; i < 60; ++i) {
        auto [new_pos, new_vel] = spring.step(pos, vel, target, 1.0f / 60.0f);
        pos = new_pos;
        vel = new_vel;
    }

    // Settles close to target smoothly
    REQUIRE(std::abs(pos - target) < 1.0f);
}

TEST_CASE("Spandana: TwoBoneIK Reach Solver", "[spandana][ik]") {
    pebble::spandana::TwoBoneIK ik(10.0f, 10.0f);

    pebble::math::vec2 root(0.0f, 0.0f);
    pebble::math::vec2 target(10.0f, 10.0f);

    auto result = ik.solve(root, target);
    REQUIRE(result.reachable);
}

TEST_CASE("Spandana: Automatic Dependency & Parallelism Inference Timeline", "[spandana][timeline]") {
    using namespace pebble::spandana::edsl;

    pebble::spandana::Timeline timeline;

    float posX = 0.0f;
    float scale = 1.0f;
    int callback_count = 0;

    pebble::spandana::ResourceKey keyX{1, 1, 0};
    pebble::spandana::ResourceKey keyScale{1, 1, 1};

    // Add parallel actions (disjoint resource keys)
    timeline.add(
        tween(posX, keyX).to(100.0f, 0.2f),
        tween(scale, keyScale).to(2.0f, 0.2f),
        // Add sequential action on same resource (keyX) -> automatically chains after first posX tween!
        tween(posX, keyX).to(0.0f, 0.2f),
        callback([&]() { ++callback_count; })
    );

    REQUIRE(timeline.total_duration() == 0.4f); // 0.2s + 0.2s chained

    // Step by 0.1s (middle of first wave)
    timeline.update(0.1f);
    REQUIRE(posX > 0.0f);
    REQUIRE(scale > 1.0f);

    // Step to 0.2s (end of first wave, start of second)
    timeline.update(0.1f);
    REQUIRE(posX == 100.0f);
    REQUIRE(scale == 2.0f);

    // Step to 0.4s (completion)
    timeline.update(0.2f);
    REQUIRE(posX == 0.0f);
    REQUIRE(timeline.is_finished());
    REQUIRE(callback_count == 1);
}

TEST_CASE("Spandana: Camera Shake Trauma Decay", "[spandana][procedural]") {
    pebble::spandana::ScreenShake2D shake;

    shake.add_trauma(0.8f);
    REQUIRE(shake.trauma() == 0.8f);

    shake.update(0.5f);
    REQUIRE(shake.trauma() < 0.8f);
}

TEST_CASE("Spandana: Spline Path Following and Tangent Orientation", "[spandana][spline]") {
    using namespace pebble::spandana::edsl;

    akruti::CubicBezierCurve bezier{
        .p0 = {0.0f, 0.0f},
        .p1 = {0.0f, 10.0f},
        .p2 = {10.0f, 10.0f},
        .p3 = {10.0f, 0.0f}
    };

    pebble::math::vec2 pos{0.0f, 0.0f};
    float rotation = 0.0f;

    pebble::spandana::Timeline timeline;
    timeline.add(
        follow_path(pos, bezier).duration(1.0f).orient_to_tangent(rotation)
    );

    // Mid-path
    timeline.update(0.5f);
    REQUIRE(pos[0] == 5.0f);
    REQUIRE(pos[1] == 7.5f);

    // End of path
    timeline.update(0.5f);
    REQUIRE(pos[0] == 10.0f);
    REQUIRE(pos[1] == 0.0f);
}

TEST_CASE("Spandana: Particle Burst Emitter", "[spandana][particles]") {
    using namespace pebble::spandana::edsl;

    pebble::spandana::Timeline timeline;
    containers::static_vector<Particle, 32> particle_buffer;
    timeline.add(
        particle_burst(particle_buffer).at({50.0f, 50.0f}).count(16).speed(100.0f, 200.0f).lifetime(0.4f)
    );

    timeline.update(0.2f);
    REQUIRE(timeline.current_time() == 0.2f);
    REQUIRE_FALSE(timeline.is_finished());

    timeline.update(0.2f);
    REQUIRE(timeline.is_finished());
}

TEST_CASE("Spandana: Verlet Secondary Cloth Dynamics", "[spandana][cloth]") {
    pebble::spandana::VerletCloth2D cloth(4, 5.0f);

    pebble::math::vec2 anchor(0.0f, 100.0f);
    cloth.set_anchor(anchor);

    // Step across frames
    for (int i = 0; i < 30; ++i) {
        cloth.update(anchor, 1.0f / 60.0f);
    }

    const auto& particles = cloth.particles();
    REQUIRE(particles.size() == 5);
    REQUIRE(particles[0].pos[0] == 0.0f);
    REQUIRE(particles[0].pos[1] == 100.0f); // Pinned anchor
    REQUIRE(particles[4].pos[1] < 100.0f);  // Draped downward under gravity

    // Convert to Akruti ChainShape for collision/rendering
    auto chain = cloth.to_chain<8>(0.5f);
    REQUIRE(chain.verts.size() == 5);
    REQUIRE(chain.radius == 0.5f);
    REQUIRE_FALSE(chain.is_loop);
}


