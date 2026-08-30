#include <catch_amalgamated.hpp>
#include <containers/matrix/dual.hpp>
#include <cmath>
#include <numbers>

using namespace ga;

TEST_CASE("Dual: variable construction", "[dual][autodiff]") {
    auto x = Dual<float,1>::variable(3.f, 0);
    CHECK(x.v == 3.f);
    CHECK(x.d[0] == 1.f);
}

TEST_CASE("Dual: constant construction", "[dual][autodiff]") {
    Dual<double,1> c(5.0);
    CHECK(c.v == 5.0);
    CHECK(c.d[0] == 0.0);
}

TEST_CASE("Dual: addition", "[dual][autodiff]") {
    auto x = Dual<float,1>::variable(2.f, 0);
    auto y = Dual<float,1>::variable(3.f, 0);
    auto z = x + y;
    CHECK(z.v == 5.f);
    CHECK(z.d[0] == 2.f);  // d(x+y)/dx = 1+1 when both seeded at 1
}

TEST_CASE("Dual: subtraction", "[dual][autodiff]") {
    auto x = Dual<float,1>::variable(5.f, 0);
    Dual<float,1> c(2.f);
    auto z = x - c;
    CHECK(z.v == 3.f);
    CHECK(z.d[0] == 1.f);
}

TEST_CASE("Dual: multiplication chain rule", "[dual][autodiff]") {
    auto x = Dual<float,1>::variable(3.f, 0);
    auto y = x * x;  // d(x^2)/dx = 2x = 6 at x=3
    CHECK(y.v == Catch::Approx(9.f));
    CHECK(y.d[0] == Catch::Approx(6.f));
}

TEST_CASE("Dual: division", "[dual][autodiff]") {
    auto x = Dual<float,1>::variable(4.f, 0);
    Dual<float,1> c(2.f);
    auto z = x / c;  // d(x/2)/dx = 0.5
    CHECK(z.v == Catch::Approx(2.f));
    CHECK(z.d[0] == Catch::Approx(0.5f));
}

TEST_CASE("Dual: negation", "[dual][autodiff]") {
    auto x = Dual<float,1>::variable(3.f, 0);
    auto z = -x;
    CHECK(z.v == -3.f);
    CHECK(z.d[0] == -1.f);
}

TEST_CASE("Dual: compound assignment", "[dual][autodiff]") {
    auto x = Dual<float,1>::variable(2.f, 0);
    x += Dual<float,1>(1.f);
    CHECK(x.v == 3.f);
    x *= Dual<float,1>::variable(2.f, 0);
    CHECK(x.v == 6.f);
}

TEST_CASE("Dual: scalar arithmetic", "[dual][autodiff]") {
    auto x = Dual<float,1>::variable(4.f, 0);
    auto z = x * 3.f;
    CHECK(z.v == 12.f);
    CHECK(z.d[0] == 3.f);
    auto w = 2.f + x;
    CHECK(w.v == 6.f);
    CHECK(w.d[0] == 1.f);
}

TEST_CASE("Dual: sqrt chain rule", "[dual][autodiff][math]") {
    // d(sqrt(x))/dx = 1/(2*sqrt(x)) at x=9 → 1/6
    auto x = Dual<float,1>::variable(9.f, 0);
    auto z = ga::sqrt(x);
    CHECK(z.v == Catch::Approx(3.f));
    CHECK(z.d[0] == Catch::Approx(1.f/6.f).epsilon(1e-6f));
}

TEST_CASE("Dual: exp chain rule", "[dual][autodiff][math]") {
    // d(exp(x))/dx = exp(x) at x=1 → e
    auto x = Dual<float,1>::variable(1.f, 0);
    auto z = ga::exp(x);
    CHECK(z.v == Catch::Approx(std::exp(1.f)));
    CHECK(z.d[0] == Catch::Approx(std::exp(1.f)));
}

TEST_CASE("Dual: log chain rule", "[dual][autodiff][math]") {
    // d(log(x))/dx = 1/x at x=2
    auto x = Dual<float,1>::variable(2.f, 0);
    auto z = ga::log(x);
    CHECK(z.v == Catch::Approx(std::log(2.f)));
    CHECK(z.d[0] == Catch::Approx(0.5f).epsilon(1e-6f));
}

TEST_CASE("Dual: sin/cos chain rule", "[dual][autodiff][math]") {
    float xv = 0.7f;
    auto x = Dual<float,1>::variable(xv, 0);
    auto s = ga::sin(x);
    auto c = ga::cos(x);
    CHECK(s.v == Catch::Approx(std::sin(xv)));
    CHECK(s.d[0] == Catch::Approx(std::cos(xv)).epsilon(1e-6f));
    CHECK(c.v == Catch::Approx(std::cos(xv)));
    CHECK(c.d[0] == Catch::Approx(-std::sin(xv)).epsilon(1e-6f));
}

TEST_CASE("Dual: pow chain rule", "[dual][autodiff][math]") {
    // d(x^3)/dx = 3x^2 at x=2 → 12
    auto x = Dual<float,1>::variable(2.f, 0);
    auto z = ga::pow(x, 3.f);
    CHECK(z.v == Catch::Approx(8.f));
    CHECK(z.d[0] == Catch::Approx(12.f).epsilon(1e-5f));
}

TEST_CASE("Dual: abs chain rule", "[dual][autodiff][math]") {
    auto x = Dual<float,1>::variable(-3.f, 0);
    auto z = ga::abs(x);
    CHECK(z.v == 3.f);
    CHECK(z.d[0] == -1.f);  // sign of value
}

