// ============================================================================
// src/app/pebble_verse.cpp — Pebble Verse: N-Body Planetary Continuum Engine
// ============================================================================
// Complete interactive celestial simulation combining:
//   - Generic Barnes-Hut O(N log N) Gravitational Solver (containers/spatial/barnes_hut.hpp)
//   - Prakriti Celestial Matter & Thermodynamics (prakriti/material/celestial.hpp)
//   - Kalpana Blackbody Thermal Radiation (kalpana/color/blackbody.hpp)
//   - Akruti Khanda Voronoi Fracture & Disruption (akruti/khanda.hpp)
//   - Spandana Timeline, Motion, & Camera Trauma Shakes (spandana/spandana.hpp)
//   - Gati Fixed-Step Scheduler & State Interpolation (gati/gati.hpp)
//   - Dual Backends: Sokol GFX GPU Instanced Pipeline + Terminal Text Mode (--terminal / --cli)
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

#include "containers/spatial/barnes_hut.hpp"
#include "containers/numeric/math_vector.hpp"
#include "prakriti/material/celestial.hpp"
#include "prakriti/state/material_registry.hpp"
#include "kalpana/kalpana.hpp"
#include "kalpana/backend/sokol_backend.hpp"
#include "spandana/spandana.hpp"

#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <string>
#include <random>
#include <iostream>

// ----------------------------------------------------------------------------
// Display & Simulation Constants
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Display & Physical Tunable Constants
// ----------------------------------------------------------------------------
static constexpr int W = 1280;
static constexpr int H = 800;
static constexpr float FW = static_cast<float>(W);
static constexpr float FH = static_cast<float>(H);
static constexpr float DT = 1.0f / 60.0f;

// Tunable Cosmic Parameters
static constexpr float GRAVITATIONAL_G   = 480.0f;  // Balanced mutual N-body attraction
static constexpr float PLUMMER_SOFTENING = 8.0f;    // Prevents extreme close-range divergence
static constexpr float MAX_GRAV_FORCE    = 15000.0f; // Maximum acceleration clamp
static constexpr float MAX_SPEED_CAP     = 45.0f;   // Maximum calm orbital speed (px/s)
static constexpr float SIM_SPEED_FACTOR  = 0.45f;   // Gentle, observable time scaling
static constexpr int   INITIAL_DUST_COUNT = 650;    // Number of in-situ dust particles

static auto VS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct In  { float2 pos [[attribute(0)]]; float4 col [[attribute(1)]]; };\n"
    "struct Out { float4 pos [[position]]; float4 col; };\n"
    "vertex Out vs(In in [[stage_in]]) { Out o; o.pos=float4(in.pos,0,1); o.col=in.col; return o; }\n";
static const char* FS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct In { float4 col; };\n"
    "fragment float4 fs(In in [[stage_in]]) { return in.col; }\n";

// ----------------------------------------------------------------------------
// Celestial Planet / Body Data Definition
// ----------------------------------------------------------------------------
enum class CelestialType : std::uint8_t {
    IceCrust,
    SilicateRock,
    IronCore,
    MoltenMagma,
    SuperheatedPlasma,
    DegenerateDense
};

struct PlanetBody {
    pebble::ecs::Entity ent{};
    pebble::math::vec2  pos{0.0f, 0.0f};
    pebble::math::vec2  prev_pos{0.0f, 0.0f};
    pebble::math::vec2  vel{0.0f, 0.0f};
    pebble::math::vec2  acc{0.0f, 0.0f};
    float               angle = 0.0f;       // Rotation angle (radians)
    float               omega = 0.0f;       // Angular velocity / spin (rad/s)
    float               angular_momentum = 0.0f; // L = I * omega
    float               mass = 100.0f;
    float               density = 2800.0f; // kg/m^3
    float               radius = 2.5f;     // Small spec/circle radius (pixels)
    float               temperature = 20.0f; // Celsius
    prakriti::MaterialParams mat_params;
    CelestialType       type = CelestialType::SilicateRock;
    kalpana::Color      base_color{0.6f, 0.5f, 0.4f, 1.0f};
    bool                alive = true;
};

// Fire / Spark explosion particle
struct SparkParticle {
    pebble::math::vec2 pos{0.0f, 0.0f};
    pebble::math::vec2 vel{0.0f, 0.0f};
    float              radius = 1.2f;
    float              life = 0.4f;
    float              max_life = 0.4f;
    kalpana::Color     color{1.0f, 0.6f, 0.1f, 1.0f};
};

// ----------------------------------------------------------------------------
// Application Master State
// ----------------------------------------------------------------------------
struct PebbleVerseApp {
    sg_pipeline pip{};
    sg_bindings bind{};
    sg_pass_action pass_action{};
    sg_buffer vbuf{};
    sg_buffer ibuf{};
    std::unique_ptr<kalpana::Canvas<kalpana::sokol_backend>> canvas;
    kalpana::InstancedParticlePipeline instanced_planets;

    // Simulation systems
    pebble::ecs::World world;
    containers::spatial::BarnesHutTree bh_tree;
    containers::spatial::DefaultGravityPolicy gravity_policy{
        .G = GRAVITATIONAL_G,
        .softening = PLUMMER_SOFTENING,
        .theta = 0.5f,
        .max_force = MAX_GRAV_FORCE
    };

