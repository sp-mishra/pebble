// ============================================================================
// pebble_tanks.cpp — Pocket Tanks Automatic Multi-Physics Artillery Battle Simulation
// ============================================================================
// Automatic Red vs. Blue Pocket Tanks combat on dynamic destructible terrain.
// Combines:
//   • Gati ECS & Clock Execution Pipeline
//   • Prakriti 2.0 SPH/XPBD Multiphysics & Phase Rule Engine (Thermodynamics, Melting, Destruction)
//   • Akruti SDF Collision Geometry
//   • Kalpana 2D Vector Engine & Sokol Metal GPU Instanced Particle Renderer
// ============================================================================
#include <sokol_app.h>
#include <sokol_gfx.h>
#include <sokol_glue.h>
#include <sokol_log.h>

#include <gati/gati.hpp>
#include <prakriti/engine.hpp>
#include <prakriti/solvers/density.hpp>
#include <prakriti/solvers/obstacle.hpp>
#include <prakriti/solvers/xpbd.hpp>
#include <prakriti/material/phase_rule.hpp>
#include <akruti/akruti.hpp>

#include <kalpana/kalpana.hpp>
#include <kalpana/backend/sokol_backend.hpp>
#include <kalpana/backend/instanced_pipeline.hpp>

#include <chrono>
#include <vector>
#include <cmath>
#include <memory>
#include <algorithm>
#include <numbers>
#include <random>

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

// Screen Dimensions & Physics World Bounds
constexpr float FW = 1280.0f;
constexpr float FH = 720.0f;
constexpr uint32_t W = 1280;
constexpr uint32_t H = 720;
constexpr float DT = 1.0f / 60.0f;

// ----------------------------------------------------------------------------
// Akruti Terrain & Obstacle Store
// ----------------------------------------------------------------------------
struct TerrainObstacles {
    std::vector<akruti::Capsule> capsules;

    template <typename Fn>
    void for_each_shape(Fn&& fn) const {
        for (const auto& cap : capsules) {
            fn(cap);
        }
    }
};

using TankMechanics = prakriti::SolverStack<
    prakriti::XpbdSolver,
    prakriti::DensitySolver,
    prakriti::ObstacleSolver<TerrainObstacles>
>;

// ----------------------------------------------------------------------------
// Tank & Shell Data Structures
// ----------------------------------------------------------------------------
enum class Team { Red, Blue };

struct PocketTank {
    uint32_t id = 0;
    Team team = Team::Red;
    float x = 0.0f;
    float y = 0.0f;
    float body_w = 26.0f;
    float body_h = 13.0f;
    float turret_angle = -0.785f; // Radians
    float target_turret_angle = -0.785f;
    float hp = 100.0f;
    float max_hp = 100.0f;
    float reload_timer = 0.0f;
    float reload_cooldown = 1.8f;
    float firing_recoil = 0.0f;
    uint32_t target_enemy_id = 0;
    bool is_destroyed = false;
    bool is_melting = false;
    float armor_temp = 20.0f;
};

struct ShellProjectile {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float radius = 3.0f;
    float damage = 38.0f;
    float blast_radius = 55.0f;
    Team team = Team::Red;
    bool active = true;
    float life = 0.0f;
};

struct BlastEffect {
    float x = 0.0f;
    float y = 0.0f;
    float radius = 0.0f;
    float max_radius = 45.0f;
    float life = 1.0f;
    Team team = Team::Red;
};

struct ShrapnelShard {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float damage = 16.0f;
    float life = 0.8f;
    bool active = true;
};

struct DestructibleBuilding {
    uint32_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float width = 38.0f;
    float height = 75.0f;
    float hp = 260.0f;
    float max_hp = 260.0f;
    uint32_t num_floors = 4;
    Team team = Team::Red;
    bool is_destroyed = false;
};

struct AirCraft {
    uint32_t id = 0;
    Team team = Team::Red;
    float x = 0.0f;
    float y = 80.0f;
    float vx = 240.0f;
    float vy = 0.0f;
    float hp = 70.0f;
    float bomb_cooldown = 2.2f;
    float bomb_timer = 0.0f;
    bool is_destroyed = false;
};

struct AAGun {
    uint32_t id = 0;
    Team team = Team::Red;
    float x = 0.0f;
    float y = 0.0f;
    float turret_angle = -1.57f;
    float target_angle = -1.57f;
    float hp = 140.0f;
    float fire_cooldown = 0.35f;
    float fire_timer = 0.0f;
    bool is_destroyed = false;
};

struct FlakShell {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float fuse_life = 0.6f;
    Team team = Team::Red;
    bool active = true;
};

struct CombustibleTree {
    uint32_t id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float height = 36.0f;
    float width = 22.0f;
    float hp = 100.0f;
    float temp = 20.0f;
    bool is_on_fire = false;
    bool is_destroyed = false;
};

struct ParticleEmber {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float life = 1.0f;
    float max_life = 1.0f;
    float size = 3.0f;
    float temp = 900.0f;
    bool is_smoke = false;
};

// ----------------------------------------------------------------------------
// Main Application State
// ----------------------------------------------------------------------------
struct PocketTanksApp {
    sg_pipeline pip{};
    sg_bindings bind{};
    sg_pass_action pass_action{};
    sg_buffer vbuf{};
    sg_buffer ibuf{};
    std::unique_ptr<kalpana::Canvas<kalpana::sokol_backend>> canvas;
    kalpana::InstancedParticlePipeline instanced_particles;

    // Gati ECS Runtime
    std::unique_ptr<gati::Game<gati::DefaultSystems>> gati_game;

    // Prakriti Multiphysics World
    TerrainObstacles terrain_obstacles;
    std::unique_ptr<prakriti::World<prakriti::DefaultMaterialLaw, prakriti::DefaultComputeBackend, TankMechanics>>
    world;
    prakriti::PhaseRuleEngine phase_engine;

    prakriti::MaterialId mat_soil{};
    prakriti::MaterialId mat_sandstone{};
    prakriti::MaterialId mat_granite{};
    prakriti::MaterialId mat_iron_ore{};
    prakriti::MaterialId mat_armor{};
    prakriti::MaterialId mat_fire{};

    // Armies, Air Force, Outposts & Combat Objects
    std::vector<PocketTank> tanks;
    std::vector<DestructibleBuilding> buildings;
    std::vector<AirCraft> aircraft;
    std::vector<AAGun> aa_guns;
    std::vector<FlakShell> flak_shells;
    std::vector<CombustibleTree> trees;
    std::vector<ShellProjectile> shells;
    std::vector<ShrapnelShard> shards;
    std::vector<BlastEffect> blasts;
    std::vector<ParticleEmber> embers;

    // Terrain Heightmap Profile
    std::vector<float> heightmap;

    // Interactive & Visual State
    float fps = 60.0f;
    uint64_t frame = 0;
    std::chrono::high_resolution_clock::time_point last_time;

    uint32_t red_tanks_alive = 0;
    uint32_t blue_tanks_alive = 0;
    float total_terrain_damage = 0.0f;
};

static PocketTanksApp g_app;

// ----------------------------------------------------------------------------
// Heightmap & Terrain Helper Functions
// ----------------------------------------------------------------------------
static float eval_terrain_height(float x) {
    const float y_base = 530.0f;
    float h = y_base - 70.0f * std::sin(x * 0.0055f)
        + 40.0f * std::cos(x * 0.012f)
        - 25.0f * std::sin(x * 0.024f);
    return std::clamp(h, 280.0f, 640.0f);
}

