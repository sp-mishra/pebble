// ============================================================================
// src/tests/containers/test_prakriti_joint.cpp — akruti joint frames driven by Prakriti's XPBD JointSolver,
// plus damage-island tracking (union_find connected components over active edges).
// ============================================================================
#include "catch_amalgamated.hpp"
#include "prakriti/prakriti.hpp"
#include "akruti/akruti.hpp"
#include "akruti/joint.hpp"
#include <containers/numeric/math_vector.hpp>
#include <cmath>
#include <vector>

using namespace prakriti;

TEST_CASE (
"joint: rod holds rest length under gravity"
,
"[prakriti][joint][rod]"
)
 {
    std::vector<akruti::Joint> joints;
    auto stack = SolverStack<JointSolver>{
        std::make_tuple(JointSolver{joints, JointConfig{.default_compliance=0.0f, .iterations=8}})};
    WorldConfig cfg; cfg.bounds = {{-100.0f, -100.0f}, {100.0f, 100.0f}};
    World<DefaultMaterialLaw, ScalarBackend, decltype(stack)> w(cfg, stack);
    auto steel = w.materials().add(MaterialRegistry::steel());
    Index anchor = w.particles().add({.position = {0.0f, 10.0f}, .mass = 0, .material = steel}); // static
    Index bob    = w.particles().add({.position = {0.0f, 8.0f},  .material = steel});
    joints.push_back(akruti::make_distance(anchor, bob, {0.0f, 0.0f}, {0.0f, 0.0f}, 2.0f));
    for (int f = 0; f < 300; ++f) w.step();
    const Scalar dist = std::hypot(w.particles().pos_x[bob] - w.particles().pos_x[anchor],
                                   w.particles().pos_y[bob] - w.particles().pos_y[anchor]);
    REQUIRE(std::fabs(dist - 2.0f) < 0.05f);
}

TEST_CASE (
"damage islands: bridge removal splits components"
,
"[prakriti][damage][islands]"
)
 {
    WorldConfig cfg;
    World<> w(cfg);
    auto steel = w.materials().add(MaterialRegistry::steel());
    for (int i = 0; i < 4; ++i) w.particles().add({.position = {float(i), 0.0f}, .material = steel});
    w.edges().add(0, 1, 1.0f);   // cluster {0,1}
    w.edges().add(2, 3, 1.0f);   // cluster {2,3}
    w.edges().add(1, 2, 1.0f);   // bridge

    DamageSolver ds; ds.track_islands = true;
    ds.rebuild_islands(w.particles(), w.edges());
    REQUIRE(ds.island_count() == 1);

    w.edges().deactivate(2);     // remove the bridge
    w.edges().compact();
    ds.rebuild_islands(w.particles(), w.edges());
    REQUIRE(ds.island_count() == 2);
    REQUIRE(ds.island_of(0) == ds.island_of(1));
    REQUIRE(ds.island_of(0) != ds.island_of(3));
}
