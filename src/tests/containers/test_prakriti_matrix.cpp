// ============================================================================
// src/tests/containers/test_prakriti_matrix.cpp — ga::StaticMatrix integration in prakriti solvers
// ============================================================================
#include "catch_amalgamated.hpp"
#include <containers/matrix/static.hpp>
#include <prakriti/prakriti.hpp>

using namespace Catch::Matchers;

// ---- ga::nrm2_sq vs. manual squared distance ----

TEST_CASE (
"prakriti: ga::nrm2_sq matches manual squared length"
,
"[prakriti][matrix]"
)
 {
    const ga::Vec2<float> v{3.0f, 4.0f};
    REQUIRE_THAT(ga::nrm2_sq(v), WithinAbs(25.0f, 1e-6f));

    const ga::Vec2<float> zero{0.0f, 0.0f};
    REQUIRE_THAT(ga::nrm2_sq(zero), WithinAbs(0.0f, 1e-6f));
}

// ---- ga::axpy XPBD position-correction pattern ----

TEST_CASE (
"prakriti: ga::axpy matches scalar XPBD correction"
,
"[prakriti][matrix][xpbd]"
)
 {
    // Simulate: two particles, wa=wb=0.5, grad=(1,0), C=0.2, alpha~0, dlambda=-0.1
    const float wa = 0.5f, wb = 0.5f;
    const float dlambda = -0.1f;
    const ga::Vec2<float> corr{1.0f * dlambda, 0.0f}; // grad * dlambda

    ga::Vec2<float> pa{0.0f, 0.0f};
    ga::Vec2<float> pb{1.0f, 0.0f};
    ga::axpy( wa, corr, pa);
    ga::axpy(-wb, corr, pb);

    // pa += wa * corr = (0,0) + 0.5 * (-0.1, 0) = (-0.05, 0)
    REQUIRE_THAT(pa(0,0), WithinAbs(-0.05f, 1e-6f));
    REQUIRE_THAT(pa(1,0), WithinAbs( 0.00f, 1e-6f));
    // pb -= wb * corr = (1,0) - 0.5 * (-0.1, 0) = (1.05, 0)
    REQUIRE_THAT(pb(0,0), WithinAbs( 1.05f, 1e-6f));
    REQUIRE_THAT(pb(1,0), WithinAbs( 0.00f, 1e-6f));
}

// ---- prakriti compute backends are all available in prakriti namespace ----

TEST_CASE (
"prakriti: compute backend types exist in prakriti namespace"
,
"[prakriti][backends]"
)
 {
    static_assert(prakriti::ComputeBackend<prakriti::ScalarBackend>);
    static_assert(prakriti::ComputeBackend<prakriti::HighwayBackend>);
    // ScalarBackend is zero-heap stack type
    static_assert(sizeof(prakriti::ScalarBackend) > 0);
}
