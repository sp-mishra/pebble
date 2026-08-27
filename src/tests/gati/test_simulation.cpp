#include "catch_amalgamated.hpp"
#include "gati/simulation.hpp"
#include "akruti/primitives.hpp"

TEST_CASE("Gati: Unified Simulation Facade", "[gati][simulation]") {
    gati::Simulation sim;

    akruti::Circle c{{0, 0}, 1.0f};
    auto h1 = sim.add_body(c, {.position = {0, 5}, .velocity = {0, 0}, .mass = 1.0f});
    auto h2 = sim.add_body(c, {.position = {0, 0}, .velocity = {0, 0}, .mass = 0.0f}); // static ground

    REQUIRE(sim.body_count() == 2);
    REQUIRE(sim.get_body(h2).is_static());

    for (int i = 0; i < 60; ++i) {
        sim.step(0.016f);
    }

    REQUIRE(sim.get_body(h1).position[1] <= 5.0f);
}
