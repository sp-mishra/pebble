#include <catch_amalgamated.hpp>
#include <kalpa/kalpa.hpp>
#include <kalpa/algo/unconstrained.hpp>
#include <kalpa/algo/global.hpp>
#include <containers/matrix/dense.hpp>

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
        template<typename V> auto operator()(const V& x) const {
            using S = typename V::value_type;
            S s{};
            for (std::size_t i = 0; i < x.size(); ++i) {
                S d = x[i] - S(static_cast<double>(i));
                s = s + d*d;
            }
            return s;
        }
    };

    ga::Vector<double> zeros(std::size_t n) { return ga::Vector<double>(n, 0.0); }

    template<typename Algo, typename LS>
    double run_solver(std::size_t n) {
        auto prob = make_problem<double>(SepQuad{});
        Solver<Algo, Derivatives<Dual,double>, LS> s;
        auto r = s.solve(prob, zeros(n));
        return r.has_value() ? r->grad_norm : 1e9;
    }
}

TEST_CASE("kalpa bench: L-BFGS scaling", "[kalpa][bench][!benchmark]") {
    // correctness guard at a representative size
    CHECK(run_solver<LBFGS<double>, Wolfe<double>>(50) < 1e-4);

    BENCHMARK("L-BFGS  n=20")  { return run_solver<LBFGS<double>, Wolfe<double>>(20); };
    BENCHMARK("L-BFGS  n=100") { return run_solver<LBFGS<double>, Wolfe<double>>(100); };
    BENCHMARK("L-BFGS  n=400") { return run_solver<LBFGS<double>, Wolfe<double>>(400); };
}

TEST_CASE("kalpa bench: gradient descent scaling", "[kalpa][bench][!benchmark]") {
    CHECK(run_solver<GradientDescent<double>, Armijo<double>>(50) < 1e-2);

    BENCHMARK("GD  n=20")  { return run_solver<GradientDescent<double>, Armijo<double>>(20); };
    BENCHMARK("GD  n=100") { return run_solver<GradientDescent<double>, Armijo<double>>(100); };
}

TEST_CASE("kalpa bench: Newton on a separable quadratic", "[kalpa][bench][!benchmark]") {
    CHECK(run_solver<Newton<double>, Wolfe<double>>(30) < 1e-6);

    BENCHMARK("Newton  n=10") { return run_solver<Newton<double>, Wolfe<double>>(10); };
    BENCHMARK("Newton  n=30") { return run_solver<Newton<double>, Wolfe<double>>(30); };
}

TEST_CASE("kalpa bench: serial vs parallel population evaluation", "[kalpa][bench][!benchmark]") {
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
