// ============================================================================
// src/app/pebble_gati_prakriti.cpp — Gati + Prakriti End-to-End Thermodynamic Boiling Showcase
// ============================================================================
// Complete integration of Gati (ECS, fixed-step clock, system schedule, transform interpolation)
// and Prakriti (PBF fluids, continuum thermodynamics, boiling phase transition, buoyancy, condensation).
// Simulates a heated cauldron with fire beneath, boiling water into buoyant steam, and
// aloft condensation into falling rain droplets.
// ============================================================================
#define SOKOL_NO_DEPRECATED
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wenum-enum-conversion"
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#pragma clang diagnostic pop

#define GATI_ENABLE_PRAKRITI 1
#define GATI_ENABLE_AKRUTI 1

#include "gati/gati.hpp"
#include "gati/material.hpp"
#include "gati/elemental.hpp"
#include "gati/material_reaction.hpp"
#include "prakriti/prakriti.hpp"
#include "prakriti/material/phase_rule.hpp"
#include "prakriti/solvers/obstacle.hpp"
#include "akruti/akruti.hpp"
#include "kalpana/kalpana.hpp"
#include "kalpana/backend/sokol_backend.hpp"

#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <string>

static const char* VS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct In  { float2 pos [[attribute(0)]]; float4 col [[attribute(1)]]; };\n"
    "struct Out { float4 pos [[position]]; float4 col; };\n"
    "vertex Out vs(In in [[stage_in]]) { Out o; o.pos=float4(in.pos,0,1); o.col=in.col; return o; }\n";
static const char* FS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct In { float4 col; };\n"
    "fragment float4 fs(In in [[stage_in]]) {\n"
    "    return in.col; }\n";

static constexpr int W = 1180;
static constexpr int H = 760;
static constexpr float FW = float(W);
static constexpr float FH = float(H);
static constexpr float DT = 1.0f / 60.0f;

// ----------------------------------------------------------------------------
// Showcase Obstacle Set (Cauldron Geometry & Hearth Stand)
// ----------------------------------------------------------------------------
struct CauldronObstacles {
    std::vector<akruti::Circle> circles;
    std::vector<akruti::Box> boxes;
    std::vector<akruti::Capsule> capsules;

    template <typename F>
    void for_each_shape(F&& f) const {
        for (const auto& c : circles) f(c);
        for (const auto& b : boxes) f(b);
        for (const auto& cap : capsules) f(cap);
    }
};

using ShowcaseMechanics = prakriti::SolverStack<
    prakriti::XpbdSolver,
    prakriti::DensitySolver,
    prakriti::ObstacleSolver<CauldronObstacles>
>;

// ----------------------------------------------------------------------------
// Flame Particle for Visual & Thermodynamic Heat Injection
// ----------------------------------------------------------------------------
struct FlameEmber {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float life = 1.0f;
    float max_life = 1.0f;
    float size = 4.0f;
    float temp = 950.0f; // °C
};

// ----------------------------------------------------------------------------
// Main Application State
// ----------------------------------------------------------------------------
struct BoilingSimApp {
    sg_pipeline pip{};
    sg_bindings bind{};
    sg_pass_action pass_action{};
    sg_buffer vbuf{};
    sg_buffer ibuf{};
    std::unique_ptr<kalpana::Canvas<kalpana::sokol_backend>> canvas;
    kalpana::InstancedParticlePipeline instanced_particles;

    // Gati Game Facade
    std::unique_ptr<gati::Game<gati::DefaultSystems>> gati_game;

    // Prakriti Multiphysics & Continuum World
    CauldronObstacles obstacles;
    std::unique_ptr<prakriti::World<prakriti::DefaultMaterialLaw, prakriti::DefaultComputeBackend, ShowcaseMechanics>>
    world;
    prakriti::PhaseRuleEngine phase_engine;

    prakriti::MaterialId mat_water{};
    prakriti::MaterialId mat_iron{};
    prakriti::MaterialId mat_fire_core{};

    // Visual Flame Embers
    std::vector<FlameEmber> embers;

