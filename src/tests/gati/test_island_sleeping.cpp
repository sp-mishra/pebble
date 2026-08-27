#include "catch_amalgamated.hpp"
#include "gati/island.hpp"
#include "gati/rigid_body.hpp"
#include "akruti/primitives.hpp"

TEST_CASE("Gati: Island Sleeping", "[gati][island_sleeping]") {
    akruti::Circle c{{0, 0}, 1.0f};
    std::vector<gati::RigidBody> bodies;
    bodies.emplace_back(c, gati::RigidBodyDesc{.position = {0, 0}, .velocity = {0, 0}, .mass = 1.0f});
    bodies.emplace_back(c, gati::RigidBodyDesc{.position = {0, 1.8f}, .velocity = {0, 0}, .mass = 1.0f});

    std::vector<gati::ContactConstraint> contacts;
    gati::ContactConstraint cc;
    cc.body_a = 0;
    cc.body_b = 1;
    contacts.push_back(cc);

    gati::UnionFindIslands island_strategy;
    auto islands = island_strategy.build(contacts, bodies);

    REQUIRE(islands.size() == 1);
    island_strategy.try_sleep(islands, bodies, 0.01f);

    REQUIRE(bodies[0].is_sleeping);
    REQUIRE(bodies[1].is_sleeping);
}
