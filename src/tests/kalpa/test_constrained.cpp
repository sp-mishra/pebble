#include <catch_amalgamated.hpp>
#include <kalpa/kalpa.hpp>
#include <kalpa/algo/constrained.hpp>
#include <kalpa/algo/unconstrained.hpp>
#include <containers/matrix/dense.hpp>
#include <cmath>
#include <vector>

using namespace kalpa;

namespace {
    ga::Vector<double> v2(double a, double b) { ga::Vector<double> v(2); v[0]=a; v[1]=b; return v; }
    ga::Vector<double> v3(double a, double b, double c) { ga::Vector<double> v(3); v[0]=a; v[1]=b; v[2]=c; return v; }

    // Local classes cannot hold member templates → objectives at file scope.
    // ½‖x − (5,5)‖²; minimum at (5,5), clamped by the constraint/domain.
    struct Shifted5 {
        template<typename V> auto operator()(const V& x) const {
            using S = typename V::value_type;
            S a = x[0] - S{5}; S b = x[1] - S{5};
            return S{0.5}*(a*a + b*b);
        }
    };
}

// ===========================================================================
// Linear program (dense revised simplex). Standard form min cᵀx, Ax=b, x≥0.
//   max x+y  s.t. x+y ≤ 4, x ≤ 3   →  min −x−y with slacks s1,s2 ≥ 0:
//   x + y + s1       = 4
//   x       + s2     = 3
//   Optimum at (3,1): objective max = 4 → simplex f (of −x−y) = −4.
// ===========================================================================
TEST_CASE("kalpa: dense simplex finds the LP vertex", "[kalpa][constrained][lp]") {
    ga::Matrix<double> A(2, 4, 0.0);
    A(0,0)=1; A(0,1)=1; A(0,2)=1;               // x+y+s1 = 4
    A(1,0)=1;           A(1,3)=1;               // x   +s2 = 3
    ga::Vector<double> b = v2(4.0, 3.0);
    ga::Vector<double> c(4, 0.0); c[0]=-1; c[1]=-1;  // min −x−y

    SimplexLP<double> lp;
    auto r = lp.solve(A, b, c);
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(3.0).margin(1e-6));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-6));
    CHECK(r->f   == Catch::Approx(-4.0).margin(1e-6));
}

// Degenerate LP — Bland's rule must terminate. Trivial equality x1=x2=... with
// redundant tight constraint. min x  s.t. x + s = 1, x ≤ 1 (extra), x≥0.
TEST_CASE("kalpa: simplex terminates on a degenerate LP (Bland)", "[kalpa][constrained][lp]") {
    ga::Matrix<double> A(2, 3, 0.0);
    A(0,0)=1; A(0,1)=1;                          // x + s1 = 1
    A(1,0)=1;          A(1,2)=1;                 // x + s2 = 1  (redundant/degenerate)
    ga::Vector<double> b = v2(1.0, 1.0);
    ga::Vector<double> c(3, 0.0); c[0]=1.0;      // min x  → optimum x=0
    SimplexLP<double> lp;
    auto r = lp.solve(A, b, c);
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(0.0).margin(1e-6));
    CHECK(r->f   == Catch::Approx(0.0).margin(1e-6));
}

TEST_CASE("kalpa: simplex reports an unbounded objective", "[kalpa][constrained][lp]") {
    // min −x  s.t.  x − s = 0 (x free upward) → unbounded below.
    ga::Matrix<double> A(1, 2, 0.0);
    A(0,0)=1; A(0,1)=-1;                         // x − s = 0
    ga::Vector<double> b(1, 0.0);
    ga::Vector<double> c(2, 0.0); c[0]=-1.0;
    SimplexLP<double> lp;
    auto r = lp.solve(A, b, c);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().cause == Cause::Unbounded);
}

// ===========================================================================
// Exact rational simplex — zero round-off. Same LP as first, exact vertex.
// ===========================================================================
TEST_CASE("kalpa: exact-rational simplex gives the exact vertex", "[kalpa][constrained][lp][exact]") {
    using F = Fraction;
    std::vector<std::vector<F>> A = {
        { F(1), F(1), F(1), F(0) },
        { F(1), F(0), F(0), F(1) },
    };
    std::vector<F> b = { F(4), F(3) };
    std::vector<F> c = { F(-1), F(-1), F(0), F(0) };
    ExactSimplexLP lp;
    auto r = lp.solve(A, b, c);
    REQUIRE(r.ok);
    CHECK(r.x[0] == F(3));                         // exact
    CHECK(r.x[1] == F(1));
    CHECK(r.objective == F(-4));
}

TEST_CASE("kalpa: Fraction normalizes and compares exactly", "[kalpa][constrained][exact]") {
    Fraction a(2, 4);                             // → 1/2
    CHECK(a.num == 1);
    CHECK(a.den == 2);
    CHECK((Fraction(1,3) + Fraction(1,6)) == Fraction(1,2));
    CHECK((Fraction(2,3) * Fraction(3,4)) == Fraction(1,2));
    CHECK(Fraction(1,3) < Fraction(1,2));
}