    // Cauldron / Hearth Geometry
    float pot_cx = FW * 0.5f;
    float pot_cy = 440.0f;
    float pot_radius = 160.0f;
    float pot_wall_thick = 14.0f;
    float pot_temp = 20.0f; // °C

    // Environmental / Thermodynamic Stats
    float avg_water_temp = 20.0f;
    float max_water_temp = 20.0f;
    float steam_count = 0;
    float liquid_count = 0;
    float total_particles = 0;

    // Heat source controls
    float fire_intensity = 0.5f; // Gentle simmer (0.0 to 3.0)
    bool heat_torch = false;
    bool ice_blast = false;

    // Interactive mouse
    float mouse_x = FW * 0.5f;
    float mouse_y = FH * 0.5f;
    bool mouse_down = false;

    // Telemetry
    int frame = 0;
    float fps = 60.0f;
    std::chrono::high_resolution_clock::time_point last_time;
};

static BoilingSimApp g_app;

// ----------------------------------------------------------------------------
// World Initialization & Setup
// ----------------------------------------------------------------------------
static void init_boiling_world() {
    auto& app = g_app;

    // 1. Initialize Gati Runtime (60Hz fixed clock with presentation alpha blending)
    gati::ClockConfig clock_cfg{
        .hz = 60.0f,
        .max_frame_dt = 0.25f,
        .max_steps = 4
    };
    app.gati_game = std::make_unique<gati::Game<gati::DefaultSystems>>(clock_cfg);

    // 2. Setup Prakriti World Configuration
    prakriti::WorldConfig cfg{};
    cfg.bounds = {{20.0f, 20.0f}, {FW - 20.0f, FH - 20.0f}};
    cfg.gravity = {0.0f, 750.0f}; // Downward gravity
    cfg.substeps = 3; // 180 Hz precision
    cfg.solver_iters = 2;
    cfg.cell_size = 11.0f; // High-resolution spatial grid for fine fluid simulation

    // 3. Build Cauldron Obstacle Geometry (Deep, tall-rimmed U-cauldron)
    app.obstacles.circles.clear();
    app.obstacles.boxes.clear();
    app.obstacles.capsules.clear();

    const float cx = app.pot_cx;
    const float cy = app.pot_cy;
    const float r = app.pot_radius;
    const float th = app.pot_wall_thick;

    // Left high rim of pot
    app.obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(cx - r, cy - 130.0f),
        pebble::math::vec2(cx - r + 20.0f, cy + 20.0f),
        th
    });
    // Right high rim of pot
    app.obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(cx + r, cy - 130.0f),
        pebble::math::vec2(cx + r - 20.0f, cy + 20.0f),
        th
    });
    // Bottom curved basin of pot
    app.obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(cx - r + 20.0f, cy + 20.0f),
        pebble::math::vec2(cx - 80.0f, cy + 95.0f),
        th
    });
    app.obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(cx - 80.0f, cy + 95.0f),
        pebble::math::vec2(cx + 80.0f, cy + 95.0f),
        th
    });
    app.obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(cx + 80.0f, cy + 95.0f),
        pebble::math::vec2(cx + r - 20.0f, cy + 20.0f),
        th
    });

    // Left and Right Hearth Leg Stands
    app.obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(cx - 120.0f, cy + 90.0f),
        pebble::math::vec2(cx - 170.0f, cy + 180.0f),
        10.0f
    });
    app.obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(cx + 120.0f, cy + 90.0f),
        pebble::math::vec2(cx + 170.0f, cy + 180.0f),
        10.0f
    });

    // Hearth Fire Pit Base Grate
    app.obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(cx - 190.0f, cy + 180.0f),
        pebble::math::vec2(cx + 190.0f, cy + 180.0f),
        12.0f
    });

    prakriti::ObstacleConfig obs_cfg{};
    obs_cfg.friction = 0.02f;
    obs_cfg.restitution = 0.20f;
    obs_cfg.contact_offset = 0.0f;
    obs_cfg.contact_stiffness = 1.0f;

    // 4. Setup Density Solver & Fine SPH Smoothing
    prakriti::DensitySolver density_solver;
    density_solver.cfg.smoothing_h = 11.0f; // Crisp high-resolution fluid kernel
    density_solver.cfg.rest_density = 0.0057f; // Calibrated Poly6 kernel sum at rest 4.6px particle spacing
    density_solver.cfg.relaxation_eps = 1e-4f;
    density_solver.cfg.scorr_k = 0.001f;

    ShowcaseMechanics mechanics_stack{
        std::make_tuple(
            prakriti::XpbdSolver{},
            density_solver,
            prakriti::ObstacleSolver<CauldronObstacles>{app.obstacles, obs_cfg}
        )
    };

    app.world = std::make_unique<prakriti::World<
        prakriti::DefaultMaterialLaw, prakriti::DefaultComputeBackend, ShowcaseMechanics>>(
        cfg, std::move(mechanics_stack)
    );

    // 5. Setup Thermodynamic Material Catalog
    prakriti::MaterialParams water_mat = prakriti::MaterialRegistry::water();
    water_mat.melt_temp = 0.0f;
    water_mat.boil_temp = 100.0f;
    water_mat.latent_heat_vapor = 2260.0f; // High latent heat of vaporization
    water_mat.conductivity = 0.6f;
    water_mat.heat_capacity = 4.184f;
    app.mat_water = app.world->materials().add(water_mat);

    prakriti::MaterialParams iron_mat = prakriti::MaterialRegistry::steel();
    iron_mat.conductivity = 80.0f; // Highly conductive metal
    iron_mat.heat_capacity = 0.45f;
    app.mat_iron = app.world->materials().add(iron_mat);

    prakriti::MaterialParams fire_mat;
    fire_mat.rest_density = 1.0f;
    fire_mat.conductivity = 150.0f;
    fire_mat.heat_capacity = 1.0f;
    app.mat_fire_core = app.world->materials().add(fire_mat);

    // Register Generic Rule-Based Phase Transformations (Boiling & Condensation)
    app.phase_engine = prakriti::PhaseRuleEngine{};
    app.phase_engine.add_rule(prakriti::rules::boiling(app.mat_water, 100.0f, -1400.0f));
    app.phase_engine.add_rule(prakriti::rules::condensation(app.mat_water, 100.0f));

    // 6. Spawn Densely Sampled Liquid Water resting in the lower cauldron basin (~600 fine particles)
    for (int row = 0; row < 18; ++row) {
        for (int col = 0; col < 42; ++col) {
            float px = (cx - 95.0f) + float(col) * 4.6f;
            float py = (cy + 15.0f) + float(row) * 4.2f;
            float dx = px - cx;
            float dy = py - (cy + 25.0f);
            if (dx * dx + dy * dy < (r - 35.0f) * (r - 35.0f) && py < cy + 85.0f) {
                app.world->particles().add({
                    .position = pebble::math::vec2(px, py),
                    .velocity = {0.0f, 0.0f},
                    .mass = 1.0f,
                    .temperature = 22.0f, // Room temperature (22°C)
                    .material = app.mat_water,
                    .f_solid = 0.0f, .f_plastic = 0.0f, .f_liquid = 1.0f, .f_gas = 0.0f
                });
            }
        }
    }

    // 7. Register Gati ECS Entities for Cauldron and Hearth
    auto& ecs_world = app.gati_game->world();

    // Cauldron Pot Entity
    auto pot_entity = ecs_world.spawn();
    ecs_world.add<gati::Transform>(pot_entity, {.position = pebble::math::vec2(cx, cy)});
    ecs_world.add<gati::MaterialComponent>(pot_entity, {
                                               .params = iron_mat,
                                               .temperature = 20.0f,
                                               .phase_fractions = {1.0f, 0.0f, 0.0f, 0.0f} // Solid cast iron
                                           });

    // Hearth Burner Entity
    auto burner_entity = ecs_world.spawn();
    ecs_world.add<gati::Transform>(burner_entity, {.position = pebble::math::vec2(cx, cy + 140.0f)});
    ecs_world.add<gati::MaterialComponent>(burner_entity, {
                                               .params = fire_mat,
                                               .temperature = 950.0f, // Glowing hearth fire
                                               .phase_fractions = {0.0f, 0.0f, 0.0f, 1.0f}
                                           });

    app.pot_temp = 20.0f;
}