// ----------------------------------------------------------------------------
// World Initialization
// ----------------------------------------------------------------------------
static void init_tanks_world() {
    auto& app = g_app;

    // 1. Initialize Gati ECS & Clock
    gati::ClockConfig clock_cfg{.hz = 60.0f, .max_frame_dt = 0.25f, .max_steps = 4};
    app.gati_game = std::make_unique<gati::Game<gati::DefaultSystems>>(clock_cfg);

    // 2. Setup Terrain Heightmap
    app.heightmap.resize(W);
    for (uint32_t x = 0; x < W; ++x) {
        app.heightmap[x] = eval_terrain_height(float(x));
    }

    // 3. Construct Terrain SDF Obstacles
    app.terrain_obstacles.capsules.clear();
    for (uint32_t x = 20; x < W - 20; x += 25) {
        float x1 = float(x);
        float x2 = float(x + 25);
        float y1 = app.heightmap[x];
        float y2 = app.heightmap[std::min(W - 1, x + 25)];
        app.terrain_obstacles.capsules.push_back(akruti::Capsule{
            pebble::math::vec2(x1, y1),
            pebble::math::vec2(x2, y2),
            12.0f
        });
    }
    // Bottom World Boundary Grate
    app.terrain_obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(10.0f, FH - 15.0f),
        pebble::math::vec2(FW - 10.0f, FH - 15.0f),
        15.0f
    });

    // 4. Initialize Prakriti SPH Multiphysics World
    prakriti::WorldConfig cfg{};
    cfg.bounds = {{10.0f, 10.0f}, {FW - 10.0f, FH - 10.0f}};
    cfg.gravity = {0.0f, 650.0f}; // Standard 2D gravity
    cfg.cell_size = 12.0f;
    cfg.substeps = 3;
    cfg.solver_iters = 2;
    cfg.dt = DT;

    prakriti::ObstacleConfig obs_cfg{};
    obs_cfg.friction = 0.15f;
    obs_cfg.restitution = 0.15f;

    prakriti::DensitySolver density_solver;
    density_solver.cfg.smoothing_h = 12.0f;
    density_solver.cfg.rest_density = 0.0065f;
    density_solver.cfg.relaxation_eps = 1e-4f;
    density_solver.cfg.scorr_k = 0.001f;

    TankMechanics mechanics{
        std::make_tuple(
            prakriti::XpbdSolver{},
            density_solver,
            prakriti::ObstacleSolver<TerrainObstacles>{app.terrain_obstacles, obs_cfg}
        )
    };

    app.world = std::make_unique<prakriti::World<
        prakriti::DefaultMaterialLaw, prakriti::DefaultComputeBackend, TankMechanics>>(
        cfg, mechanics
    );

    // 5. Register Geological & Combat Materials
    prakriti::MaterialParams topsoil_mat{
        .rest_density = 0.9f, .conductivity = 12.0f, .heat_capacity = 1.4f, .melt_temp = 650.0f
    };
    app.mat_soil = app.world->materials().add(topsoil_mat);

    prakriti::MaterialParams sandstone_mat{
        .rest_density = 1.4f, .conductivity = 22.0f, .heat_capacity = 1.0f, .melt_temp = 850.0f
    };
    app.mat_sandstone = app.world->materials().add(sandstone_mat);

    prakriti::MaterialParams granite_mat{
        .rest_density = 2.2f, .conductivity = 35.0f, .heat_capacity = 0.8f, .melt_temp = 1200.0f
    };
    app.mat_granite = app.world->materials().add(granite_mat);

    prakriti::MaterialParams iron_ore_mat{
        .rest_density = 3.6f, .conductivity = 65.0f, .heat_capacity = 0.45f, .melt_temp = 1400.0f
    };
    app.mat_iron_ore = app.world->materials().add(iron_ore_mat);

    prakriti::MaterialParams armor_mat{
        .rest_density = 2.5f, .conductivity = 45.0f, .heat_capacity = 0.5f, .melt_temp = 950.0f
    };
    app.mat_armor = app.world->materials().add(armor_mat);

    prakriti::MaterialParams fire_mat{.rest_density = 0.2f, .conductivity = 120.0f, .heat_capacity = 1.0f};
    app.mat_fire = app.world->materials().add(fire_mat);

    // Phase Rules
    app.phase_engine = prakriti::PhaseRuleEngine{};
    app.phase_engine.add_rule(prakriti::rules::boiling(app.mat_soil, 650.0f, -800.0f));

    // 6. Spawn Multi-Composition SPH Terrain Particles (Heterogeneous Geological Strata)
    for (uint32_t x = 30; x < W - 30; x += 14) {
        float ground_y = app.heightmap[x];
        for (float y = ground_y + 6.0f; y < ground_y + 55.0f; y += 10.0f) {
            float depth = y - ground_y;
            prakriti::MaterialId mat = app.mat_soil;
            if (depth > 35.0f) {
                mat = ((x / 28) % 5 == 0) ? app.mat_iron_ore : app.mat_granite;
            }
            else if (depth > 12.0f) {
                mat = app.mat_sandstone;
            }
            app.world->particles().add({
                .position = pebble::math::vec2(float(x), y),
                .velocity = {0.0f, 0.0f},
                .mass = (depth > 35.0f ? 2.0f : 1.0f),
                .temperature = 18.0f,
                .material = mat,
                .f_solid = 1.0f, .f_plastic = 0.0f, .f_liquid = 0.0f, .f_gas = 0.0f
            });
        }
    }

    // 7. Spawn Combustible Pine Trees along Hill Ridges
    app.trees.clear();
    uint32_t tree_id = 1;
    for (float tx = 130.0f; tx <= 1150.0f; tx += 50.0f) {
        uint32_t ix = std::clamp(uint32_t(tx), 0u, W - 1);
        float ty = app.heightmap[ix];
        app.trees.push_back({
            .id = tree_id++,
            .x = tx,
            .y = ty,
            .height = 36.0f,
            .width = 22.0f,
            .hp = 100.0f,
            .temp = 20.0f,
            .is_on_fire = false,
            .is_destroyed = false
        });
    }

    // 8. Spawn Red & Blue Tank Battalions (50 Mini-Tanks Total)
    app.tanks.clear();
    uint32_t tank_id = 1;

    // Red Frontline & Artillery Tanks (Left Flank)
    for (float tx = 60.0f; tx <= 560.0f; tx += 20.0f) {
        uint32_t ix = std::clamp(uint32_t(tx), 0u, W - 1);
        float ty = app.heightmap[ix] - 8.0f;
        app.tanks.push_back({
            .id = tank_id++,
            .team = Team::Red,
            .x = tx,
            .y = ty,
            .turret_angle = -0.55f,
            .target_turret_angle = -0.55f,
            .hp = 100.0f,
            .reload_cooldown = 1.2f + float(std::rand() % 40) * 0.02f
        });
    }

    // Blue Frontline & Artillery Tanks (Right Flank)
    for (float tx = 720.0f; tx <= 1220.0f; tx += 20.0f) {
        uint32_t ix = std::clamp(uint32_t(tx), 0u, W - 1);
        float ty = app.heightmap[ix] - 8.0f;
        app.tanks.push_back({
            .id = tank_id++,
            .team = Team::Blue,
            .x = tx,
            .y = ty,
            .turret_angle = -2.55f,
            .target_turret_angle = -2.55f,
            .hp = 100.0f,
            .reload_cooldown = 1.2f + float(std::rand() % 40) * 0.02f
        });
    }

    // 9. Spawn Destructible City Command Buildings & Bunkers
    app.buildings.clear();
    uint32_t bldg_id = 1;

    // Red Command Bases
    for (float bx : {110.0f, 250.0f, 390.0f}) {
        uint32_t ix = std::clamp(uint32_t(bx), 0u, W - 1);
        float by = app.heightmap[ix];
        app.buildings.push_back({
            .id = bldg_id++,
            .x = bx,
            .y = by,
            .width = 38.0f,
            .height = 75.0f,
            .hp = 260.0f,
            .max_hp = 260.0f,
            .num_floors = 4,
            .team = Team::Red,
            .is_destroyed = false
        });
    }

    // Blue Command Bases
    for (float bx : {890.0f, 1030.0f, 1170.0f}) {
        uint32_t ix = std::clamp(uint32_t(bx), 0u, W - 1);
        float by = app.heightmap[ix];
        app.buildings.push_back({
            .id = bldg_id++,
            .x = bx,
            .y = by,
            .width = 38.0f,
            .height = 75.0f,
            .hp = 260.0f,
            .max_hp = 260.0f,
            .num_floors = 4,
            .team = Team::Blue,
            .is_destroyed = false
        });
    }

    // 10. Spawn Anti-Aircraft (AA) Flak Cannon Batteries
    app.aa_guns.clear();
    uint32_t aa_id = 1;
    for (float ax : {180.0f, 340.0f}) {
        uint32_t ix = std::clamp(uint32_t(ax), 0u, W - 1);
        app.aa_guns.push_back({
            .id = aa_id++,
            .team = Team::Red,
            .x = ax,
            .y = app.heightmap[ix],
            .turret_angle = -1.35f,
            .hp = 140.0f,
            .fire_cooldown = 0.35f
        });
    }

    for (float ax : {940.0f, 1100.0f}) {
        uint32_t ix = std::clamp(uint32_t(ax), 0u, W - 1);
        app.aa_guns.push_back({
            .id = aa_id++,
            .team = Team::Blue,
            .x = ax,
            .y = app.heightmap[ix],
            .turret_angle = -1.80f,
            .hp = 140.0f,
            .fire_cooldown = 0.35f
        });
    }

    // 11. Spawn Air Support Bombing Aircraft
    app.aircraft.clear();
    app.aircraft.push_back({.id = 1, .team = Team::Red, .x = 50.0f, .y = 85.0f, .vx = 220.0f, .hp = 80.0f});
    app.aircraft.push_back({.id = 2, .team = Team::Red, .x = -150.0f, .y = 120.0f, .vx = 240.0f, .hp = 80.0f});
    app.aircraft.push_back({.id = 3, .team = Team::Blue, .x = FW - 50.0f, .y = 95.0f, .vx = -220.0f, .hp = 80.0f});
    app.aircraft.push_back({.id = 4, .team = Team::Blue, .x = FW + 150.0f, .y = 135.0f, .vx = -240.0f, .hp = 80.0f});

    app.shells.clear();
    app.flak_shells.clear();
    app.blasts.clear();
    app.embers.clear();
}