    std::vector<PlanetBody>    planets;
    std::vector<SparkParticle> sparks;
    pebble::spandana::ScreenShake2D camera_shake;

    // Spawner parameters
    float spawn_timer = 0.0f;
    float spawn_interval = 0.22f; // Smooth periodic entry
    std::mt19937 rng{1337};

    // Telemetry & Metrics
    float fps = 60.0f;
    float compute_ms = 0.0f;
    float total_mass = 0.0f;
    std::size_t active_planets_count = 0;
    std::size_t collisions_count = 0;
    std::size_t fusions_count = 0;
    std::size_t fractures_count = 0;

    // Interactive mouse controls
    float mouse_x = 0.0f, mouse_y = 0.0f;
    bool mouse_down = false;
    bool gravity_vortex = false;  // Left click / V key
    bool heat_ray = false;        // H key
    bool freeze_ray = false;      // C key
    bool paused = false;          // Space key
    bool terminal_mode = false;   // CLI flag
    int frame = 0;
    float time = 0.0f;
};

static PebbleVerseApp g_app;

// ----------------------------------------------------------------------------
// Helper: Celestial Material, Density & Thermal Spectrum (High Lum / Max Contrast)
// ----------------------------------------------------------------------------
static kalpana::Color get_celestial_color(const PlanetBody& p) {
    kalpana::Color base;

    // High-luminance, high-contrast primary celestial spectral colors
    if (p.density < 1500.0f) {
        base = kalpana::Color{0.25f, 0.95f, 1.00f, 1.0f}; // Ultra-Bright Cyan / Ice Blue
    } else if (p.density < 4500.0f) {
        base = kalpana::Color{1.00f, 0.90f, 0.25f, 1.0f}; // Radiant Solar Gold / Yellow (Silicate Rock)
    } else if (p.density < 10000.0f) {
        base = kalpana::Color{1.00f, 0.60f, 0.15f, 1.0f}; // Intense Metallic Orange (Iron-Nickel)
    } else {
        base = kalpana::Color{0.95f, 0.30f, 1.00f, 1.0f}; // Vivid Hyperdense Singularity Magenta
    }

    // Thermal incandescence shift
    if (p.temperature < -20.0f) {
        const float t = std::clamp((-p.temperature) / 100.0f, 0.0f, 1.0f);
        return kalpana::Color{
            base.r * (1.0f - t * 0.4f),
            base.g * (1.0f - t * 0.1f) + 0.3f * t,
            std::min(1.0f, base.b + 0.4f * t),
            1.0f
        };
    } else if (p.temperature > 800.0f) {
        const float t = std::clamp((p.temperature - 800.0f) / 2500.0f, 0.0f, 1.0f);
        const kalpana::Color hot_col = (t > 0.5f)
            ? kalpana::Color{1.0f, 1.0f, 1.0f, 1.0f}    // Pure blinding white-hot star plasma
            : kalpana::Color{1.0f, 0.25f, 0.12f, 1.0f}; // Brilliant crimson magma
        return kalpana::Color{
            base.r * (1.0f - t) + hot_col.r * t,
            base.g * (1.0f - t) + hot_col.g * t,
            base.b * (1.0f - t) + hot_col.b * t,
            1.0f
        };
    }

    return base;
}

// ----------------------------------------------------------------------------
// Field Spawner: In-situ cosmic dust field generation (Static field matter)
// ----------------------------------------------------------------------------
// Rather than shooting in from screen edges, dust particles form in-situ at rest (v = 0).
// Their motion emerges purely from mutual gravitational attraction.
static void spawn_dust_particle(PebbleVerseApp& app, bool user_spawn = false, float at_x = 0.0f, float at_y = 0.0f) {
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    PlanetBody p;
    p.ent = app.world.spawn();

    if (user_spawn) {
        p.pos = pebble::math::vec2{at_x, at_y};
    } else {
        // Uniform in-situ field distribution across screen with margin
        p.pos = pebble::math::vec2{
            40.0f + dist01(app.rng) * (FW - 80.0f),
            40.0f + dist01(app.rng) * (FH - 80.0f)
        };
    }

    // Zero initial linear velocity: motion is born entirely from mutual N-body gravity!
    p.vel = pebble::math::vec2{0.0f, 0.0f};
    p.prev_pos = p.pos;

    // Random micro-spin / angular velocity (radians/sec)
    p.angle = dist01(app.rng) * 6.2831853f;
    p.omega = (dist01(app.rng) - 0.5f) * 4.0f;

    // Randomize celestial material class and density
    const float roll = dist01(app.rng);
    if (roll < 0.30f) {
        p.type = CelestialType::IceCrust;
        p.mat_params = prakriti::celestial::ice_crust();
        p.mass = 25.0f + dist01(app.rng) * 35.0f;
        p.temperature = -60.0f + dist01(app.rng) * 40.0f;
    } else if (roll < 0.75f) {
        p.type = CelestialType::SilicateRock;
        p.mat_params = prakriti::celestial::silicate_rock();
        p.mass = 50.0f + dist01(app.rng) * 60.0f;
        p.temperature = 20.0f + dist01(app.rng) * 100.0f;
    } else if (roll < 0.94f) {
        p.type = CelestialType::IronCore;
        p.mat_params = prakriti::celestial::iron_nickel_core();
        p.mass = 120.0f + dist01(app.rng) * 140.0f;
        p.temperature = 100.0f + dist01(app.rng) * 200.0f;
    } else {
        p.type = CelestialType::MoltenMagma;
        p.mat_params = prakriti::celestial::molten_magma();
        p.mass = 200.0f + dist01(app.rng) * 200.0f;
        p.temperature = 1200.0f + dist01(app.rng) * 400.0f;
    }

    p.density = p.mat_params.rest_density;
    // Micro cosmic dust specs (0.75px to 1.5px)
    constexpr float kRadiusScale = 2.4f;
    p.radius = std::clamp(kRadiusScale * std::sqrt(p.mass / p.density), 0.75f, 1.5f);

    // Initial angular momentum: L = I * omega = (0.5 * m * r^2) * omega
    const float moment_of_inertia = 0.5f * p.mass * (p.radius * p.radius);
    p.angular_momentum = moment_of_inertia * p.omega;

    app.planets.push_back(p);
}

