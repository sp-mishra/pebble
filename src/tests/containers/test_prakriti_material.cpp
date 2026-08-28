// ============================================================================
// src/tests/containers/test_prakriti_material.cpp — phase blend, temperature->phase, EOS, constitutive law.
// ============================================================================
#include "catch_amalgamated.hpp"
#include "prakriti/material/phase.hpp"
#include "prakriti/material/eos.hpp"
#include "prakriti/material/constitutive.hpp"
#include "prakriti/state/material_registry.hpp"
#include "containers/spatial/spatial_hash_grid.hpp"
#include "containers/dynamic/soa_vector.hpp"
#include "containers/spatial/barnes_hut.hpp"
#include "gati/stepper/block_stepper.hpp"
#include "gati/world/spatial_tile_streamer.hpp"

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

#include "prakriti/material/celestial.hpp"

TEST_CASE("comet tail sublimation activates near hot stars", "[prakriti][celestial][comet]") {
    pebble::math::vec2 star_pos{0.0f, 0.0f};
    const float star_temp = 5500.0f;
    const float star_radius = 12.0f;

    // 1. Cold comet far away (-80 C) -> No sublimation
    pebble::math::vec2 cold_pos{1000.0f, 0.0f};
    auto res_cold = prakriti::celestial::evaluate_comet_tail_sublimation(cold_pos, -80.0f, star_pos, star_temp, star_radius, 0.016f);
    REQUIRE_FALSE(res_cold.is_sublimating);

    // 2. Active comet at perihelion (T = 50 C, distance = 100 px) -> Outgassing tail pointing radially away
    pebble::math::vec2 active_pos{100.0f, 0.0f};
    auto res_active = prakriti::celestial::evaluate_comet_tail_sublimation(active_pos, 50.0f, star_pos, star_temp, star_radius, 0.016f);
    REQUIRE(res_active.is_sublimating);
    REQUIRE(res_active.mass_loss_rate > 0.0f);
    REQUIRE(res_active.tail_direction[0] == Catch::Approx(1.0f).margin(1e-3));
    REQUIRE(res_active.tail_direction[1] == Catch::Approx(0.0f).margin(1e-3));
    REQUIRE(res_active.is_ion_plasma);
}

TEST_CASE("open world boundless culling and rim detection", "[prakriti][celestial][open_world]") {
    pebble::math::vec2 center{640.0f, 400.0f};

    // Body inside active viewport
    pebble::math::vec2 in_bounds{600.0f, 400.0f};
    auto cull_in = prakriti::celestial::evaluate_open_world_bounds(in_bounds, center, 3000.0f, 1500.0f);
    REQUIRE_FALSE(cull_in.should_recycle);
    REQUIRE_FALSE(cull_in.is_in_outer_rim);

    // Body in outer rim
    pebble::math::vec2 rim_pos{2400.0f, 400.0f};
    auto cull_rim = prakriti::celestial::evaluate_open_world_bounds(rim_pos, center, 3000.0f, 1500.0f);
    REQUIRE_FALSE(cull_rim.should_recycle);
    REQUIRE(cull_rim.is_in_outer_rim);

    // Body far in deep space
    pebble::math::vec2 deep_pos{5000.0f, 400.0f};
    auto cull_deep = prakriti::celestial::evaluate_open_world_bounds(deep_pos, center, 3000.0f, 1500.0f);
    REQUIRE(cull_deep.should_recycle);
}

TEST_CASE("external inflow entity generation", "[prakriti][celestial][inflow]") {
    const float vw = 1280.0f;
    const float vh = 800.0f;
    
    auto galaxy = prakriti::celestial::generate_random_inflow(vw, vh, 200.0f, 0.0f, 0.5f, prakriti::celestial::InflowEntityType::RogueProtogalaxy);
    REQUIRE(galaxy.core_mass >= 400.0f);
    REQUIRE(galaxy.satellite_count >= 8);
    // Spawns outside viewport perimeter
    REQUIRE(galaxy.spawn_pos[0] > vw);

    auto star = prakriti::celestial::generate_random_inflow(vw, vh, 200.0f, 3.14159f, 0.8f, prakriti::celestial::InflowEntityType::HypervelocityStar);
    REQUIRE(star.core_mass >= 300.0f);
    REQUIRE(star.satellite_count == 0);
    // Ingress velocity directed inwards
    REQUIRE(star.ingress_vel[0] > 0.0f);
}

