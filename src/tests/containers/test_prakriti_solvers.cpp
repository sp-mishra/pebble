// ============================================================================
// src/tests/containers/test_prakriti_solvers.cpp — XPBD constraint convergence + damage/fracture.
// ============================================================================
#include "catch_amalgamated.hpp"
#include "prakriti/prakriti.hpp"
#include <containers/numeric/math_vector.hpp>

using namespace prakriti;

// Build a minimal solver context around two particles + one edge.
namespace {
    struct Rig {
        WorldConfig cfg;
        ParticleStore P;
        EdgeStore E;
        MaterialRegistry M;
        SpatialHash grid{1.0f};
        DefaultMaterialLaw law;

        MaterialId mat = M.add(MaterialRegistry::steel());

        Index add(pebble::math::vec2 pos, Scalar mass) {
            return P.add({.position = pos, .mass = mass, .material = mat});
        }

        SolverContext<DefaultMaterialLaw> ctx(Scalar dt) {
            return SolverContext<DefaultMaterialLaw>{P, E, M, grid, law, cfg, dt};
        }
    };
}

TEST_CASE (
"XPBD distance constraint pulls stretched edge toward rest length"
,
"[prakriti][xpbd]"
)
 {
    Rig r;
    Index a = r.add({0.0f, 0.0f}, 0);   // static anchor
    Index b = r.add({2.0f, 0.0f}, 1);   // free, stretched (rest = 1)
    r.E.add(a, b, 1.0f);
    r.P.set_pred(a, r.P.pos_v(a));
    r.P.set_pred(b, r.P.pos_v(b));

    XpbdSolver xpbd;
    const Scalar dt = 1.0f / 480.0f; // small substep => stiff
    auto ctx = r.ctx(dt);
    for (int it = 0; it < 40; ++it) { xpbd.solve(ctx); }

    const Scalar len = pebble::math::distance(r.P.pred_v(a), r.P.pred_v(b));
    REQUIRE(len < 2.0f);              // moved inward
    REQUIRE(len == Catch::Approx(1.0f).margin(0.2f)); // near rest length
}

TEST_CASE (
"damage accumulates and fractures over-stretched edge"
,
"[prakriti][damage]"
)
 {
    Rig r;
    Index a = r.add({0.0f, 0.0f}, 0);
    Index b = r.add({3.0f, 0.0f}, 1); // ε = (3-1)/1 = 2 >> ultimate_strain
    r.E.add(a, b, 1.0f);
    r.P.set_pred(a, r.P.pos_v(a));
    r.P.set_pred(b, r.P.pos_v(b));

    DamageSolver dmg;
    auto ctx = r.ctx(1.0f / 60.0f);
    dmg.solve(ctx);
    // Edge should be fractured (compacted out) after huge strain.
    REQUIRE(r.E.size() == 0);
}

TEST_CASE (
"damage does nothing below yield"
,
"[prakriti][damage]"
)
 {
    Rig r;
    Index a = r.add({0.0f, 0.0f}, 0);
    Index b = r.add({1.0001f, 0.0f}, 1); // tiny strain, below yield
    r.E.add(a, b, 1.0f);
    r.P.set_pred(a, r.P.pos_v(a));
    r.P.set_pred(b, r.P.pos_v(b));

    DamageSolver dmg;
    auto ctx = r.ctx(1.0f / 60.0f);
    dmg.solve(ctx);
    REQUIRE(r.E.size() == 1);
    REQUIRE(r.E.damage[0] == Catch::Approx(0));
}