// ----------------------------------------------------------------------------
// Physics & Thermodynamics Substep Loop
// ----------------------------------------------------------------------------
static void update_thermodynamics_and_phase(float dt) {
    auto& app = g_app;
    if (!app.world) return;

    auto& P = app.world->particles();
    const std::size_t N = P.size();
    const float cx = app.pot_cx;
    const float cy = app.pot_cy;
    const float base_y = cy + 90.0f;

    // 1. Interactive Heat Torch / Ice Blast from user input
    if (app.mouse_down || app.heat_torch || app.ice_blast) {
        float target_x = app.mouse_down ? app.mouse_x : cx;
        float target_y = app.mouse_down ? app.mouse_y : base_y;

        for (std::size_t i = 0; i < N; ++i) {
            float dx = P.pos_x[i] - target_x;
            float dy = P.pos_y[i] - target_y;
            float d2 = dx * dx + dy * dy;
            if (d2 < 70.0f * 70.0f) {
                if (app.heat_torch || (app.mouse_down && app.mouse_y > cy)) {
                    P.temperature[i] += 450.0f * dt;
                }
                else if (app.ice_blast || (app.mouse_down && app.mouse_y <= cy)) {
                    P.temperature[i] -= 450.0f * dt;
                }
            }
        }
    }

    // 2. Heat conduction from Burner Fire into the Pot Base
    float flame_heat = 550.0f * app.fire_intensity;
    app.pot_temp += (flame_heat - app.pot_temp) * (0.25f * dt);

    // 3. Step Rule-Based Phase Transition Engine
    app.phase_engine.step(P, app.world->materials(), dt);

    // 4. Heat Conduction from Hearth/Pot into Liquid Water, Organic Convection, & Steam Ascent
    float temp_sum = 0.0f;
    float max_temp = 0.0f;
    int water_p_count = 0;
    int steam_p_count = 0;
    int liquid_p_count = 0;

    for (std::size_t i = 0; i < N; ++i) {
        if (P.material[i] != app.mat_water) continue;
        water_p_count++;

        // Conductive heat transfer from cauldron metal walls to liquid water inside pot (Realistic heat capacity rate)
        if (P.pos_y[i] > cy - 60.0f && std::abs(P.pos_x[i] - cx) < app.pot_radius + 15.0f) {
            float heat_flux = (app.pot_temp - P.temperature[i]) * 0.45f * dt;
            P.temperature[i] += std::max(0.0f, heat_flux);
        }

        // Ambient thermal dissipation (Cooling at top sky zone)
        float ambient_t = (P.pos_y[i] < 220.0f) ? 12.0f : 24.0f; // Cold sky aloft (12°C)
        float cooling_rate = (P.pos_y[i] < 220.0f) ? 22.0f : 3.0f;
        P.temperature[i] += (ambient_t - P.temperature[i]) * (cooling_rate * 0.01f) * dt;

        // Thermal expansion & organic convection in warm liquid water (T > 25°C)
        if (P.f_liquid[i] > 0.4f && P.temperature[i] > 25.0f) {
            float heat_ratio = std::clamp((P.temperature[i] - 20.0f) / 80.0f, 0.0f, 1.0f);

            // Upward convective acceleration + radial sloshing
            float convection = -heat_ratio * 220.0f;
            P.vel_y[i] += convection * dt;

            float radial = (P.pos_x[i] - cx) * 0.04f * heat_ratio;
            P.vel_x[i] += radial * dt;

            // Micro-bubble agitation near boiling
            if (P.temperature[i] > 85.0f) {
                P.vel_x[i] += (float(std::rand() % 30) - 15.0f) * 10.0f * dt;
                P.vel_y[i] -= float(std::rand() % 40) * 8.0f * dt;
            }
        }

        // Steam Vapor Buoyancy Ascent & Billowing Plumes (Closed Cycle: Steam rises -> cools aloft -> condenses to rain)
        if (P.f_gas[i] > 0.3f) {
            steam_p_count++;
            float buoyancy = -1200.0f * P.f_gas[i];
            P.vel_y[i] += buoyancy * dt;

            float plume_wobble = std::sin(P.pos_y[i] * 0.08f + float(app.frame) * 0.25f);
            P.vel_x[i] += plume_wobble * 80.0f * dt;
        }
        else {
            liquid_p_count++;
        }

        temp_sum += P.temperature[i];
        if (P.temperature[i] > max_temp) max_temp = P.temperature[i];
    }

    // 4. Spawn & Update Hearth Flame Embers beneath the Pot
    if (app.fire_intensity > 0.05f) {
        for (int k = 0; k < int(app.fire_intensity * 4.0f); ++k) {
            float ex = cx + (float(std::rand() % 180) - 90.0f);
            float ey = cy + 140.0f + float(std::rand() % 25);
            app.embers.push_back({
                .x = ex,
                .y = ey,
                .vx = (float(std::rand() % 60) - 30.0f),
                .vy = -(120.0f + float(std::rand() % 140) * app.fire_intensity),
                .life = 1.0f,
                .max_life = 0.4f + float(std::rand() % 30) * 0.01f,
                .size = 3.0f + float(std::rand() % 5),
                .temp = 850.0f + float(std::rand() % 400) * app.fire_intensity
            });
        }
    }

    // Update embers
    for (auto& em : app.embers) {
        em.x += em.vx * dt;
        em.y += em.vy * dt;
        em.life -= dt / em.max_life;
    }
    std::erase_if(app.embers, [](const FlameEmber& e) { return e.life <= 0.0f; });

    // 5. Update Telemetry
    app.total_particles = float(water_p_count);
    app.liquid_count = float(liquid_p_count);
    app.steam_count = float(steam_p_count);
    app.avg_water_temp = water_p_count > 0 ? (temp_sum / float(water_p_count)) : 20.0f;
    app.max_water_temp = max_temp;
}

