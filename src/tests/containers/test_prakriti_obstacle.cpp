// ============================================================================
// src/tests/containers/test_prakriti_obstacle.cpp — akruti↔Prakriti obstacle contact: settle, restitution, friction.
// ============================================================================
#include "catch_amalgamated.hpp"
#include "prakriti/prakriti.hpp"
#include "akruti/akruti.hpp"
#include <containers/numeric/math_vector.hpp>
#include <cmath>

using namespace prakriti;

// Floor half-plane: solid below y=0, normal +y.
struct FloorSet {
    akruti::HalfPlane floor{akruti::Vec2<akruti::Scalar>{0.0f, 1.0f}, akruti::Vec2<akruti::Scalar>{0.0f, 0.0f}};
    template <class Fn> void for_each_shape(Fn&& fn) const { fn(floor); }
};

TEST_CASE("obstacle: particle settles on floor", "[prakriti][obstacle]") {
    FloorSet floor;
    auto stack = SolverStack<XpbdSolver, ObstacleSolver<FloorSet>>{
        std::make_tuple(XpbdSolver{}, ObstacleSolver<FloorSet>{floor, ObstacleConfig{.friction=0.5f, .restitution=0.0f}})};
    WorldConfig cfg; cfg.bounds = {{-100.0f, -100.0f}, {100.0f, 100.0f}};
    World<DefaultMaterialLaw, ScalarBackend, decltype(stack)> w(cfg, stack);
    auto steel = w.materials().add(MaterialRegistry::steel());
    w.particles().add({.position = {0.0f, 5.0f}, .material = steel});
    for (int f = 0; f < 240; ++f) w.step();
    REQUIRE(std::fabs(w.particles().pos_y[0]) < 0.05f);
}

TEST_CASE("obstacle: restitution rebounds upward", "[prakriti][obstacle][restitution]") {
    FloorSet floor;
    auto stack = SolverStack<XpbdSolver, ObstacleSolver<FloorSet>>{
        std::make_tuple(XpbdSolver{}, ObstacleSolver<FloorSet>{floor, ObstacleConfig{.friction=0.0f, .restitution=0.8f}})};
    WorldConfig cfg; cfg.bounds = {{-100.0f, -100.0f}, {100.0f, 100.0f}};
    World<DefaultMaterialLaw, ScalarBackend, decltype(stack)> w(cfg, stack);
    auto steel = w.materials().add(MaterialRegistry::steel());
    w.particles().add({.position = {0.0f, 3.0f}, .velocity = {0.0f, 0.0f}, .material = steel});
    bool impacted = false; Scalar apex = -1e9f;
    for (int f = 0; f < 400; ++f) {
        w.step();
        const Scalar y = w.particles().pos_y[0];
        if (!impacted && y < 0.02f) impacted = true;
        if (impacted) apex = std::max(apex, y);
    }
    // XPBD substep damping bleeds energy, so we require a meaningful bounce, not ideal e^2 h.
    REQUIRE(apex > 0.15f);
}

// Dynamic Body obstacle set
struct DynamicObstacleSet {
    akruti::DynamicBody<akruti::Circle> body{akruti::Circle{{0.0f, 0.0f}, 2.0f}, 10.0f, 20.0f};
    template <class Fn> void for_each_shape(Fn&& fn) const { fn(body); }
};

TEST_CASE("obstacle: particle deflects off dynamic Akruti body", "[prakriti][obstacle][dynamic_body]") {
    DynamicObstacleSet dyn_obs;
    dyn_obs.body.position = {0.0f, 0.0f};

    auto stack = SolverStack<XpbdSolver, ObstacleSolver<DynamicObstacleSet>>{
        std::make_tuple(XpbdSolver{}, ObstacleSolver<DynamicObstacleSet>{dyn_obs, ObstacleConfig{.friction=0.0f, .restitution=0.5f, .contact_offset=0.1f}})};
    WorldConfig cfg; cfg.bounds = {{-100.0f, -100.0f}, {100.0f, 100.0f}};
    World<DefaultMaterialLaw, ScalarBackend, decltype(stack)> w(cfg, stack);
    auto steel = w.materials().add(MaterialRegistry::steel());

    // Particle falling straight toward the dynamic body center
    w.particles().add({.position = {0.0f, 5.0f}, .velocity = {0.0f, -10.0f}, .material = steel});

    for (int f = 0; f < 30; ++f) w.step();

    // Particle must be kept outside the dynamic body circle boundary (radius 2.0)
    pebble::math::vec2 p_pos = w.particles().pos_v(0);
    Scalar d = dyn_obs.body.sdf(p_pos);
    REQUIRE(d >= -0.05f);
}
