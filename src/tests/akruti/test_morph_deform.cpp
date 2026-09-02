#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include "akruti/morph.hpp"
#include "akruti/deform.hpp"

TEST_CASE (
"Akruti: Continuous SDF Shape Morphing"
,
"[akruti][morph]"
)
 {
    akruti::Circle circle{{0.0f, 0.0f}, 10.0f};
    akruti::Box box{{0.0f, 0.0f}, {10.0f, 10.0f}};

    auto morpher = akruti::morph(circle, box, 0.0f);

    // At t=0, behaves like circle
    REQUIRE(morpher.sdf({0.0f, 0.0f}) == -10.0f);

    // At t=1, behaves like box
    morpher.t = 1.0f;
    REQUIRE(morpher.sdf({0.0f, 0.0f}) == -10.0f);

    // At t=0.5, interpolates distance fields
    morpher.t = 0.5f;
    float d = morpher.sdf({15.0f, 0.0f});
    REQUIRE(d > 0.0f);
}

TEST_CASE (
"Akruti: Geometric Space Deformations (Bend, Taper, SquashStretch)"
,
"[akruti][deform]"
)
 {
    akruti::Box box{{0.0f, 0.0f}, {10.0f, 10.0f}};

    // 1. Bent Shape
    auto bent = akruti::deform::bend(box, 0.01f);
    REQUIRE(bent.sdf({0.0f, 0.0f}) == -10.0f);

    // 2. Tapered Shape
    auto tapered = akruti::deform::taper(box, 0.1f);
    REQUIRE(tapered.sdf({0.0f, 0.0f}) == -10.0f);

    // 3. Volume-Preserving Squash and Stretch
    auto squashed = akruti::deform::squash_stretch(box, 2.0f); // 2x wide, 0.5x tall
    auto aabb = squashed.aabb();
    REQUIRE(aabb.hi[0] == 20.0f);
    REQUIRE(aabb.hi[1] == 5.0f);
}
