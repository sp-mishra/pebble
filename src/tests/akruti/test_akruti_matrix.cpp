#include <catch_amalgamated.hpp>
#include <containers/matrix/static.hpp>
#include <akruti/math.hpp>
#include <cmath>

// ============================================================================
// test_akruti_matrix.cpp — ga::StaticMatrix<float,2,2> correctness as akruti::Mat2
// ============================================================================

using namespace Catch::Matchers;

TEST_CASE("akruti::Mat2 aliases ga::StaticMatrix<float,2,2>", "[akruti][matrix]") {
    static_assert(std::is_same_v<akruti::Mat2<float>, ga::StaticMatrix<float, 2, 2>>);
    static_assert(std::is_same_v<akruti::Mat2<double>, ga::StaticMatrix<double, 2, 2>>);
}

TEST_CASE("akruti::Mat2 element access via (r,c)", "[akruti][matrix]") {
    const akruti::Mat2<float> m{1.0f, 2.0f, 3.0f, 4.0f};
    REQUIRE(m(0,0) == 1.0f);
    REQUIRE(m(0,1) == 2.0f);
    REQUIRE(m(1,0) == 3.0f);
    REQUIRE(m(1,1) == 4.0f);
}

TEST_CASE("akruti::make_rotation2d correctness", "[akruti][matrix]") {
    const float angle = std::numbers::pi_v<float> / 4.0f; // 45°
    const auto R = akruti::make_rotation2d(angle);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    REQUIRE_THAT(R(0,0), WithinAbs(c, 1e-5f));
    REQUIRE_THAT(R(0,1), WithinAbs(-s, 1e-5f));
    REQUIRE_THAT(R(1,0), WithinAbs(s, 1e-5f));
    REQUIRE_THAT(R(1,1), WithinAbs(c, 1e-5f));
}

TEST_CASE("akruti::Mat2 * Vec2 bridge operator", "[akruti][matrix]") {
    const auto R = akruti::make_rotation2d(std::numbers::pi_v<float> / 2.0f); // 90°: (1,0) → (0,1)
    const akruti::Vec2<float> v{1.0f, 0.0f};
    const akruti::Vec2<float> result = R * v;
    REQUIRE_THAT(result.x, WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(result.y, WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("akruti::Mat2 identity and inverse round-trip", "[akruti][matrix]") {
    const float angle = 0.7f;
    const auto R = akruti::make_rotation2d(angle);
    const auto Rinv = R.inv();
    const auto I = R * Rinv;
    REQUIRE_THAT(I(0,0), WithinAbs(1.0f, 1e-5f));
    REQUIRE_THAT(I(0,1), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(I(1,0), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(I(1,1), WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("akruti::Mat2 near-singular determinant", "[akruti][matrix]") {
    // Near-singular: columns nearly parallel
    const akruti::Mat2<float> m{1.0f, 1.0001f, 0.0f, 0.0001f};
    const float det = m(0,0) * m(1,1) - m(0,1) * m(1,0);
    REQUIRE(std::abs(det) < 1e-3f);
}

TEST_CASE("akruti::Mat2 is zero-heap (stack-only)", "[akruti][matrix]") {
    // StaticMatrix is backed by std::array — stack allocation only
    static_assert(sizeof(akruti::Mat2<float>) == 4 * sizeof(float));
    static_assert(sizeof(akruti::Mat2<double>) == 4 * sizeof(double));
}
