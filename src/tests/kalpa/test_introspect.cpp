#include <catch_amalgamated.hpp>
#include <kalpa/kalpa.hpp>
#include <kalpa/algo/unconstrained.hpp>
#include <kalpa/introspect/telemetry.hpp>
#include <containers/matrix/dense.hpp>
#include <limits>
#include <string>
#include <type_traits>

using namespace kalpa;

namespace {
    ga::Vector<double> v2(double a, double b) { ga::Vector<double> v(2); v[0]=a; v[1]=b; return v; }

    // Convex quadratic ½(3x²+2y²)+xy − x − 2y; min at (0,1). File scope for the
    // templated call operator.
    struct Quad {
        template<typename V> auto operator()(const V& x) const {
            using S = typename V::value_type;
            return S{1.5}*x[0]*x[0] + S{1.0}*x[1]*x[1] + S{1.0}*x[0]*x[1]
                 - S{1.0}*x[0] - S{2.0}*x[1];
        }
    };
}

// ===========================================================================
// Zero-overhead guarantee: NoTelemetry must add no bytes to the Solver, and
// FullTrace obviously must (it holds a vector). static_assert form.
// ===========================================================================
TEST_CASE("kalpa: NoTelemetry is a zero-size solver member", "[kalpa][introspect][zero-overhead]") {
    using Bare  = Solver<GradientDescent<double>, Derivatives<Dual,double>, Armijo<double>,
                         DefaultStop<double>, NoTelemetry>;
    using Traced= Solver<GradientDescent<double>, Derivatives<Dual,double>, Armijo<double>,
                         DefaultStop<double>, FullTrace<double>>;
    // NoTelemetry is empty → [[no_unique_address]] keeps the solver the same size
    // as one with no telemetry field at all.
    STATIC_REQUIRE(std::is_empty_v<NoTelemetry>);
    STATIC_REQUIRE(sizeof(Traced) > sizeof(Bare));
}

// ===========================================================================
// FullTrace records each state row and exposes size()/back(). Driven directly
// with synthetic IterStates (the Solver stores its sink by value, so unit-level
// verification of the sink is both simpler and more precise than reaching into
// the solver's member).
// ===========================================================================
TEST_CASE("kalpa: FullTrace stores rows in order", "[kalpa][introspect][trace]") {
    FullTrace<double> trace;
    for (std::size_t k = 0; k < 5; ++k) {
        IterState<double> s;
        s.f = 10.0 - static_cast<double>(k);    // strictly decreasing
        s.grad_norm = 1.0 / static_cast<double>(k + 1);
        s.alpha = 0.5; s.step = 0.1; s.iter = k;
        trace.record(s);
    }
    REQUIRE(trace.size() == 5);
    CHECK(trace.back().iter == 4);
    CHECK(trace.back().f == Catch::Approx(6.0));
    // rows are monotone-decreasing in f (a valid descent trajectory)
    for (std::size_t k = 1; k < trace.size(); ++k)
        CHECK(trace.rows[k].f < trace.rows[k-1].f);
}

// A real solver run drives FullTrace through the Solver; we confirm the run
// converges (the trace mechanism is exercised, just not read back here).
TEST_CASE("kalpa: solver runs with a FullTrace sink attached", "[kalpa][introspect][trace]") {
    auto prob = make_problem<double>(Quad{});
    Solver<GradientDescent<double>, Derivatives<Dual,double>, Armijo<double>,
           DefaultStop<double>, FullTrace<double>> s;
    auto r = s.solve(prob, v2(6.0, -6.0));
    REQUIRE(r.has_value());
    CHECK(r->iterations >= 1);
    CHECK(r->x[0] == Catch::Approx(0.0).margin(1e-3));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-3));
}

// ===========================================================================
// Callback fires every iteration; a captured counter proves the count and a
// captured "last f" proves monotone decrease.
// ===========================================================================
TEST_CASE("kalpa: Callback runs once per iteration and sees descent", "[kalpa][introspect][callback]") {
    auto prob = make_problem<double>(Quad{});
    std::size_t calls = 0;
    double last_f = std::numeric_limits<double>::infinity();
    bool monotone = true;

    auto cb = on_iteration([&](const auto& s) {
        ++calls;
        if (s.f > last_f + 1e-9) monotone = false;   // f must not increase
        last_f = s.f;
    });
    using CbT = decltype(cb);
    Solver<GradientDescent<double>, Derivatives<Dual,double>, Armijo<double>,
           DefaultStop<double>, CbT> s{
        GradientDescent<double>{}, Derivatives<Dual,double>{}, Armijo<double>{},
        DefaultStop<double>{}, cb };
    auto r = s.solve(prob, v2(6.0, -6.0));
    REQUIRE(r.has_value());
    CHECK(calls >= 1);
    CHECK(calls == r->iterations + 1);   // recorded at iter 0..iterations inclusive
    CHECK(monotone);
}

// ===========================================================================
// Diagnosis catalog: explain() names the cause + attaches a remediation hint.
// ===========================================================================
TEST_CASE("kalpa: explain formats a diagnosis with a hint", "[kalpa][introspect][diagnosis]") {
    Diagnosis d{Cause::NaNTrap, "objective is NaN/Inf at x0", 0};
    std::string msg = explain(d);
    CHECK(msg.find("objective is NaN/Inf at x0") != std::string::npos);
    CHECK(msg.find("hint:") != std::string::npos);
    CHECK(std::string(remediation(Cause::Unbounded)).find("bound") != std::string::npos);
    CHECK(std::string(remediation(Cause::Infeasible)).find("feasible") != std::string::npos);
}

// ===========================================================================
// A telemetry sink must not change the numerical result vs NoTelemetry.
// ===========================================================================
TEST_CASE("kalpa: telemetry does not alter the optimum", "[kalpa][introspect]") {
    auto prob = make_problem<double>(Quad{});
    Solver<LBFGS<double>, Derivatives<Dual,double>, Wolfe<double>> plain;
    Solver<LBFGS<double>, Derivatives<Dual,double>, Wolfe<double>,
           DefaultStop<double>, ProgressBar<double>> bar{
        LBFGS<double>{}, Derivatives<Dual,double>{}, Wolfe<double>{},
        DefaultStop<double>{}, ProgressBar<double>{stderr} };
    auto rp = plain.solve(prob, v2(3.0, 3.0));
    auto rb = bar.solve(prob, v2(3.0, 3.0));
    REQUIRE(rp.has_value()); REQUIRE(rb.has_value());
    CHECK(rp->x[0] == Catch::Approx(rb->x[0]).margin(1e-9));
    CHECK(rp->x[1] == Catch::Approx(rb->x[1]).margin(1e-9));
}