// ===========================================================================
// Equality-constrained QP via ga::schur_solve KKT.
//   min ½(x²+y²)  s.t.  x + y = 2.
//   H = I, c = 0, A = [1 1], b = [2].  Optimum x=y=1, λ = −1.
// ===========================================================================
TEST_CASE("kalpa: equality QP solves the KKT system", "[kalpa][constrained][qp]") {
    ga::Matrix<double> H = ga::Matrix<double>::identity(2);
    ga::Vector<double> c(2, 0.0);
    ga::Matrix<double> A(1, 2, 0.0); A(0,0)=1; A(0,1)=1;
    ga::Vector<double> b(1, 2.0);

    EqualityQP<double> qp;
    auto r = qp.solve(H, c, A, b);
    REQUIRE(r.has_value());
    const auto& [x, lam] = *r;
    CHECK(x[0] == Catch::Approx(1.0).margin(1e-9));
    CHECK(x[1] == Catch::Approx(1.0).margin(1e-9));
    // KKT residual  Hx + c + Aᵀλ = 0  and  Ax = b
    CHECK(x[0] + lam[0] == Catch::Approx(0.0).margin(1e-9));   // 1*x0 + Aᵀλ
    CHECK(x[0] + x[1]   == Catch::Approx(2.0).margin(1e-9));   // equality feasible
}

// ===========================================================================
// Projections — every projected point must be feasible & idempotent inside.
// ===========================================================================
TEST_CASE("kalpa: box projection clamps and is idempotent inside", "[kalpa][constrained][project]") {
    Box<double> box(v2(-1.0,-1.0), v2(1.0,1.0));
    ga::Vector<double> out(2);

    box.project(v2(0.3, -0.5), out);              // interior → unchanged
    CHECK(out[0] == Catch::Approx(0.3));
    CHECK(out[1] == Catch::Approx(-0.5));

    box.project(v2(5.0, -9.0), out);              // outside → clamped to corner
    CHECK(out[0] == Catch::Approx(1.0));
    CHECK(out[1] == Catch::Approx(-1.0));
}

TEST_CASE("kalpa: ball projection lands on the sphere", "[kalpa][constrained][project]") {
    Ball<double> ball; ball.center = v2(0.0,0.0); ball.radius = 2.0;
    ga::Vector<double> out(2);
    ball.project(v2(6.0, 0.0), out);              // outside along +x
    CHECK(std::sqrt(out[0]*out[0] + out[1]*out[1]) == Catch::Approx(2.0).margin(1e-9));

    ball.project(v2(0.5, 0.5), out);              // inside → unchanged
    CHECK(out[0] == Catch::Approx(0.5));
    CHECK(out[1] == Catch::Approx(0.5));
}

TEST_CASE("kalpa: polytope projection yields a feasible point", "[kalpa][constrained][project]") {
    // Halfspaces  x ≤ 1,  y ≤ 1,  −x ≤ 0,  −y ≤ 0  (unit box as a polytope).
    ga::Matrix<double> A(4, 2, 0.0);
    A(0,0)= 1;             // x ≤ 1
    A(1,1)= 1;             // y ≤ 1
    A(2,0)=-1;             // −x ≤ 0
    A(3,1)=-1;             // −y ≤ 0
    ga::Vector<double> b(4); b[0]=1; b[1]=1; b[2]=0; b[3]=0;
    Polytope<double> P; P.A = A; P.b = b;
    ga::Vector<double> out(2);
    P.project(v2(5.0, -3.0), out);
    // feasible: each aᵢᵀx ≤ bᵢ (small tol)
    for (std::size_t r = 0; r < 4; ++r) {
        double ax = A(r,0)*out[0] + A(r,1)*out[1];
        CHECK(ax <= b[r] + 1e-6);
    }
}

// ===========================================================================
// Projected gradient descent onto a box — constrained quadratic optimum.
//   min ½‖x − t‖²,  t = (5,5),  x ∈ [−1,1]²  →  optimum at (1,1) (corner).
// ===========================================================================
TEST_CASE("kalpa: projected gradient hits the constrained optimum", "[kalpa][constrained][pgd]") {
    auto prob = make_problem(Shifted5{}, Unconstrained<ga::Vector<double>>{},
                             Box<double>(v2(-1.0,-1.0), v2(1.0,1.0)));
    Solver<ProjectedGradient<double>, Derivatives<Dual,double>, Armijo<double>> s;
    auto r = s.solve(prob, v2(0.0, 0.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(1.0).margin(1e-4));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-4));
}

// ===========================================================================
// Frank-Wolfe over a box via a coordinate LMO.
//   min ½‖x − t‖², t=(5,5), box [−1,1]²  → same corner optimum (1,1).
//   LMO(g): vertex minimizing gᵀs → per-coord s_i = (g_i>0? lo : hi).
// ===========================================================================
TEST_CASE("kalpa: Frank-Wolfe converges over a box domain", "[kalpa][constrained][fw]") {
    auto prob = make_problem<double>(Shifted5{});
    FrankWolfe<double> fw;
    auto lmo = [](const ga::Vector<double>& g, ga::Vector<double>& s) {
        for (std::size_t i = 0; i < g.size(); ++i) s[i] = (g[i] > 0.0) ? -1.0 : 1.0;
    };
    auto r = fw.solve(prob, v2(0.0, 0.0), Derivatives<Dual,double>{}, lmo);
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(1.0).margin(1e-2));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-2));
}
