#include <catch_amalgamated.hpp>
#include <kalpa/kalpa.hpp>
#include <kalpa/algo/least_squares.hpp>
#include <containers/matrix/dense.hpp>
#include <cmath>
#include <vector>

using namespace kalpa;

// ===========================================================================
// Nonlinear least-squares oracles. Each residual functor r_i(x) is written
// generically over the vector element type (value pass S=double, Dual pass
// S=ga::Dual<double,1>), so Derivatives<Dual> can build the Jacobian rows.
// Local classes cannot carry a member template → residuals live at file scope.
// ===========================================================================
namespace {
    ga::Vector<double> v2(double a, double b) { ga::Vector<double> v(2); v[0]=a; v[1]=b; return v; }

    // --- exponential model fit  y = a·e^{b t} ------------------------------
    // Residuals rᵢ = a·e^{b·tᵢ} − yᵢ over data sampled from a*=2, b*=0.5.
    // A parameter vector p = (a, b); the fit should recover (2, 0.5).
    // Unqualified exp + a using-declaration: std::exp for the double value pass,
    // ga::exp (found by ADL) for the ga::Dual gradient pass.
    struct ExpFitRes {
        double t;
        double y;
        template<typename V> auto operator()(const V& p) const {
            using S = typename V::value_type;
            using std::exp;
            return p[0] * exp(p[1] * S(t)) - S(y);
        }
    };
    std::vector<ExpFitRes> exp_dataset() {
        std::vector<ExpFitRes> r;
        const double a = 2.0, b = 0.5;
        for (int k = 0; k <= 10; ++k) {
            const double t = 0.1 * k;
            r.push_back(ExpFitRes{t, a * std::exp(b * t)});   // noise-free
        }
        return r;
    }
}

// ===========================================================================
// Levenberg–Marquardt on Rosenbrock-as-NLS → (1,1), residual 0, f=0.
// ===========================================================================
TEST_CASE("kalpa: Levenberg–Marquardt solves Rosenbrock residuals", "[kalpa][lsq][lm]") {
    // Rosen_r0 and Rosen_r1 are distinct types, so a homogeneous std::vector
    // needs a single residual type. Use an index-selecting adapter that carries
    // both the value and the Dual overload the AD Jacobian pass requires.
    struct RosenRes {
        int which;
        double operator()(const ga::Vector<double>& x) const {
            if (which == 0) return 1.0 - x[0];
            return 10.0 * (x[1] - x[0]*x[0]);
        }
        // Dual overload for the AD Jacobian pass.
        ga::Dual<double,1> operator()(const ga::Vector<ga::Dual<double,1>>& x) const {
            using S = ga::Dual<double,1>;
            if (which == 0) return S{1} - x[0];
            return S{10} * (x[1] - x[0]*x[0]);
        }
    };
    std::vector<RosenRes> res{ RosenRes{0}, RosenRes{1} };

    LevenbergMarquardt<double> lm; lm.max_iter = 200;
    auto r = lm.solve(res, v2(-1.2, 1.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(1.0).margin(1e-5));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-5));
    CHECK(r->f == Catch::Approx(0.0).margin(1e-9));
    CHECK(r->residual_norm < 1e-5);
    CHECK(r->grad_norm < 1e-6);
    CHECK(r->status == Status::Converged);
}

// ===========================================================================
// Gauss–Newton on the same residuals (QR least-squares path) → (1,1).
// ===========================================================================
TEST_CASE("kalpa: Gauss–Newton solves Rosenbrock residuals", "[kalpa][lsq][gn]") {
    struct RosenRes {
        int which;
        double operator()(const ga::Vector<double>& x) const {
            if (which == 0) return 1.0 - x[0];
            return 10.0 * (x[1] - x[0]*x[0]);
        }
        ga::Dual<double,1> operator()(const ga::Vector<ga::Dual<double,1>>& x) const {
            using S = ga::Dual<double,1>;
            if (which == 0) return S{1} - x[0];
            return S{10} * (x[1] - x[0]*x[0]);
        }
    };
    std::vector<RosenRes> res{ RosenRes{0}, RosenRes{1} };

    GaussNewton<double> gn; gn.max_iter = 100;
    auto r = gn.solve(res, v2(-1.2, 1.0));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(1.0).margin(1e-5));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-5));
    CHECK(r->residual_norm < 1e-5);
}

// ===========================================================================
// LM recovers the parameters of an exponential model  y = a·e^{b t}.
//   Data generated noise-free from (a,b) = (2, 0.5); LM must return them.
// ===========================================================================
TEST_CASE("kalpa: Levenberg–Marquardt fits an exponential model", "[kalpa][lsq][lm]") {
    auto data = exp_dataset();
    LevenbergMarquardt<double> lm; lm.max_iter = 200;
    auto r = lm.solve(data, v2(1.0, 1.0));           // start away from the truth
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(2.0).margin(1e-4));   // a
    CHECK(r->x[1] == Catch::Approx(0.5).margin(1e-4));   // b
    CHECK(r->residual_norm < 1e-4);                       // near-perfect fit
}

// ===========================================================================
// Determinism: LM is a deterministic descent — same start ⇒ identical result.
// ===========================================================================
TEST_CASE("kalpa: Levenberg–Marquardt is deterministic", "[kalpa][lsq][lm][determinism]") {
    auto data = exp_dataset();
    LevenbergMarquardt<double> lm;
    auto a = lm.solve(data, v2(1.0, 1.0));
    auto b = lm.solve(data, v2(1.0, 1.0));
    REQUIRE(a.has_value()); REQUIRE(b.has_value());
    CHECK(a->x[0] == b->x[0]);
    CHECK(a->x[1] == b->x[1]);
    CHECK(a->residual_norm == b->residual_norm);
}

// ===========================================================================
// Parallel Jacobian parity: ParallelJacobian must reach the same point as the
// serial default (differential validation of the pravaha row-fill path).
// ===========================================================================
TEST_CASE("kalpa: LM parallel Jacobian matches the serial path", "[kalpa][lsq][lm][parallel]") {
    auto data = exp_dataset();
    LevenbergMarquardt<double, SerialJacobian>   lm_ser;
    LevenbergMarquardt<double, ParallelJacobian> lm_par;
    auto rs = lm_ser.solve(data, v2(1.0, 1.0));
    auto rp = lm_par.solve(data, v2(1.0, 1.0));
    REQUIRE(rs.has_value()); REQUIRE(rp.has_value());
    CHECK(rp->x[0] == Catch::Approx(rs->x[0]).margin(1e-9));
    CHECK(rp->x[1] == Catch::Approx(rs->x[1]).margin(1e-9));
    CHECK(rp->residual_norm == Catch::Approx(rs->residual_norm).margin(1e-9));
}
