#include <catch_amalgamated.hpp>
#include <kalpa/kalpa.hpp>
#include <kalpa/algo/unconstrained.hpp>
#include <containers/matrix/dense.hpp>

using namespace kalpa;
using namespace kalpa::edsl;

namespace {
    ga::Vector<double> v2(double a, double b) { ga::Vector<double> v(2); v[0]=a; v[1]=b; return v; }

    // Hand-written twin of the EDSL objective, for parity checks. File scope
    // because it carries a templated call operator.
    struct Functor {
        template<typename V> auto operator()(const V& x) const {
            using S = typename V::value_type;
            S a = x[0] - S{1}; S b = x[1] - S{2};
            return a*a + b*b;
        }
    };
}

// ===========================================================================
// EDSL objective == functor objective at the optimum.
//   minimize (x−1)² + (y−2)²  →  x*=(1,2), f*=0.
// ===========================================================================
TEST_CASE("kalpa: EDSL objective reaches the analytic optimum", "[kalpa][edsl]") {
    auto x = vars();
    auto prob = minimize<double>( sq(x[0] - constant(1.0)) + sq(x[1] - constant(2.0)) );
    Solver<LBFGS<double>, Derivatives<Dual,double>, Wolfe<double>> s;
    auto r = s.solve(prob, v2(-3.0, 5.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(1.0).margin(1e-5));
    CHECK(r->x[1] == Catch::Approx(2.0).margin(1e-5));
    CHECK(r->f   == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("kalpa: EDSL and functor solvers agree", "[kalpa][edsl]") {
    auto x = vars();
    auto eprob = minimize<double>( sq(x[0] - constant(1.0)) + sq(x[1] - constant(2.0)) );
    auto fprob = make_problem<double>(Functor{});

    Solver<LBFGS<double>, Derivatives<Dual,double>, Wolfe<double>> se, sf;
    auto re = se.solve(eprob, v2(4.0, -4.0));
    auto rf = sf.solve(fprob, v2(4.0, -4.0));
    REQUIRE(re.has_value()); REQUIRE(rf.has_value());
    CHECK(re->x[0] == Catch::Approx(rf->x[0]).margin(1e-6));
    CHECK(re->x[1] == Catch::Approx(rf->x[1]).margin(1e-6));
}

// ===========================================================================
// EDSL graph value == functor value, pointwise.
// ===========================================================================
TEST_CASE("kalpa: EDSL evaluates to the same value as the functor", "[kalpa][edsl]") {
    auto x = vars();
    auto expr = wrap( sq(x[0] - constant(1.0)) + sq(x[1] - constant(2.0)) );
    Functor f;
    for (auto p : { v2(0.0,0.0), v2(1.0,2.0), v2(-2.5,3.3), v2(10.0,-7.0) }) {
        CHECK(expr(p) == Catch::Approx(f(p)));
    }
}

// ===========================================================================
// EDSL graph gradient (via Derivatives<Dual>) == functor gradient == analytic.
//   ∇f = [2(x−1), 2(y−2)].
// ===========================================================================
TEST_CASE("kalpa: EDSL graph gradient matches ga::Dual on the functor", "[kalpa][edsl][derivatives]") {
    auto x = vars();
    auto expr = wrap( sq(x[0] - constant(1.0)) + sq(x[1] - constant(2.0)) );
    Functor f;

    ga::Vector<double> p = v2(0.7, -0.4);
    ga::Vector<double> ge(2), gf(2);
    Derivatives<Dual,double> d;
    d.grad(expr, p, ge);       // gradient of the EDSL graph
    d.grad(f,    p, gf);       // gradient of the hand functor
    CHECK(ge[0] == Catch::Approx(gf[0]).epsilon(1e-9));
    CHECK(ge[1] == Catch::Approx(gf[1]).epsilon(1e-9));
    // analytic
    CHECK(ge[0] == Catch::Approx(2.0*(0.7 - 1.0)).epsilon(1e-9));
    CHECK(ge[1] == Catch::Approx(2.0*(-0.4 - 2.0)).epsilon(1e-9));
}

// ===========================================================================
// var()/constant()/sq() compose: a product term and a division term evaluate.
//   g(x) = (x0 * x1) / constant(2) + sq(x0)   at (3,4) = 12/2 + 9 = 15.
// ===========================================================================
TEST_CASE("kalpa: EDSL supports mul/div/sq composition", "[kalpa][edsl]") {
    auto x = vars();
    auto expr = wrap( (x[0] * x[1]) / constant(2.0) + sq(x[0]) );
    CHECK(expr(v2(3.0, 4.0)) == Catch::Approx(15.0));
}

// ===========================================================================
// subject_to — comparison expressions become signed residual functors.
//   Eq:  (x0+x1) == 1   → residual = x0+x1−1.
//   Le:  x0 <= 2         → residual = x0−2  (feasible ⇔ ≤ 0).
//   Ge:  x0 >= 0         → residual = 0−x0 = −x0 (feasible ⇔ ≤ 0).
// ===========================================================================
TEST_CASE("kalpa: subject_to builds an equality residual", "[kalpa][edsl][constraints]") {
    auto x = vars();
    auto cs = subject_to( (x[0] + x[1]) == constant(1.0) );
    CHECK(cs.count() == 1);
    // residual x0+x1−1: zero on the constraint surface, +1 off it.
    CHECK(cs.residual(0, v2(0.3, 0.7)) == Catch::Approx(0.0).margin(1e-12));
    CHECK(cs.residual(0, v2(1.0, 1.0)) == Catch::Approx(1.0).margin(1e-12));
    CHECK(cs.feasible(v2(0.3, 0.7)));
    CHECK_FALSE(cs.feasible(v2(1.0, 1.0)));
}

TEST_CASE("kalpa: subject_to encodes <= and >= sign conventions", "[kalpa][edsl][constraints]") {
    auto x = vars();
    auto le = subject_to( x[0] <= constant(2.0) );   // x0−2 ≤ 0
    CHECK(le.residual(0, v2(3.0, 0.0)) == Catch::Approx(1.0).margin(1e-12));   // violated → +1
    CHECK(le.residual(0, v2(1.0, 0.0)) == Catch::Approx(-1.0).margin(1e-12));  // slack   → −1
    CHECK(le.feasible(v2(1.0, 0.0)));
    CHECK_FALSE(le.feasible(v2(3.0, 0.0)));

    auto ge = subject_to( x[0] >= constant(0.0) );   // −x0 ≤ 0
    CHECK(ge.residual(0, v2(-1.0, 0.0)) == Catch::Approx(1.0).margin(1e-12));  // x0<0 → +1
    CHECK(ge.residual(0, v2( 2.0, 0.0)) == Catch::Approx(-2.0).margin(1e-12)); // x0>0 → −2
    CHECK(ge.feasible(v2(2.0, 0.0)));
    CHECK_FALSE(ge.feasible(v2(-1.0, 0.0)));
}

TEST_CASE("kalpa: multiple subject_to constraints count and evaluate", "[kalpa][edsl][constraints]") {
    auto x = vars();
    auto cs = subject_to( (x[0] + x[1]) == constant(2.0),
                          x[0] >= constant(0.0) );
    CHECK(cs.count() == 2);
    CHECK(cs.residual(0, v2(1.0, 1.0)) == Catch::Approx(0.0).margin(1e-12));
    CHECK(cs.residual(1, v2(1.0, 1.0)) == Catch::Approx(-1.0).margin(1e-12));
    CHECK(cs.feasible(v2(1.0, 1.0)));
}

// ===========================================================================
// Auto — structural analysis selects a solver and drives the EDSL objective
// to its optimum. (x−1)²+(y−2)² is smooth + convex + low-dim → Newton/LBFGS.
// ===========================================================================
TEST_CASE("kalpa: Auto selects a gradient method for a smooth convex objective", "[kalpa][edsl][auto]") {
    auto x = vars();
    auto expr = sq(x[0] - constant(1.0)) + sq(x[1] - constant(2.0));

    vakya::property_store store;
    auto ch = choose(expr, store);
    CHECK(ch.analysis.smooth);
    CHECK(ch.analysis.convex);
    CHECK(ch.analysis.dim == 2);
    CHECK((ch.algo == MethodChoice::Newton || ch.algo == MethodChoice::LBFGS));
}

TEST_CASE("kalpa: Auto solves the EDSL objective to the analytic optimum", "[kalpa][edsl][auto]") {
    auto x = vars();
    auto prob = minimize<double>( sq(x[0] - constant(1.0)) + sq(x[1] - constant(2.0)) );
    Auto<double> automatic;
    auto r = automatic.solve(prob, v2(-3.0, 5.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(1.0).margin(1e-5));
    CHECK(r->x[1] == Catch::Approx(2.0).margin(1e-5));
    CHECK(r->f   == Catch::Approx(0.0).margin(1e-9));
}

// A division by a variable makes the objective non-convex; analysis flags it.
TEST_CASE("kalpa: Auto flags a non-convex division objective", "[kalpa][edsl][auto]") {
    auto x = vars();
    auto expr = sq(x[0] - constant(1.0)) + constant(1.0) / (x[1] + constant(3.0));
    vakya::property_store store;
    auto ch = choose(expr, store);
    CHECK_FALSE(ch.analysis.convex);   // div ⇒ non-convex
    CHECK(ch.analysis.smooth);         // still smooth ⇒ a gradient method
    CHECK((ch.algo == MethodChoice::LBFGS));
}
