#include "catch_amalgamated.hpp"
#include "prakriti/prakriti.hpp"
#include <cmath>

// Appended: coverage for the new prakriti::World::apply_radial_impulse — the matter/physics
// owner API that Spandana's RadialImpulseAction delegates to (Spandana never simulates inline).

using prakriti::World;
using prakriti::Scalar;

TEST_CASE (
"Prakriti: apply_radial_impulse pushes dynamic particles outward"
,
"[prakriti][impulse]"
)
 {
    World<> world;
    auto& P = world.particles();

    // Dynamic particle to the +x of the blast center.
    const auto right = P.add({.position = {10.0f, 0.0f}, .velocity = {0.0f, 0.0f}, .mass = 1.0f});
    // Dynamic particle to the -y.
    const auto down  = P.add({.position = {0.0f, -10.0f}, .velocity = {0.0f, 0.0f}, .mass = 1.0f});

    world.apply_radial_impulse(pebble::math::vec2{0.0f, 0.0f}, /*radius*/ 50.0f, /*mag*/ 100.0f);

    // Velocity must point away from center: +x for `right`, -y for `down`.
    REQUIRE(P.vel_x[right] > 0.0f);
    REQUIRE(std::fabs(P.vel_y[right]) < 1e-4f);
    REQUIRE(P.vel_y[down] < 0.0f);
    REQUIRE(std::fabs(P.vel_x[down]) < 1e-4f);
}

TEST_CASE (
"Prakriti: apply_radial_impulse respects radius cutoff"
,
"[prakriti][impulse]"
)
 {
    World<> world;
    auto& P = world.particles();
    const auto far = P.add({.position = {1000.0f, 0.0f}, .mass = 1.0f});

    world.apply_radial_impulse(pebble::math::vec2{0.0f, 0.0f}, /*radius*/ 50.0f, /*mag*/ 100.0f);

    // Outside radius => untouched.
    REQUIRE(P.vel_x[far] == 0.0f);
    REQUIRE(P.vel_y[far] == 0.0f);
}

TEST_CASE (
"Prakriti: apply_radial_impulse skips static particles"
,
"[prakriti][impulse]"
)
 {
    World<> world;
    auto& P = world.particles();
    const auto stat = P.add({.position = {5.0f, 0.0f}, .mass = 0.0f}); // mass 0 => static

    REQUIRE(P.is_static(stat));
    world.apply_radial_impulse(pebble::math::vec2{0.0f, 0.0f}, 50.0f, 100.0f);

    REQUIRE(P.vel_x[stat] == 0.0f);
    REQUIRE(P.vel_y[stat] == 0.0f);
}

TEST_CASE (
"Prakriti: apply_radial_impulse falloff — nearer gains more speed"
,
"[prakriti][impulse]"
)
 {
    World<> world;
    auto& P = world.particles();
    const auto near = P.add({.position = {5.0f, 0.0f}, .mass = 1.0f});
    const auto far  = P.add({.position = {40.0f, 0.0f}, .mass = 1.0f});

    world.apply_radial_impulse(pebble::math::vec2{0.0f, 0.0f}, 50.0f, 100.0f, /*falloff_pow*/ 1.0f);

    REQUIRE(P.vel_x[near] > P.vel_x[far]); // linear falloff => closer is faster
}