TEST_CASE("roche lobe overflow mass transfer", "[prakriti][celestial][rlof]") {
    // Large donor star (R = 25 px, M = 200) close to massive accretor (M = 800) at distance 40 px
    auto res = prakriti::celestial::evaluate_roche_lobe_overflow(200.0f, 25.0f, 800.0f, 40.0f, 0.016f);
    REQUIRE(res.is_overflowing);
    REQUIRE(res.roche_lobe_radius < 25.0f);
    REQUIRE(res.mass_transfer_rate > 0.0f);

    // Far away star -> No overflow
    auto res_far = prakriti::celestial::evaluate_roche_lobe_overflow(200.0f, 25.0f, 800.0f, 300.0f, 0.016f);
    REQUIRE_FALSE(res_far.is_overflowing);
}

TEST_CASE("tidal locking dissipation torque", "[prakriti][celestial][tidal]") {
    // Rapidly rotating moon (omega = 25 rad/s) in close orbit (d = 30 px, v_rel = 15 px/s -> Omega_orb = 0.5 rad/s)
    auto res = prakriti::celestial::evaluate_tidal_locking_torque(
        50.0f, 6.0f, 25.0f, 500.0f, 30.0f, 15.0f, 0.016f
    );
    // Torque should oppose spin discrepancy (negative torque)
    REQUIRE(res.spin_torque < 0.0f);
    REQUIRE(res.orbital_damping_force > 0.0f);
}

TEST_CASE("sedov-taylor blast wave expansion", "[prakriti][celestial][sedov]") {
    const float e_blast = 2000.0f;
    const float rho = 1.0f;
    
    float r1 = prakriti::celestial::compute_sedov_taylor_radius(e_blast, rho, 0.1f);
    float r2 = prakriti::celestial::compute_sedov_taylor_radius(e_blast, rho, 0.5f);
    REQUIRE(r1 > 0.0f);
    REQUIRE(r2 > r1); // Self-similar expansion over time
}

TEST_CASE("mhd magnetic flux tube between rotating stars", "[prakriti][celestial][mhd]") {
    pebble::math::vec2 p1{100.0f, 100.0f};
    pebble::math::vec2 p2{200.0f, 100.0f};
    auto tube = prakriti::celestial::compute_mhd_flux_tube(p1, 300.0f, 15.0f, p2, 400.0f, -15.0f);
    REQUIRE(tube.field_strength > 0.0f);
    REQUIRE(tube.current_intensity > 0.0f);
}

TEST_CASE("planetary jeans atmospheric retention and escape", "[prakriti][celestial][jeans]") {
    // 1. Massive, cold planet (M = 150, R = 4.0, T = 15 C) -> High retention
    auto retained = prakriti::celestial::evaluate_jeans_atmospheric_escape(150.0f, 4.0f, 15.0f, 0.0f, 0.016f);
    REQUIRE(retained.retains_atmosphere);
    REQUIRE(retained.escape_velocity > retained.thermal_velocity);

    // 2. Lightweight, hot asteroid (M = 8, R = 1.5, T = 800 C) -> Active blowout loss
    auto escaped = prakriti::celestial::evaluate_jeans_atmospheric_escape(8.0f, 1.5f, 800.0f, 5.0f, 0.016f);
    REQUIRE_FALSE(escaped.retains_atmosphere);
    REQUIRE(escaped.jeans_loss_rate > 0.0f);
}

TEST_CASE("surface hydrology and liquid ocean condensation", "[prakriti][celestial][hydrology]") {
    // Habitable temperate planet with water volatile content
    auto hydro_hab = prakriti::celestial::evaluate_surface_hydrology_phase(22.0f, 0.4f, 120.0f);
    REQUIRE(hydro_hab.in_habitable_zone);
    REQUIRE(hydro_hab.crust_solidification > 0.8f);
    REQUIRE(hydro_hab.ocean_coverage > 0.2f);

    // Scorching molten magma planet (T = 1200 C) -> No liquid oceans
    auto hydro_hot = prakriti::celestial::evaluate_surface_hydrology_phase(1200.0f, 0.4f, 120.0f);
    REQUIRE_FALSE(hydro_hot.in_habitable_zone);
    REQUIRE(hydro_hot.ocean_coverage == Catch::Approx(0.0f));
}

TEST_CASE("strange quark star deconfined phase transition", "[prakriti][celestial][strange_star]") {
    // High-mass neutron star under extreme core pressure
    auto trans = prakriti::celestial::evaluate_strange_star_transition(860.0f, true, 240.0f);
    REQUIRE(trans.triggers_strange_star);
    REQUIRE(trans.deconfined_energy_release > 0.0f);
    REQUIRE(trans.strange_radius < 2.6f);
}