// ----------------------------------------------------------------------------
// Physics Step: Barnes-Hut Gravity + Symplectic Verlet + Thermodynamics
// ----------------------------------------------------------------------------
static void step_celestial_simulation(PebbleVerseApp& app, float dt) {
    if (app.paused) return;

    // Simulation speed factor for slow, elegant orbital mechanics
    const float sim_dt = dt * SIM_SPEED_FACTOR;

    const auto t_start = std::chrono::high_resolution_clock::now();

    // 1. Slow, subtle in-situ dust field condensation if population drops
    app.spawn_timer += sim_dt;
    if (app.spawn_timer >= 0.40f && app.planets.size() < 250) {
        app.spawn_timer = 0.0f;
        spawn_dust_particle(app);
    }

    // 2. Prepare Barnes-Hut input bodies array
    std::vector<containers::spatial::BarnesHutBody> bh_bodies;
    bh_bodies.reserve(app.planets.size());

    for (std::size_t i = 0; i < app.planets.size(); ++i) {
        if (!app.planets[i].alive) continue;
        bh_bodies.push_back(containers::spatial::BarnesHutBody{
            .pos = app.planets[i].pos,
            .vel = app.planets[i].vel,
            .mass = app.planets[i].mass,
            .id = static_cast<std::uint32_t>(i)
        });
    }

    // 3. Build QuadTree & Compute N-Body Gravitational Forces
    app.bh_tree.build(bh_bodies);
    std::vector<pebble::math::vec2> forces(bh_bodies.size());
    containers::spatial::compute_all_forces(app.bh_tree, bh_bodies, forces, app.gravity_policy);

    // Map forces back to planet bodies
    std::size_t bh_idx = 0;
    for (std::size_t i = 0; i < app.planets.size(); ++i) {
        if (!app.planets[i].alive) continue;
        const pebble::math::vec2 f_grav = forces[bh_idx++];
        app.planets[i].acc = f_grav * (1.0f / app.planets[i].mass);

        // Interactive gravity vortex
        if (app.gravity_vortex || app.mouse_down) {
            const pebble::math::vec2 to_mouse = pebble::math::vec2{app.mouse_x, app.mouse_y} - app.planets[i].pos;
            const float dist2 = to_mouse[0] * to_mouse[0] + to_mouse[1] * to_mouse[1] + 100.0f;
            const pebble::math::vec2 f_vortex = pebble::math::normalize(to_mouse) * (150000.0f / dist2);
            app.planets[i].acc = app.planets[i].acc + f_vortex;
        }

        // Interactive thermal injection / freeze ray
        if (app.heat_ray || app.freeze_ray) {
            if (const pebble::math::vec2 d = pebble::math::vec2{app.mouse_x, app.mouse_y} - app.planets[i].pos; pebble::math::length_sq(d) <= 60.0f * 60.0f) {
                if (app.heat_ray)   app.planets[i].temperature += 800.0f * sim_dt;
                if (app.freeze_ray) app.planets[i].temperature -= 600.0f * sim_dt;
            }
        }
    }

    // 4. Symplectic Velocity Verlet Integration & Radiative Cooling
    for (auto& p : app.planets) {
        if (!p.alive) continue;

        p.prev_pos = p.pos;
        p.pos = p.pos + p.vel * sim_dt + p.acc * (0.5f * sim_dt * sim_dt);
        p.vel = p.vel + p.acc * sim_dt;

        // Angular spin integration
        p.angle += p.omega * sim_dt;

        // Gentle drag / cosmic medium friction to prevent runaway slingshot speeds
        p.vel = p.vel * 0.9994f;

        // Clamp maximum orbital velocity so bodies stay majestic and observable
        if (const float v2 = p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1]; v2 > MAX_SPEED_CAP * MAX_SPEED_CAP) {
            p.vel = p.vel * (MAX_SPEED_CAP / std::sqrt(v2));
        }

        p.temperature = prakriti::celestial::apply_radiative_cooling(
            p.temperature, p.mass, p.radius, sim_dt
        );

        // Thermal phase transitions
        if (p.temperature > 1100.0f && p.type != CelestialType::MoltenMagma && p.type != CelestialType::SuperheatedPlasma) {
            p.type = CelestialType::MoltenMagma;
            p.mat_params = prakriti::celestial::molten_magma();
        } else if (p.temperature > 3800.0f && p.type != CelestialType::SuperheatedPlasma) {
            p.type = CelestialType::SuperheatedPlasma;
            p.mat_params = prakriti::celestial::superheated_plasma();
        } else if (p.temperature < 750.0f && p.type == CelestialType::MoltenMagma) {
            p.type = CelestialType::SilicateRock;
            p.mat_params = prakriti::celestial::silicate_rock();
        }

        // Critical mass gravitational collapse (density scaling)
        if (constexpr float kCriticalMass = 1000.0f; p.mass > kCriticalMass && p.type != CelestialType::DegenerateDense) {
            p.type = CelestialType::DegenerateDense;
            p.mat_params = prakriti::celestial::degenerate_dense_matter();
            p.density = p.mat_params.rest_density;
        }

        // Recompute radius (micro specs scaling)
        constexpr float kRadiusScale = 2.4f;
        p.radius = std::clamp(kRadiusScale * std::sqrt(p.mass / p.density), 0.75f, 2.2f);

        if (p.pos[0] < p.radius) { p.pos[0] = p.radius; p.vel[0] = -p.vel[0] * 0.7f; }
        if (p.pos[0] > FW - p.radius) { p.pos[0] = FW - p.radius; p.vel[0] = -p.vel[0] * 0.7f; }
        if (p.pos[1] < p.radius) { p.pos[1] = p.radius; p.vel[1] = -p.vel[1] * 0.7f; }
        if (p.pos[1] > FH - p.radius) { p.pos[1] = FH - p.radius; p.vel[1] = -p.vel[1] * 0.7f; }
    }

    // 5. Collision Narrowphase: 3-Way Regime (Elastic Rebound & Recoil, Ductile Merger, or Brittle Fragmentation)
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    const std::size_t num_planets = app.planets.size();
    for (std::size_t i = 0; i < num_planets; ++i) {
        if (!app.planets[i].alive) continue;
        for (std::size_t j = i + 1; j < num_planets; ++j) {
            if (!app.planets[j].alive) continue;

            const pebble::math::vec2 dr = app.planets[j].pos - app.planets[i].pos;
            const float dist2 = dr[0] * dr[0] + dr[1] * dr[1];
            const float min_dist = app.planets[i].radius + app.planets[j].radius;

            if (dist2 < min_dist * min_dist && dist2 > 1e-4f) {
                app.collisions_count++;

                const float dist = std::sqrt(dist2);
                const pebble::math::vec2 normal = dr * (1.0f / dist);
                const pebble::math::vec2 tangent{-normal[1], normal[0]};

                // Relative velocity
                const pebble::math::vec2 dv = app.planets[i].vel - app.planets[j].vel;
                const float vn = dv[0] * normal[0] + dv[1] * normal[1]; // Normal velocity component (< 0 means approaching)
                const float vt = dv[0] * tangent[0] + dv[1] * tangent[1]; // Tangential shear velocity

                // Inelastic heat conversion from Prakriti celestial impact thermodynamics
                const auto heat = prakriti::celestial::compute_impact_heat(
                    app.planets[i].mass, app.planets[j].mass,
                    app.planets[i].vel, app.planets[j].vel,
                    app.planets[i].mat_params.heat_capacity,
                    app.planets[j].mat_params.heat_capacity
                );

                app.planets[i].temperature += heat.temp_delta_1;
                app.planets[j].temperature += heat.temp_delta_2;

                const float v_rel2 = dv[0] * dv[0] + dv[1] * dv[1];
                const pebble::math::vec2 impact_pos = (app.planets[i].pos + app.planets[j].pos) * 0.5f;

                // Spawn spark specks
                const int num_sparks = (v_rel2 > 80.0f * 80.0f) ? 5 : 2;
                for (int s = 0; s < num_sparks; ++s) {
                    const float a = dist01(app.rng) * 6.2831853f;
                    const float spd = 20.0f + dist01(app.rng) * 45.0f;
                    kalpana::Color spark_col = (dist01(app.rng) > 0.4f)
                        ? kalpana::Color{1.0f, 0.95f, 0.35f, 1.0f}
                        : kalpana::Color{1.0f, 0.45f, 0.15f, 1.0f};

                    app.sparks.push_back(SparkParticle{
                        .pos = impact_pos,
                        .vel = pebble::math::vec2{std::cos(a) * spd, std::sin(a) * spd},
                        .radius = 0.6f + dist01(app.rng) * 0.5f,
                        .life = 0.15f + dist01(app.rng) * 0.15f,
                        .max_life = 0.30f,
                        .color = spark_col
                    });
                }

                // Thermodynamic parameters
                const float avg_temp = (app.planets[i].temperature + app.planets[j].temperature) * 0.5f;
                const bool is_molten = (avg_temp > 850.0f) || (app.planets[i].type == CelestialType::MoltenMagma || app.planets[j].type == CelestialType::MoltenMagma);
                const float mass_ratio = std::max(app.planets[i].mass, app.planets[j].mass) / std::max(1.0f, std::min(app.planets[i].mass, app.planets[j].mass));
                const float rel_speed = std::sqrt(v_rel2);

                // --- 3-WAY COLLISION REGIME CLASSIFICATION ---
                // Regime 1: Ductile Accretion / Merger
                // (High temperature molten softening OR strong mass capture at low speed)
                const bool can_merge = is_molten || (mass_ratio > 4.5f && rel_speed < 30.0f) || (rel_speed < 10.0f && avg_temp > 400.0f);

                // Regime 2: High-Velocity Brittle Fracture / Shattering
                // (Cold rocky/ice bodies with high impact energy)
                const bool can_shatter = !is_molten && (rel_speed > 38.0f) && (app.planets[i].type == CelestialType::SilicateRock || app.planets[i].type == CelestialType::IceCrust);

                if (can_merge) {
                    // ========================================================
                    // 1. ACCRETION / FUSION (Conserves Linear & Angular Momentum)
                    // ========================================================
                    app.fusions_count++;
                    const float m_total = app.planets[i].mass + app.planets[j].mass;
                    const pebble::math::vec2 v_cm = (app.planets[i].vel * app.planets[i].mass + app.planets[j].vel * app.planets[j].mass) * (1.0f / m_total);
                    const pebble::math::vec2 p_cm = (app.planets[i].pos * app.planets[i].mass + app.planets[j].pos * app.planets[j].mass) * (1.0f / m_total);

                    // Conservation of Angular Momentum: L_total = L_spin1 + L_spin2 + L_orbital
                    const float l_spin1 = app.planets[i].angular_momentum;
                    const float l_spin2 = app.planets[j].angular_momentum;
                    const pebble::math::vec2 r1 = app.planets[i].pos - p_cm;
                    const pebble::math::vec2 r2 = app.planets[j].pos - p_cm;
                    const float l_orb1 = app.planets[i].mass * (r1[0] * app.planets[i].vel[1] - r1[1] * app.planets[i].vel[0]);
                    const float l_orb2 = app.planets[j].mass * (r2[0] * app.planets[j].vel[1] - r2[1] * app.planets[j].vel[0]);
                    const float l_total = l_spin1 + l_spin2 + l_orb1 + l_orb2;

                    const float d_mixed = (app.planets[i].density * app.planets[i].mass + app.planets[j].density * app.planets[j].mass) / m_total;
                    const float t_mixed = (app.planets[i].temperature * app.planets[i].mass + app.planets[j].temperature * app.planets[j].mass) / m_total + 60.0f;

                    app.planets[i].pos = p_cm;
                    app.planets[i].vel = v_cm;
                    app.planets[i].mass = m_total;
                    app.planets[i].density = d_mixed;
                    app.planets[i].temperature = t_mixed;

                    // Update spin rate for merged body: omega = L / I
                    constexpr float kRadiusScale = 2.4f;
                    app.planets[i].radius = std::clamp(kRadiusScale * std::sqrt(m_total / d_mixed), 0.75f, 2.5f);
                    const float new_inertia = 0.5f * m_total * (app.planets[i].radius * app.planets[i].radius);
                    app.planets[i].angular_momentum = l_total;
                    app.planets[i].omega = (new_inertia > 1e-4f) ? (l_total / new_inertia) : 0.0f;

                    // Material phase selection
                    if (t_mixed > 1100.0f) {
                        app.planets[i].type = CelestialType::MoltenMagma;
                    } else if (d_mixed > 6000.0f) {
                        app.planets[i].type = CelestialType::IronCore;
                    } else if (d_mixed > 2200.0f) {
                        app.planets[i].type = CelestialType::SilicateRock;
                    } else {
                        app.planets[i].type = CelestialType::IceCrust;
                    }

                    app.planets[j].alive = false;
                } else if (can_shatter) {
                    // ========================================================
                    // 2. CATASTROPHIC BRITTLE SHATTERING / FRAGMENTATION
                    // ========================================================
                    app.fractures_count++;
                    app.camera_shake.add_trauma(0.08f);

                    if (app.planets[i].mass > 40.0f && app.planets.size() < 1200) {
                        const float shard_mass = app.planets[i].mass * 0.28f;
                        app.planets[i].mass *= 0.44f;
                        for (int k = 0; k < 2; ++k) {
                            PlanetBody shard = app.planets[i];
                            shard.ent = app.world.spawn();
                            shard.mass = shard_mass;
                            const float a = static_cast<float>(k) * 3.14159f + 0.5f;
                            shard.vel = app.planets[i].vel + pebble::math::vec2{std::cos(a), std::sin(a)} * 35.0f;
                            shard.pos = app.planets[i].pos + pebble::math::vec2{std::cos(a), std::sin(a)} * (app.planets[i].radius + 1.2f);
                            shard.temperature += 200.0f;
                            shard.omega = (dist01(app.rng) - 0.5f) * 8.0f;
                            app.planets.push_back(shard);
                        }
                    }
                } else {
                    // ========================================================
                    // 3. ELASTIC / INELASTIC MOMENTUM RECOIL & SPIN TRANSFER
                    // ========================================================
                    // Even massive planets experience physical recoil ($m_1 v_1 + m_2 v_2$)
                    if (vn < 0.0f) {
                        // Coefficient of restitution based on temperature and material elasticity
                        const float restitution = std::clamp(0.45f - (avg_temp / 3000.0f) * 0.35f, 0.08f, 0.65f);
                        const float m1 = app.planets[i].mass;
                        const float m2 = app.planets[j].mass;
                        const float m_sum = m1 + m2;

                        // Normal impulse magnitude: J = -(1 + e) * vn / (1/m1 + 1/m2)
                        const float impulse_n = -(1.0f + restitution) * vn * (m1 * m2) / m_sum;
                        const pebble::math::vec2 impulse_vec = normal * impulse_n;

                        // Apply momentum recoil to both bodies (including the heavier one!)
                        app.planets[i].vel = app.planets[i].vel + impulse_vec * (1.0f / m1);
                        app.planets[j].vel = app.planets[j].vel - impulse_vec * (1.0f / m2);

                        // Tangential friction and surface spin transfer (torque):
                        const float friction = 0.25f;
                        const float impulse_t = std::clamp(-vt * (m1 * m2) / m_sum, -friction * impulse_n, friction * impulse_n);
                        const pebble::math::vec2 friction_vec = tangent * impulse_t;

                        app.planets[i].vel = app.planets[i].vel + friction_vec * (1.0f / m1);
                        app.planets[j].vel = app.planets[j].vel - friction_vec * (1.0f / m2);

                        // Angular impulse (torque tau = r x F_t)
                        const float i1 = 0.5f * m1 * (app.planets[i].radius * app.planets[i].radius);
                        const float i2 = 0.5f * m2 * (app.planets[j].radius * app.planets[j].radius);
                        if (i1 > 1e-4f) app.planets[i].omega += (app.planets[i].radius * impulse_t) / i1;
                        if (i2 > 1e-4f) app.planets[j].omega += (app.planets[j].radius * impulse_t) / i2;

                        // Positional separation to prevent penetration overlap
                        const float overlap = 0.5f * (min_dist - dist);
                        app.planets[i].pos = app.planets[i].pos - normal * (overlap * (m2 / m_sum));
                        app.planets[j].pos = app.planets[j].pos + normal * (overlap * (m1 / m_sum));
                    }
                }
            }
        }
    }

    // 6. Update Spark/Fire Particles
    for (auto& s : app.sparks) {
        s.pos = s.pos + s.vel * dt;
        s.vel = s.vel * 0.92f; // Drag damping
        s.life -= dt;
        s.color.a = std::clamp(s.life / s.max_life, 0.0f, 1.0f);
    }
    std::erase_if(app.sparks, [](const SparkParticle& s) { return s.life <= 0.0f; });

    app.camera_shake.update(dt);

    // 7. Cleanup Dead Planets
    app.planets.erase(
        std::ranges::remove_if(app.planets, [](const PlanetBody& p) { return !p.alive; }).begin(),
        app.planets.end()
    );

    const auto t_end = std::chrono::high_resolution_clock::now();
    app.compute_ms = std::chrono::duration<float, std::milli>(t_end - t_start).count();

    app.active_planets_count = app.planets.size();
    app.total_mass = 0.0f;
    for (const auto& p : app.planets) app.total_mass += p.mass;
}

