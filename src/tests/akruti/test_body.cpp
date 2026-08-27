#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include "akruti/body.hpp"

TEST_CASE("Akruti: DynamicBody 6-DOF Kinematics & SDF Transformation", "[akruti][body]") {
    akruti::Box box{{0.0f, 0.0f}, {10.0f, 10.0f}};
    akruti::DynamicBody body{box, 2.0f, 10.0f};

    body.position = {50.0f, 100.0f};
    body.angle = 0.0f;

    // Center of body in world space should be interior
    REQUIRE(body.sdf({50.0f, 100.0f}) == -10.0f);

    // Outside body
    REQUIRE(body.sdf({70.0f, 100.0f}) == 10.0f);

    // Rotate body 90 degrees (pi/2)
    body.angle = 1.5707963f;
    REQUIRE(std::abs(body.sdf({50.0f, 100.0f}) - (-10.0f)) < 1e-4f);

    // Linear integration under gravity
    body.step(0.1f, {0.0f, 100.0f});
    REQUIRE(body.linear_vel[1] == 10.0f);
    REQUIRE(body.position[1] == 101.0f);

    // Impulse and torque application
    body.apply_impulse({0.0f, -20.0f}, {55.0f, 101.0f}); // offset impulse on x axis
    REQUIRE(body.linear_vel[1] == 0.0f); // canceled velocity
    REQUIRE(body.torque != 0.0f); // angular torque produced
}
