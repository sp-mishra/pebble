#include <catch_amalgamated.hpp>
#include <kalpa/kalpa.hpp>
#include <kalpa/algo/unconstrained.hpp>
#include <kalpa/algo/global.hpp>
#include <kalpa/algo/least_squares.hpp>
#include <kalpa/algo/constrained.hpp>
#include <containers/matrix/dense.hpp>
#include <vector>

using namespace kalpa;

// ===========================================================================
// bench_kalpa.cpp — Catch2 [!benchmark] timings. NOT a CI correctness gate:
// these measure solver throughput / scaling, and are skipped unless the
// benchmark tag is requested. Each still asserts convergence so a broken
// build fails loudly rather than reporting a fast-but-wrong number.
// Run:  ./pebble_tests "[kalpa][bench]" --benchmark-samples 20
// ===========================================================================

namespace {
    // Separable quadratic  Σ (xᵢ − i)²  — minimum at xᵢ = i, f* = 0.
    // Scales to any n; the analytic optimum is trivial to check.
    struct SepQuad {
        template <typename V>
        auto operator()(const V& x) const {
            using S = typename V::value_type;
            S s{};
            for (std::size_t i = 0; i < x.size(); ++i) {
                S d = x[i] - S(static_cast<double>(i));
                s = s + d * d;
            }
            return s;
        }
    };

    ga::Vector<double> zeros(std::size_t n) { return ga::Vector<double>(n, 0.0); }

    template <typename Algo, typename LS>
    double run_solver(std::size_t n) {
        auto prob = make_problem<double>(SepQuad{});
        Solver<Algo, Derivatives<Dual, double>, LS> s;
        auto r = s.solve(prob, zeros(n));
        return r.has_value() ? r->grad_norm : 1e9;
    }
}

TEST_CASE (
"kalpa bench: L-BFGS scaling"
,
"[kalpa][bench][!benchmark]"
)
 {
    // correctness guard at a representative size
    CHECK(run_solver<LBFGS<double>, Wolfe<double>>(50) < 1e-4);

    BENCHMARK("L-BFGS  n=20")  { return run_solver<LBFGS<double>, Wolfe<double>>(20); };
    BENCHMARK("L-BFGS  n=100") { return run_solver<LBFGS<double>, Wolfe<double>>(100); };
    BENCHMARK("L-BFGS  n=400") { return run_solver<LBFGS<double>, Wolfe<double>>(400); };
}

TEST_CASE (
"kalpa bench: gradient descent scaling"
,
"[kalpa][bench][!benchmark]"
)
 {
    CHECK(run_solver<GradientDescent<double>, Armijo<double>>(50) < 1e-2);

    BENCHMARK("GD  n=20")  { return run_solver<GradientDescent<double>, Armijo<double>>(20); };
    BENCHMARK("GD  n=100") { return run_solver<GradientDescent<double>, Armijo<double>>(100); };
}

TEST_CASE (
"kalpa bench: Newton on a separable quadratic"
,
"[kalpa][bench][!benchmark]"
)
 {
    CHECK(run_solver<Newton<double>, Wolfe<double>>(30) < 1e-6);

    BENCHMARK("Newton  n=10") { return run_solver<Newton<double>, Wolfe<double>>(10); };
    BENCHMARK("Newton  n=30") { return run_solver<Newton<double>, Wolfe<double>>(30); };
}