// ----------------------------------------------------------------------------
// Autonomous AI Ballistic Aiming & Tank Combat Loop
// ----------------------------------------------------------------------------
static void update_pocket_tanks_combat(float dt) {
    auto& app = g_app;

    app.red_tanks_alive = 0;
    app.blue_tanks_alive = 0;

    // Update Combustible Trees & Fire Propagation
    for (auto& tree : app.trees) {
        uint32_t ix = std::clamp(uint32_t(tree.x), 0u, W - 1);
        tree.y = app.heightmap[ix];

        if (tree.is_destroyed) continue;

        tree.temp += (20.0f - tree.temp) * (0.4f * dt);
        if (tree.temp > 180.0f) tree.is_on_fire = true;

        if (tree.is_on_fire) {
            tree.hp -= 28.0f * dt;
            tree.temp = std::max(tree.temp, 700.0f);

            // Emit crackling fire embers & dark smoke
            if (app.frame % 2 == 0) {
                app.embers.push_back({
                    .x = tree.x + (float(std::rand() % 18) - 9.0f),
                    .y = tree.y - (float(std::rand() % 24) + 8.0f),
                    .vx = (float(std::rand() % 36) - 18.0f),
                    .vy = -(40.0f + float(std::rand() % 65)),
                    .life = 0.8f, .max_life = 0.5f, .size = 3.2f,
                    .temp = 950.0f, .is_smoke = (app.frame % 4 == 0)
                });
            }

            // Fire spreads to nearby trees and tanks
            for (auto& other_tree : app.trees) {
                if (other_tree.is_destroyed || other_tree.id == tree.id) continue;
                float dx = other_tree.x - tree.x;
                float dy = other_tree.y - tree.y;
                if (dx * dx + dy * dy < 55.0f * 55.0f) {
                    other_tree.temp += 140.0f * dt;
                }
            }
            for (auto& tank : app.tanks) {
                if (tank.is_destroyed) continue;
                float dx = tank.x - tree.x;
                float dy = tank.y - tree.y;
                if (dx * dx + dy * dy < 50.0f * 50.0f) {
                    tank.armor_temp += 160.0f * dt;
                }
            }
        }

        if (tree.hp <= 0.0f) {
            tree.hp = 0.0f;
            tree.is_destroyed = true;
        }
    }

    for (auto& tank : app.tanks) {
        // Dynamic Terrain Sticking & Gravity Fall into Craters
        uint32_t tx_idx = std::clamp(uint32_t(tank.x), 0u, W - 1);
        float target_ground_y = app.heightmap[tx_idx] - (tank.body_h * 0.5f);
        if (tank.y < target_ground_y) {
            tank.y += std::min(450.0f * dt, target_ground_y - tank.y);
        }
        else {
            tank.y = target_ground_y;
        }

        if (tank.is_destroyed) continue;
        if (tank.team == Team::Red) app.red_tanks_alive++;
        else app.blue_tanks_alive++;

        // Progressive Thermal & Fire Damage over time (Not instant)
        tank.armor_temp += (20.0f - tank.armor_temp) * (0.6f * dt);
        if (tank.armor_temp > 220.0f) {
            float temp_ratio = (tank.armor_temp - 200.0f) / 600.0f;
            tank.hp -= temp_ratio * 16.0f * dt; // Progressive heat degradation

            if (tank.armor_temp > 600.0f) tank.is_melting = true;

            // Smoke & fire plumes while burning
            if (app.frame % 3 == 0) {
                app.embers.push_back({
                    .x = tank.x + (float(std::rand() % 16) - 8.0f),
                    .y = tank.y - 6.0f,
                    .vx = (float(std::rand() % 40) - 20.0f),
                    .vy = -(35.0f + float(std::rand() % 50)),
                    .life = 0.8f, .max_life = 0.5f, .size = 3.0f,
                    .temp = tank.armor_temp,
                    .is_smoke = (tank.armor_temp < 500.0f)
                });
            }
        }

        if (tank.hp <= 0.0f) {
            tank.hp = 0.0f;
            tank.is_destroyed = true;

            // Spawn explosion wreck blast
            app.blasts.push_back({
                .x = tank.x, .y = tank.y, .radius = 0.0f, .max_radius = 50.0f, .life = 1.0f, .team = tank.team
            });
            for (int k = 0; k < 18; ++k) {
                app.embers.push_back({
                    .x = tank.x + (float(std::rand() % 20) - 10.0f),
                    .y = tank.y + (float(std::rand() % 16) - 8.0f),
                    .vx = (float(std::rand() % 160) - 80.0f),
                    .vy = -(80.0f + float(std::rand() % 120)),
                    .life = 1.0f,
                    .max_life = 0.6f + float(std::rand() % 40) * 0.01f,
                    .size = 3.0f + float(std::rand() % 5),
                    .temp = 900.0f,
                    .is_smoke = (k % 2 == 0)
                });
            }
            continue;
        }

        // Recoil decay
        if (tank.firing_recoil > 0.0f) {
            tank.firing_recoil -= 6.0f * dt;
            if (tank.firing_recoil < 0.0f) tank.firing_recoil = 0.0f;
        }

        // Autonomous Target Selection & Parabolic Trajectory Aiming
        tank.reload_timer += dt;

        PocketTank* target_enemy = nullptr;
        float min_dist = 9999.0f;

        for (auto& enemy : app.tanks) {
            if (enemy.is_destroyed || enemy.team == tank.team) continue;
            float dx = enemy.x - tank.x;
            float dy = enemy.y - tank.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < min_dist) {
                min_dist = dist;
                target_enemy = &enemy;
            }
        }

        if (target_enemy) {
            float dx = target_enemy->x - tank.x;
            float dy = target_enemy->y - tank.y;
            float gravity = 650.0f;
            float v0 = std::clamp(std::sqrt(std::abs(dx) * gravity * 1.05f), 420.0f, 780.0f);

            float val = std::clamp((gravity * std::abs(dx)) / (v0 * v0), 0.1f, 0.98f);
            float theta = 0.5f * std::asin(val);

            if (std::abs(dx) > 400.0f) {
                theta = (std::numbers::pi_v<float> * 0.5f) - theta; // High lob arc
            }

            if (dx < 0.0f) {
                tank.target_turret_angle = -(std::numbers::pi_v<float> - theta);
            }
            else {
                tank.target_turret_angle = -theta;
            }

            float angle_diff = tank.target_turret_angle - tank.turret_angle;
            tank.turret_angle += angle_diff * std::min(1.0f, 3.5f * dt);

            if (tank.reload_timer >= tank.reload_cooldown && std::abs(angle_diff) < 0.15f) {
                tank.reload_timer = 0.0f;
                tank.firing_recoil = 1.0f;

                float barrel_len = 16.0f;
                float muzzle_x = tank.x + std::cos(tank.turret_angle) * barrel_len;
                float muzzle_y = tank.y + std::sin(tank.turret_angle) * barrel_len;

                float vx = std::cos(tank.turret_angle) * v0;
                float vy = std::sin(tank.turret_angle) * v0;

                app.shells.push_back({
                    .x = muzzle_x,
                    .y = muzzle_y,
                    .vx = vx,
                    .vy = vy,
                    .radius = 3.2f,
                    .damage = 25.0f, // Initial hit damage (rest from shrapnel & heat)
                    .blast_radius = 55.0f,
                    .team = tank.team,
                    .active = true,
                    .life = 0.0f
                });

                for (int m = 0; m < 5; ++m) {
                    app.embers.push_back({
                        .x = muzzle_x,
                        .y = muzzle_y,
                        .vx = vx * 0.2f + (float(std::rand() % 60) - 30.0f),
                        .vy = vy * 0.2f + (float(std::rand() % 60) - 30.0f),
                        .life = 0.6f,
                        .max_life = 0.3f,
                        .size = 2.5f,
                        .temp = 1100.0f,
                        .is_smoke = false
                    });
                }
            }
        }
    }

    // Update Artillery Shell Trajectories
    for (auto& shell : app.shells) {
        if (!shell.active) continue;
        shell.life += dt;
        shell.x += shell.vx * dt;
        shell.y += shell.vy * dt;
        shell.vy += 650.0f * dt;

        if (app.frame % 2 == 0) {
            app.embers.push_back({
                .x = shell.x,
                .y = shell.y,
                .vx = (float(std::rand() % 30) - 15.0f),
                .vy = -(15.0f + float(std::rand() % 20)),
                .life = 1.0f,
                .max_life = 0.5f,
                .size = 3.5f,
                .temp = 500.0f,
                .is_smoke = true
            });
        }

        uint32_t ix = std::clamp(uint32_t(shell.x), 0u, W - 1);
        float terrain_y = app.heightmap[ix];

        bool hit_terrain = (shell.y >= terrain_y);
        bool hit_tank = false;
        bool hit_building = false;

        // Check Tank Hits
        for (auto& tank : app.tanks) {
            if (tank.is_destroyed) continue;
            if (tank.team == shell.team && shell.life < 0.2f) continue;

            float dx = shell.x - tank.x;
            float dy = shell.y - tank.y;
            if (dx * dx + dy * dy < (tank.body_w * 0.6f) * (tank.body_w * 0.6f)) {
                hit_tank = true;
                tank.hp -= shell.damage;
                tank.armor_temp += 300.0f;
                break;
            }
        }

        // Check Destructible Building Hits
        for (auto& bldg : app.buildings) {
            if (bldg.is_destroyed) continue;
            if (shell.x >= bldg.x - bldg.width * 0.5f && shell.x <= bldg.x + bldg.width * 0.5f &&
                shell.y >= bldg.y - bldg.height && shell.y <= bldg.y) {
                hit_building = true;
                bldg.hp -= shell.damage * 1.4f;
                if (bldg.hp <= 0.0f) {
                    bldg.hp = 0.0f;
                    bldg.is_destroyed = true;
                }
                break;
            }
        }

        // Trigger Realistic Multi-Stage Bomb Explosion & Soil Fountain
        if (hit_terrain || hit_tank || hit_building || shell.x < 10.0f || shell.x > FW - 10.0f || shell.y > FH -
            10.0f) {
            shell.active = false;

            // Phase 1: Supersonic Pressure Wave & Core Fireball Flash
            app.blasts.push_back({
                .x = shell.x,
                .y = shell.y,
                .radius = 0.0f,
                .max_radius = 65.0f,
                .life = 1.0f,
                .team = shell.team
            });

            // Phase 2: High-Velocity Incendiary Shrapnel Shards (18 tungsten casing fragments)
            for (int s = 0; s < 18; ++s) {
                float angle = float(s) * (6.2831853f / 18.0f) + (float(std::rand() % 25) * 0.01f);
                float speed = 350.0f + float(std::rand() % 420);
                app.shards.push_back({
                    .x = shell.x,
                    .y = shell.y,
                    .vx = std::cos(angle) * speed,
                    .vy = std::sin(angle) * speed,
                    .damage = 18.0f,
                    .life = 0.9f,
                    .active = true
                });
            }

            // Phase 3: Carve Terrain Crater & Blast SPH Soil Particles into Explosive Fountain
            if (hit_terrain || shell.y >= terrain_y - 25.0f) {
                int crater_r = 38;
                for (int cx = -crater_r; cx <= crater_r; ++cx) {
                    int tx = std::clamp(int(shell.x) + cx, 0, int(W) - 1);
                    float dist = std::abs(float(cx));
                    float depth = std::sqrt(std::max(0.0f, float(crater_r * crater_r) - dist * dist)) * 0.82f;
                    app.heightmap[tx] = std::min(FH - 20.0f, app.heightmap[tx] + depth);
                }

                // Blast SPH soil particles inside crater radius into an explosive dirt fountain plume
                if (app.world) {
                    auto& P = app.world->particles();
                    const std::size_t N = P.size();
                    for (std::size_t i = 0; i < N; ++i) {
                        float dx = P.pos_x[i] - shell.x;
                        float dy = P.pos_y[i] - shell.y;
                        if (dx * dx + dy * dy < 48.0f * 48.0f) {
                            P.vel_x[i] = dx * 10.0f + (float(std::rand() % 140) - 70.0f);
                            P.vel_y[i] = -(350.0f + float(std::rand() % 400)); // Upward dirt fountain
                            P.temperature[i] += 550.0f;
                        }
                    }
                }

                // Rebuild Terrain Obstacle Capsules
                app.terrain_obstacles.capsules.clear();
                for (uint32_t x = 20; x < W - 20; x += 25) {
                    float x1 = float(x);
                    float x2 = float(x + 25);
                    float y1 = app.heightmap[x];
                    float y2 = app.heightmap[std::min(W - 1, x + 25)];
                    app.terrain_obstacles.capsules.push_back(akruti::Capsule{
                        pebble::math::vec2(x1, y1),
                        pebble::math::vec2(x2, y2),
                        12.0f
                    });
                }
            }

            // Phase 4: Radial Area-of-Effect Thermal Heating & Pressure Wave
            for (auto& tank : app.tanks) {
                if (tank.is_destroyed) continue;
                float dx = tank.x - shell.x;
                float dy = tank.y - shell.y;
                float d2 = dx * dx + dy * dy;
                if (d2 < shell.blast_radius * shell.blast_radius) {
                    float dist = std::sqrt(d2);
                    float damage_ratio = 1.0f - (dist / shell.blast_radius);
                    tank.hp -= 18.0f * damage_ratio;
                    tank.armor_temp += 280.0f * damage_ratio;
                }
            }

            // Phase 4b: Heat & Ignite Combustible Trees
            for (auto& tree : app.trees) {
                if (tree.is_destroyed) continue;
                float dx = tree.x - shell.x;
                float dy = tree.y - shell.y;
                float d2 = dx * dx + dy * dy;
                if (d2 < (shell.blast_radius + 20.0f) * (shell.blast_radius + 20.0f)) {
                    float dist = std::sqrt(d2);
                    float damage_ratio = 1.0f - (dist / (shell.blast_radius + 20.0f));
                    tree.hp -= 40.0f * damage_ratio;
                    tree.temp += 380.0f * damage_ratio;
                }
            }

            // Phase 5: Rising Mushroom Cloud & Billowing Dark Smoke Column
            for (int k = 0; k < 28; ++k) {
                bool is_dark_smoke = (k % 2 == 0);
                float smoke_vy = is_dark_smoke
                                     ? -(140.0f + float(std::rand() % 160))
                                     : -(80.0f + float(std::rand() % 180));
                app.embers.push_back({
                    .x = shell.x + (float(std::rand() % 24) - 12.0f),
                    .y = shell.y,
                    .vx = (float(std::rand() % 240) - 120.0f),
                    .vy = smoke_vy,
                    .life = 1.0f,
                    .max_life = 0.5f + float(std::rand() % 50) * 0.01f,
                    .size = is_dark_smoke ? (4.0f + float(std::rand() % 6)) : (2.5f + float(std::rand() % 4)),
                    .temp = is_dark_smoke ? 400.0f : (1100.0f + float(std::rand() % 400)),
                    .is_smoke = is_dark_smoke
                });
            }
        }
    }

    std::erase_if(app.shells, [](const ShellProjectile& s) { return !s.active; });

    // Update Flying Shrapnel Shards & Collisions
    for (auto& shard : app.shards) {
        if (!shard.active) continue;
        shard.life -= dt;
        shard.x += shard.vx * dt;
        shard.y += shard.vy * dt;
        shard.vy += 450.0f * dt; // Shard gravity

        // Check Tank Collisions
        for (auto& tank : app.tanks) {
            if (tank.is_destroyed) continue;
            float dx = shard.x - tank.x;
            float dy = shard.y - tank.y;
            if (dx * dx + dy * dy < 20.0f * 20.0f) {
                shard.active = false;
                tank.hp -= shard.damage;
                tank.armor_temp += 140.0f;
                // Shard Impact Spark
                app.embers.push_back({
                    .x = shard.x,
                    .y = shard.y,
                    .vx = -shard.vx * 0.25f,
                    .vy = -shard.vy * 0.25f,
                    .life = 0.4f,
                    .max_life = 0.3f,
                    .size = 2.5f,
                    .temp = 1050.0f,
                    .is_smoke = false
                });
                break;
            }
        }

        // Check Tree Collisions
        for (auto& tree : app.trees) {
            if (!shard.active || tree.is_destroyed) continue;
            float dx = shard.x - tree.x;
            float dy = shard.y - (tree.y - 18.0f);
            if (dx * dx + dy * dy < 18.0f * 18.0f) {
                shard.active = false;
                tree.hp -= shard.damage;
                tree.temp += 160.0f;
                break;
            }
        }

        if (shard.life <= 0.0f) shard.active = false;
    }
    std::erase_if(app.shards, [](const ShrapnelShard& s) { return !s.active; });

    // Update Aircraft Flight & Aerial Bombing Runs
    for (auto& plane : app.aircraft) {
        if (plane.is_destroyed) continue;

        plane.x += plane.vx * dt;

        if (plane.vx > 0.0f && plane.x > FW + 100.0f) plane.x = -150.0f;
        if (plane.vx < 0.0f && plane.x < -100.0f) plane.x = FW + 150.0f;

        // Exhaust trail
        if (app.frame % 3 == 0) {
            app.embers.push_back({
                .x = plane.x - (plane.vx > 0.0f ? 15.0f : -15.0f),
                .y = plane.y + 2.0f,
                .vx = -plane.vx * 0.1f,
                .vy = -(10.0f + float(std::rand() % 15)),
                .life = 0.6f, .max_life = 0.4f, .size = 3.0f, .temp = 300.0f, .is_smoke = true
            });
        }

        // Drop Heavy Aerial Gravity Bombs over enemy ground units
        plane.bomb_timer += dt;
        if (plane.bomb_timer >= plane.bomb_cooldown && plane.x > 100.0f && plane.x < FW - 100.0f) {
            plane.bomb_timer = 0.0f;
            app.shells.push_back({
                .x = plane.x,
                .y = plane.y + 12.0f,
                .vx = plane.vx * 0.35f,
                .vy = 140.0f,
                .radius = 4.5f,
                .damage = 65.0f,
                .blast_radius = 70.0f,
                .team = plane.team,
                .active = true,
                .life = 0.0f
            });
        }

        if (plane.hp <= 0.0f) {
            plane.is_destroyed = true;
            app.blasts.push_back({
                .x = plane.x, .y = plane.y, .radius = 0.0f, .max_radius = 60.0f, .life = 1.0f, .team = plane.team
            });
        }
    }

    // Update AA Flak Guns
    for (auto& aa : app.aa_guns) {
        uint32_t ix = std::clamp(uint32_t(aa.x), 0u, W - 1);
        aa.y = app.heightmap[ix];

        if (aa.is_destroyed) continue;

        AirCraft* target_plane = nullptr;
        float min_dist = 9999.0f;
        for (auto& plane : app.aircraft) {
            if (plane.is_destroyed || plane.team == aa.team) continue;
            float dx = plane.x - aa.x;
            float dy = plane.y - aa.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < min_dist) {
                min_dist = dist;
                target_plane = &plane;
            }
        }

        if (target_plane) {
            float dx = target_plane->x - aa.x;
            float dy = target_plane->y - aa.y;
            aa.target_angle = std::atan2(dy, dx);
            aa.turret_angle += (aa.target_angle - aa.turret_angle) * std::min(1.0f, 6.0f * dt);

            aa.fire_timer += dt;
            if (aa.fire_timer >= aa.fire_cooldown) {
                aa.fire_timer = 0.0f;
                float v0 = 880.0f;
                app.flak_shells.push_back({
                    .x = aa.x + std::cos(aa.turret_angle) * 16.0f,
                    .y = aa.y + std::sin(aa.turret_angle) * 16.0f,
                    .vx = std::cos(aa.turret_angle) * v0,
                    .vy = std::sin(aa.turret_angle) * v0,
                    .fuse_life = std::clamp(min_dist / v0, 0.15f, 1.1f),
                    .team = aa.team,
                    .active = true
                });
            }
        }
    }

    // Update Mid-Air Flak Shells & Proximity Detonations
    for (auto& flak : app.flak_shells) {
        if (!flak.active) continue;
        flak.fuse_life -= dt;
        flak.x += flak.vx * dt;
        flak.y += flak.vy * dt;

        app.embers.push_back({
            .x = flak.x, .y = flak.y,
            .vx = (float(std::rand() % 20) - 10.0f), .vy = (float(std::rand() % 20) - 10.0f),
            .life = 0.3f, .max_life = 0.2f, .size = 2.0f, .temp = 1200.0f, .is_smoke = false
        });

        bool hit_plane = false;
        for (auto& plane : app.aircraft) {
            if (plane.is_destroyed || plane.team == flak.team) continue;
            float dx = flak.x - plane.x;
            float dy = flak.y - plane.y;
            if (dx * dx + dy * dy < 35.0f * 35.0f) {
                hit_plane = true;
                plane.hp -= 35.0f;
                break;
            }
        }

        if (flak.fuse_life <= 0.0f || hit_plane || flak.y < 10.0f) {
            flak.active = false;
            for (int p = 0; p < 12; ++p) {
                app.embers.push_back({
                    .x = flak.x + (float(std::rand() % 16) - 8.0f),
                    .y = flak.y + (float(std::rand() % 16) - 8.0f),
                    .vx = (float(std::rand() % 120) - 60.0f),
                    .vy = (float(std::rand() % 120) - 60.0f),
                    .life = 0.8f, .max_life = 0.5f, .size = 4.5f, .temp = 400.0f, .is_smoke = true
                });
            }
        }
    }
    std::erase_if(app.flak_shells, [](const FlakShell& f) { return !f.active; });

    // Update Expanding Blast Shockwaves
    for (auto& blast : app.blasts) {
        blast.radius += (blast.max_radius - blast.radius) * (6.0f * dt);
        blast.life -= 2.2f * dt;
    }
    std::erase_if(app.blasts, [](const BlastEffect& b) { return b.life <= 0.0f; });

    // Update Embers & Smoke
    for (auto& em : app.embers) {
        em.x += em.vx * dt;
        em.y += em.vy * dt;
        em.life -= dt / em.max_life;
    }
    std::erase_if(app.embers, [](const ParticleEmber& e) { return e.life <= 0.0f; });
}