// ----------------------------------------------------------------------------
// Scene Geometry Building for Kalpana Vector Engine
// ----------------------------------------------------------------------------
static void build_scene(kalpana::Scene& scene) {
    auto& app = g_app;
    app.instanced_particles.begin(); // Reset GPU particle instance stream every frame
    scene.clear_color(kalpana::Color{0.03f, 0.04f, 0.06f, 1.0f});

    const float cx = app.pot_cx;
    const float cy = app.pot_cy;
    const float r = app.pot_radius;

    // 1. Background Grid & Cold Aloft Canopy
    {
        kalpana::Color gc{0.06f, 0.08f, 0.12f, 1.0f};
        kalpana::Path grid_lines;
        for (int i = 0; i <= 24; ++i) {
            float x = float(i) / 24.0f * FW;
            grid_lines.move_to(x, 0.0f);
            grid_lines.line_to(x, FH);
        }
        for (int j = 0; j <= 16; ++j) {
            float y = float(j) / 16.0f * FH;
            grid_lines.move_to(0.0f, y);
            grid_lines.line_to(FW, y);
        }
        scene.add(kalpana::Node::shape(grid_lines, kalpana::Paint::stroke(gc, 1.0f)));

        // Cold Aloft Condensation Sky Overlay
        kalpana::Path sky;
        sky.rect(20.0f, 20.0f, FW - 40.0f, 160.0f);
        scene.add(kalpana::Node::shape(sky, kalpana::Paint::fill(kalpana::Color{0.06f, 0.12f, 0.22f, 0.40f})));
    }

    // 2. Hearth Stand & Burner Pit Grate
    {
        kalpana::Path stand;
        stand.move_to(cx - 190.0f, cy + 180.0f);
        stand.line_to(cx + 190.0f, cy + 180.0f);
        stand.move_to(cx - 120.0f, cy + 85.0f);
        stand.line_to(cx - 170.0f, cy + 180.0f);
        stand.move_to(cx + 120.0f, cy + 85.0f);
        stand.line_to(cx + 170.0f, cy + 180.0f);
        scene.add(kalpana::Node::shape(stand, kalpana::Paint::stroke(kalpana::Color{0.25f, 0.28f, 0.35f, 1.0f}, 8.0f)));
    }

    // 4. Flame Embers Glow
    for (const auto& em : app.embers) {
        float life_alpha = std::clamp(em.life, 0.0f, 1.0f);
        kalpana::Color flame_col;
        if (em.temp > 1000.0f) {
            flame_col = {1.0f, 0.95f, 0.40f, life_alpha * 0.9f};
        }
        else if (em.temp > 700.0f) {
            flame_col = {1.0f, 0.45f, 0.05f, life_alpha * 0.85f};
        }
        else {
            flame_col = {0.85f, 0.15f, 0.05f, life_alpha * 0.6f};
        }
        kalpana::Path ep;
        ep.circle(em.x, em.y, em.size * (0.5f + 0.5f * life_alpha));
        scene.add(kalpana::Node::shape(ep, kalpana::Paint::fill(flame_col)));
    }

    // 5. Cauldron Outer Metal Hull (Heat-tinted cast iron)
    float pot_heat_norm = std::clamp((app.pot_temp - 20.0f) / 500.0f, 0.0f, 1.0f);
    kalpana::Color pot_iron_col = {
        0.20f + 0.50f * pot_heat_norm,
        0.22f + 0.10f * (1.0f - pot_heat_norm),
        0.25f + 0.10f * (1.0f - pot_heat_norm),
        1.0f
    };

    kalpana::Path pot_hull;
    pot_hull.move_to(cx - r, cy - 130.0f);
    pot_hull.line_to(cx - r + 20.0f, cy + 20.0f);
    pot_hull.line_to(cx - 80.0f, cy + 95.0f);
    pot_hull.line_to(cx + 80.0f, cy + 95.0f);
    pot_hull.line_to(cx + r - 20.0f, cy + 20.0f);
    pot_hull.line_to(cx + r, cy - 130.0f);
    scene.add(kalpana::Node::shape(pot_hull, kalpana::Paint::stroke(pot_iron_col, app.pot_wall_thick)));
    scene.add(kalpana::Node::shape(pot_hull, kalpana::Paint::stroke(kalpana::Color{0.4f, 0.5f, 0.65f, 0.6f}, 2.0f)));

    // 6. HUD Telemetry Bar
    {
        kalpana::Path hud_bg;
        hud_bg.rect(20.0f, 20.0f, FW - 40.0f, 65.0f);
        scene.add(kalpana::Node::shape(hud_bg, kalpana::Paint::fill(kalpana::Color{0.08f, 0.11f, 0.16f, 0.88f})));
        scene.add(
            kalpana::Node::shape(hud_bg, kalpana::Paint::stroke(kalpana::Color{0.20f, 0.35f, 0.50f, 0.60f}, 1.5f)));

        // Water Temperature Gauge Bar
        float temp_bar_w = 240.0f;
        float temp_fill_w = temp_bar_w * std::clamp(app.avg_water_temp / 140.0f, 0.0f, 1.0f);
        kalpana::Path t_bg, t_fill;
        t_bg.rect(35.0f, 52.0f, temp_bar_w, 14.0f);
        t_fill.rect(35.0f, 52.0f, temp_fill_w, 14.0f);
        scene.add(kalpana::Node::shape(t_bg, kalpana::Paint::fill(kalpana::Color{0.12f, 0.15f, 0.20f, 1.0f})));
        scene.add(kalpana::Node::shape(t_fill, kalpana::Paint::fill(
                                           app.avg_water_temp > 98.0f
                                               ? kalpana::Color{1.0f, 0.30f, 0.10f, 1.0f}
                                               : kalpana::Color{0.15f, 0.70f, 1.0f, 1.0f})));
        scene.add(kalpana::Node::shape(t_bg, kalpana::Paint::stroke(kalpana::Color{0.35f, 0.45f, 0.55f, 0.8f}, 1.0f)));
    }

    // 7. Render Instanced Water, Bubbles and Steam Clouds
    if (app.world) {
        auto& P = app.world->particles();
        const std::size_t N = P.size();

        for (std::size_t i = 0; i < N; ++i) {
            float px = P.pos_x[i];
            float py = P.pos_y[i];
            float t = P.temperature[i];
            float f_gas = P.f_gas[i];

            kalpana::Color p_col;
            float p_radius = 5.0f;

            if (f_gas > 0.4f) {
                // STEAM VAPOR: Soft billowing white-cyan mist
                p_radius = 4.2f + f_gas * 2.6f;
                float steam_alpha = std::clamp(0.20f + 0.35f * f_gas, 0.15f, 0.60f);
                p_col = {0.90f, 0.96f, 1.0f, steam_alpha};
            }
            else if (t >= 95.0f) {
                // BOILING FROTH: Micro-bubbly cyan-white
                p_radius = 3.0f;
                p_col = {0.90f, 0.98f, 1.0f, 0.95f};
            }
            else if (t >= 60.0f) {
                // HOT WATER: Warm aquamarine
                float warm_prog = (t - 60.0f) / 35.0f;
                p_radius = 2.7f;
                p_col = {0.15f + 0.50f * warm_prog, 0.70f + 0.25f * warm_prog, 0.95f, 0.92f};
            }
            else {
                // COOL LIQUID WATER: Crisp translucent sapphire blue droplet
                float cool_prog = std::clamp((t - 15.0f) / 45.0f, 0.0f, 1.0f);
                p_radius = 2.6f;
                p_col = {0.10f, 0.50f + 0.25f * cool_prog, 0.92f + 0.08f * cool_prog, 0.88f};
            }

            app.instanced_particles.add_instance(px, py, p_radius, p_col);
        }
    }
}

