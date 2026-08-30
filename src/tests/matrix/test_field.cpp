#include <catch_amalgamated.hpp>
#include <containers/matrix/ganita.hpp>
#include <cmath>

using namespace ga;

// ============================================================================
// test_field.cpp — multi-channel field calculus
// ============================================================================

TEST_CASE("Field: construction and channel access", "[field][channels]") {
    Field<2, float> f(4, 4, 1.0f);
    REQUIRE(f.rows() == 4);
    REQUIRE(f.cols() == 4);
    auto& ch0 = f.channel(0);
    ch0.at(1,1) = 7.f;
    CHECK(f.channel(0).at(1,1) == Catch::Approx(7.f));
    CHECK(f.channel(1).at(1,1) == Catch::Approx(0.f));
}

TEST_CASE("Field: channel(c) out of bounds throws", "[field][channels]") {
    Field<2, float> f(3, 3, 1.0f);
    REQUIRE_THROWS(f.channel(2));
}

TEST_CASE("Field: mass per channel", "[field][mass]") {
    Field<2, float> f(3, 3, 1.0f);
    for (std::size_t i=0;i<3;++i) for (std::size_t j=0;j<3;++j) f.channel(0).at(i,j)=1.f;
    for (std::size_t i=0;i<3;++i) for (std::size_t j=0;j<3;++j) f.channel(1).at(i,j)=2.f;
    CHECK(f.mass(0) == Catch::Approx(9.f).epsilon(1e-5f));
    CHECK(f.mass(1) == Catch::Approx(18.f).epsilon(1e-5f));
}

TEST_CASE("Field: laplacian_ch produces correct size", "[field][laplacian]") {
    Field<2, float> f(4, 4, 1.0f);
    auto lap = f.laplacian_ch<NeumannBC>(0);
    CHECK(lap.rows() == 4);
    CHECK(lap.cols() == 4);
}

TEST_CASE("Field: grad_ch on constant field → zero gradient", "[field][gradient]") {
    Field<2, float> f(4, 4, 1.0f);
    for (std::size_t i=0;i<4;++i) for (std::size_t j=0;j<4;++j) f.channel(0).at(i,j)=5.f;
    auto [gx, gy] = f.grad_ch<NeumannBC>(0);
    for (std::size_t i=1;i<3;++i)
        for (std::size_t j=1;j<3;++j) {
            CHECK(gx.at(i,j) == Catch::Approx(0.f).margin(1e-6f));
            CHECK(gy.at(i,j) == Catch::Approx(0.f).margin(1e-6f));
        }
}

TEST_CASE("Field: advect with zero velocity → no change", "[field][advect]") {
    Field<3, float> f(4, 4, 1.0f);
    // channels 0=vx, 1=vy, 2=density
    for (std::size_t i=0;i<4;++i) for (std::size_t j=0;j<4;++j) {
        f.channel(0).at(i,j)=0.f;
        f.channel(1).at(i,j)=0.f;
        f.channel(2).at(i,j)=float(i+j+1);
    }
    f.advect<NeumannBC>(0, 1, 0.01f);
    for (std::size_t i=0;i<4;++i)
        for (std::size_t j=0;j<4;++j)
            CHECK(f.channel(2).at(i,j) == Catch::Approx(float(i+j+1)).epsilon(0.05f));
}

TEST_CASE("Field: diffuse reduces peak value", "[field][diffuse]") {
    Field<2, float> f(5, 5, 1.0f);
    for (std::size_t i=0;i<5;++i) for (std::size_t j=0;j<5;++j) f.channel(0).at(i,j)=0.f;
    f.channel(0).at(2,2) = 100.f;
    float before = f.channel(0).at(2,2);
    f.diffuse<NeumannBC>(0, 0.1f, 0.01f, 10);
    float after = f.channel(0).at(2,2);
    CHECK(after < before);  // peak should have spread
}

TEST_CASE("FluidField: type alias has 3 channels", "[field][fluid]") {
    FluidField<float> ff(4, 4, 1.0f);
    // vx, vy, pressure = 3 channels
    REQUIRE_NOTHROW(ff.channel(0));
    REQUIRE_NOTHROW(ff.channel(1));
    REQUIRE_NOTHROW(ff.channel(2));
    REQUIRE_THROWS(ff.channel(3));
}

TEST_CASE("PaintField: type alias channels", "[field][paint]") {
    // PaintField<1> = 2+1 = 3 channels
    PaintField<1, float> pf(4, 4, 1.0f);
    REQUIRE_NOTHROW(pf.channel(0));
    REQUIRE_NOTHROW(pf.channel(2));
    REQUIRE_THROWS(pf.channel(3));
}
