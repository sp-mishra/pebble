// ============================================================================
// src/tests/containers/test_prakriti_material.cpp — phase blend, temperature->phase, EOS, constitutive law.
// ============================================================================
#include "catch_amalgamated.hpp"
#include "prakriti/material/phase.hpp"
#include "prakriti/material/eos.hpp"
#include "prakriti/material/constitutive.hpp"
#include "prakriti/state/material_registry.hpp"

using namespace prakriti;

TEST_CASE("phase fractions sum to one across temperature", "[prakriti][material]") {
    auto p = MaterialRegistry::water();
    for (Scalar t = -50; t <= 200; t += 10) {
        auto pf = phase_from_temperature(t, p);
        const Scalar s = pf.solid() + pf.plastic() + pf.liquid() + pf.gas();
        REQUIRE(s == Catch::Approx(1.0f).margin(1e-4));
    }
}

TEST_CASE("cold water is solid, hot is gas", "[prakriti][material]") {
    auto p = MaterialRegistry::water();
    auto cold = phase_from_temperature(-20, p);
    REQUIRE(cold.solid() > 0.9f);
    auto hot = phase_from_temperature(200, p);
    REQUIRE(hot.gas() > 0.9f);
    auto mid = phase_from_temperature(50, p);
    REQUIRE(mid.liquid() > 0.5f);
}

TEST_CASE("phase_blend is barycentric", "[prakriti][material]") {
    PhaseFractions pf; pf.f = {0.25f, 0.25f, 0.25f, 0.25f};
    std::array<Scalar, kPhaseCount> vals{1, 2, 3, 4};
    REQUIRE(phase_blend(vals, pf) == Catch::Approx(2.5f));
}

TEST_CASE("Tait EOS rises above rest density, clamps negatives", "[prakriti][material]") {
    auto p = MaterialRegistry::water();
    const Scalar rho0 = p.rest_density;
    Scalar p_hi = tait_pressure(rho0 * 1.1f, rho0, p, 0, 20, true);
    Scalar p_eq = tait_pressure(rho0,        rho0, p, 0, 20, true);
    REQUIRE(p_hi > p_eq);
    // Below rest density with clamp on -> zero.
    Scalar p_lo = tait_pressure(rho0 * 0.5f, rho0, p, 0, 20, true);
    REQUIRE(p_lo == Catch::Approx(0));
    // Gas fraction adds thermal expansion pressure.
    Scalar p_gas = tait_pressure(rho0, rho0, p, 1.0f, 100, true);
    REQUIRE(p_gas > p_eq);
}

TEST_CASE("DefaultMaterialLaw coefficients", "[prakriti][material]") {
    DefaultMaterialLaw law;
    auto p = MaterialRegistry::water();
    PhaseFractions liquid; liquid.f = {0, 0, 1, 0};
    // structural_alpha inflates compliance with damage.
    REQUIRE(law.structural_alpha(1e-3f, 0.0f) == Catch::Approx(1e-3f));
    REQUIRE(law.structural_alpha(1e-3f, 0.5f) > 1e-3f);
    // gas lowers target density.
    PhaseFractions gas; gas.f = {0, 0, 0, 1};
    REQUIRE(law.target_density(p, gas) < law.target_density(p, liquid));
}

TEST_CASE("dry ice sublimates directly from solid to gas", "[prakriti][material][sublimation]") {
    auto dry_ice = MaterialRegistry::dry_ice();
    auto cold = phase_from_temperature(-100, dry_ice);
    REQUIRE(cold.solid() > 0.9f);
    REQUIRE(cold.liquid() == Catch::Approx(0.0f));

    auto warm = phase_from_temperature(20, dry_ice);
    REQUIRE(warm.gas() > 0.9f);
    REQUIRE(warm.liquid() == Catch::Approx(0.0f));
}

