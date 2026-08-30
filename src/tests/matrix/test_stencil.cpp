#include <catch_amalgamated.hpp>
#include <containers/matrix/ganita.hpp>
#include <cmath>
#include <numbers>

using namespace ga;

// ============================================================================
// test_stencil.cpp — FD operators + WENO + boundary conditions
// ============================================================================

TEST_CASE("Grid2D: construction and element access", "[stencil][grid2d]") {
    Grid2D<float> g(4, 5, 0.1f);
    REQUIRE(g.rows() == 4);
    REQUIRE(g.cols() == 5);
    CHECK(g.h == Catch::Approx(0.1f));
    g.at(1, 2) = 3.14f;
    CHECK(g.at(1, 2) == Catch::Approx(3.14f));
    const auto& cg = g;
    CHECK(cg.at(1, 2) == Catch::Approx(3.14f));
}

TEST_CASE("laplacian: zero field → zero laplacian", "[stencil][laplacian]") {
    Grid2D<float> g(4, 4, 1.0f);
    for (std::size_t i=0;i<4;++i) for (std::size_t j=0;j<4;++j) g.at(i,j)=0.f;
    auto lap = laplacian(g);  // default BC = NeumannBC
    for (std::size_t i=0;i<4;++i)
        for (std::size_t j=0;j<4;++j)
            CHECK(lap.at(i,j) == Catch::Approx(0.f).margin(1e-6f));
}

TEST_CASE("laplacian: quadratic field f=x^2+y^2 → Δf = 4/h^2 * h^2 = 4 (2nd order)", "[stencil][laplacian]") {
    // f(i,j) = (i*h)^2 + (j*h)^2, Δf = 4.0 everywhere
    constexpr std::size_t N = 6;
    constexpr float h = 1.0f;
    Grid2D<float> g(N, N, h);
    for (std::size_t i=0;i<N;++i)
        for (std::size_t j=0;j<N;++j)
            g.at(i,j) = float(i*i) + float(j*j);
    auto lap = laplacian(g);  // NeumannBC default
    // Interior nodes should have Δf = 4.0 (discrete 5-point stencil with h=1)
    for (std::size_t i=1;i<N-1;++i)
        for (std::size_t j=1;j<N-1;++j)
            CHECK(lap.at(i,j) == Catch::Approx(4.0f).epsilon(0.01f));
}

TEST_CASE("gradient: constant field → zero gradient", "[stencil][gradient]") {
    Grid2D<float> g(4, 4, 1.0f);
    for (std::size_t i=0;i<4;++i) for (std::size_t j=0;j<4;++j) g.at(i,j)=5.f;
    auto [gx, gy] = gradient(g);
    for (std::size_t i=1;i<3;++i)
        for (std::size_t j=1;j<3;++j) {
            CHECK(gx.at(i,j) == Catch::Approx(0.f).margin(1e-6f));
            CHECK(gy.at(i,j) == Catch::Approx(0.f).margin(1e-6f));
        }
}

TEST_CASE("divergence: zero velocity field → zero divergence", "[stencil][divergence]") {
    Grid2D<float> vx(4, 4, 1.0f), vy(4, 4, 1.0f);
    for (std::size_t i=0;i<4;++i) for (std::size_t j=0;j<4;++j) { vx.at(i,j)=0.f; vy.at(i,j)=0.f; }
    auto div = divergence(vx, vy);
    for (std::size_t i=1;i<3;++i)
        for (std::size_t j=1;j<3;++j)
            CHECK(div.at(i,j) == Catch::Approx(0.f).margin(1e-6f));
}

TEST_CASE("conv2d: identity kernel", "[stencil][conv2d]") {
    Grid2D<float> g(4, 4, 1.0f);
    for (std::size_t i=0;i<4;++i) for (std::size_t j=0;j<4;++j) g.at(i,j)=float(i*4+j+1);
    std::vector<std::vector<float>> k = {{0,0,0},{0,1,0},{0,0,0}};
    auto out = conv2d(g, k);
    for (std::size_t i=1;i<3;++i)
        for (std::size_t j=1;j<3;++j)
            CHECK(out.at(i,j) == Catch::Approx(g.at(i,j)).margin(1e-5f));
}

TEST_CASE("jacobi_diffuse: mass conservation", "[stencil][diffuse]") {
    // Total mass should be approximately conserved after diffusion
    Grid2D<float> u(5, 5, 1.0f);
    for (std::size_t i=0;i<5;++i) for (std::size_t j=0;j<5;++j) u.at(i,j)=0.f;
    u.at(2,2) = 25.f;  // delta-like source
    float mass_before = 0.f;
    for (std::size_t i=0;i<5;++i) for (std::size_t j=0;j<5;++j) mass_before += u.at(i,j);
    auto ud = jacobi_diffuse(u, 0.1f, 0.01f);
    float mass_after = 0.f;
    for (std::size_t i=0;i<5;++i) for (std::size_t j=0;j<5;++j) mass_after += ud.at(i,j);
    // Neumann BCs conserve mass
    CHECK(mass_after == Catch::Approx(mass_before).epsilon(0.01f));
}

TEST_CASE("advect_semilagrangian: shape preservation (zero velocity)", "[stencil][advect]") {
    Grid2D<float> f(5, 5, 1.0f), vx(5, 5, 1.0f), vy(5, 5, 1.0f);
    for (std::size_t i=0;i<5;++i) for (std::size_t j=0;j<5;++j) {
        f.at(i,j)  = float(i+j);
        vx.at(i,j) = 0.f;
        vy.at(i,j) = 0.f;
    }
    auto fa = advect_semilagrangian(f, vx, vy, 0.01f);
    for (std::size_t i=0;i<5;++i)
        for (std::size_t j=0;j<5;++j)
            CHECK(fa.at(i,j) == Catch::Approx(f.at(i,j)).epsilon(0.05f));
}

TEST_CASE("weno3: zero velocity → no change", "[stencil][weno3]") {
    Grid2D<float> f(6, 6, 1.0f), vx(6, 6, 1.0f), vy(6, 6, 1.0f);
    for (std::size_t i=0;i<6;++i) for (std::size_t j=0;j<6;++j) {
        f.at(i,j)  = float(i+j);
        vx.at(i,j) = 0.f;
        vy.at(i,j) = 0.f;
    }
    auto fw = weno3<float, ts::DefaultStoragePolicy, ts::DefaultComputationPolicy, WrapBC>(f, vx, vy, 0.01f);
    for (std::size_t i=0;i<6;++i)
        for (std::size_t j=0;j<6;++j)
            CHECK(fw.at(i,j) == Catch::Approx(f.at(i,j)).epsilon(0.01f));
}