TEST_CASE("Dual: grad helper function", "[dual][autodiff][grad]") {
    // grad of f(x) = x^2 at x=5 → 10
    auto g = ga::grad([](auto x){ return x*x; }, 5.f);
    CHECK(g == Catch::Approx(10.f).epsilon(1e-5f));
}

TEST_CASE("Dual: grad of sin", "[dual][autodiff][grad]") {
    float xv = 1.2f;
    auto g = ga::grad([](auto x){ return ga::sin(x); }, xv);
    CHECK(g == Catch::Approx(std::cos(xv)).epsilon(1e-5f));
}

TEST_CASE("Dual: grad_vec for multivariate f", "[dual][autodiff][grad_vec]") {
    // f(x,y) = x^2 + x*y; ∂f/∂x = 2x+y = 5, ∂f/∂y = x = 2 at (2,1)
    auto g = ga::grad_vec<float,2>([](const auto& v){
        return v[0]*v[0] + v[0]*v[1];
    }, std::array<float,2>{2.f, 1.f});
    CHECK(g[0] == Catch::Approx(5.f).epsilon(1e-5f));
    CHECK(g[1] == Catch::Approx(2.f).epsilon(1e-5f));
}

TEST_CASE("Dual: N=2 independent variables", "[dual][autodiff][multivariable]") {
    // f(x,y) = x*y + y^2; compute both partials simultaneously
    using D2 = Dual<float, 2>;
    auto x = D2::variable(3.f, 0);
    auto y = D2::variable(2.f, 1);
    auto z = x*y + y*y;  // value = 3*2 + 4 = 10; df/dx=y=2, df/dy=x+2y=7
    CHECK(z.v == Catch::Approx(10.f));
    CHECK(z.d[0] == Catch::Approx(2.f).epsilon(1e-5f));
    CHECK(z.d[1] == Catch::Approx(7.f).epsilon(1e-5f));
}

TEST_CASE("Dual: comparison uses value only", "[dual][autodiff]") {
    Dual<float,1> a(2.f), b(3.f);
    CHECK(a < b);
    CHECK(b > a);
    CHECK(!(a == b));
    Dual<float,1> c(2.f);
    CHECK(a == c);
}

TEST_CASE("DualScalar alias works", "[dual][autodiff]") {
    DualScalar<double> x = Dual<double,1>::variable(1.0, 0);
    CHECK(x.dim == 1);
    CHECK(x.d[0] == 1.0);
}

// ===========================================================================
// hessian_vec / hessian — appended HVP tests (kalpa enhancement)
// ===========================================================================

TEST_CASE("hessian_vec: quadratic exactness ∇²f·v = A v", "[dual][autodiff][hvp]") {
    // f(x) = ½ xᵀA x with A = [[3,1],[1,2]] → Hessian ≡ A (constant).
    // ∇²f(x)·v must equal A·v regardless of x.
    auto f = [](const std::array<Dual<double,1>,2>& x) {
        auto q = x[0]*x[0]*1.5 + x[1]*x[1]*1.0 + x[0]*x[1]*1.0;  // ½(3x²+2y²)+xy
        return q;
    };
    std::array<double,2> x{0.7, -1.3};
    std::array<double,2> v{1.0, 2.0};                 // A v = [3*1+1*2, 1*1+2*2] = [5,5]
    auto hv = hessian_vec<double,2>(f, x, v);
    CHECK(hv[0] == Catch::Approx(5.0).epsilon(1e-4));
    CHECK(hv[1] == Catch::Approx(5.0).epsilon(1e-4));

    std::array<double,2> x2{10.0, 4.0};               // location-independent
    auto hv2 = hessian_vec<double,2>(f, x2, v);
    CHECK(hv2[0] == Catch::Approx(5.0).epsilon(1e-4));
    CHECK(hv2[1] == Catch::Approx(5.0).epsilon(1e-4));
}

TEST_CASE("hessian: dense matches analytic + symmetric", "[dual][autodiff][hvp]") {
    // Same quadratic → dense Hessian == A = [[3,1],[1,2]].
    auto f = [](const std::array<Dual<double,1>,2>& x) {
        return x[0]*x[0]*1.5 + x[1]*x[1]*1.0 + x[0]*x[1]*1.0;
    };
    std::array<double,2> x{0.3, 0.9};
    auto H = hessian<double,2>(f, x);
    CHECK(H[0][0] == Catch::Approx(3.0).epsilon(1e-4));
    CHECK(H[0][1] == Catch::Approx(1.0).epsilon(1e-4));
    CHECK(H[1][0] == Catch::Approx(1.0).epsilon(1e-4));
    CHECK(H[1][1] == Catch::Approx(2.0).epsilon(1e-4));
    CHECK(H[0][1] == Catch::Approx(H[1][0]));          // symmetry
}

TEST_CASE("hessian_vec: nonlinear column parity with hessian", "[dual][autodiff][hvp]") {
    // f(x,y) = x²y + sin(y). Hessian·e₀ must equal column 0 of dense hessian.
    auto f = [](const std::array<Dual<double,1>,2>& x) {
        return x[0]*x[0]*x[1] + ga::sin(x[1]);
    };
    std::array<double,2> x{1.1, 0.4};
    auto H = hessian<double,2>(f, x);
    std::array<double,2> e0{1.0, 0.0};
    auto col0 = hessian_vec<double,2>(f, x, e0);
    CHECK(col0[0] == Catch::Approx(H[0][0]).epsilon(1e-3));
    CHECK(col0[1] == Catch::Approx(H[1][0]).epsilon(1e-3));
}