// ----------------------------------------------------------------------------
// Sokol Application Callbacks
// ----------------------------------------------------------------------------
static void init_cb() {
    auto& app = g_app;

    sg_desc gfx{};
    gfx.environment = sglue_environment();
    gfx.logger.func = slog_func;
    sg_setup(&gfx);

    // Direct dynamic GPU vertex and index stream buffers
    {
        sg_buffer_desc d{};
        d.size = 128 * 1024 * sizeof(kalpana::sokol_backend::Vertex);
        d.usage.stream_update = true;
        app.vbuf = sg_make_buffer(d);
    }
    {
        sg_buffer_desc d{};
        d.usage.index_buffer = true;
        d.usage.stream_update = true;
        d.size = 256 * 1024 * sizeof(std::uint32_t);
        app.ibuf = sg_make_buffer(d);
    }

    app.bind.vertex_buffers[0] = app.vbuf;
    app.bind.index_buffer = app.ibuf;

    sg_shader_desc shd{};
#if defined(SOKOL_METAL)
    shd.vertex_func.source = VS_METAL;
    shd.vertex_func.entry = "vs";
    shd.fragment_func.source = FS_METAL;
    shd.fragment_func.entry = "fs";
#endif

    sg_shader shdr = sg_make_shader(shd);

    sg_pipeline_desc pd{};
    pd.shader = shdr;
    pd.index_type = SG_INDEXTYPE_UINT32;
    pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2; // position
    pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT4; // color (RGBA)
    app.pip = sg_make_pipeline(pd);

    app.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    app.pass_action.colors[0].clear_value = {0.03f, 0.04f, 0.06f, 1.0f};
    app.canvas = std::make_unique<kalpana::Canvas<kalpana::sokol_backend>>(W, H);
    app.instanced_particles.init(65536);
    app.last_time = std::chrono::high_resolution_clock::now();

    init_boiling_world();
}