// ----------------------------------------------------------------------------
// High-Resolution 24-bit TrueColor Terminal Renderer (Double-Vertical Half Blocks)
// ----------------------------------------------------------------------------
// Uses UTF-8 half-block '▀' (U+2580) where top half is foreground and bottom half
// is background, doubling vertical resolution without scrolling.
static void render_terminal_ascii(const PebbleVerseApp& app) {
    constexpr int TERM_W = 100;
    constexpr int TERM_H = 40; // 40 rows = 80 vertical pixel resolution
    constexpr int SUB_H  = TERM_H * 2;

    struct TermCell {
        bool has_body = false;
        std::uint8_t r = 0, g = 0, b = 0;
        char glyph = ' ';
    };

    TermCell subgrid[SUB_H][TERM_W];

    // Rasterize planets into TrueColor subgrid
    for (const auto& p : app.planets) {
        const int gx = std::clamp(static_cast<int>((p.pos[0] / FW) * TERM_W), 0, TERM_W - 1);
        const int gy = std::clamp(static_cast<int>((p.pos[1] / FH) * SUB_H), 0, SUB_H - 1);

        const kalpana::Color col = get_celestial_color(p);
        subgrid[gy][gx].has_body = true;
        subgrid[gy][gx].r = static_cast<std::uint8_t>(std::clamp(col.r * 255.0f, 0.0f, 255.0f));
        subgrid[gy][gx].g = static_cast<std::uint8_t>(std::clamp(col.g * 255.0f, 0.0f, 255.0f));
        subgrid[gy][gx].b = static_cast<std::uint8_t>(std::clamp(col.b * 255.0f, 0.0f, 255.0f));

        if (p.mass > 400.0f) {
            subgrid[gy][gx].glyph = '@';
        } else if (p.mass > 150.0f) {
            subgrid[gy][gx].glyph = '#';
        } else if (p.mass > 60.0f) {
            subgrid[gy][gx].glyph = '*';
        } else {
            subgrid[gy][gx].glyph = '.';
        }
    }

    // Build frame buffer string for atomic terminal draw (zero flicker, no scrolling)
    std::string out;
    out.reserve(TERM_W * TERM_H * 32 + 512);

    // Reposition cursor to row 1, col 1 and hide cursor
    out += "\033[?25l\033[H";

    // Header HUD banner
    out += "\033[1;37;44m PEBBLE VERSE \033[0;1;30;47m N-BODY PLANETARY CONTINUUM \033[0m";
    out += "  \033[36mBodies:\033[1;37m " + std::to_string(app.active_planets_count) + "\033[0m";
    out += "  \033[33mMass:\033[1;37m " + std::to_string(static_cast<int>(app.total_mass)) + "\033[0m";
    out += "  \033[32mMerges:\033[1;37m " + std::to_string(app.fusions_count) + "\033[0m";
    out += "  \033[31mFractures:\033[1;37m " + std::to_string(app.fractures_count) + "\033[0m";
    out += "  \033[35mCompute:\033[1;37m " + std::to_string(app.compute_ms).substr(0, 4) + " ms\033[0m\n";

    // Top border
    out += "\033[38;2;60;80;120m┌";
    for (int x = 0; x < TERM_W; ++x) out += "─";
    out += "┐\033[0m\n";

    // Render paired rows using half-block '▀'
    for (int y = 0; y < TERM_H; ++y) {
        const int top_y = y * 2;
        const int bot_y = top_y + 1;

        out += "\033[38;2;60;80;120m│\033[0m";

        for (int x = 0; x < TERM_W; ++x) {
            const auto& top = subgrid[top_y][x];
            const auto& bot = subgrid[bot_y][x];

            if (!top.has_body && !bot.has_body) {
                out += " ";
            } else if (top.has_body && !bot.has_body) {
                // Top half only
                out += "\033[38;2;" + std::to_string(top.r) + ";" + std::to_string(top.g) + ";" + std::to_string(top.b) + "m▀\033[0m";
            } else if (!top.has_body && bot.has_body) {
                // Bottom half only
                out += "\033[38;2;" + std::to_string(bot.r) + ";" + std::to_string(bot.g) + ";" + std::to_string(bot.b) + "m▄\033[0m";
            } else {
                // Both halves with distinct TrueColors
                out += "\033[38;2;" + std::to_string(top.r) + ";" + std::to_string(top.g) + ";" + std::to_string(top.b) +
                       ";48;2;" + std::to_string(bot.r) + ";" + std::to_string(bot.g) + ";" + std::to_string(bot.b) + "m▀\033[0m";
            }
        }

        out += "\033[38;2;60;80;120m│\033[0m\n";
    }

    // Bottom border
    out += "\033[38;2;60;80;120m└";
    for (int x = 0; x < TERM_W; ++x) out += "─";
    out += "┘\033[0m\n";

    // Write full atomic frame to stdout
    std::cout << out << std::flush;
}