// ----------------------------------------------------------------------------
// Scene Building for Kalpana Vector & GPU Instanced Rendering
// ----------------------------------------------------------------------------
static void build_tanks_scene(kalpana::Scene& scene) {
    auto& app = g_app;
    app.instanced_particles.begin();
    scene.clear_color(kalpana::Color{0.04f, 0.05f, 0.08f, 1.0f});

    // 1. Background Grid & Atmosphere Sky
    {
        kalpana::Color gc{0.08f, 0.10f, 0.14f, 1.0f};
        kalpana::Path grid_lines;
        for (int i = 0; i <= 32; ++i) {
            float x = float(i) / 32.0f * FW;
            grid_lines.move_to(x, 0.0f);
            grid_lines.line_to(x, FH);
        }
        for (int j = 0; j <= 18; ++j) {
            float y = float(j) / 18.0f * FH;
            grid_lines.move_to(0.0f, y);
            grid_lines.line_to(FW, y);
        }
        scene.add(kalpana::Node::shape(grid_lines, kalpana::Paint::stroke(gc, 1.0f)));
    }

    // 2. Multi-Layered Destructible Terrain Strata (Bedrock, Subsoil, Top Grass Crust)
    {
        // Deep Bedrock Earth Fill
        kalpana::Path bedrock_fill;
        bedrock_fill.move_to(0.0f, FH);
        bedrock_fill.line_to(0.0f, app.heightmap[0]);
        for (uint32_t x = 4; x < W; x += 4) {
            bedrock_fill.line_to(float(x), app.heightmap[x]);
        }
        bedrock_fill.line_to(FW, FH);
        bedrock_fill.close();
        scene.add(kalpana::Node::shape(bedrock_fill, kalpana::Paint::fill(kalpana::Color{0.10f, 0.12f, 0.10f, 1.0f})));

        // Subsoil Layer Band
        kalpana::Path subsoil_fill;
        subsoil_fill.move_to(0.0f, app.heightmap[0]);
        for (uint32_t x = 4; x < W; x += 4) {
            subsoil_fill.line_to(float(x), app.heightmap[x]);
        }
        scene.add(kalpana::Node::shape(subsoil_fill,
                                       kalpana::Paint::stroke(kalpana::Color{0.22f, 0.28f, 0.18f, 1.0f}, 12.0f)));

        // Surface Grass Crust
        kalpana::Path grass_crust;
        grass_crust.move_to(0.0f, app.heightmap[0]);
        for (uint32_t x = 4; x < W; x += 4) {
            grass_crust.line_to(float(x), app.heightmap[x]);
        }
        scene.add(kalpana::Node::shape(grass_crust,
                                       kalpana::Paint::stroke(kalpana::Color{0.32f, 0.58f, 0.22f, 1.0f}, 4.0f)));
    }

    // 2b. Render Combustible Pine Trees (Normal, Burning, or Charred)
    for (const auto& tree : app.trees) {
        if (tree.is_destroyed) {
            // Charred Stump
            kalpana::Path stump;
            stump.rect(tree.x - 2.5f, tree.y - 8.0f, 5.0f, 8.0f);
            scene.add(kalpana::Node::shape(stump, kalpana::Paint::fill(kalpana::Color{0.12f, 0.12f, 0.12f, 0.95f})));
            continue;
        }

        // Tree Trunk
        kalpana::Path trunk;
        trunk.rect(tree.x - 3.0f, tree.y - 12.0f, 6.0f, 12.0f);
        scene.add(kalpana::Node::shape(trunk, kalpana::Paint::fill(kalpana::Color{0.24f, 0.16f, 0.10f, 1.0f})));

        // Pine Foliage Pyramid
        kalpana::Path foliage;
        foliage.move_to(tree.x, tree.y - tree.height);
        foliage.line_to(tree.x - tree.width * 0.5f, tree.y - 10.0f);
        foliage.line_to(tree.x + tree.width * 0.5f, tree.y - 10.0f);
        foliage.close();

        kalpana::Color foliage_col = tree.is_on_fire
                                         ? kalpana::Color{0.95f, 0.38f, 0.08f, 1.0f} // Burning Orange
                                         : kalpana::Color{0.18f, 0.44f, 0.14f, 1.0f}; // Pine Green

        scene.add(kalpana::Node::shape(foliage, kalpana::Paint::fill(foliage_col)));

        // Flame Cap Overlay when On Fire
        if (tree.is_on_fire) {
            kalpana::Path flame_top;
            flame_top.circle(tree.x + (float(std::rand() % 8) - 4.0f), tree.y - tree.height * 0.65f, 6.0f);
            scene.add(kalpana::Node::shape(flame_top, kalpana::Paint::fill(kalpana::Color{1.0f, 0.85f, 0.20f, 0.85f})));
        }
    }

    // 2c. Render Destructible Command Buildings & Bunkers
    for (const auto& bldg : app.buildings) {
        if (bldg.is_destroyed) {
            // Collapsed Concrete Rubble
            kalpana::Path rubble;
            rubble.rect(bldg.x - bldg.width * 0.5f, bldg.y - 15.0f, bldg.width, 15.0f);
            scene.add(kalpana::Node::shape(rubble, kalpana::Paint::fill(kalpana::Color{0.18f, 0.18f, 0.20f, 0.90f})));
            continue;
        }

        // Multi-floor Concrete Facade
        kalpana::Path facade;
        facade.rect(bldg.x - bldg.width * 0.5f, bldg.y - bldg.height, bldg.width, bldg.height);
        kalpana::Color facade_col = (bldg.team == Team::Red)
                                        ? kalpana::Color{0.32f, 0.22f, 0.22f, 1.0f}
                                        : kalpana::Color{0.20f, 0.26f, 0.35f, 1.0f};
        scene.add(kalpana::Node::shape(facade, kalpana::Paint::fill(facade_col)));
        scene.add(kalpana::Node::shape(
            facade, kalpana::Paint::stroke(kalpana::Color{0.10f, 0.12f, 0.15f, 1.0f}, 2.0f)));

        // Illuminated Windows per Floor
        for (uint32_t f = 0; f < bldg.num_floors; ++f) {
            float floor_y = bldg.y - bldg.height + 10.0f + float(f) * 16.0f;
            kalpana::Path win1, win2;
            win1.rect(bldg.x - 12.0f, floor_y, 8.0f, 10.0f);
            win2.rect(bldg.x + 4.0f, floor_y, 8.0f, 10.0f);
            scene.add(kalpana::Node::shape(win1, kalpana::Paint::fill(kalpana::Color{1.0f, 0.90f, 0.40f, 0.85f})));
            scene.add(kalpana::Node::shape(win2, kalpana::Paint::fill(kalpana::Color{1.0f, 0.90f, 0.40f, 0.85f})));
        }
    }

    // 2d. Render Anti-Aircraft (AA) Flak Cannon Batteries
    for (const auto& aa : app.aa_guns) {
        if (aa.is_destroyed) continue;

        // Base Platform
        kalpana::Path base;
        base.rect(aa.x - 12.0f, aa.y - 6.0f, 24.0f, 6.0f);
        scene.add(kalpana::Node::shape(base, kalpana::Paint::fill(kalpana::Color{0.25f, 0.28f, 0.32f, 1.0f})));

        // Dual Flak Cannon Barrels pointing into sky
        float barrel_len = 22.0f;
        float mx = aa.x + std::cos(aa.turret_angle) * barrel_len;
        float my = aa.y + std::sin(aa.turret_angle) * barrel_len;

        kalpana::Path b1;
        b1.move_to(aa.x, aa.y - 4.0f);
        b1.line_to(mx, my);
        scene.add(kalpana::Node::shape(b1, kalpana::Paint::stroke(kalpana::Color{0.90f, 0.92f, 0.95f, 1.0f}, 3.0f)));
    }

    // 2e. Render Air Support Bombing Aircraft
    for (const auto& plane : app.aircraft) {
        if (plane.is_destroyed) continue;

        float dir = plane.vx > 0.0f ? 1.0f : -1.0f;

        // Jet Fuselage
        kalpana::Path jet;
        jet.move_to(plane.x + 22.0f * dir, plane.y);
        jet.line_to(plane.x - 18.0f * dir, plane.y - 6.0f);
        jet.line_to(plane.x - 14.0f * dir, plane.y + 6.0f);
        jet.close();

        kalpana::Color plane_col = (plane.team == Team::Red)
                                       ? kalpana::Color{0.92f, 0.30f, 0.25f, 1.0f}
                                       : kalpana::Color{0.25f, 0.60f, 0.95f, 1.0f};

        scene.add(kalpana::Node::shape(jet, kalpana::Paint::fill(plane_col)));
        scene.add(kalpana::Node::shape(jet, kalpana::Paint::stroke(kalpana::Color{0.95f, 0.95f, 0.98f, 1.0f}, 1.5f)));

        // Swept Wings
        kalpana::Path wing;
        wing.move_to(plane.x, plane.y);
        wing.line_to(plane.x - 8.0f * dir, plane.y - 14.0f);
        wing.line_to(plane.x - 12.0f * dir, plane.y + 14.0f);
        wing.close();
        scene.add(kalpana::Node::shape(wing, kalpana::Paint::stroke(plane_col, 2.5f)));
    }

    // 3. Render Red & Blue Mini-Tanks
    for (const auto& tank : app.tanks) {
        if (tank.is_destroyed) {
            // Render Destroyed Wreckage
            kalpana::Path wreck;
            wreck.rect(tank.x - tank.body_w * 0.5f, tank.y - tank.body_h * 0.5f, tank.body_w, tank.body_h);
            scene.add(kalpana::Node::shape(wreck, kalpana::Paint::fill(kalpana::Color{0.12f, 0.12f, 0.14f, 0.9f})));
            continue;
        }

        kalpana::Color body_col = (tank.team == Team::Red)
                                      ? kalpana::Color{0.88f, 0.22f, 0.22f, 1.0f}
                                      : kalpana::Color{0.22f, 0.55f, 0.92f, 1.0f};

        if (tank.is_melting) {
            body_col = kalpana::Color{1.0f, 0.45f, 0.10f, 1.0f}; // Molten glow
        }

        // Tank Tread Chassis
        kalpana::Path chassis;
        chassis.round_rect(tank.x - tank.body_w * 0.5f, tank.y - tank.body_h * 0.5f, tank.body_w, tank.body_h, 3.0f,
                           3.0f);
        scene.add(kalpana::Node::shape(chassis, kalpana::Paint::fill(body_col)));
        scene.add(
            kalpana::Node::shape(chassis, kalpana::Paint::stroke(kalpana::Color{0.08f, 0.08f, 0.10f, 1.0f}, 1.5f)));

        // Tank Turret Barrel
        float recoil_offset = tank.firing_recoil * 3.5f;
        float barrel_len = 16.0f - recoil_offset;
        float muzzle_x = tank.x + std::cos(tank.turret_angle) * barrel_len;
        float muzzle_y = tank.y + std::sin(tank.turret_angle) * barrel_len;

        kalpana::Path barrel;
        barrel.move_to(tank.x, tank.y - 2.0f);
        barrel.line_to(muzzle_x, muzzle_y);
        scene.add(kalpana::Node::shape(
            barrel, kalpana::Paint::stroke(kalpana::Color{0.85f, 0.88f, 0.92f, 1.0f}, 3.5f)));

        // Turret Cap Dome
        kalpana::Path dome;
        dome.circle(tank.x, tank.y - 2.0f, 5.0f);
        scene.add(kalpana::Node::shape(dome, kalpana::Paint::fill(kalpana::Color{0.95f, 0.95f, 0.98f, 1.0f})));

        // Health Bar Indicator
        float hp_ratio = std::clamp(tank.hp / tank.max_hp, 0.0f, 1.0f);
        kalpana::Path hp_bg;
        hp_bg.rect(tank.x - 14.0f, tank.y - 15.0f, 28.0f, 3.0f);
        scene.add(kalpana::Node::shape(hp_bg, kalpana::Paint::fill(kalpana::Color{0.1f, 0.1f, 0.1f, 0.8f})));

        kalpana::Path hp_fg;
        hp_fg.rect(tank.x - 14.0f, tank.y - 15.0f, 28.0f * hp_ratio, 3.0f);
        kalpana::Color hp_col = hp_ratio > 0.5f
                                    ? kalpana::Color{0.2f, 0.85f, 0.3f, 1.0f}
                                    : kalpana::Color{0.9f, 0.2f, 0.2f, 1.0f};
        scene.add(kalpana::Node::shape(hp_fg, kalpana::Paint::fill(hp_col)));
    }

    // 4. Render Active Shell Projectiles & AA Flak Tracers
    for (const auto& shell : app.shells) {
        kalpana::Path shell_path;
        shell_path.circle(shell.x, shell.y, shell.radius);
        kalpana::Color shell_col = (shell.team == Team::Red)
                                       ? kalpana::Color{1.0f, 0.85f, 0.20f, 1.0f}
                                       : kalpana::Color{0.30f, 0.85f, 1.0f, 1.0f};
        scene.add(kalpana::Node::shape(shell_path, kalpana::Paint::fill(shell_col)));
    }

    for (const auto& flak : app.flak_shells) {
        if (!flak.active) continue;
        kalpana::Path tracer;
        tracer.move_to(flak.x, flak.y);
        tracer.line_to(flak.x - flak.vx * 0.03f, flak.y - flak.vy * 0.03f);
        scene.add(kalpana::Node::shape(
            tracer, kalpana::Paint::stroke(kalpana::Color{1.0f, 0.95f, 0.50f, 0.95f}, 2.2f)));
    }

    // 4b. Render Flying Shrapnel Shards
    for (const auto& shard : app.shards) {
        if (!shard.active) continue;
        kalpana::Path streak;
        streak.move_to(shard.x, shard.y);
        streak.line_to(shard.x - shard.vx * 0.04f, shard.y - shard.vy * 0.04f);
        scene.add(kalpana::Node::shape(
            streak, kalpana::Paint::stroke(kalpana::Color{1.0f, 0.90f, 0.35f, 0.90f}, 1.8f)));
    }

    // 5. Render Expanding Explosive Shockwave Rings
    for (const auto& blast : app.blasts) {
        kalpana::Path ring;
        ring.circle(blast.x, blast.y, blast.radius);
        float alpha = std::clamp(blast.life, 0.0f, 1.0f);
        kalpana::Color blast_col = (blast.team == Team::Red)
                                       ? kalpana::Color{1.0f, 0.40f, 0.10f, alpha * 0.85f}
                                       : kalpana::Color{0.20f, 0.70f, 1.0f, alpha * 0.85f};
        scene.add(kalpana::Node::shape(ring, kalpana::Paint::stroke(blast_col, 4.0f * alpha)));
    }

    // 6. Push Dynamic Instanced Airborne Debris & Molten Earth Sparks (Heterogeneous Composition)
    if (app.world) {
        auto& P = app.world->particles();
        const std::size_t N = P.size();
        for (std::size_t i = 0; i < N; ++i) {
            float px = P.pos_x[i];
            float py = P.pos_y[i];
            float vx = P.vel_x[i];
            float vy = P.vel_y[i];
            float spd2 = vx * vx + vy * vy;
            float t = P.temperature[i];
            auto mat = P.material[i];

            // Render active airborne debris (blasted into the air) or molten soil sparks
            if (spd2 > 120.0f || t > 180.0f) {
                kalpana::Color p_col{0.55f, 0.42f, 0.28f, 0.90f}; // Topsoil brown
                if (mat == app.mat_sandstone) {
                    p_col = {0.72f, 0.58f, 0.42f, 0.90f}; // Sandstone tan
                }
                else if (mat == app.mat_granite) {
                    p_col = {0.35f, 0.38f, 0.42f, 0.95f}; // Granite slate gray
                }
                else if (mat == app.mat_iron_ore) {
                    p_col = {0.72f, 0.28f, 0.16f, 0.95f}; // Metallic iron ore rust
                }

                if (t > 700.0f) {
                    p_col = {1.0f, 0.50f, 0.10f, 0.95f}; // Molten rock/magma
                }
                app.instanced_particles.add_instance(px, py, 2.8f, p_col);
            }
        }
    }

    // Push Visual Fire Embers & Smoke
    for (const auto& em : app.embers) {
        float alpha = std::clamp(em.life, 0.0f, 1.0f);
        kalpana::Color col;
        if (em.is_smoke) {
            col = {0.45f, 0.48f, 0.52f, alpha * 0.40f};
        }
        else if (em.temp > 1000.0f) {
            col = {1.0f, 0.95f, 0.40f, alpha * 0.90f};
        }
        else {
            col = {1.0f, 0.40f, 0.08f, alpha * 0.85f};
        }
        app.instanced_particles.add_instance(em.x, em.y, em.size, col);
    }

    // 7. HUD Telemetry Overlay Banner
    {
        kalpana::Path hud_bg;
        hud_bg.rect(15.0f, 15.0f, FW - 30.0f, 48.0f);
        scene.add(kalpana::Node::shape(hud_bg, kalpana::Paint::fill(kalpana::Color{0.06f, 0.08f, 0.12f, 0.85f})));
        scene.add(kalpana::Node::shape(
            hud_bg, kalpana::Paint::stroke(kalpana::Color{0.25f, 0.35f, 0.45f, 0.8f}, 1.5f)));

        // Red Score Status Indicator
        kalpana::Path red_ind;
        red_ind.circle(45.0f, 39.0f, 8.0f);
        scene.add(kalpana::Node::shape(red_ind, kalpana::Paint::fill(kalpana::Color{0.9f, 0.2f, 0.2f, 1.0f})));

        // Blue Score Status Indicator
        kalpana::Path blue_ind;
        blue_ind.circle(FW - 45.0f, 39.0f, 8.0f);
        scene.add(kalpana::Node::shape(blue_ind, kalpana::Paint::fill(kalpana::Color{0.2f, 0.5f, 0.9f, 1.0f})));
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

    {
        sg_buffer_desc d{};
        d.usage.vertex_buffer = true;
        d.usage.stream_update = true;
        d.size = 256 * 1024 * sizeof(kalpana::sokol_backend::Vertex);
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
    pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT4;
    app.pip = sg_make_pipeline(pd);

    app.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    app.pass_action.colors[0].clear_value = {0.04f, 0.05f, 0.08f, 1.0f};
    app.canvas = std::make_unique<kalpana::Canvas<kalpana::sokol_backend>>(W, H);
    app.instanced_particles.init(65536);
    app.last_time = std::chrono::high_resolution_clock::now();

    init_tanks_world();
}

static void frame_cb() {
    auto& app = g_app;
    auto now = std::chrono::high_resolution_clock::now();
    float real_dt = std::chrono::duration<float>(now - app.last_time).count();
    app.last_time = now;
    if (real_dt <= 0.0f || real_dt > 0.1f) real_dt = DT;
    app.fps = 0.95f * app.fps + 0.05f * (1.0f / real_dt);
    app.frame++;

    // 1. Advance Gati ECS Runtime & Step Pocket Tanks Combat logic
    app.gati_game->update(real_dt);
    update_pocket_tanks_combat(real_dt);
    if (app.world) {
        app.world->step();
    }

    // 2. Build Vector Scene with Kalpana & Populate Instanced Particles
    kalpana::Scene scene;
    build_tanks_scene(scene);
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

    // 1. Draw Instanced Particles (Smoke, Fire, Terrain Dust)
    app.instanced_particles.render(FW, FH);

    // 2. Draw Vector Overlays (Terrain, Tanks, Shells, Shockwaves, HUD)
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
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
        if (ev->key_code == SAPP_KEYCODE_R) {
            init_tanks_world(); // Reset simulation
        }
    }
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    sapp_desc app{};
    app.init_cb = init_cb;
    app.frame_cb = frame_cb;
    app.cleanup_cb = cleanup_cb;
    app.event_cb = event_cb;
    app.width = W;
    app.height = H;
    app.window_title = "Pebble — Pocket Tanks Multi-Physics Battle Engine (Gati + Prakriti + Akruti + Kalpana)";
    app.icon.sokol_default = true;
    app.logger.func = slog_func;
    return app;
}
