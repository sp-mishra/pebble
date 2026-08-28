#include <catch_amalgamated.hpp>
#include "containers/spatial/barnes_hut.hpp"
#include <vector>
#include <cmath>

using namespace containers::spatial;

TEST_CASE("BarnesHut: Gravitational Field Solver & Multipole Evaluation", "[barnes_hut][gravity][nbody]") {
    BarnesHutTree tree;

    SECTION("2-Body Newtonian Interaction") {
        std::vector<BarnesHutBody> bodies = {
            {pebble::math::vec2{-100.0f, 0.0f}, pebble::math::vec2{0.0f, 0.0f}, 1000.0f, 0},
            {pebble::math::vec2{ 100.0f, 0.0f}, pebble::math::vec2{0.0f, 0.0f}, 1000.0f, 1}
        };

        tree.build(bodies);

        DefaultGravityPolicy policy;
        policy.G = 1.0f;
        policy.softening = 0.0f; // Pure point mass for analytical check
        policy.theta = 0.0f;     // Exact tree leaf evaluation

        const pebble::math::vec2 f0 = tree.compute_force(bodies[0].pos, bodies[0].mass, 0, bodies, policy);
        const pebble::math::vec2 f1 = tree.compute_force(bodies[1].pos, bodies[1].mass, 1, bodies, policy);

        // F = G * m1 * m2 / r^2 = 1.0 * 1000 * 1000 / (200)^2 = 1,000,000 / 40,000 = 25.0
        REQUIRE(f0[0] == Catch::Approx(25.0f).margin(0.1f));
        REQUIRE(f0[1] == Catch::Approx(0.0f).margin(0.01f));

        // Equal and opposite (Newton's third law)
        REQUIRE(f1[0] == Catch::Approx(-25.0f).margin(0.1f));
        REQUIRE(f1[1] == Catch::Approx(0.0f).margin(0.01f));
    }

    SECTION("Multipole MAC Approximation Accuracy") {
        // Cluster of bodies far away vs single probe body
        std::vector<BarnesHutBody> bodies = {
            {pebble::math::vec2{0.0f, 0.0f}, pebble::math::vec2{0.0f, 0.0f}, 100.0f, 0},
            // Cluster centered around (1000, 1000)
            {pebble::math::vec2{995.0f, 995.0f}, pebble::math::vec2{0.0f, 0.0f}, 500.0f, 1},
            {pebble::math::vec2{1005.0f, 1005.0f}, pebble::math::vec2{0.0f, 0.0f}, 500.0f, 2}
        };

        tree.build(bodies);

        DefaultGravityPolicy policy;
        policy.G = 1.0f;
        policy.softening = 1.0f;
        policy.theta = 0.5f; // Standard Barnes-Hut opening criterion

        const pebble::math::vec2 f_bh = tree.compute_force(bodies[0].pos, bodies[0].mass, 0, bodies, policy);

        // Direct analytical distance to center of mass (1000, 1000)
        // r = sqrt(1000^2 + 1000^2) = 1414.21356, total cluster mass = 1000
        // F = G * 100 * 1000 / (2 * 10^6) = 100,000 / 2,000,000 = 0.05
        const float expected_fx = 0.05f * (1000.0f / 1414.21356f); // ~0.03535
        const float expected_fy = 0.05f * (1000.0f / 1414.21356f);

        REQUIRE(f_bh[0] == Catch::Approx(expected_fx).margin(0.005f));
        REQUIRE(f_bh[1] == Catch::Approx(expected_fy).margin(0.005f));
    }

    SECTION("Batch Force Calculation Interface") {
        std::vector<BarnesHutBody> bodies = {
            {pebble::math::vec2{-50.0f, 0.0f}, pebble::math::vec2{0.0f, 0.0f}, 100.0f, 0},
            {pebble::math::vec2{ 50.0f, 0.0f}, pebble::math::vec2{0.0f, 0.0f}, 100.0f, 1},
            {pebble::math::vec2{  0.0f, 50.0f}, pebble::math::vec2{0.0f, 0.0f}, 100.0f, 2}
        };

        tree.build(bodies);

        std::vector<pebble::math::vec2> forces(bodies.size());
        compute_all_forces(tree, bodies, forces);

        REQUIRE(forces.size() == 3);
        // Net force symmetry check
        const pebble::math::vec2 f_total = forces[0] + forces[1] + forces[2];
        REQUIRE(f_total[0] == Catch::Approx(0.0f).margin(0.1f));
        REQUIRE(f_total[1] == Catch::Approx(0.0f).margin(0.1f));
    }
}