// ----------------------------------------------------------------------------
// GPU Render Frame: Sokol GFX Instanced Particle Pipeline
// ----------------------------------------------------------------------------
static void render_gpu_frame(PebbleVerseApp& app) {
    kalpana::Scene scene;
    app.instanced_planets.begin();

    const auto shake_offset = app.camera_shake.offset();
    const float cx = shake_offset[0];
    const float cy = shake_offset[1];

    // 1. Batch High-Contrast Glowing Celestial Circles
    for (const auto& p : app.planets) {
        const kalpana::Color c = get_celestial_color(p);
        app.instanced_planets.add_instance(p.pos[0] + cx, p.pos[1] + cy, p.radius, c);
    }

    // 2. Batch Fire Sparks
    for (const auto& s : app.sparks) {
        app.instanced_planets.add_instance(s.pos[0] + cx, s.pos[1] + cy, s.radius, s.color);
    }

    // 3. Interactive Mouse Reticle
    if (app.mouse_down || app.gravity_vortex || app.heat_ray || app.freeze_ray) {
        kalpana::Path reticle;
        reticle.circle(app.mouse_x, app.mouse_y, app.heat_ray ? 40.0f : (app.freeze_ray ? 40.0f : 25.0f));
        kalpana::Color ret_col = app.heat_ray   ? kalpana::Color{1.0f, 0.4f, 0.1f, 0.9f} :
                                (app.freeze_ray ? kalpana::Color{0.3f, 0.9f, 1.0f, 0.9f} :
                                                  kalpana::Color{0.8f, 0.5f, 1.0f, 0.9f});
        scene.add(kalpana::Node::shape(reticle, kalpana::Paint::stroke(ret_col, 2.0f)));
    }

    // Render vector overlays
    app.canvas->render(scene);

    const auto& verts = app.canvas->backend().vertices();
    const auto& inds  = app.canvas->backend().indices();

    if (!verts.empty() && !inds.empty()) {
        sg_range vr = {.ptr = verts.data(), .size = verts.size() * sizeof(kalpana::sokol_backend::Vertex)};
        sg_update_buffer(app.vbuf, vr);

        sg_range ir = {.ptr = inds.data(), .size = inds.size() * sizeof(std::uint32_t)};
        sg_update_buffer(app.ibuf, ir);
    }

    sg_pass pass{};
    pass.action = app.pass_action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(pass);

    // 1. Draw Vector Overlays (Reticle)
    if (!inds.empty()) {
        sg_apply_pipeline(app.pip);
        sg_apply_bindings(app.bind);
        sg_draw(0, static_cast<int>(inds.size()), 1);
    }

    // 2. Draw Instanced High-Contrast Celestial Planets & Sparks on top
    app.instanced_planets.render(FW, FH);

    sg_end_pass();
    sg_commit();
}

