#include <catch_amalgamated.hpp>
#include <kalpa/kalpa.hpp>
#include <kalpa/algo/unconstrained.hpp>
#include <containers/matrix/dense.hpp>
#include <containers/matrix/dual.hpp>
#include <cmath>

using namespace kalpa;

// ===========================================================================
// Oracles. Objectives are written over ga::Vector<S> generically so the same
// callable serves the value pass (S=double) and the Dual gradient pass
// (S=ga::Dual<double,1>), which Derivatives<Dual> requires.
// ===========================================================================

// Convex quadratic  ½ xᵀA x − bᵀx,  A = [[3,1],[1,2]], b = [1,2].
// Unique minimizer solves A x* = b → x* = ([0, 1] since 3x+y=1,x+2y=2 → x=0,y=1).
struct Quadratic {
    template<typename V>
    auto operator()(const V& x) const {
        using S = typename V::value_type;
        return S{1.5}*x[0]*x[0] + S{1.0}*x[1]*x[1] + S{1.0}*x[0]*x[1]
             - S{1.0}*x[0] - S{2.0}*x[1];
    }
};

// Rosenbrock  (1−x)² + 100(y−x²)²,  minimizer (1,1), f*=0.
struct Rosenbrock {
    template<typename V>
    auto operator()(const V& x) const {
        using S = typename V::value_type;
        S a = S{1} - x[0];
        S b = x[1] - x[0]*x[0];
        return a*a + S{100}*b*b;
    }
};

namespace {
    ga::Vector<double> vec2(double a, double b) { ga::Vector<double> v(2); v[0]=a; v[1]=b; return v; }

    // Local classes cannot hold member templates → keep this at file scope.
    // log(x0) is NaN for x0 < 0, exercising the solver's NaN trap. Unqualified
    // `log` with both namespaces in scope resolves to ga::log for the Dual pass
    // (ADL) and std::log for the plain-double value / line-search pass.
    struct NanObj {
        template<typename V> auto operator()(const V& x) const {
            using std::log; using ga::log;
            return log(x[0]);
        }
    };
}

TEST_CASE("kalpa: gradient descent on convex quadratic", "[kalpa][unconstrained]") {
    auto prob = make_problem<double>(Quadratic{});
    Solver<GradientDescent<double>, Derivatives<Dual,double>, Armijo<double>> s;
    auto r = s.solve(prob, vec2(5.0, -3.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(0.0).margin(1e-4));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-4));
    CHECK(r->grad_norm < 1e-5);
}

TEST_CASE("kalpa: L-BFGS matches the direct ga::solve minimizer", "[kalpa][unconstrained]") {
    auto prob = make_problem<double>(Quadratic{});
    Solver<LBFGS<double>, Derivatives<Dual,double>, Wolfe<double>> s;
    auto r = s.solve(prob, vec2(-4.0, 4.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(0.0).margin(1e-5));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-5));
}

TEST_CASE("kalpa: BFGS on convex quadratic", "[kalpa][unconstrained]") {
    auto prob = make_problem<double>(Quadratic{});
    Solver<BFGS<double>, Derivatives<Dual,double>, Wolfe<double>> s;
    auto r = s.solve(prob, vec2(2.0, 2.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(0.0).margin(1e-5));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-5));
}

TEST_CASE("kalpa: nonlinear CG (Polak-Ribiere) on quadratic", "[kalpa][unconstrained]") {
    auto prob = make_problem<double>(Quadratic{});
    Solver<ConjugateGradient<double>, Derivatives<Dual,double>, Wolfe<double>> s;
    auto r = s.solve(prob, vec2(3.0, 3.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(0.0).margin(1e-4));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-4));
}

TEST_CASE("kalpa: Newton reaches quadratic optimum quickly", "[kalpa][unconstrained]") {
    auto prob = make_problem<double>(Quadratic{});
    Solver<Newton<double>, Derivatives<Dual,double>, Wolfe<double>> s;
    auto r = s.solve(prob, vec2(9.0, -9.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(0.0).margin(1e-6));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-6));
    CHECK(r->iterations < 15);           // Newton on a quadratic is fast
}

TEST_CASE("kalpa: L-BFGS solves Rosenbrock", "[kalpa][unconstrained][rosenbrock]") {
    auto prob = make_problem<double>(Rosenbrock{});
    Solver<LBFGS<double>, Derivatives<Dual,double>, Wolfe<double>,
           DefaultStop<double>> s;
    auto r = s.solve(prob, vec2(-1.2, 1.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(1.0).margin(1e-3));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-3));
    CHECK(r->f == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("kalpa: trust-region Newton-CG solves Rosenbrock", "[kalpa][unconstrained][rosenbrock]") {
    auto prob = make_problem<double>(Rosenbrock{});
    // TR returns a full step; a unit-α Armijo applies it.
    Armijo<double> ls; ls.alpha0 = 1.0;
    Solver<TrustRegionNewtonCG<double>, Derivatives<Dual,double>, Armijo<double>> s{
        TrustRegionNewtonCG<double>{}, Derivatives<Dual,double>{}, ls };
    auto r = s.solve(prob, vec2(-1.2, 1.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(1.0).margin(1e-2));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-2));
}

// ---- derivative parity: Dual ≈ FiniteDiff on the same objective -----------
TEST_CASE("kalpa: Dual and FiniteDiff gradients agree", "[kalpa][derivatives]") {
    Quadratic f;
    ga::Vector<double> x = vec2(0.7, -0.4);
    ga::Vector<double> gd(2), gf(2);
    Derivatives<Dual,double>{}.grad(f, x, gd);
    Derivatives<FiniteDiff,double>{}.grad(f, x, gf);
    CHECK(gd[0] == Catch::Approx(gf[0]).epsilon(1e-5));
    CHECK(gd[1] == Catch::Approx(gf[1]).epsilon(1e-5));
    // analytic gradient at x: ∂f = [3x+y-1, 2y+x-2] = [3*.7-.4-1, -0.8+0.7-2]
    CHECK(gd[0] == Catch::Approx(3*0.7 - 0.4 - 1.0).epsilon(1e-6));
    CHECK(gd[1] == Catch::Approx(2*(-0.4) + 0.7 - 2.0).epsilon(1e-6));
}

// ---- diagnosis path: NaN objective at x0 is trapped -----------------------
TEST_CASE("kalpa: NaN start is reported as a Diagnosis", "[kalpa][diagnosis]") {
    auto prob = make_problem<double>(NanObj{});
    Solver<GradientDescent<double>, Derivatives<Dual,double>, Armijo<double>> s;
    auto r = s.solve(prob, vec2(-1.0, 0.0));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().cause == Cause::NaNTrap);
}