TEST_CASE("pulsar timing array nano-hertz gw modulation", "[prakriti][celestial][pta]") {
    const float base_omega = 45.0f;
    const float gw_strain = 0.4f;
    auto pta = prakriti::celestial::evaluate_pulsar_gw_timing_residual(base_omega, gw_strain, 2.0f, 0.125f);
    REQUIRE(pta.modulated_spin != base_omega);
    REQUIRE(std::abs(pta.timing_shift_ns) > 0.0f);
}

#include "prakriti/celestial/sector_types.hpp"
#include "prakriti/celestial/sector_generator.hpp"
#include "prakriti/celestial/sector_multipole.hpp"
#include "prakriti/celestial/sector_cache_manager.hpp"

TEST_CASE("splitmix64 spatial hash determinism and sector generation", "[prakriti][celestial][openworld]") {
    prakriti::celestial::SectorKey k1{4, -8};
    prakriti::celestial::SectorKey k2{4, -8};
    prakriti::celestial::SectorKey k3{5, -8};

    // Identical coordinates produce identical 64-bit seeds
    REQUIRE(prakriti::celestial::compute_sector_seed(k1.x, k1.y) == prakriti::celestial::compute_sector_seed(k2.x, k2.y));
    REQUIRE(prakriti::celestial::compute_sector_seed(k1.x, k1.y) != prakriti::celestial::compute_sector_seed(k3.x, k3.y));

    // Procedural matter generation consistency
    auto sec1 = prakriti::celestial::generate_procedural_sector(k1);
    auto sec2 = prakriti::celestial::generate_procedural_sector(k2);
    REQUIRE(sec1.bodies.size() == sec2.bodies.size());
    REQUIRE(sec1.total_mass == Catch::Approx(sec2.total_mass));
    REQUIRE(sec1.barycenter_x == Catch::Approx(sec2.barycenter_x));
}

TEST_CASE("fast multipole method far field gravity tensor", "[prakriti][celestial][fmm]") {
    prakriti::celestial::SectorData sec;
    sec.total_mass = 12000.0f;
    sec.barycenter_x = 10000.0f;
    sec.barycenter_y = 10000.0f;
    sec.quadrupole.qxx = 50000.0f;
    sec.quadrupole.qyy = 50000.0f;

    pebble::math::vec2 probe_pos{0.0f, 0.0f};
    auto a_grav = prakriti::celestial::compute_sector_far_field_gravity(probe_pos, sec, 18000.0f);

    // Gravity vector should pull towards the distant sector barycenter
    REQUIRE(a_grav[0] > 0.0f);
    REQUIRE(a_grav[1] > 0.0f);
}

TEST_CASE("glaze sector data serialization and kosha cache", "[prakriti][celestial][glaze_cache]") {
    prakriti::celestial::SectorCacheManager mgr(16);

    prakriti::celestial::SectorKey key{12, -3};
    auto sec = mgr.get_or_generate_sector(key, 9999ULL);
    REQUIRE(!sec.bodies.empty());

    // Modify sector state (simulate celestial evolution)
    sec.bodies[0].mass = 999.0f;
    mgr.freeze_sector(sec);

    // Re-fetch from cache
    auto sec_restored = mgr.get_or_generate_sector(key, 9999ULL);
    REQUIRE(sec_restored.bodies[0].mass == Catch::Approx(999.0f));
}

TEST_CASE("dormant sector macro node collective gravity calculation", "[prakriti][celestial][collective_gravity]") {
    prakriti::celestial::SectorMacroNode macro;
    macro.total_mass = 5000.0f;
    macro.bx = 1200.0f;
    macro.by = 800.0f;
    macro.q.qxx = 200.0f;
    macro.q.qyy = 200.0f;

    pebble::math::vec2 probe{0.0f, 0.0f};
    auto a_coll = prakriti::celestial::compute_collective_macro_gravity(probe, macro, 18000.0f);

    REQUIRE(a_coll[0] > 0.0f);
    REQUIRE(a_coll[1] > 0.0f);
}