static void frame_cb() {
    auto& app = g_app;
    auto now = std::chrono::high_resolution_clock::now();
    float real_dt = std::chrono::duration<float>(now - app.last_time).count();
    app.last_time = now;
    if (real_dt <= 0.0f || real_dt > 0.1f) real_dt = DT;
    app.fps = 0.95f * app.fps + 0.05f * (1.0f / real_dt);
    app.frame++;

    // 1. Advance Gati ECS Runtime & Step Prakriti Multiphysics
    app.gati_game->update(real_dt);
    update_thermodynamics_and_phase(real_dt);
    if (app.world) {
        app.world->step();
    }

    // 2. Build Vector Scene with Kalpana & Populate Instanced Particles
    kalpana::Scene scene;
    build_scene(scene);
    app.canvas->render(scene);

    const auto& verts = app.canvas->backend().vertices();
    const auto& indices = app.canvas->backend().indices();

    if (!verts.empty() && !indices.empty()) {
        sg_range vr = {verts.data(), verts.size() * sizeof(kalpana::sokol_backend::Vertex)};
        sg_update_buffer(app.vbuf, vr);

        sg_range ir = {indices.data(), indices.size() * sizeof(std::uint32_t)};
        sg_update_buffer(app.ibuf, ir);
    }

    sg_pass pass{};
    pass.action = app.pass_action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(pass);

    // 1. Draw Instanced Particle Cloud
    app.instanced_particles.render(FW, FH);

    // 2. Draw Vector Overlays (Cauldron, Stand, Embers, HUD)
    if (!indices.empty()) {
        sg_apply_pipeline(app.pip);
        sg_apply_bindings(app.bind);
        sg_draw(0, static_cast<int>(indices.size()), 1);
    }
    sg_end_pass();
    sg_commit();
}

