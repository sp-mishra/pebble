// ============================================================================
// src/tests/containers/test_prakriti_compute.cpp — ComputeBackend parity across tiers.
// The ScalarBackend (tier 1) is reference. HighwayBackend (tier 2, Google Highway)
// and PravahaBackend (tier 3, parallel task graph) must reproduce it on all primitives.
// ============================================================================
#include "catch_amalgamated.hpp"
#include "prakriti/compute/scalar_backend.hpp"
#include "prakriti/compute/highway_backend.hpp"
#include "prakriti/compute/pravaha_backend.hpp"
#include <vector>
#include <random>
#include <cmath>

using namespace prakriti;

namespace {
struct Inputs {
    std::vector<Scalar> base, mask, v, s, p, q;
    explicit Inputs(std::size_t n) : base(n), mask(n), v(n), s(n), p(n), q(n) {
        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> U(-4, 4);
        for (std::size_t i = 0; i < n; ++i) {
            base[i] = U(rng); mask[i] = (i % 6) ? 1.f : 0.f;
            v[i] = U(rng); s[i] = U(rng); p[i] = U(rng); q[i] = U(rng);
        }
    }
};

// Run all six primitives on a backend, return concatenated outputs for comparison.
template <class B>
std::vector<Scalar> run_all(const B& b, const Inputs& in, Scalar k) {
    const std::size_t n = in.base.size();
    std::vector<Scalar> o_axpy = in.v, o_mul = in.p, o_clamp = in.p, o_copy(n);
    std::vector<Scalar> o_pred(n), o_sub(n);
    b.axpy_const_masked({o_axpy.data(), n}, {in.mask.data(), n}, k);
    b.predict({o_pred.data(), n}, {in.base.data(), n}, {in.mask.data(), n}, {in.v.data(), n}, k);
    b.sub_scale({o_sub.data(), n}, {in.p.data(), n}, {in.q.data(), n}, k);
    b.mul_col({o_mul.data(), n}, {in.s.data(), n});
    b.copy({o_copy.data(), n}, {in.base.data(), n});
    b.clamp({o_clamp.data(), n}, Scalar(-2), Scalar(2));
    std::vector<Scalar> all;
    for (auto* vptr : {&o_axpy, &o_pred, &o_sub, &o_mul, &o_copy, &o_clamp})
        all.insert(all.end(), vptr->begin(), vptr->end());
    return all;
}

Scalar max_abs_diff(const std::vector<Scalar>& a, const std::vector<Scalar>& b) {
    Scalar m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}
} // namespace

TEST_CASE("ScalarBackend primitives are self-consistent", "[prakriti][compute]") {
    Inputs in(257);
    const auto a = run_all(ScalarBackend{}, in, 0.017f);
    const auto b = run_all(ScalarBackend{}, in, 0.017f);
    REQUIRE(max_abs_diff(a, b) == Catch::Approx(0));
}

#if defined(PRAKRITI_HAS_HIGHWAY_BACKEND)
TEST_CASE("HighwayBackend matches ScalarBackend", "[prakriti][compute][highway]") {
    Inputs in(1003); // non-lane-multiple exercises the scalar tail
    const auto ref = run_all(ScalarBackend{}, in, 0.017f);
    const auto got = run_all(HighwayBackend{}, in, 0.017f);
    REQUIRE(max_abs_diff(ref, got) < 1e-4f);

    // Vectorized kinetic energy reduction test
    Scalar ke_scalar = ScalarBackend{}.kinetic_energy({in.v.data(), in.v.size()}, {in.s.data(), in.s.size()}, {in.mask.data(), in.mask.size()});
    Scalar ke_hwy    = HighwayBackend{}.kinetic_energy({in.v.data(), in.v.size()}, {in.s.data(), in.s.size()}, {in.mask.data(), in.mask.size()});
    REQUIRE(ke_scalar > 0.0f);
    REQUIRE(std::abs(ke_scalar - ke_hwy) < 1e-3f);
}
#endif

#if defined(PRAKRITI_HAS_PRAVAHA_BACKEND)
TEST_CASE("PravahaBackend matches ScalarBackend", "[prakriti][compute][pravaha]") {
    Inputs in(2048);
    const auto ref = run_all(ScalarBackend{}, in, 0.017f);
    const auto got = run_all(PravahaBackend{2, 256}, in, 0.017f);
    REQUIRE(max_abs_diff(ref, got) < 1e-4f);

    Scalar ke_scalar  = ScalarBackend{}.kinetic_energy({in.v.data(), in.v.size()}, {in.s.data(), in.s.size()}, {in.mask.data(), in.mask.size()});
    Scalar ke_pravaha = PravahaBackend{2, 256}.kinetic_energy({in.v.data(), in.v.size()}, {in.s.data(), in.s.size()}, {in.mask.data(), in.mask.size()});
    REQUIRE(ke_scalar > 0.0f);
    REQUIRE(std::abs(ke_scalar - ke_pravaha) < 1e-3f);
}
#endif

