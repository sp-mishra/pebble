#include "catch_amalgamated.hpp"
#include "gati/island_solver.hpp"
#include "akruti/primitives.hpp"

TEST_CASE("Gati: SequentialImpulseSolver", "[gati][island_solver]") {
    akruti::Circle c{{0, 0}, 1.0f};
    std::vector<gati::RigidBody> bodies;
    bodies.emplace_back(c, gati::RigidBodyDesc{.position = {0, 0}, .mass = 1.0f});
    bodies.emplace_back(c, gati::RigidBodyDesc{.position = {0, 1.8f}, .mass = 1.0f});

    std::vector<gati::ContactConstraint> contacts;
    gati::ContactConstraint cc;
    cc.body_a = 0;
    cc.body_b = 1;
    cc.normal = {0, 1};
    cc.penetration = 0.2f;
    cc.friction = 0.3f;
    cc.restitution = 0.0f;
    contacts.push_back(cc);

    gati::SequentialImpulseSolver solver;
    gati::SolverContext ctx{bodies, contacts, 0.016f, 10, 10};

    solver.solve_velocities(ctx);
    solver.solve_positions(ctx);

    REQUIRE(contacts[0].normal_impulse_accum >= 0.0f);
}