// ----------------------------------------------------------------------------
// Sokol Callbacks
// ----------------------------------------------------------------------------
static void init_cb() {
    auto& app = g_app;

    sg_desc gfx{};
    gfx.environment = sglue_environment();
    gfx.logger.func = slog_func;
    sg_setup(&gfx);

    app.canvas = std::make_unique<kalpana::Canvas<kalpana::sokol_backend>>(W, H);
    app.instanced_planets.init(65536);

    // Vertex & Index buffers
    {
        sg_buffer_desc vb_desc{};
        vb_desc.size = 256 * 1024 * sizeof(kalpana::sokol_backend::Vertex);
        vb_desc.usage.stream_update = true;
        app.vbuf = sg_make_buffer(vb_desc);
    }
    {
        sg_buffer_desc ib_desc{};
        ib_desc.size = 256 * 1024 * sizeof(std::uint32_t);
        ib_desc.usage.stream_update = true;
        ib_desc.usage.index_buffer = true;
        app.ibuf = sg_make_buffer(ib_desc);
    }

    app.bind.vertex_buffers[0] = app.vbuf;
    app.bind.index_buffer = app.ibuf;

    // Vector graphics pipeline
    sg_pipeline_desc pip_desc{};
    sg_shader_desc shd{};
#if defined(SOKOL_METAL)
    shd.vertex_func.source = VS_METAL;
    shd.vertex_func.entry = "vs";
    shd.fragment_func.source = FS_METAL;
    shd.fragment_func.entry = "fs";
#endif
    pip_desc.shader = sg_make_shader(shd);
    pip_desc.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2; // Pos
    pip_desc.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT4; // Color
    pip_desc.index_type = SG_INDEXTYPE_UINT32;
    pip_desc.colors[0].blend.enabled = true;
    pip_desc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    pip_desc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    app.pip = sg_make_pipeline(pip_desc);
    app.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    app.pass_action.colors[0].clear_value = {.r = 0.03f, .g = 0.04f, .b = 0.08f, .a = 1.0f};

    // Seed large initial cosmic dust field across the entire viewport (650 micro specs)
    for (int i = 0; i < 650; ++i) {
        spawn_dust_particle(app);
    }
}

