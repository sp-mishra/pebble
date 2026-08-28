#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include "akruti/narrowphase.hpp"
#include <cmath>

TEST_CASE("Akruti: Capsule vs Capsule 2-Point Contact Manifold", "[akruti][narrowphase]") {
    // Two horizontal parallel capsules stacked vertically
    akruti::Capsule cap_a{
        .a = {-5.0f, 0.0f},
        .b = {5.0f, 0.0f},
        .radius = 1.0f
    };

    akruti::Capsule cap_b{
        .a = {-5.0f, 1.8f},
        .b = {5.0f, 1.8f},
        .radius = 1.0f
    };

    akruti::Manifold m = akruti::collide_capsule_capsule(cap_a, cap_b);
    REQUIRE(m.hit == true);
    REQUIRE(m.depth == Catch::Approx(0.2f).margin(1e-3f));
    // Parallel capsules should yield 2 contact points for stable stacking
    REQUIRE(m.points.size() == 2);
}

TEST_CASE("Akruti: Capsule vs OBB Manifold", "[akruti][narrowphase]") {
    akruti::Capsule cap{
        .a = {-2.0f, 1.8f},
        .b = {2.0f, 1.8f},
        .radius = 1.0f
    };

    akruti::OrientedBox box = akruti::OrientedBox::from_angle({0.0f, 0.0f}, {4.0f, 1.0f}, 0.0f);

    akruti::Manifold m = akruti::collide_capsule_obb(cap, box);
    REQUIRE(m.hit == true);
    REQUIRE(m.depth == Catch::Approx(0.2f).margin(1e-3f));
    REQUIRE(m.points.size() >= 1);
}

TEST_CASE("Akruti: Direct Static Narrowphase Matrix & Warm-Started SimplexCache", "[akruti][narrowphase][matrix]") {
    akruti::Circle circle{{0.0f, 0.0f}, 1.0f};
    akruti::Box box{{0.0f, 1.5f}, {1.0f, 1.0f}};

    akruti::SimplexCache cache{};
    REQUIRE_FALSE(cache.valid);

    // Frame 1: Matrix dispatch initial evaluation
    akruti::Manifold m1 = akruti::collide_matrix(
        akruti::ShapeType::Circle, &circle,
        akruti::ShapeType::Box, &box,
        &cache);

    REQUIRE(m1.hit == true);
    REQUIRE(m1.depth == Catch::Approx(0.5f).margin(1e-3f));

    // Frame 2: Continuous evaluation with warm-started cache
    akruti::Manifold m2 = akruti::collide_matrix(
        akruti::ShapeType::Circle, &circle,
        akruti::ShapeType::Box, &box,
        &cache);

    REQUIRE(m2.hit == true);
    REQUIRE(m2.depth == Catch::Approx(0.5f).margin(1e-3f));
}

