#include <catch_amalgamated.hpp>
#include <kalpa/kalpa.hpp>
#include <kalpa/algo/global.hpp>
#include <containers/matrix/dense.hpp>
#include <cmath>
#include <numbers>

using namespace kalpa;

// ===========================================================================
// Global-optimization test objectives. Plain (Vector<double> → double) — the
// derivative-free methods never differentiate, so no Dual overload needed.
// File scope: TEST_CASE-local classes may not carry the templated call op the
// solvers instantiate. All are non-negative with a known global minimum 0.
// ===========================================================================
namespace {
    ga::Vector<double> vN(std::initializer_list<double> xs) {
        ga::Vector<double> v(xs.size()); std::size_t i = 0;
        for (double x : xs) v[i++] = x;
        return v;
    }

    // Sphere — convex, min 0 at origin. Sanity oracle.
    struct Sphere {
        double operator()(const ga::Vector<double>& x) const {
            double s = 0; for (std::size_t i = 0; i < x.size(); ++i) s += x[i]*x[i];
            return s;
        }
    };

    // Rastrigin — highly multimodal, min 0 at origin.
    struct Rastrigin {
        double operator()(const ga::Vector<double>& x) const {
            const double A = 10.0;
            double s = A * static_cast<double>(x.size());
            for (std::size_t i = 0; i < x.size(); ++i)
                s += x[i]*x[i] - A*std::cos(2.0*std::numbers::pi*x[i]);
            return s;
        }
    };

    // Rosenbrock (2D) — curved valley, min 0 at (1,1).
    struct Rosen {
        double operator()(const ga::Vector<double>& x) const {
            double a = 1.0 - x[0], b = x[1] - x[0]*x[0];
            return a*a + 100.0*b*b;
        }
    };
}

TEST_CASE("kalpa: CMA-ES minimizes the sphere to near zero", "[kalpa][global][cmaes]") {
    CMAES<double> es; es.max_gen = 400; es.sigma0 = 0.5;
    auto r = es.solve(Sphere{}, vN({3.0, -2.0, 1.5}), Rng{42});
    REQUIRE(r.has_value());
    CHECK(r->f == Catch::Approx(0.0).margin(1e-6));
    for (std::size_t i = 0; i < r->x.size(); ++i)
        CHECK(std::abs(r->x[i]) < 1e-3);
}

TEST_CASE("kalpa: CMA-ES escapes Rastrigin local minima", "[kalpa][global][cmaes]") {
    CMAES<double> es; es.max_gen = 600; es.sigma0 = 2.0;
    auto r = es.solve(Rastrigin{}, vN({4.0, -3.0}), Rng{7});
    REQUIRE(r.has_value());
    // Start f(4,-3) ≈ 25; a single CMA-ES run reliably escapes the outer rings
    // and settles in the innermost cluster of minima. It does not guarantee the
    // exact global basin from one restart, so accept the low-ring neighborhood
    // (Rastrigin's minima sit on the integer lattice; the first ring is f≈1).
    CHECK(r->f < 3.0);
}

TEST_CASE("kalpa: differential evolution solves Rastrigin", "[kalpa][global][de]") {
    DifferentialEvolution<double> de; de.max_gen = 400; de.pop_size = 40;
    auto r = de.solve(Rastrigin{}, vN({-5.12,-5.12}), vN({5.12,5.12}), Rng{123});
    REQUIRE(r.has_value());
    CHECK(r->f < 1.0);
    CHECK(std::abs(r->x[0]) < 0.5);
    CHECK(std::abs(r->x[1]) < 0.5);
}

TEST_CASE("kalpa: Nelder-Mead descends Rosenbrock", "[kalpa][global][neldermead]") {
    NelderMead<double> nm; nm.max_iter = 4000; nm.step = 0.3;
    auto r = nm.solve(Rosen{}, vN({-1.2, 1.0}));
    REQUIRE(r.has_value());
    CHECK(r->x[0] == Catch::Approx(1.0).margin(1e-2));
    CHECK(r->x[1] == Catch::Approx(1.0).margin(1e-2));
}

TEST_CASE("kalpa: simulated annealing reduces the sphere objective", "[kalpa][global][sa]") {
    SimulatedAnnealing<double> sa; sa.max_iter = 20000; sa.T0 = 2.0; sa.proposal_sd = 0.4;
    auto r = sa.solve(Sphere{}, vN({5.0, -4.0}), Rng{99});
    REQUIRE(r.has_value());
    CHECK(r->f < 1e-1);                // stochastic — coarse tolerance
}

// ===========================================================================
// Determinism: identical seed → identical trajectory / result (hard plan req).
// ===========================================================================
TEST_CASE("kalpa: CMA-ES is deterministic under a fixed seed", "[kalpa][global][determinism]") {
    CMAES<double> es; es.max_gen = 100;
    auto a = es.solve(Sphere{}, vN({2.0, 2.0}), Rng{2024});
    auto b = es.solve(Sphere{}, vN({2.0, 2.0}), Rng{2024});
    REQUIRE(a.has_value()); REQUIRE(b.has_value());
    CHECK(a->f == b->f);
    CHECK(a->x[0] == b->x[0]);
    CHECK(a->x[1] == b->x[1]);
}

TEST_CASE("kalpa: DE is deterministic under a fixed seed", "[kalpa][global][determinism]") {
    DifferentialEvolution<double> de; de.max_gen = 80; de.pop_size = 30;
    auto a = de.solve(Rastrigin{}, vN({-5.0,-5.0}), vN({5.0,5.0}), Rng{555});
    auto b = de.solve(Rastrigin{}, vN({-5.0,-5.0}), vN({5.0,5.0}), Rng{555});
    REQUIRE(a.has_value()); REQUIRE(b.has_value());
    CHECK(a->f == b->f);
    CHECK(a->x[0] == b->x[0]);
    CHECK(a->x[1] == b->x[1]);
}

TEST_CASE("kalpa: different seeds give different DE trajectories", "[kalpa][global][determinism]") {
    DifferentialEvolution<double> de; de.max_gen = 40; de.pop_size = 30;
    auto a = de.solve(Rastrigin{}, vN({-5.0,-5.0}), vN({5.0,5.0}), Rng{1});
    auto b = de.solve(Rastrigin{}, vN({-5.0,-5.0}), vN({5.0,5.0}), Rng{2});
    REQUIRE(a.has_value()); REQUIRE(b.has_value());
    // near-certain to differ before full convergence
    CHECK(a->x[0] != b->x[0]);
}
