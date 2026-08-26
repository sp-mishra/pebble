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
