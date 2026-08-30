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