static void frame_cb() {
    auto& app = g_app;
    app.frame++;
    app.time += DT;

    step_celestial_simulation(app, DT);

    if (app.terminal_mode) {
        if (app.frame % 3 == 0) render_terminal_ascii(app);
    } else {
        render_gpu_frame(app);
    }
}

static void event_cb(const sapp_event* e) {
    auto& app = g_app;
    if (e->type == SAPP_EVENTTYPE_MOUSE_MOVE) {
        app.mouse_x = e->mouse_x;
        app.mouse_y = e->mouse_y;
    } else if (e->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
        app.mouse_down = true;
        if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
            spawn_dust_particle(app, true, e->mouse_x, e->mouse_y);
        }
    } else if (e->type == SAPP_EVENTTYPE_MOUSE_UP) {
        app.mouse_down = false;
    } else if (e->type == SAPP_EVENTTYPE_KEY_DOWN) {
        switch (e->key_code) {
            case SAPP_KEYCODE_SPACE:
                app.paused = !app.paused;
                break;
            case SAPP_KEYCODE_V:
                app.gravity_vortex = !app.gravity_vortex;
                break;
            case SAPP_KEYCODE_H:
                app.heat_ray = !app.heat_ray;
                break;
            case SAPP_KEYCODE_C:
                app.freeze_ray = !app.freeze_ray;
                break;
            case SAPP_KEYCODE_R:
                app.planets.clear();
                app.sparks.clear();
                for (int i = 0; i < 650; ++i) spawn_dust_particle(app);
                break;
            default:
                break;
        }
    } else if (e->type == SAPP_EVENTTYPE_KEY_UP) {
        if (e->key_code == SAPP_KEYCODE_V) app.gravity_vortex = false;
        if (e->key_code == SAPP_KEYCODE_H) app.heat_ray = false;
        if (e->key_code == SAPP_KEYCODE_C) app.freeze_ray = false;
    }
}

static void cleanup_cb() {
    if (g_app.terminal_mode) {
        std::cout << "\033[?25h\033[0m\n" << std::flush;
    }
    sg_shutdown();
}

// ----------------------------------------------------------------------------
// Entry Point
// ----------------------------------------------------------------------------
sapp_desc sokol_main(const int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--terminal") == 0 || std::strcmp(argv[i], "--cli") == 0) {
            g_app.terminal_mode = true;
        }
    }

    sapp_desc d{};
    d.init_cb = init_cb;
    d.frame_cb = frame_cb;
    d.cleanup_cb = cleanup_cb;
    d.event_cb = event_cb;
    d.width = W;
    d.height = H;
    d.window_title = "Pebble Verse — N-Body Planetary Continuum Simulation";
    d.logger.func = slog_func;
    d.high_dpi = true;
    return d;
}