static void cleanup_cb() {
    g_app.canvas.reset();
    g_app.world.reset();
    g_app.gati_game.reset();
    sg_shutdown();
}

static void event_cb(const sapp_event* ev) {
    auto& app = g_app;
    if (ev->type == SAPP_EVENTTYPE_MOUSE_MOVE) {
        app.mouse_x = ev->mouse_x;
        app.mouse_y = ev->mouse_y;
    }
    else if (ev->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
        app.mouse_down = true;
        app.mouse_x = ev->mouse_x;
        app.mouse_y = ev->mouse_y;
    }
    else if (ev->type == SAPP_EVENTTYPE_MOUSE_UP) {
        app.mouse_down = false;
    }
    else if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
        if (ev->key_code == SAPP_KEYCODE_SPACE) {
            app.fire_intensity = std::min(3.0f, app.fire_intensity + 0.5f);
            app.heat_torch = true;
        }
        else if (ev->key_code == SAPP_KEYCODE_C) {
            app.fire_intensity = std::max(0.0f, app.fire_intensity - 0.5f);
            app.ice_blast = true;
        }
        else if (ev->key_code == SAPP_KEYCODE_F) {
            // [F] Add Fresh Fine Water Stream from top Faucet (Capacity up to 2,500 particles)
            if (app.world && app.world->particles().size() < 2500) {
                for (int k = 0; k < 10; ++k) {
                    float jx = app.pot_cx + (float(std::rand() % 30) - 15.0f);
                    float jy = 60.0f + float(std::rand() % 20);
                    app.world->particles().add({
                        .position = pebble::math::vec2(jx, jy),
                        .velocity = {float(std::rand() % 24 - 12), 190.0f},
                        .mass = 1.0f,
                        .temperature = 16.0f,
                        .material = app.mat_water,
                        .f_solid = 0.0f, .f_plastic = 0.0f, .f_liquid = 1.0f, .f_gas = 0.0f
                    });
                }
            }
        }
        else if (ev->key_code == SAPP_KEYCODE_R) {
            init_boiling_world();
        }
    }
    else if (ev->type == SAPP_EVENTTYPE_KEY_UP) {
        if (ev->key_code == SAPP_KEYCODE_SPACE) {
            app.heat_torch = false;
        }
        else if (ev->key_code == SAPP_KEYCODE_C) {
            app.ice_blast = false;
        }
    }
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    sapp_desc d{};
    d.init_cb = init_cb;
    d.frame_cb = frame_cb;
    d.cleanup_cb = cleanup_cb;
    d.event_cb = event_cb;
    d.width = W;
    d.height = H;
    d.window_title = "Pebble — Gati + Prakriti Thermodynamic Boiling & Evaporation Cycle";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}