TEST_CASE (
"kalpa bench: serial vs parallel population evaluation"
,
"[kalpa][bench][!benchmark]"
)
 {
    // Cross-check: serial and pravaha-parallel DE reach the same neighborhood
    // of the optimum from a fixed seed (differential validation of the parallel
    // eval path against the scalar path).
    auto lo = ga::Vector<double>(8, -10.0);
    auto hi = ga::Vector<double>(8,  10.0);

    DifferentialEvolution<double, SerialEval>   de_ser; de_ser.max_gen = 120; de_ser.pop_size = 60;
    DifferentialEvolution<double, ParallelEval> de_par; de_par.max_gen = 120; de_par.pop_size = 60;

    auto rs = de_ser.solve(SepQuad{}, lo, hi, Rng{2025});
    auto rp = de_par.solve(SepQuad{}, lo, hi, Rng{2025});
    REQUIRE(rs.has_value()); REQUIRE(rp.has_value());
    CHECK(rs->f < 5.0);
    CHECK(rp->f < 5.0);   // parallel path is correct, not just fast

    BENCHMARK("DE serial   pop=60 n=8") {
        DifferentialEvolution<double, SerialEval> de; de.max_gen = 120; de.pop_size = 60;
        return de.solve(SepQuad{}, lo, hi, Rng{2025})->f;
    };
    BENCHMARK("DE parallel pop=60 n=8") {
        DifferentialEvolution<double, ParallelEval> de; de.max_gen = 120; de.pop_size = 60;
        return de.solve(SepQuad{}, lo, hi, Rng{2025})->f;
    };
}

// ===========================================================================
// Nonlinear least-squares scaling. Residuals rᵢ(x) = xᵢ − i over m = n comps
// (SepQuad's per-term structure as an NLS): minimum xᵢ = i, ‖r‖ = 0. Scales
// with the residual/parameter count.
// ===========================================================================
namespace {
    // r_i(x) = x[i] − i. One residual functor per component, built at a fixed
    // index; file scope for the templated call operator.
    struct ResIdx {
        std::size_t i;

        template <typename V>
        auto operator()(const V& x) const {
            using S = typename V::value_type;
            return x[i] - S(static_cast<double>(i));
        }
    };

    std::vector<ResIdx> make_residuals(std::size_t n) {
        std::vector<ResIdx> r;
        r.reserve(n);
        for (std::size_t i = 0; i < n; ++i) r.push_back(ResIdx{i});
        return r;
    }

    double run_lm(std::size_t n) {
        LevenbergMarquardt<double> lm;
        auto r = lm.solve(make_residuals(n), zeros(n));
        return r.has_value() ? r->residual_norm : 1e9;
    }
}

TEST_CASE (
"kalpa bench: Levenberg–Marquardt scaling"
,
"[kalpa][bench][!benchmark]"
)
 {
    CHECK(run_lm(50) < 1e-6);      // correctness guard

    BENCHMARK("LM  n=10")  { return run_lm(10); };
    BENCHMARK("LM  n=50")  { return run_lm(50); };
    BENCHMARK("LM  n=200") { return run_lm(200); };
}

// ===========================================================================
// Inequality-SQP timing on the canonical  min ‖x‖²  s.t.  2−x₀−x₁ ≤ 0.
// ===========================================================================
namespace {
    struct BenchSumSq2 {
        template <typename V>
        auto operator()(const V& x) const { return x[0] * x[0] + x[1] * x[1]; }
    };

    struct BenchIneqGe2 {
        template <typename V>
        auto operator()(const V& x) const {
            using S = typename V::value_type;
            return S{2} - x[0] - x[1];
        }
    };

    double run_sqp_ineq() {
        std::vector<BenchIneqGe2> ineq{BenchIneqGe2{}};
        std::vector<BenchIneqGe2> eq{}; // empty; reuse type
        SQP_Ineq<double> sqp;
        ga::Vector<double> x0(2, 0.0);
        // empty eq set of a distinct dummy type would work too; use no equalities
        std::vector<BenchSumSq2> none{};
        auto r = sqp.solve(BenchSumSq2{}, x0, Derivatives<Dual, double>{}, none, ineq);
        return r.has_value() ? r->f : 1e9;
    }
}

TEST_CASE (
"kalpa bench: inequality SQP timing"
,
"[kalpa][bench][!benchmark]"
)
 {
    CHECK(run_sqp_ineq() == Catch::Approx(2.0).margin(1e-2));

    BENCHMARK("SQP_Ineq  (2 var, 1 ineq)") { return run_sqp_ineq(); };
}
