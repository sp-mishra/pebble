#include "catch_amalgamated.hpp"
#include "gati/rigid_body.hpp"
#include "akruti/primitives.hpp"

TEST_CASE("Gati: RigidBody", "[gati][rigid_body]") {
    akruti::Circle c{{0, 0}, 1.0f};
    gati::RigidBody body(c, {.position = {0, 10}, .velocity = {0, -5}, .mass = 2.0f});

    REQUIRE(!body.is_static());
    REQUIRE(body.mass == 2.0f);
    REQUIRE(body.inv_mass == 0.5f);

    body.step(0.1f, {0.0f, -10.0f});
    REQUIRE(body.velocity[1] < -5.0f);
}