TEST_CASE("spatial hash grid insertion and neighbor query", "[containers][spatial][spatial_hash_grid]") {
    containers::spatial::SpatialHashGrid<std::uint32_t, 32.0f, 1024> grid(100);
    grid.insert(1, 10.0f, 10.0f);
    grid.insert(2, 15.0f, 12.0f);
    grid.insert(3, 500.0f, 500.0f);

    REQUIRE(grid.size() == 3);

    std::vector<std::uint32_t> found;
    grid.for_each_neighbor(12.0f, 11.0f, [&](std::uint32_t id, float x, float y) {
        (void)x; (void)y;
        found.push_back(id);
    });

    REQUIRE(found.size() == 2);
    REQUIRE(std::find(found.begin(), found.end(), 1) != found.end());
    REQUIRE(std::find(found.begin(), found.end(), 2) != found.end());
    REQUIRE(std::find(found.begin(), found.end(), 3) == found.end());
}

TEST_CASE("soa vector contiguous column storage and swap pop", "[containers][dynamic][soa_vector]") {
    containers::dynamic::SoAVector<float, float, int> soa;
    soa.push_back(1.0f, 2.0f, 100);
    soa.push_back(3.0f, 4.0f, 200);
    soa.push_back(5.0f, 6.0f, 300);

    REQUIRE(soa.size() == 3);

    auto col0 = soa.get_column<0>();
    REQUIRE(col0[0] == Catch::Approx(1.0f));
    REQUIRE(col0[1] == Catch::Approx(3.0f));
    REQUIRE(col0[2] == Catch::Approx(5.0f));

    soa.swap_pop_back(0);
    REQUIRE(soa.size() == 2);
    REQUIRE(soa.get_column<0>()[0] == Catch::Approx(5.0f));
}

TEST_CASE("hierarchical block stepper rung computation", "[gati][stepper][block_stepper]") {
    const auto rung_slow = gati::stepper::compute_acceleration_rung(0.001f, 10.0f);
    const auto rung_fast = gati::stepper::compute_acceleration_rung(500.0f, 20.0f);

    REQUIRE(rung_slow == 0);
    REQUIRE(rung_fast >= 2);
}

TEST_CASE("static and small soa vector policy storage", "[containers][dynamic][soa_vector]") {
    containers::dynamic::StaticSoAVector<8, float, float> static_soa;
    static_soa.push_back(10.0f, 20.0f);
    static_soa.push_back(30.0f, 40.0f);
    REQUIRE(static_soa.size() == 2);
    REQUIRE(static_soa.get_column<0>()[0] == Catch::Approx(10.0f));

    containers::dynamic::SmallSoAVector<4, float, float> small_soa;
    small_soa.push_back(100.0f, 200.0f);
    REQUIRE(small_soa.size() == 1);
    REQUIRE(small_soa.get_column<1>()[0] == Catch::Approx(200.0f));
}

TEST_CASE("gati spatial tile streamer viewport tracking", "[gati][world][spatial_tile_streamer]") {
    gati::world::SpatialTileStreamer<320.0f, 200.0f> streamer;
    std::size_t discovered = 0;
    std::size_t active = 0;

    streamer.update_viewport(
        pebble::math::vec2{160.0f, 100.0f},
        320.0f, 200.0f, 0.0f,
        [&](const gati::world::TileCoord&) { ++discovered; },
        [&](const gati::world::TileCoord&) { ++active; }
    );

    REQUIRE(discovered > 0);
    REQUIRE(active > 0);
    REQUIRE(streamer.visited_count() == discovered);
}

TEST_CASE("barnes hut parallel force sweep calculation", "[containers][spatial][barnes_hut]") {
    containers::spatial::BarnesHutTree tree;
    std::vector<containers::spatial::BarnesHutBody> bodies = {
        {.pos = {0.0f, 0.0f}, .vel = {0.0f, 0.0f}, .mass = 1000.0f, .id = 0},
        {.pos = {100.0f, 0.0f}, .vel = {0.0f, 0.0f}, .mass = 10.0f, .id = 1},
        {.pos = {-100.0f, 0.0f}, .vel = {0.0f, 0.0f}, .mass = 10.0f, .id = 2}
    };

    tree.build(bodies);
    std::vector<pebble::math::vec2> forces(bodies.size());
    containers::spatial::compute_all_forces(tree, bodies, forces);

    REQUIRE(forces.size() == 3);
    // Body 1 at (100, 0) should feel gravitational pull towards (0, 0) (negative X force)
    REQUIRE(forces[1][0] < 0.0f);
    // Body 2 at (-100, 0) should feel gravitational pull towards (0, 0) (positive X force)
    REQUIRE(forces[2][0] > 0.0f);
}





