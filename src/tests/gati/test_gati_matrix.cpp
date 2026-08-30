#include <catch_amalgamated.hpp>
#include <containers/matrix/static.hpp>
#include <gati/math.hpp>
#include <cmath>

// ============================================================================
// test_gati_matrix.cpp — gati::Mat2 = ga::StaticMatrix<float,2,2> correctness
// ============================================================================

using namespace Catch::Matchers;

TEST_CASE("gati::Mat2 aliases ga::StaticMatrix<float,2,2>", "[gati][matrix]") {
    static_assert(std::is_same_v<gati::Mat2, ga::StaticMatrix<float, 2, 2>>);
}

TEST_CASE("ga::quad_form_2d matches hand-computed effective mass", "[gati][matrix][physics]") {
    // Two identical bodies: inv_mass=1, inv_inertia=1, r=(1,0), n=(0,1)
    // cross2d(r,n) = r.x*n.y - r.y*n.x = 1*1 - 0*0 = 1
    // quad_form = inv_mass + 1^2 * inv_inertia = 1 + 1 = 2
    const ga::Vec2<float> r{1.0f, 0.0f};
    const ga::Vec2<float> n{0.0f, 1.0f};
    const float qf = ga::quad_form_2d(1.0f, 1.0f, r, n);
    REQUIRE_THAT(qf, WithinAbs(2.0f, 1e-6f));

    // Two-body effective normal mass: both identical bodies
    const float kNormal = ga::quad_form_2d(1.0f, 1.0f, r, n)
                        + ga::quad_form_2d(1.0f, 1.0f, r, n);
    REQUIRE_THAT(kNormal, WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(1.0f / kNormal, WithinAbs(0.25f, 1e-6f));
}

TEST_CASE("ga::quad_form_2d static body (inv_mass=0, inv_inertia=0)", "[gati][matrix][physics]") {
    const ga::Vec2<float> r{0.5f, 0.5f};
    const ga::Vec2<float> n{1.0f, 0.0f};
    REQUIRE_THAT(ga::quad_form_2d(0.0f, 0.0f, r, n), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("ga::axpy position correction two-body", "[gati][matrix][physics]") {
    // body A at (0,0), body B at (1,0), normal=(1,0), correction_mag=0.1, percent=0.4
    const float correction_mag = 0.1f * 0.4f; // = 0.04
    const ga::Vec2<float> corr{1.0f * correction_mag, 0.0f};
    ga::Vec2<float> pos_a{0.0f, 0.0f};
    ga::Vec2<float> pos_b{1.0f, 0.0f};
    ga::axpy(-1.0f, corr, pos_a); // bA inv_mass=1, subtract
    ga::axpy( 1.0f, corr, pos_b); // bB inv_mass=1, add
    REQUIRE_THAT(pos_a(0,0), WithinAbs(-0.04f, 1e-6f));
    REQUIRE_THAT(pos_b(0,0), WithinAbs( 1.04f, 1e-6f));
}

TEST_CASE("gati::Mat2 identity and matrix multiply", "[gati][matrix]") {
    const auto I = gati::Mat2::identity();
    const gati::Mat2 A{1.0f, 2.0f, 3.0f, 4.0f};
    const gati::Mat2 result = I * A;
    REQUIRE_THAT(result(0,0), WithinAbs(A(0,0), 1e-6f));
    REQUIRE_THAT(result(0,1), WithinAbs(A(0,1), 1e-6f));
    REQUIRE_THAT(result(1,0), WithinAbs(A(1,0), 1e-6f));
    REQUIRE_THAT(result(1,1), WithinAbs(A(1,1), 1e-6f));
}
