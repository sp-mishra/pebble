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
#include <thread>
#include <csignal>

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
    DegenerateDense,
    NeutronStar,
    BlackHoleSingularity
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
    float               radius = 2.5f;     // Primary spec/circle radius (pixels)
    float               temperature = 20.0f; // Celsius
    prakriti::MaterialParams mat_params;
    CelestialType       type = CelestialType::SilicateRock;
    kalpana::Color      base_color{0.6f, 0.5f, 0.4f, 1.0f};
    bool                alive = true;
    bool                is_singularity = false;   // Collapsed black hole state
    bool                is_neutron_star = false; // Dense pulsar state

    // Gradual Contact Binary / Dumbbell Coalescence
    // When 2 bodies merge, they start as a contact binary joined at their tips.
    // Over time (driven by angular momentum, viscosity & thermal ductility), they relax into a single circle.
    bool                is_merging = false;
    float               merge_progress = 1.0f; // 0.0 = initial contact dumbbell -> 1.0 = fully relaxed sphere
    float               merge_duration = 2.5f; // Duration of gradual coalescence (seconds)
    float               lobe2_radius = 0.0f;   // Secondary attached lobe radius
    float               lobe2_mass = 0.0f;     // Secondary lobe mass (can break during violent impact!)
    pebble::math::vec2  lobe2_offset{0.0f, 0.0f}; // Local displacement vector from center

    // Accretion halo & orbital trail history ring buffer
    static constexpr int kMaxTrail = 8;
    pebble::math::vec2 trail_history[kMaxTrail]{};
    int                trail_head = 0;
    int                trail_count = 0;
    float              trail_timer = 0.0f;
};

// Evaporated / boiled nebula gas cloud particle
struct NebulaGasParticle {
    pebble::math::vec2 pos{0.0f, 0.0f};
    pebble::math::vec2 vel{0.0f, 0.0f};
    float              radius = 1.0f;
    float              life = 1.0f;
    float              max_life = 1.0f;
    kalpana::Color     color{0.3f, 0.8f, 1.0f, 0.4f};
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

// Relativistic Polar Matter Jet Particle
struct RelativisticJetParticle {
    pebble::math::vec2 pos{0.0f, 0.0f};
    pebble::math::vec2 vel{0.0f, 0.0f};
    float              radius = 1.4f;
    float              life = 0.5f;
    float              max_life = 0.5f;
    kalpana::Color     color{0.4f, 0.85f, 1.0f, 0.9f};
};

// Relativistic Gravitational Wave Space-time Ripple
struct GravitationalWaveRipple {
    pebble::math::vec2 center{0.0f, 0.0f};
    float              radius = 2.0f;
    float              max_radius = 280.0f;
    float              expansion_speed = 190.0f;
    float              amplitude = 1.0f;
    float              life = 1.0f;
    float              max_life = 1.0f;
};

// Relativistic Accretion Disk Flare (ISCO Infall Burst)
struct AccretionFlare {
    pebble::math::vec2 pos{0.0f, 0.0f};
    float              radius = 2.0f;
    float              life = 0.35f;
    float              max_life = 0.35f;
    kalpana::Color     color{1.0f, 0.95f, 0.5f, 1.0f};
};

enum class SpectralViewMode : std::uint8_t {
    OpticalRGB,      // Natural true-color material and incandescence
    ThermalInfrared, // Temperature-dominant thermal gradient
    RadioXRay        // High-energy magnetic & relativistic radiation (Pulsars & Singularities)
};

// Interactive slingshot launcher state
struct SlingshotLauncher {
    bool active = false;
    float start_x = 0.0f;
    float start_y = 0.0f;
    float current_x = 0.0f;
    float current_y = 0.0f;
    float mass = 150.0f;
    CelestialType type = CelestialType::IronCore;
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

    std::vector<PlanetBody>              planets;
    std::vector<SparkParticle>           sparks;
    std::vector<NebulaGasParticle>       nebulae;
    std::vector<RelativisticJetParticle> jets;
    std::vector<GravitationalWaveRipple> gw_ripples;
    std::vector<AccretionFlare>          flares;
    pebble::spandana::ScreenShake2D      camera_shake;
    SlingshotLauncher                    slingshot;

    // Initial Simulation Startup Config Modal State
    bool in_startup_modal = true; // Displays startup config screen until ENTER is pressed
    int  config_selected_row = 0; // 0=Dust Count, 1=Gravitational G, 2=Initial Distribution, 3=Barnes-Hut Theta
    int  config_initial_dust_count = 650; // Range: 150 to 1500
    float config_grav_g = 18000.0f;       // Range: 5000 to 45000
    int  config_dist_mode = 0;            // 0=Uniform Cosmic Field, 1=Barycentric Cluster, 2=Dual Infall Cloud
    float config_bh_theta = 0.5f;         // 0.3 to 0.8

    // Multi-Spectral View & Cosmic Nucleosynthesis
    SpectralViewMode view_mode = SpectralViewMode::OpticalRGB;
    float cosmic_metallicity_z = 0.02f; // Current universe heavy element fraction (0.02 to 0.45)
    std::size_t supernova_count = 0;

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
    std::size_t gas_particles_count = 0;

    // Interactive mouse & keyboard controls
    float mouse_x = 0.0f, mouse_y = 0.0f;
    bool mouse_down = false;
    bool gravity_vortex = false;  // Left click / V key
    bool heat_ray = false;        // H key
    bool freeze_ray = false;      // C key
    bool paused = false;          // Space key
    float time_dilation = 1.0f;   // Time dilation factor (adjusted via - / + or [ / ])
    int selected_mat_index = 2;   // 1=Ice, 2=Silicate, 3=Iron, 4=Magma/Plasma
    bool terminal_mode = false;   // CLI flag
    int frame = 0;
    float time = 0.0f;
};

static PebbleVerseApp g_app;

/// ----------------------------------------------------------------------------
// Helper: Celestial Material, Density, Thermal Spectrum & Relativistic Doppler Shifts
// ----------------------------------------------------------------------------
static kalpana::Color get_celestial_color(const PlanetBody& p, SpectralViewMode mode = SpectralViewMode::OpticalRGB) {
    if (p.is_singularity || p.type == CelestialType::BlackHoleSingularity) {
        if (mode == SpectralViewMode::RadioXRay) {
            return kalpana::Color{0.9f, 0.4f, 1.0f, 1.0f}; // Intense Synchrotron Radiation Violet
        }
        return kalpana::Color{0.02f, 0.01f, 0.05f, 1.0f}; // Absolute Black Hole Horizon
    }
    if (p.is_neutron_star || p.type == CelestialType::NeutronStar) {
        if (mode == SpectralViewMode::RadioXRay) {
            return kalpana::Color{0.2f, 1.0f, 0.95f, 1.0f}; // Blinding Magnetar X-Ray Beam
        }
        return kalpana::Color{0.35f, 0.95f, 1.0f, 1.0f}; // Radiant Electric Cyan Pulsar
    }

    // ── Mode 1: Thermal Infrared View ─────────────────────────────────────────
    if (mode == SpectralViewMode::ThermalInfrared) {
        // Map temperature range [-100C to 4000C] to Heatmap gradient: Deep Navy -> Magenta -> Red -> Gold -> White
        const float t_norm = std::clamp((p.temperature + 100.0f) / 3500.0f, 0.0f, 1.0f);
        if (t_norm < 0.25f) {
            const float f = t_norm / 0.25f;
            return kalpana::Color{0.1f + f * 0.4f, 0.05f, 0.4f + f * 0.4f, 1.0f};
        } else if (t_norm < 0.60f) {
            const float f = (t_norm - 0.25f) / 0.35f;
            return kalpana::Color{0.5f + f * 0.5f, 0.1f + f * 0.35f, 0.8f * (1.0f - f), 1.0f};
        } else {
            const float f = (t_norm - 0.60f) / 0.40f;
            return kalpana::Color{1.0f, 0.45f + f * 0.55f, f * 0.9f, 1.0f};
        }
    }

    // ── Mode 2: Radio / X-Ray Relativistic View ──────────────────────────────
    if (mode == SpectralViewMode::RadioXRay) {
        const float speed = std::sqrt(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1]);
        const float relativistic_glow = std::clamp(speed / 40.0f, 0.15f, 0.95f);
        if (p.temperature > 1500.0f) {
            return kalpana::Color{0.3f, 0.85f, 1.0f, relativistic_glow};
        }
        return kalpana::Color{0.2f, 0.3f, 0.5f, 0.35f}; // Background cold matter
    }

    // ── Mode 0: Optical RGB True-Color Spectrum ──────────────────────────────
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
        base = kalpana::Color{
            base.r * (1.0f - t * 0.4f),
            base.g * (1.0f - t * 0.1f) + 0.3f * t,
            std::min(1.0f, base.b + 0.4f * t),
            1.0f
        };
    } else if (p.temperature > 800.0f) {
        const float t = std::clamp((p.temperature - 800.0f) / 2500.0f, 0.0f, 1.0f);
        const kalpana::Color hot_col = (t > 0.5f)
            ? kalpana::Color{1.0f, 1.0f, 1.0f, 1.0f}    // Pure white-hot star plasma
            : kalpana::Color{1.0f, 0.25f, 0.12f, 1.0f}; // Brilliant crimson magma
        base = kalpana::Color{
            base.r * (1.0f - t) + hot_col.r * t,
            base.g * (1.0f - t) + hot_col.g * t,
            base.b * (1.0f - t) + hot_col.b * t,
            1.0f
        };
    }

    // Relativistic Doppler Optical Shift:
    const float v_speed = std::sqrt(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1]);
    if (v_speed > 10.0f) {
        const float doppler = std::clamp(p.vel[0] / 65.0f, -0.6f, 0.6f);
        if (doppler > 0.0f) {
            base.r = std::clamp(base.r * (1.0f - doppler * 0.4f), 0.0f, 1.0f);
            base.g = std::clamp(base.g * (1.0f + doppler * 0.2f), 0.0f, 1.0f);
            base.b = std::clamp(base.b + doppler * 0.45f, 0.0f, 1.0f);
        } else {
            const float rf = -doppler;
            base.r = std::clamp(base.r + rf * 0.45f, 0.0f, 1.0f);
            base.g = std::clamp(base.g * (1.0f - rf * 0.3f), 0.0f, 1.0f);
            base.b = std::clamp(base.b * (1.0f - rf * 0.5f), 0.0f, 1.0f);
        }
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
        if (app.config_dist_mode == 1) {
            // Mode 1: Central Barycentric Galactic Cluster
            const float r = std::pow(dist01(app.rng), 0.65f) * (FH * 0.42f);
            const float th = dist01(app.rng) * 6.2831853f;
            p.pos = pebble::math::vec2{FW * 0.5f + std::cos(th) * r, FH * 0.5f + std::sin(th) * r};
        } else if (app.config_dist_mode == 2) {
            // Mode 2: Dual Infall Binary Accretion Clouds
            const float side = (dist01(app.rng) < 0.5f) ? -1.0f : 1.0f;
            const float cx = FW * 0.5f + side * (FW * 0.22f);
            const float cy = FH * 0.5f + (dist01(app.rng) - 0.5f) * 60.0f;
            const float r = std::pow(dist01(app.rng), 0.7f) * 110.0f;
            const float th = dist01(app.rng) * 6.2831853f;
            p.pos = pebble::math::vec2{cx + std::cos(th) * r, cy + std::sin(th) * r};
        } else {
            // Mode 0: Uniform Cosmic Primordial Field
            p.pos = pebble::math::vec2{
                40.0f + dist01(app.rng) * (FW - 80.0f),
                40.0f + dist01(app.rng) * (FH - 80.0f)
            };
        }
    }

    // Zero initial linear velocity: motion is born entirely from mutual N-body gravity!
    p.vel = pebble::math::vec2{0.0f, 0.0f};
    p.prev_pos = p.pos;

    // Random micro-spin / angular velocity (radians/sec)
    p.angle = dist01(app.rng) * 6.2831853f;
    p.omega = (dist01(app.rng) - 0.5f) * 4.0f;

    // Randomize celestial material class and density
    if (const float roll = dist01(app.rng); roll < 0.30f) {
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
// Pure N-Body Physical Simulation Loop
// ----------------------------------------------------------------------------
static void step_celestial_simulation(PebbleVerseApp& app, float dt) {
    if (app.paused) return;

    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    // Simulation speed factor for slow, elegant orbital mechanics with time dilation
    const float sim_dt = dt * SIM_SPEED_FACTOR * app.time_dilation;

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

        // Gradual Contact Binary Coalescence Relaxation
        // Tips slowly pull inward toward center of mass as matter relaxes into a sphere
        if (p.is_merging) {
            // Hotter/molten bodies relax faster into a circle; colder bodies hold dumbbell shape longer
            const float viscosity_rate = (p.temperature > 900.0f) ? 0.70f : 0.35f;
            p.merge_progress += (sim_dt / p.merge_duration) * viscosity_rate;
            if (p.merge_progress >= 1.0f) {
                p.merge_progress = 1.0f;
                p.is_merging = false;
                p.lobe2_offset = pebble::math::vec2{0.0f, 0.0f};
                p.lobe2_radius = 0.0f;
                p.lobe2_mass = 0.0f;
            } else {
                // Decay secondary lobe offset inward toward center of mass
                const float shrink = (1.0f - p.merge_progress);
                p.lobe2_offset = p.lobe2_offset * (1.0f - sim_dt * 1.5f * viscosity_rate);
            }
        }

        // Accretion halo & trail history recording (every 3 frames)
        p.trail_timer += sim_dt;
        if (p.trail_timer >= 0.05f) {
            p.trail_timer = 0.0f;
            p.trail_history[p.trail_head] = p.pos;
            p.trail_head = (p.trail_head + 1) % PlanetBody::kMaxTrail;
            if (p.trail_count < PlanetBody::kMaxTrail) p.trail_count++;
        }

        // ── Pure In-Situ Organic Stellar Evolution via Prakriti Physics Engine ────────
        if (!p.is_singularity) {
            const auto evol = prakriti::celestial::evaluate_stellar_evolution(p.mass, p.density, p.temperature, p.radius, sim_dt);
            
            p.temperature += evol.fusion_heat_rate;
            p.density += evol.core_compression_rate;

            if (evol.phase == prakriti::celestial::StellarPhase::BlackHoleSingularity) {
                p.is_singularity = true;
                p.is_neutron_star = false;
                p.type = CelestialType::BlackHoleSingularity;
                p.mat_params = prakriti::celestial::black_hole_singularity();
                p.density = p.mat_params.rest_density;
                p.radius = evol.event_horizon_radius;
                p.temperature = 12000.0f; // Relativistic accretion temperature

                // Supernova Blast & Relativistic Shockwave Ejecta
                for (int s = 0; s < 42; ++s) {
                    const float a = static_cast<float>(s) * (6.2831853f / 42.0f);
                    const float spd = 70.0f + dist01(app.rng) * 110.0f;
                    app.sparks.push_back(SparkParticle{
                        .pos = p.pos,
                        .vel = p.vel + pebble::math::vec2{std::cos(a), std::sin(a)} * spd,
                        .radius = 2.2f + dist01(app.rng) * 2.0f,
                        .life = 0.9f + dist01(app.rng) * 0.4f,
                        .max_life = 1.3f,
                        .color = kalpana::Color{1.0f, 0.95f, 0.45f, 1.0f}
                    });
                }
            } else if (evol.phase == prakriti::celestial::StellarPhase::NeutronStar && !p.is_neutron_star) {
                p.is_neutron_star = true;
                p.type = CelestialType::NeutronStar;
                p.mat_params = prakriti::celestial::neutron_star();
                p.density = p.mat_params.rest_density;
                p.radius = evol.event_horizon_radius;
                p.temperature = 4500.0f;
                p.omega *= 3.5f; // Rapid pulsar spinup from conservation of angular momentum

                // Pulsar core shockwave
                for (int s = 0; s < 24; ++s) {
                    const float a = static_cast<float>(s) * (6.2831853f / 24.0f);
                    const float spd = 40.0f + dist01(app.rng) * 60.0f;
                    app.sparks.push_back(SparkParticle{
                        .pos = p.pos,
                        .vel = p.vel + pebble::math::vec2{std::cos(a), std::sin(a)} * spd,
                        .radius = 1.6f + dist01(app.rng) * 1.5f,
                        .life = 0.6f + dist01(app.rng) * 0.3f,
                        .max_life = 0.9f,
                        .color = kalpana::Color{0.4f, 0.95f, 1.0f, 1.0f}
                    });
                }
            } else if (evol.phase == prakriti::celestial::StellarPhase::MainSequenceStar) {
                p.type = CelestialType::SuperheatedPlasma;
            }
        }

        // Prakriti Phase Engine: Boiling & Degassing at high temperatures (> 1400°C)
        if (p.temperature > 1400.0f && app.nebulae.size() < 400 && dist01(app.rng) < 0.12f) {
            const float a = dist01(app.rng) * 6.2831853f;
            const float spd = 4.0f + dist01(app.rng) * 12.0f;
            kalpana::Color gas_col = (p.type == CelestialType::IceCrust)
                ? kalpana::Color{0.4f, 0.85f, 1.0f, 0.35f}  // Ionized water vapor
                : kalpana::Color{1.0f, 0.45f, 0.15f, 0.35f}; // Silicate/iron vapor
            app.nebulae.push_back(NebulaGasParticle{
                .pos = p.pos,
                .vel = p.vel * 0.4f + pebble::math::vec2{std::cos(a) * spd, std::sin(a) * spd},
                .radius = 1.0f + dist01(app.rng) * 1.2f,
                .life = 0.8f + dist01(app.rng) * 0.8f,
                .max_life = 1.6f,
                .color = gas_col
            });
        }

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

    // Roche Limit Tidal Disruption Check for massive celestial nodes
    for (std::size_t i = 0; i < app.planets.size(); ++i) {
        if (!app.planets[i].alive || app.planets[i].mass < 350.0f) continue;
        for (std::size_t j = 0; j < app.planets.size(); ++j) {
            if (i == j || !app.planets[j].alive || app.planets[j].mass >= app.planets[i].mass * 0.15f) continue;

            const pebble::math::vec2 dr = app.planets[j].pos - app.planets[i].pos;
            const float dist2 = dr[0] * dr[0] + dr[1] * dr[1];
            // Roche Limit: d_tidal = R_heavy * (2 * rho_heavy / rho_light)^(1/3)
            const float roche_radius = prakriti::celestial::compute_roche_limit(
                app.planets[i].radius, app.planets[i].density, app.planets[j].density
            );

            if (dist2 < roche_radius * roche_radius && dist2 > (app.planets[i].radius + app.planets[j].radius) * (app.planets[i].radius + app.planets[j].radius)) {
                // Tidal disruption: rip smaller body into an accretion stream of micro dust
                if (app.planets[j].mass > 8.0f && app.planets.size() < 1200) {
                    const float shard_m = app.planets[j].mass * 0.45f;
                    app.planets[j].mass *= 0.55f;
                    PlanetBody stream_spec = app.planets[j];
                    stream_spec.ent = app.world.spawn();
                    stream_spec.mass = shard_m;
                    const pebble::math::vec2 tangent{-dr[1], dr[0]};
                    stream_spec.vel = app.planets[j].vel + pebble::math::normalize(tangent) * 8.0f;
                    stream_spec.temperature += 150.0f;
                    app.planets.push_back(stream_spec);
                }
            }
        }
    }

    // 5. Collision Narrowphase: 3-Way Regime (Elastic Rebound & Recoil, Ductile Merger, or Brittle Fragmentation)
    const std::size_t num_planets = app.planets.size();
    for (std::size_t i = 0; i < num_planets; ++i) {
        if (!app.planets[i].alive) continue;
        for (std::size_t j = i + 1; j < num_planets; ++j) {
            if (!app.planets[j].alive) continue;

            const pebble::math::vec2 dr = app.planets[j].pos - app.planets[i].pos;
            const float dist2 = dr[0] * dr[0] + dr[1] * dr[1];

            if (const float min_dist = app.planets[i].radius + app.planets[j].radius; dist2 < min_dist * min_dist && dist2 > 1e-4f) {
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

                // Thermodynamic parameters & Collision Regime Evaluation via Prakriti Celestial Engine
                const float avg_temp = (app.planets[i].temperature + app.planets[j].temperature) * 0.5f;
                const bool is_molten_1 = (app.planets[i].temperature > 850.0f) || (app.planets[i].type == CelestialType::MoltenMagma);
                const bool is_molten_2 = (app.planets[j].temperature > 850.0f) || (app.planets[j].type == CelestialType::MoltenMagma);
                const float rel_speed = std::sqrt(v_rel2);

                const auto decision = prakriti::celestial::evaluate_collision_regime(
                    app.planets[i].mass, app.planets[j].mass,
                    rel_speed, avg_temp,
                    is_molten_1, is_molten_2,
                    app.planets[i].is_merging, app.planets[j].is_merging
                );

                if (decision.regime == prakriti::celestial::CollisionRegime::MidMergeDisruption) {
                    app.fractures_count++;
                    if (app.planets[i].is_merging && app.planets[i].lobe2_mass > 5.0f) {
                        PlanetBody torn_lobe;
                        torn_lobe.ent = app.world.spawn();
                        torn_lobe.mass = app.planets[i].lobe2_mass;
                        torn_lobe.density = app.planets[i].density;
                        constexpr float kRadiusScale = 2.4f;
                        torn_lobe.radius = std::clamp(kRadiusScale * std::sqrt(torn_lobe.mass / torn_lobe.density), 0.75f, 2.0f);
                        torn_lobe.pos = app.planets[i].pos + app.planets[i].lobe2_offset;
                        torn_lobe.vel = app.planets[i].vel + pebble::math::vec2{normal[1], -normal[0]} * 25.0f;
                        torn_lobe.temperature = app.planets[i].temperature + 100.0f;
                        torn_lobe.type = app.planets[i].type;
                        torn_lobe.is_merging = false;
                        app.planets.push_back(torn_lobe);

                        app.planets[i].mass -= app.planets[i].lobe2_mass;
                        app.planets[i].is_merging = false;
                        app.planets[i].lobe2_radius = 0.0f;
                        app.planets[i].lobe2_offset = pebble::math::vec2{0.0f, 0.0f};
                    }
                } else if (decision.regime == prakriti::celestial::CollisionRegime::DuctileMerge) {
                    // ========================================================
                    // 1. GRADUAL CONTACT BINARY ACCRETION (Joined at tips -> Sphere)
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

                    // Compute contact dumbbell geometry (joined at the touching tips)
                    const pebble::math::vec2 touch_vector = app.planets[j].pos - app.planets[i].pos;
                    const float touch_dist = std::max(0.5f, std::sqrt(touch_vector[0] * touch_vector[0] + touch_vector[1] * touch_vector[1]));

                    app.planets[i].pos = p_cm;
                    app.planets[i].vel = v_cm;
                    app.planets[i].mass = m_total;
                    app.planets[i].density = d_mixed;
                    app.planets[i].temperature = t_mixed;

                    // Initialize Contact Binary Dumbbell State
                    app.planets[i].is_merging = true;
                    app.planets[i].merge_progress = 0.0f;
                    // Secondary lobe attached offset from center of mass
                    app.planets[i].lobe2_offset = (app.planets[j].pos - p_cm);
                    app.planets[i].lobe2_radius = app.planets[j].radius;
                    app.planets[i].lobe2_mass = app.planets[j].mass;

                    // Update spin rate for merged body: omega = L / I
                    constexpr float kRadiusScale = 2.4f;
                    app.planets[i].radius = std::clamp(kRadiusScale * std::sqrt(m_total / d_mixed), 0.75f, 2.5f);
                    const float new_inertia = 0.5f * m_total * (app.planets[i].radius * app.planets[i].radius);
                    app.planets[i].angular_momentum = l_total;
                    app.planets[i].omega = (new_inertia > 1e-4f) ? (l_total / new_inertia) : 0.0f;

                    // ── Gravitational Radiation Chirp Emission (Only for Extreme Relativistic Mergers) ──
                    const auto gw = prakriti::celestial::compute_gravitational_wave_emission(app.planets[i].mass, app.planets[j].mass, touch_dist, rel_speed);
                    if (gw.emits_wave && m_total >= 950.0f) {
                        app.gw_ripples.push_back(GravitationalWaveRipple{
                            .center = p_cm,
                            .radius = 2.0f,
                            .max_radius = 180.0f,
                            .expansion_speed = 140.0f,
                            .amplitude = std::clamp(gw.wave_amplitude * 0.4f, 0.1f, 0.5f),
                            .life = 0.65f,
                            .max_life = 0.65f
                        });
                        app.camera_shake.add_trauma(0.15f);
                    }

                    // ── Relativistic ISCO Accretion Flares (when black holes or pulsars consume matter) ──
                    if (app.planets[i].is_singularity || app.planets[i].type == CelestialType::BlackHoleSingularity ||
                        app.planets[j].is_singularity || app.planets[j].type == CelestialType::BlackHoleSingularity) {
                        
                        const float bh_r = app.planets[i].is_singularity ? app.planets[i].radius : app.planets[j].radius;
                        const auto isco = prakriti::celestial::evaluate_isco_accretion(touch_dist, m_total, bh_r);
                        if (isco.triggers_flare) {
                            app.flares.push_back(AccretionFlare{
                                .pos = p_cm,
                                .radius = isco.flare_radius * 1.4f,
                                .life = 0.40f,
                                .max_life = 0.40f,
                                .color = kalpana::Color{1.0f, 0.85f, 0.3f, 0.95f}
                            });
                        }
                    }

                    // If the primary body is a Black Hole Singularity, emit Relativistic Polar Jets!
                    if (app.planets[i].is_singularity || app.planets[i].type == CelestialType::BlackHoleSingularity) {
                        const float jet_angle = app.planets[i].angle + 1.5707963f; // Perpendicular to accretion plane
                        const pebble::math::vec2 jet_dir1{std::cos(jet_angle), std::sin(jet_angle)};
                        const pebble::math::vec2 jet_dir2{-std::cos(jet_angle), -std::sin(jet_angle)};

                        for (int j_idx = 0; j_idx < 14; ++j_idx) {
                            const float j_spd = 130.0f + dist01(app.rng) * 190.0f; // Ultra-relativistic speed
                            const float spread = (dist01(app.rng) - 0.5f) * 0.15f;
                            const pebble::math::vec2 d1 = pebble::math::normalize(jet_dir1 + pebble::math::vec2{spread, spread});
                            const pebble::math::vec2 d2 = pebble::math::normalize(jet_dir2 + pebble::math::vec2{spread, spread});

                            app.jets.push_back(RelativisticJetParticle{
                                .pos = app.planets[i].pos + d1 * (app.planets[i].radius + 2.0f),
                                .vel = app.planets[i].vel + d1 * j_spd,
                                .radius = 1.5f + dist01(app.rng) * 1.2f,
                                .life = 0.50f + dist01(app.rng) * 0.35f,
                                .max_life = 0.85f,
                                .color = kalpana::Color{0.35f, 0.85f, 1.0f, 0.95f} // Cyan relativistic beam
                            });
                            app.jets.push_back(RelativisticJetParticle{
                                .pos = app.planets[i].pos + d2 * (app.planets[i].radius + 2.0f),
                                .vel = app.planets[i].vel + d2 * j_spd,
                                .radius = 1.5f + dist01(app.rng) * 1.2f,
                                .life = 0.50f + dist01(app.rng) * 0.35f,
                                .max_life = 0.85f,
                                .color = kalpana::Color{0.85f, 0.45f, 1.0f, 0.95f} // Violet relativistic beam
                            });
                        }
                    }

                    app.planets[j].alive = false;
                } else if (decision.regime == prakriti::celestial::CollisionRegime::BrittleFracture) {
                    // ========================================================
                    // 2. CATASTROPHIC BRITTLE SHATTERING / VORONOI FRAGMENTATION
                    // ========================================================
                    app.fractures_count++;

                    if (app.planets[i].mass > 30.0f && app.planets.size() < 1200) {
                        const int num_shards = (app.planets[i].mass > 120.0f) ? 4 : 3;
                        const float shard_mass_each = (app.planets[i].mass * 0.70f) / static_cast<float>(num_shards);
                        app.planets[i].mass *= 0.30f; // Core remnants

                        const float impact_angle = std::atan2(normal[1], normal[0]);

                        for (int k = 0; k < num_shards; ++k) {
                            PlanetBody shard = app.planets[i];
                            shard.ent = app.world.spawn();
                            shard.mass = shard_mass_each * (0.8f + dist01(app.rng) * 0.4f);
                            shard.density = app.planets[i].density;
                            shard.radius = std::clamp(2.4f * std::sqrt(shard.mass / shard.density), 0.75f, 1.8f);

                            // Fan out ejecta along impact dispersal cone
                            const float spread = (static_cast<float>(k) - (num_shards - 1) * 0.5f) * 0.85f + (dist01(app.rng) - 0.5f) * 0.3f;
                            const float ejecta_angle = impact_angle + 3.14159f + spread;
                            const float ejecta_speed = 25.0f + dist01(app.rng) * 35.0f;

                            shard.vel = app.planets[i].vel + pebble::math::vec2{std::cos(ejecta_angle), std::sin(ejecta_angle)} * ejecta_speed;
                            shard.pos = app.planets[i].pos + pebble::math::vec2{std::cos(ejecta_angle), std::sin(ejecta_angle)} * (app.planets[i].radius + shard.radius + 1.0f);
                            shard.temperature = app.planets[i].temperature + 350.0f;
                            shard.omega = (dist01(app.rng) - 0.5f) * 12.0f;
                            shard.angular_momentum = 0.5f * shard.mass * (shard.radius * shard.radius) * shard.omega;
                            shard.is_merging = false;

                            app.planets.push_back(shard);
                        }

                        // Spawn impact flash sparks
                        for (int s = 0; s < 8; ++s) {
                            const float sa = dist01(app.rng) * 6.2831853f;
                            const float ss = 40.0f + dist01(app.rng) * 70.0f;
                            app.sparks.push_back(SparkParticle{
                                .pos = app.planets[i].pos,
                                .vel = app.planets[i].vel + pebble::math::vec2{std::cos(sa), std::sin(sa)} * ss,
                                .radius = 1.2f + dist01(app.rng) * 1.0f,
                                .life = 0.35f + dist01(app.rng) * 0.25f,
                                .max_life = 0.6f,
                                .color = kalpana::Color{1.0f, 0.5f + dist01(app.rng) * 0.4f, 0.1f, 1.0f}
                            });
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
                        constexpr float friction = 0.25f;
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

    // 7. Update Nebula Gas Cloud Particles
    for (auto& [pos, vel, radius, life, max_life, color] : app.nebulae) {
        pos = pos + vel * dt;
        radius += 0.8f * dt; // Expansion
        life -= dt;
        color.a = std::clamp(life / max_life, 0.0f, 1.0f) * 0.35f;
    }
    std::erase_if(app.nebulae, [](const NebulaGasParticle& n) { return n.life <= 0.0f; });
    app.gas_particles_count = app.nebulae.size();

    // 8. Update Relativistic Polar Jet Particles
    for (auto& j : app.jets) {
        j.pos = j.pos + j.vel * dt;
        j.vel = j.vel * 0.985f;
        j.life -= dt;
        j.color.a = std::clamp(j.life / j.max_life, 0.0f, 1.0f) * 0.95f;
    }
    std::erase_if(app.jets, [](const RelativisticJetParticle& j) { return j.life <= 0.0f; });

    // 9. Update Gravitational Wave Ripples (Space-time metric expansion)
    for (auto& gw : app.gw_ripples) {
        gw.radius += gw.expansion_speed * dt;
        gw.life -= dt;
    }
    std::erase_if(app.gw_ripples, [](const GravitationalWaveRipple& gw) { return gw.life <= 0.0f; });

    // 10. Update Relativistic ISCO Accretion Flares
    for (auto& f : app.flares) {
        f.radius += 4.0f * dt;
        f.life -= dt;
        f.color.a = std::clamp(f.life / f.max_life, 0.0f, 1.0f);
    }
    std::erase_if(app.flares, [](const AccretionFlare& f) { return f.life <= 0.0f; });

    app.camera_shake.update(dt);

    // 8. Cleanup Dead Planets
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
// Uses ANSI alternate screen buffer (\033[?1049h) and UTF-8 half-block '▀' (U+2580).
#include <sys/ioctl.h>
#include <unistd.h>

static void render_terminal_ascii(const PebbleVerseApp& app) {
    // Dynamically query terminal window size
    int term_cols = 80;
    int term_rows = 24;
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10 && ws.ws_row > 5) {
        term_cols = ws.ws_col;
        term_rows = ws.ws_row;
    }

    const int TERM_W = std::clamp(term_cols - 2, 40, 120);
    const int TERM_H = std::clamp(term_rows - 4, 15, 45); // Reserve 4 rows for HUD, top border, bottom border
    const int SUB_H  = TERM_H * 2;

    struct TermCell {
        bool has_body = false;
        std::uint8_t r = 0, g = 0, b = 0;
        char glyph = ' ';
    };

    std::vector<std::vector<TermCell>> subgrid(SUB_H, std::vector<TermCell>(TERM_W));

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

    // Alternate screen buffer + Reposition cursor to row 1, col 1 + hide cursor
    out += "\033[?1049h\033[?25l\033[H";

    // Header HUD banner
    out += "\033[1;37;44m PEBBLE VERSE \033[0;1;30;47m N-BODY PLANETARY CONTINUUM \033[0m";
    out += "  \033[36mBodies:\033[1;37m " + std::to_string(app.active_planets_count) + "\033[0m";
    out += "  \033[33mMass:\033[1;37m " + std::to_string(static_cast<int>(app.total_mass)) + "\033[0m";
    out += "  \033[32mMerges:\033[1;37m " + std::to_string(app.fusions_count) + "\033[0m";
    out += "  \033[31mFractures:\033[1;37m " + std::to_string(app.fractures_count) + "\033[0m";
    out += "  \033[35mCompute:\033[1;37m " + std::to_string(app.compute_ms).substr(0, 4) + " ms\033[0m\033[K\n";

    // Top border
    out += "\033[38;2;60;80;120m┌";
    for (int x = 0; x < TERM_W; ++x) out += "─";
    out += "┐\033[0m\033[K\n";

    // Render paired rows using half-block '▀'
    for (int y = 0; y < TERM_H; ++y) {
        const int top_y = y * 2;
        const int bot_y = top_y + 1;

        out += "\033[38;2;60;80;120m│\033[0m";

        for (int x = 0; x < TERM_W; ++x) {
            const auto& top = subgrid[top_y][x];

            if (const auto& bot = subgrid[bot_y][x]; !top.has_body && !bot.has_body) {
                out += ' ';
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

        out += "\033[38;2;60;80;120m│\033[0m\033[K\n";
    }

    // Bottom border (no trailing newline to avoid scrolling)
    out += "\033[38;2;60;80;120m└";
    for (int x = 0; x < TERM_W; ++x) out += "─";
    out += "┘\033[0m\033[K";

    // Write full atomic frame to stdout
    std::cout << out << std::flush;
}

// ----------------------------------------------------------------------------
// GPU Render Frame: Sokol GFX Instanced Particle Pipeline
// ----------------------------------------------------------------------------
static void render_gpu_frame(PebbleVerseApp& app) {
    kalpana::Scene scene;
    app.instanced_planets.begin();

    // 1. Batch Orbital Motion Trails & Accretion Halos
    for (const auto& p : app.planets) {
        if (p.trail_count > 1) {
            const kalpana::Color col = get_celestial_color(p, app.view_mode);
            for (int t = 0; t < p.trail_count; ++t) {
                const int idx = (p.trail_head - 1 - t + PlanetBody::kMaxTrail) % PlanetBody::kMaxTrail;
                const float fade = 1.0f - static_cast<float>(t) / static_cast<float>(p.trail_count);
                kalpana::Color t_col = col;
                t_col.a = fade * 0.40f;
                app.instanced_planets.add_instance(p.trail_history[idx][0], p.trail_history[idx][1], p.radius * fade * 0.85f, t_col);
            }
        }
        // Celestial Body Core & Contact Binary Lobe (Dumbbell shape)
        const kalpana::Color c = get_celestial_color(p, app.view_mode);
        app.instanced_planets.add_instance(p.pos[0], p.pos[1], p.radius, c);

        // If in gradual coalescence, render the secondary connected lobe and bridging neck
        if (p.is_merging && p.lobe2_radius > 0.4f) {
            const pebble::math::vec2 lobe_pos = p.pos + p.lobe2_offset;
            const float lobe_r = p.lobe2_radius * (1.0f - p.merge_progress * 0.5f);
            app.instanced_planets.add_instance(lobe_pos[0], lobe_pos[1], lobe_r, c);

            // Connecting neck speck
            const pebble::math::vec2 neck_pos = p.pos + p.lobe2_offset * 0.5f;
            const float neck_r = std::min(p.radius, lobe_r) * 0.8f;
            app.instanced_planets.add_instance(neck_pos[0], neck_pos[1], neck_r, c);
        }
    }

    // 2. Batch Evaporated Nebula Gas Clouds
    for (const auto& n : app.nebulae) {
        app.instanced_planets.add_instance(n.pos[0], n.pos[1], n.radius, n.color);
    }

    // 3. Batch Fire Sparks
    for (const auto& s : app.sparks) {
        app.instanced_planets.add_instance(s.pos[0], s.pos[1], s.radius, s.color);
    }

    // 4. Batch Relativistic Polar Matter Jets
    for (const auto& j : app.jets) {
        app.instanced_planets.add_instance(j.pos[0], j.pos[1], j.radius, j.color);
    }

    // 5. Batch Relativistic ISCO Accretion Flares
    for (const auto& f : app.flares) {
        app.instanced_planets.add_instance(f.pos[0], f.pos[1], f.radius, f.color);
    }

    // 6. Relativistic Gravitational Wave Space-time Ripples (Ultra-Subtle & Faint)
    for (const auto& gw : app.gw_ripples) {
        const float alpha = std::clamp(gw.life / gw.max_life, 0.0f, 1.0f) * 0.18f; // Very faint whisper
        kalpana::Path gw_ring;
        gw_ring.circle(gw.center[0], gw.center[1], gw.radius);
        scene.add(kalpana::Node::shape(gw_ring, kalpana::Paint::stroke(kalpana::Color{0.35f, 0.7f, 1.0f, alpha}, 0.75f)));
    }

    // 5. Planetary Velocity Indicators (for Top Massive Bodies)
    {
        // Find top 5 heaviest bodies
        std::vector<const PlanetBody*> heavy_bodies;
        heavy_bodies.reserve(app.planets.size());
        for (const auto& p : app.planets) {
            if (p.alive && p.mass > 40.0f) heavy_bodies.push_back(&p);
        }
        std::sort(heavy_bodies.begin(), heavy_bodies.end(), [](const PlanetBody* a, const PlanetBody* b) {
            return a->mass > b->mass;
        });

        const std::size_t n_vectors = std::min(heavy_bodies.size(), std::size_t(5));
        for (std::size_t i = 0; i < n_vectors; ++i) {
            const auto* p = heavy_bodies[i];
            const float v_mag = std::sqrt(p->vel[0] * p->vel[0] + p->vel[1] * p->vel[1]);
            if (v_mag > 1.0f) {
                // Velocity vector arrow
                kalpana::Path v_arrow;
                v_arrow.move_to(p->pos[0], p->pos[1]);
                const pebble::math::vec2 v_end = p->pos + p->vel * 0.85f;
                v_arrow.line_to(v_end[0], v_end[1]);
                scene.add(kalpana::Node::shape(v_arrow, kalpana::Paint::stroke(kalpana::Color{0.2f, 0.9f, 0.7f, 0.45f}, 1.2f)));
            }
        }
    }

    // 6. Gravitational Lensing, Photon Rings & Relativistic Event Horizons
    for (const auto& p : app.planets) {
        if (!p.alive) continue;
        if (p.is_singularity || p.type == CelestialType::BlackHoleSingularity) {
            // Relativistic Event Horizon (Black Void Core)
            app.instanced_planets.add_instance(p.pos[0], p.pos[1], p.radius, kalpana::Color{0.01f, 0.01f, 0.03f, 1.0f});

            // Glowing Superheated Accretion Ring (ISCO - Innermost Stable Circular Orbit)
            const float photon_ring_r = p.radius * 2.2f;
            app.instanced_planets.add_instance(p.pos[0], p.pos[1], photon_ring_r, kalpana::Color{1.0f, 0.65f, 0.2f, 0.85f});

            // Relativistic Gravitational Distortion Corona
            const float warp_r = p.radius * 4.5f;
            app.instanced_planets.add_instance(p.pos[0], p.pos[1], warp_r, kalpana::Color{0.6f, 0.3f, 1.0f, 0.25f});
        } else if (p.mass > 250.0f) {
            const float lens_r = p.radius * (1.8f + (p.mass / 1000.0f) * 1.5f);
            kalpana::Color lens_col = (p.type == CelestialType::DegenerateDense)
                ? kalpana::Color{0.7f, 0.4f, 1.0f, 0.22f} // Violet gravitational warp
                : ((p.temperature > 1000.0f)
                    ? kalpana::Color{1.0f, 0.6f, 0.1f, 0.18f}  // Incandescent thermal aura
                    : kalpana::Color{0.3f, 0.7f, 1.0f, 0.15f}); // Subdued gravitational ring
            app.instanced_planets.add_instance(p.pos[0], p.pos[1], lens_r, lens_col);
        }
    }

    // 5. Interactive Slingshot Launcher Vector Guide & Future Trajectory Predictor
    if (app.slingshot.active) {
        // Aim line
        kalpana::Path sling_line;
        sling_line.move_to(app.slingshot.start_x, app.slingshot.start_y);
        sling_line.line_to(app.slingshot.current_x, app.slingshot.current_y);
        scene.add(kalpana::Node::shape(sling_line, kalpana::Paint::stroke(kalpana::Color{0.2f, 0.95f, 0.4f, 0.85f}, 2.0f)));

        kalpana::Path sling_head;
        sling_head.circle(app.slingshot.start_x, app.slingshot.start_y, 4.0f);
        scene.add(kalpana::Node::shape(sling_head, kalpana::Paint::fill(kalpana::Color{0.2f, 0.95f, 0.4f, 0.9f})));

        // Trajectory Predictor: 24-step forward Verlet lookahead
        const float dx = app.slingshot.start_x - app.slingshot.current_x;
        const float dy = app.slingshot.start_y - app.slingshot.current_y;
        pebble::math::vec2 sim_pos{app.slingshot.start_x, app.slingshot.start_y};
        pebble::math::vec2 sim_vel{dx * 0.45f, dy * 0.45f};

        kalpana::Path traj_path;
        traj_path.move_to(sim_pos[0], sim_pos[1]);
        for (int step = 0; step < 24; ++step) {
            constexpr float pred_dt = 0.08f;
            // Sample gravitational force from nearby massive bodies
            pebble::math::vec2 f_grav{0.0f, 0.0f};
            for (const auto& p : app.planets) {
                if (!p.alive) continue;
                const pebble::math::vec2 d = p.pos - sim_pos;
                const float dist2 = d[0] * d[0] + d[1] * d[1] + 100.0f;
                f_grav = f_grav + pebble::math::normalize(d) * (GRAVITATIONAL_G * p.mass / dist2);
            }
            sim_vel = sim_vel + f_grav * pred_dt;
            sim_pos = sim_pos + sim_vel * pred_dt;
            traj_path.line_to(sim_pos[0], sim_pos[1]);
        }
        scene.add(kalpana::Node::shape(traj_path, kalpana::Paint::stroke(kalpana::Color{0.3f, 0.9f, 0.6f, 0.45f}, 1.5f)));
    }

    // 6. Real-Time Orbital Minimap / Radar Inset (Bottom-Right)
    {
        constexpr float radar_w = 120.0f;
        constexpr float radar_h = 75.0f;
        const float radar_x = FW - radar_w - 12.0f;
        const float radar_y = FH - radar_h - 12.0f;

        // Radar background & frame border
        kalpana::Path r_box;
        r_box.rect(radar_x, radar_y, radar_w, radar_h);
        scene.add(kalpana::Node::shape(r_box, kalpana::Paint::fill(kalpana::Color{0.02f, 0.04f, 0.08f, 0.85f})));
        scene.add(kalpana::Node::shape(r_box, kalpana::Paint::stroke(kalpana::Color{0.2f, 0.5f, 0.85f, 0.7f}, 1.0f)));

        // Center crosshair
        kalpana::Path r_cross;
        r_cross.move_to(radar_x + radar_w * 0.5f - 6.0f, radar_y + radar_h * 0.5f);
        r_cross.line_to(radar_x + radar_w * 0.5f + 6.0f, radar_y + radar_h * 0.5f);
        r_cross.move_to(radar_x + radar_w * 0.5f, radar_y + radar_h * 0.5f - 6.0f);
        r_cross.line_to(radar_x + radar_w * 0.5f, radar_y + radar_h * 0.5f + 6.0f);
        scene.add(kalpana::Node::shape(r_cross, kalpana::Paint::stroke(kalpana::Color{0.25f, 0.6f, 0.9f, 0.4f}, 0.8f)));

        // Map bodies onto radar coordinates
        for (const auto& p : app.planets) {
            if (!p.alive) continue;
            // Map viewport [0, FW] x [0, FH] to radar box
            const float rx = radar_x + (p.pos[0] / FW) * radar_w;
            const float ry = radar_y + (p.pos[1] / FH) * radar_h;
            if (rx >= radar_x && rx <= radar_x + radar_w && ry >= radar_y && ry <= radar_y + radar_h) {
                kalpana::Path p_dot;
                const float dot_r = (p.mass > 500.0f) ? 2.0f : ((p.mass > 100.0f) ? 1.4f : 0.9f);
                p_dot.circle(rx, ry, dot_r);
                scene.add(kalpana::Node::shape(p_dot, kalpana::Paint::fill(get_celestial_color(p))));
            }
        }
    }

    // High-Legibility Vector Stroke Typography (Continuous crisp anti-aliased line strokes)
    auto draw_stroke_char = [&](const float x, const float y, const char ch, const kalpana::Color col, const float w = 7.0f, const float h = 11.0f) -> float {
        kalpana::Path p;
        const float x0 = x, x1 = x + w * 0.5f, x2 = x + w;
        const float y0 = y, y1 = y + h * 0.5f, y2 = y + h;

        switch (ch) {
            case 'A':
                p.move_to(x0, y2); p.line_to(x0, y1); p.line_to(x1, y0); p.line_to(x2, y1); p.line_to(x2, y2);
                p.move_to(x0, y1); p.line_to(x2, y1);
                break;
            case 'B':
                p.move_to(x0, y2); p.line_to(x0, y0); p.line_to(x1 + 1.0f, y0); p.line_to(x2, y0 + h * 0.25f);
                p.line_to(x1 + 1.0f, y1); p.line_to(x2, y1 + h * 0.25f); p.line_to(x1 + 1.0f, y2); p.line_to(x0, y2);
                p.move_to(x0, y1); p.line_to(x1 + 1.0f, y1);
                break;
            case 'C':
                p.move_to(x2, y0); p.line_to(x0, y0); p.line_to(x0, y2); p.line_to(x2, y2);
                break;
            case 'D':
                p.move_to(x0, y0); p.line_to(x1, y0); p.line_to(x2, y1); p.line_to(x1, y2); p.line_to(x0, y2); p.line_to(x0, y0);
                break;
            case 'E':
                p.move_to(x2, y0); p.line_to(x0, y0); p.line_to(x0, y2); p.line_to(x2, y2);
                p.move_to(x0, y1); p.line_to(x1 + 1.0f, y1);
                break;
            case 'F':
                p.move_to(x2, y0); p.line_to(x0, y0); p.line_to(x0, y2);
                p.move_to(x0, y1); p.line_to(x1 + 1.0f, y1);
                break;
            case 'G':
                p.move_to(x2, y0); p.line_to(x0, y0); p.line_to(x0, y2); p.line_to(x2, y2); p.line_to(x2, y1); p.line_to(x1, y1);
                break;
            case 'H':
                p.move_to(x0, y0); p.line_to(x0, y2);
                p.move_to(x2, y0); p.line_to(x2, y2);
                p.move_to(x0, y1); p.line_to(x2, y1);
                break;
            case 'I':
                p.move_to(x0, y0); p.line_to(x2, y0);
                p.move_to(x1, y0); p.line_to(x1, y2);
                p.move_to(x0, y2); p.line_to(x2, y2);
                break;
            case 'K':
                p.move_to(x0, y0); p.line_to(x0, y2);
                p.move_to(x2, y0); p.line_to(x0, y1); p.line_to(x2, y2);
                break;
            case 'L':
                p.move_to(x0, y0); p.line_to(x0, y2); p.line_to(x2, y2);
                break;
            case 'M':
                p.move_to(x0, y2); p.line_to(x0, y0); p.line_to(x1, y1); p.line_to(x2, y0); p.line_to(x2, y2);
                break;
            case 'N':
                p.move_to(x0, y2); p.line_to(x0, y0); p.line_to(x2, y2); p.line_to(x2, y0);
                break;
            case 'O':
            case '0':
                p.move_to(x0, y0); p.line_to(x2, y0); p.line_to(x2, y2); p.line_to(x0, y2); p.line_to(x0, y0);
                break;
            case 'P':
                p.move_to(x0, y2); p.line_to(x0, y0); p.line_to(x2, y0); p.line_to(x2, y1); p.line_to(x0, y1);
                break;
            case 'R':
                p.move_to(x0, y2); p.line_to(x0, y0); p.line_to(x2, y0); p.line_to(x2, y1); p.line_to(x0, y1);
                p.move_to(x1, y1); p.line_to(x2, y2);
                break;
            case 'S':
                p.move_to(x2, y0); p.line_to(x0, y0); p.line_to(x0, y1); p.line_to(x2, y1); p.line_to(x2, y2); p.line_to(x0, y2);
                break;
            case 'T':
                p.move_to(x0, y0); p.line_to(x2, y0);
                p.move_to(x1, y0); p.line_to(x1, y2);
                break;
            case 'U':
                p.move_to(x0, y0); p.line_to(x0, y2); p.line_to(x2, y2); p.line_to(x2, y0);
                break;
            case 'V':
                p.move_to(x0, y0); p.line_to(x1, y2); p.line_to(x2, y0);
                break;
            case 'W':
                p.move_to(x0, y0); p.line_to(x0, y2); p.line_to(x1, y1); p.line_to(x2, y2); p.line_to(x2, y0);
                break;
            case 'X':
                p.move_to(x0, y0); p.line_to(x2, y2);
                p.move_to(x2, y0); p.line_to(x0, y2);
                break;
            case 'Y':
                p.move_to(x0, y0); p.line_to(x1, y1); p.line_to(x2, y0);
                p.move_to(x1, y1); p.line_to(x1, y2);
                break;
            case 'Z':
                p.move_to(x0, y0); p.line_to(x2, y0); p.line_to(x0, y2); p.line_to(x2, y2);
                break;
            case 'Q':
                p.move_to(x0, y0); p.line_to(x2, y0); p.line_to(x2, y2); p.line_to(x0, y2); p.line_to(x0, y0);
                p.move_to(x1, y1); p.line_to(x2 + 1.0f, y2 + 1.0f);
                break;
            case '<':
                p.move_to(x2, y0); p.line_to(x0, y1); p.line_to(x2, y2);
                break;
            case '>':
                p.move_to(x0, y0); p.line_to(x2, y1); p.line_to(x0, y2);
                break;
            case '(':
                p.move_to(x2, y0); p.line_to(x0, y1); p.line_to(x2, y2);
                break;
            case ')':
                p.move_to(x0, y0); p.line_to(x2, y1); p.line_to(x0, y2);
                break;
            case '%':
                p.circle(x0 + 1.5f, y0 + 2.0f, 1.0f);
                p.move_to(x0, y2); p.line_to(x2, y0);
                p.circle(x2 - 1.5f, y2 - 2.0f, 1.0f);
                break;
            case '1':
                p.move_to(x0, y0 + 3.0f); p.line_to(x1, y0); p.line_to(x1, y2);
                p.move_to(x0, y2); p.line_to(x2, y2);
                break;
            case '2':
                p.move_to(x0, y0); p.line_to(x2, y0); p.line_to(x2, y1); p.line_to(x0, y2); p.line_to(x2, y2);
                break;
            case '3':
                p.move_to(x0, y0); p.line_to(x2, y0); p.line_to(x2, y1); p.line_to(x0, y1);
                p.move_to(x2, y1); p.line_to(x2, y2); p.line_to(x0, y2);
                break;
            case '4':
                p.move_to(x0, y0); p.line_to(x0, y1); p.line_to(x2, y1);
                p.move_to(x2, y0); p.line_to(x2, y2);
                break;
            case '5':
                p.move_to(x2, y0); p.line_to(x0, y0); p.line_to(x0, y1); p.line_to(x2, y1); p.line_to(x2, y2); p.line_to(x0, y2);
                break;
            case '6':
                p.move_to(x2, y0); p.line_to(x0, y0); p.line_to(x0, y2); p.line_to(x2, y2); p.line_to(x2, y1); p.line_to(x0, y1);
                break;
            case '7':
                p.move_to(x0, y0); p.line_to(x2, y0); p.line_to(x1, y2);
                break;
            case '8':
                p.move_to(x0, y0); p.line_to(x2, y0); p.line_to(x2, y2); p.line_to(x0, y2); p.line_to(x0, y0);
                p.move_to(x0, y1); p.line_to(x2, y1);
                break;
            case '9':
                p.move_to(x2, y1); p.line_to(x0, y1); p.line_to(x0, y0); p.line_to(x2, y0); p.line_to(x2, y2); p.line_to(x0, y2);
                break;
            case ':':
                p.circle(x1, y0 + 3.0f, 1.0f);
                p.circle(x1, y2 - 3.0f, 1.0f);
                break;
            case '.':
                p.circle(x1, y2 - 1.0f, 1.0f);
                break;
            case '-':
                p.move_to(x0, y1); p.line_to(x2, y1);
                break;
            case '+':
                p.move_to(x0, y1); p.line_to(x2, y1);
                p.move_to(x1, y0 + 2.0f); p.line_to(x1, y2 - 2.0f);
                break;
            case '/':
                p.move_to(x0, y2); p.line_to(x2, y0);
                break;
            case '[':
                p.move_to(x2, y0); p.line_to(x0, y0); p.line_to(x0, y2); p.line_to(x2, y2);
                break;
            case ']':
                p.move_to(x0, y0); p.line_to(x2, y0); p.line_to(x2, y2); p.line_to(x0, y2);
                break;
            case ' ':
                return w * 0.6f;
            default:
                break;
        }

        scene.add(kalpana::Node::shape(p, kalpana::Paint::stroke(col, 1.4f)));
        return w + 3.5f; // Character spacing
    };

    // 6. Top Header Vector HUD Banner with Crisp Stroke Text
    {
        auto draw_text = [&](const float x, const float y, const std::string_view str, const kalpana::Color col, const float w = 7.0f, const float h = 11.0f) -> float {
            float cx = x;
            for (const char ch : str) {
                cx += draw_stroke_char(cx, y, ch, col, w, h);
            }
            return cx;
        };
        // Top HUD background banner strip
        kalpana::Path hud_bg;
        hud_bg.rect(0.0f, 0.0f, FW, 32.0f);
        scene.add(kalpana::Node::shape(hud_bg, kalpana::Paint::fill(kalpana::Color{0.02f, 0.04f, 0.08f, 0.94f})));

        kalpana::Path hud_border;
        hud_border.move_to(0.0f, 32.0f);
        hud_border.line_to(FW, 32.0f);
        scene.add(kalpana::Node::shape(hud_border, kalpana::Paint::stroke(kalpana::Color{0.2f, 0.4f, 0.7f, 0.6f}, 1.0f)));

        // Title Tag: "PEBBLE"
        draw_text(12.0f, 10.0f, "PEBBLE", kalpana::Color{0.4f, 0.8f, 1.0f, 0.95f}, 8.0f, 12.0f);

        // 1) Cyan Dot + "BODIES: <count>"
        kalpana::Path bodies_dot;
        bodies_dot.circle(105.0f, 16.0f, 3.5f);
        scene.add(kalpana::Node::shape(bodies_dot, kalpana::Paint::fill(kalpana::Color{0.2f, 0.85f, 1.0f, 0.95f})));
        draw_text(115.0f, 10.0f, "BODIES:" + std::to_string(app.active_planets_count), kalpana::Color{0.2f, 0.85f, 1.0f, 0.95f}, 6.5f, 11.0f);

        // 2) Green Dot + "MERGES: <count>"
        kalpana::Path merge_dot;
        merge_dot.circle(235.0f, 16.0f, 3.5f);
        scene.add(kalpana::Node::shape(merge_dot, kalpana::Paint::fill(kalpana::Color{0.25f, 0.95f, 0.45f, 0.95f})));
        draw_text(245.0f, 10.0f, "MERGES:" + std::to_string(app.fusions_count), kalpana::Color{0.25f, 0.95f, 0.45f, 0.95f}, 6.5f, 11.0f);

        // 3) Red Dot + "SHARDS: <count>"
        kalpana::Path frac_dot;
        frac_dot.circle(385.0f, 16.0f, 3.5f);
        scene.add(kalpana::Node::shape(frac_dot, kalpana::Paint::fill(kalpana::Color{1.0f, 0.35f, 0.35f, 0.95f})));
        draw_text(395.0f, 10.0f, "SHARDS:" + std::to_string(app.fractures_count), kalpana::Color{1.0f, 0.35f, 0.35f, 0.95f}, 6.5f, 11.0f);

        // 4) Violet Dot + "COMPUTE: <time>MS"
        kalpana::Path comp_dot;
        comp_dot.circle(535.0f, 16.0f, 3.5f);
        scene.add(kalpana::Node::shape(comp_dot, kalpana::Paint::fill(kalpana::Color{0.85f, 0.45f, 1.0f, 0.95f})));
        draw_text(545.0f, 10.0f, "COMPUTE:" + std::to_string(static_cast<int>(app.compute_ms)) + "MS", kalpana::Color{0.85f, 0.45f, 1.0f, 0.95f}, 6.5f, 11.0f);

        // 5) Active Configured Parameters Display (Clean Physics Telemetry, No Controls Text)
        // G Constant
        std::string g_str = "G:" + std::to_string(static_cast<int>(app.config_grav_g));
        draw_text(660.0f, 10.0f, g_str, kalpana::Color{1.0f, 0.85f, 0.35f, 0.95f}, 6.5f, 11.0f);

        // Barnes-Hut Theta Precision
        const int theta_pct = static_cast<int>(app.config_bh_theta * 100.0f);
        std::string th_str = "THETA:0." + std::to_string(theta_pct);
        draw_text(745.0f, 10.0f, th_str, kalpana::Color{0.45f, 0.85f, 1.0f, 0.95f}, 6.5f, 11.0f);

        // Spectral View Mode (Toggleable via V / 1-3)
        const char* view_str = (app.view_mode == SpectralViewMode::OpticalRGB) ? "VIEW:OPTICAL" :
                              ((app.view_mode == SpectralViewMode::ThermalInfrared) ? "VIEW:THERMAL" : "VIEW:RADIO/XRAY");
        const kalpana::Color view_col = (app.view_mode == SpectralViewMode::OpticalRGB) ? kalpana::Color{0.5f, 0.95f, 1.0f, 0.95f} :
                                       ((app.view_mode == SpectralViewMode::ThermalInfrared) ? kalpana::Color{1.0f, 0.45f, 0.2f, 0.95f} :
                                                                                              kalpana::Color{0.85f, 0.4f, 1.0f, 0.95f});
        draw_text(850.0f, 10.0f, view_str, view_col, 6.5f, 11.0f);

        // Cosmic Metallicity Z Index
        const int z_pct = static_cast<int>(app.cosmic_metallicity_z * 100.0f);
        std::string z_str = "Z:0." + std::to_string(z_pct);
        draw_text(995.0f, 10.0f, z_str, kalpana::Color{1.0f, 0.75f, 0.3f, 0.95f}, 6.5f, 11.0f);

        // Time Dilation Speed
        const int speed_pct = static_cast<int>(app.time_dilation * 100.0f);
        std::string time_str = "TIME:" + std::to_string(speed_pct / 100) + "." + std::to_string((speed_pct % 100) / 10) + "X";
        draw_text(1085.0f, 10.0f, time_str, kalpana::Color{0.6f, 0.95f, 0.7f, 0.95f}, 6.5f, 11.0f);
    }

    // 7. Interactive Mouse Reticle
    if (app.mouse_down && !app.in_startup_modal) {
        kalpana::Path reticle;
        reticle.circle(app.mouse_x, app.mouse_y, 25.0f);
        scene.add(kalpana::Node::shape(reticle, kalpana::Paint::stroke(kalpana::Color{0.8f, 0.5f, 1.0f, 0.9f}, 2.0f)));
    }

    // 8. Startup Parameter Configuration Popup Modal
    if (app.in_startup_modal) {
        auto draw_modal_text = [&](const float x, const float y, const std::string_view str, const kalpana::Color col, const float w = 7.5f, const float h = 12.0f) -> float {
            float cx = x;
            for (const char ch : str) {
                cx += draw_stroke_char(cx, y, ch, col, w, h);
            }
            return cx;
        };

        // Darkened glassmorphism backdrop
        kalpana::Path backdrop;
        backdrop.rect(0.0f, 0.0f, FW, FH);
        scene.add(kalpana::Node::shape(backdrop, kalpana::Paint::fill(kalpana::Color{0.01f, 0.02f, 0.05f, 0.88f})));

        // Modal Frame Card (Centered)
        constexpr float mw = 580.0f;
        constexpr float mh = 380.0f;
        const float mx = (FW - mw) * 0.5f;
        const float my = (FH - mh) * 0.5f;

        kalpana::Path modal_card;
        modal_card.rect(mx, my, mw, mh);
        scene.add(kalpana::Node::shape(modal_card, kalpana::Paint::fill(kalpana::Color{0.04f, 0.07f, 0.14f, 0.98f})));
        scene.add(kalpana::Node::shape(modal_card, kalpana::Paint::stroke(kalpana::Color{0.3f, 0.65f, 1.0f, 0.85f}, 1.5f)));

        // Modal Header Banner
        kalpana::Path header_strip;
        header_strip.rect(mx, my, mw, 45.0f);
        scene.add(kalpana::Node::shape(header_strip, kalpana::Paint::fill(kalpana::Color{0.08f, 0.14f, 0.28f, 0.95f})));

        draw_modal_text(mx + 25.0f, my + 15.0f, "PEBBLE VERSE : COSMOLOGICAL CONFIG", kalpana::Color{0.4f, 0.9f, 1.0f, 1.0f}, 8.5f, 14.0f);

        // Parameters Rows
        const float start_y = my + 65.0f;
        constexpr float row_step = 55.0f;

        // Row 0: Dust Count
        {
            const bool sel = (app.config_selected_row == 0);
            const kalpana::Color row_col = sel ? kalpana::Color{1.0f, 0.85f, 0.2f, 1.0f} : kalpana::Color{0.7f, 0.8f, 0.9f, 0.85f};
            if (sel) {
                kalpana::Path sel_box;
                sel_box.rect(mx + 15.0f, start_y - 4.0f, mw - 30.0f, 44.0f);
                scene.add(kalpana::Node::shape(sel_box, kalpana::Paint::stroke(kalpana::Color{1.0f, 0.85f, 0.2f, 0.7f}, 1.2f)));
            }
            draw_modal_text(mx + 30.0f, start_y + 4.0f, "[1] INITIAL BODIES / DUST", row_col, 7.0f, 11.0f);
            draw_modal_text(mx + 340.0f, start_y + 4.0f, "< " + std::to_string(app.config_initial_dust_count) + " SPECS >", sel ? kalpana::Color{0.3f, 0.95f, 1.0f, 1.0f} : row_col, 7.5f, 12.0f);
            draw_modal_text(mx + 30.0f, start_y + 22.0f, "TOTAL PRIMORDIAL MATTER PARTICLES SEEDED IN COSMOS", kalpana::Color{0.45f, 0.6f, 0.75f, 0.75f}, 5.0f, 9.0f);
        }

        // Row 1: Gravitational Constant G
        {
            const float ry = start_y + row_step;
            const bool sel = (app.config_selected_row == 1);
            const kalpana::Color row_col = sel ? kalpana::Color{1.0f, 0.85f, 0.2f, 1.0f} : kalpana::Color{0.7f, 0.8f, 0.9f, 0.85f};
            if (sel) {
                kalpana::Path sel_box;
                sel_box.rect(mx + 15.0f, ry - 4.0f, mw - 30.0f, 44.0f);
                scene.add(kalpana::Node::shape(sel_box, kalpana::Paint::stroke(kalpana::Color{1.0f, 0.85f, 0.2f, 0.7f}, 1.2f)));
            }
            draw_modal_text(mx + 30.0f, ry + 4.0f, "[2] GRAVITATIONAL G", row_col, 7.0f, 11.0f);
            draw_modal_text(mx + 340.0f, ry + 4.0f, "< " + std::to_string(static_cast<int>(app.config_grav_g)) + " N*M2 >", sel ? kalpana::Color{0.3f, 0.95f, 1.0f, 1.0f} : row_col, 7.5f, 12.0f);
            draw_modal_text(mx + 30.0f, ry + 22.0f, "FUNDAMENTAL N-BODY ACCELERATION CONSTANT", kalpana::Color{0.45f, 0.6f, 0.75f, 0.75f}, 5.0f, 9.0f);
        }

        // Row 2: Primordial Distribution Pattern
        {
            const float ry = start_y + row_step * 2.0f;
            const bool sel = (app.config_selected_row == 2);
            const kalpana::Color row_col = sel ? kalpana::Color{1.0f, 0.85f, 0.2f, 1.0f} : kalpana::Color{0.7f, 0.8f, 0.9f, 0.85f};
            if (sel) {
                kalpana::Path sel_box;
                sel_box.rect(mx + 15.0f, ry - 4.0f, mw - 30.0f, 44.0f);
                scene.add(kalpana::Node::shape(sel_box, kalpana::Paint::stroke(kalpana::Color{1.0f, 0.85f, 0.2f, 0.7f}, 1.2f)));
            }
            const std::string mode_str = (app.config_dist_mode == 0) ? "UNIFORM COSMIC" :
                                        ((app.config_dist_mode == 1) ? "BARYCENTRIC CLUSTER" : "DUAL ACCRETION");
            draw_modal_text(mx + 30.0f, ry + 4.0f, "[3] MATTER PATTERN", row_col, 7.0f, 11.0f);
            draw_modal_text(mx + 340.0f, ry + 4.0f, "< " + mode_str + " >", sel ? kalpana::Color{0.3f, 0.95f, 1.0f, 1.0f} : row_col, 6.5f, 11.0f);
            draw_modal_text(mx + 30.0f, ry + 22.0f, "SPATIAL GEOMETRY OF IN-SITU MATRICES", kalpana::Color{0.45f, 0.6f, 0.75f, 0.75f}, 5.0f, 9.0f);
        }

        // Row 3: Barnes-Hut Theta Precision
        {
            const float ry = start_y + row_step * 3.0f;
            const bool sel = (app.config_selected_row == 3);
            const kalpana::Color row_col = sel ? kalpana::Color{1.0f, 0.85f, 0.2f, 1.0f} : kalpana::Color{0.7f, 0.8f, 0.9f, 0.85f};
            if (sel) {
                kalpana::Path sel_box;
                sel_box.rect(mx + 15.0f, ry - 4.0f, mw - 30.0f, 44.0f);
                scene.add(kalpana::Node::shape(sel_box, kalpana::Paint::stroke(kalpana::Color{1.0f, 0.85f, 0.2f, 0.7f}, 1.2f)));
            }
            const int theta_pct = static_cast<int>(app.config_bh_theta * 100.0f);
            draw_modal_text(mx + 30.0f, ry + 4.0f, "[4] QUADTREE THETA", row_col, 7.0f, 11.0f);
            draw_modal_text(mx + 340.0f, ry + 4.0f, "< 0." + std::to_string(theta_pct) + " MACRO >", sel ? kalpana::Color{0.3f, 0.95f, 1.0f, 1.0f} : row_col, 7.5f, 12.0f);
            draw_modal_text(mx + 30.0f, ry + 22.0f, "MULTIPOLE OPENING ANGLE RESOLUTION", kalpana::Color{0.45f, 0.6f, 0.75f, 0.75f}, 5.0f, 9.0f);
        }

        // Bottom Action Bar: Launch Prompt
        kalpana::Path btn_box;
        btn_box.rect(mx + 120.0f, my + mh - 58.0f, mw - 240.0f, 36.0f);
        scene.add(kalpana::Node::shape(btn_box, kalpana::Paint::fill(kalpana::Color{0.12f, 0.45f, 0.95f, 0.9f})));
        scene.add(kalpana::Node::shape(btn_box, kalpana::Paint::stroke(kalpana::Color{0.4f, 0.85f, 1.0f, 1.0f}, 1.5f)));

        draw_modal_text(mx + 155.0f, my + mh - 47.0f, "PRESS [ENTER] TO START SIMULATION", kalpana::Color{1.0f, 1.0f, 1.0f, 1.0f}, 7.0f, 12.0f);

        // Sub-instruction
        draw_modal_text(mx + 115.0f, my + mh - 16.0f, "[UP/DOWN]: SELECT   [LEFT/RIGHT]: ADJUST VALUE", kalpana::Color{0.5f, 0.7f, 0.9f, 0.8f}, 5.5f, 9.5f);
    }

    // Render vector overlays
    app.canvas->render(scene);

    static constexpr std::size_t kMaxVBufBytes = 4 * 1024 * 1024; // 4MB
    static constexpr std::size_t kMaxIBufBytes = 4 * 1024 * 1024; // 4MB

    const auto& verts = app.canvas->backend().vertices();
    const auto& inds  = app.canvas->backend().indices();

    if (!verts.empty() && !inds.empty()) {
        const std::size_t v_bytes = std::min(verts.size() * sizeof(kalpana::sokol_backend::Vertex), kMaxVBufBytes);
        sg_range vr = {.ptr = verts.data(), .size = v_bytes};
        sg_update_buffer(app.vbuf, vr);

        const std::size_t i_bytes = std::min(inds.size() * sizeof(std::uint32_t), kMaxIBufBytes);
        sg_range ir = {.ptr = inds.data(), .size = i_bytes};
        sg_update_buffer(app.ibuf, ir);
    }

    sg_pass pass{};
    pass.action = app.pass_action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(pass);

    // 1. Draw Vector Overlays (Reticle, HUD & Radar)
    if (!inds.empty()) {
        const int num_inds = static_cast<int>(std::min(inds.size(), kMaxIBufBytes / sizeof(std::uint32_t)));
        sg_apply_pipeline(app.pip);
        sg_apply_bindings(app.bind);
        sg_draw(0, num_inds, 1);
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
        vb_desc.size = 4 * 1024 * 1024; // 4MB stream buffer
        vb_desc.usage.stream_update = true;
        app.vbuf = sg_make_buffer(vb_desc);
    }
    {
        sg_buffer_desc ib_desc{};
        ib_desc.size = 4 * 1024 * 1024; // 4MB index buffer
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

    if (!app.in_startup_modal) {
        step_celestial_simulation(app, DT);
    }

    if (app.terminal_mode) {
        if (app.frame % 3 == 0) render_terminal_ascii(app);

        // Keep Metal swapchain valid if window exists
        sg_pass pass{};
        pass.action = app.pass_action;
        pass.swapchain = sglue_swapchain();
        sg_begin_pass(pass);
        sg_end_pass();
        sg_commit();
    } else {
        render_gpu_frame(app);
    }
}

static void event_cb(const sapp_event* e) {
    auto& app = g_app;
    if (e->type == SAPP_EVENTTYPE_MOUSE_MOVE) {
        app.mouse_x = e->mouse_x;
        app.mouse_y = e->mouse_y;
        if (app.slingshot.active) {
            app.slingshot.current_x = e->mouse_x;
            app.slingshot.current_y = e->mouse_y;
        }
        if (e->scroll_y > 0.1f) {
            app.selected_mat_index = (app.selected_mat_index % 4) + 1;
        } else if (e->scroll_y < -0.1f) {
            app.selected_mat_index = (app.selected_mat_index == 1) ? 4 : app.selected_mat_index - 1;
        }
    } else if (e->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
        if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && !app.gravity_vortex && !app.heat_ray && !app.freeze_ray) {
            app.slingshot.active = true;
            app.slingshot.start_x = e->mouse_x;
            app.slingshot.start_y = e->mouse_y;
            app.slingshot.current_x = e->mouse_x;
            app.slingshot.current_y = e->mouse_y;
        } else if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
            spawn_dust_particle(app, true, e->mouse_x, e->mouse_y);
        } else {
            app.mouse_down = true;
        }
    } else if (e->type == SAPP_EVENTTYPE_MOUSE_UP) {
        if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && app.slingshot.active) {
            app.slingshot.active = false;
            const float dx = app.slingshot.start_x - e->mouse_x;
            const float dy = app.slingshot.start_y - e->mouse_y;

            // Launch new custom celestial body configured with the selected material type
            PlanetBody p;
            p.ent = app.world.spawn();
            p.pos = pebble::math::vec2{app.slingshot.start_x, app.slingshot.start_y};
            p.prev_pos = p.pos;
            p.vel = pebble::math::vec2{dx * 0.45f, dy * 0.45f};

            if (app.selected_mat_index == 1) {
                p.type = CelestialType::IceCrust;
                p.mat_params = prakriti::celestial::ice_crust();
                p.mass = 80.0f;
                p.temperature = -70.0f;
            } else if (app.selected_mat_index == 2) {
                p.type = CelestialType::SilicateRock;
                p.mat_params = prakriti::celestial::silicate_rock();
                p.mass = 140.0f;
                p.temperature = 40.0f;
            } else if (app.selected_mat_index == 3) {
                p.type = CelestialType::IronCore;
                p.mat_params = prakriti::celestial::iron_nickel_core();
                p.mass = 240.0f;
                p.temperature = 220.0f;
            } else {
                p.type = CelestialType::MoltenMagma;
                p.mat_params = prakriti::celestial::molten_magma();
                p.mass = 450.0f;
                p.temperature = 1800.0f;
            }

            p.density = p.mat_params.rest_density;
            constexpr float kRadiusScale = 2.4f;
            p.radius = std::clamp(kRadiusScale * std::sqrt(p.mass / p.density), 1.5f, 3.8f);
            p.angular_momentum = 0.5f * p.mass * (p.radius * p.radius) * 4.0f;
            p.omega = 4.0f;
            app.planets.push_back(p);
        }
        app.mouse_down = false;
    } else if (e->type == SAPP_EVENTTYPE_KEY_DOWN) {
        if (app.in_startup_modal) {
            switch (e->key_code) {
                case SAPP_KEYCODE_UP:
                    app.config_selected_row = (app.config_selected_row == 0) ? 3 : app.config_selected_row - 1;
                    break;
                case SAPP_KEYCODE_DOWN:
                    app.config_selected_row = (app.config_selected_row + 1) % 4;
                    break;
                case SAPP_KEYCODE_LEFT:
                    if (app.config_selected_row == 0) {
                        app.config_initial_dust_count = std::max(100, app.config_initial_dust_count - 100);
                    } else if (app.config_selected_row == 1) {
                        app.config_grav_g = std::max(4000.0f, app.config_grav_g - 2000.0f);
                    } else if (app.config_selected_row == 2) {
                        app.config_dist_mode = (app.config_dist_mode == 0) ? 2 : app.config_dist_mode - 1;
                    } else if (app.config_selected_row == 3) {
                        app.config_bh_theta = std::max(0.3f, app.config_bh_theta - 0.05f);
                    }
                    break;
                case SAPP_KEYCODE_RIGHT:
                    if (app.config_selected_row == 0) {
                        app.config_initial_dust_count = std::min(1500, app.config_initial_dust_count + 100);
                    } else if (app.config_selected_row == 1) {
                        app.config_grav_g = std::min(45000.0f, app.config_grav_g + 2000.0f);
                    } else if (app.config_selected_row == 2) {
                        app.config_dist_mode = (app.config_dist_mode + 1) % 3;
                    } else if (app.config_selected_row == 3) {
                        app.config_bh_theta = std::min(0.85f, app.config_bh_theta + 0.05f);
                    }
                    break;
                case SAPP_KEYCODE_ENTER:
                case SAPP_KEYCODE_KP_ENTER:
                case SAPP_KEYCODE_SPACE: {
                    // Apply parameters and launch pure autonomous physical simulation
                    app.in_startup_modal = false;
                    app.gravity_policy.G = app.config_grav_g;
                    app.gravity_policy.theta = app.config_bh_theta;

                    app.planets.clear();
                    app.sparks.clear();
                    app.nebulae.clear();
                    app.jets.clear();

                    for (int i = 0; i < app.config_initial_dust_count; ++i) {
                        spawn_dust_particle(app);
                    }
                    break;
                }
                default:
                    break;
            }
            return;
        }

        switch (e->key_code) {
            case SAPP_KEYCODE_1: app.selected_mat_index = 1; break; // Ice Crust
            case SAPP_KEYCODE_2: app.selected_mat_index = 2; break; // Silicate Rock
            case SAPP_KEYCODE_3: app.selected_mat_index = 3; break; // Iron Core
            case SAPP_KEYCODE_4: app.selected_mat_index = 4; break; // Molten Magma
            case SAPP_KEYCODE_LEFT_BRACKET:
            case SAPP_KEYCODE_MINUS:
                app.time_dilation = std::max(0.1f, app.time_dilation * 0.75f);
                break;
            case SAPP_KEYCODE_RIGHT_BRACKET:
            case SAPP_KEYCODE_EQUAL:
                app.time_dilation = std::min(4.0f, app.time_dilation * 1.35f);
                break;
            case SAPP_KEYCODE_V:
                if (app.view_mode == SpectralViewMode::OpticalRGB) {
                    app.view_mode = SpectralViewMode::ThermalInfrared;
                } else if (app.view_mode == SpectralViewMode::ThermalInfrared) {
                    app.view_mode = SpectralViewMode::RadioXRay;
                } else {
                    app.view_mode = SpectralViewMode::OpticalRGB;
                }
                break;
            case SAPP_KEYCODE_SPACE:
                app.paused = !app.paused;
                break;
            case SAPP_KEYCODE_R:
                app.planets.clear();
                app.sparks.clear();
                app.nebulae.clear();
                app.jets.clear();
                app.gw_ripples.clear();
                app.flares.clear();
                for (int i = 0; i < app.config_initial_dust_count; ++i) spawn_dust_particle(app);
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
        std::cout << "\033[?1049l\033[?25h\033[0m\n" << std::flush;
    }
    sg_shutdown();
}

// ----------------------------------------------------------------------------
// Pure CLI Terminal Loop (Headless / No Window)
// ----------------------------------------------------------------------------
static volatile sig_atomic_t g_cli_running = 1;

static void cli_sigint_handler(int) {
    g_cli_running = 0;
}

static void run_pure_cli_loop() {
    auto& app = g_app;
    app.terminal_mode = true;
    std::signal(SIGINT, cli_sigint_handler);

    // Initialize in-situ cosmic dust field
    for (int i = 0; i < 650; ++i) spawn_dust_particle(app);

    std::cout << "\033[?1049h\033[?25l" << std::flush;

    while (g_cli_running) {
        app.frame++;
        app.time += DT;
        step_celestial_simulation(app, DT);
        render_terminal_ascii(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS terminal refresh
    }

    // Clean exit: restore terminal buffer and cursor
    std::cout << "\033[?1049l\033[?25h\033[0m\n" << std::flush;
    std::exit(0);
}

// ----------------------------------------------------------------------------
// Entry Point
// ----------------------------------------------------------------------------
sapp_desc sokol_main(const int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--terminal") == 0 || std::strcmp(argv[i], "--cli") == 0) {
            run_pure_cli_loop(); // Runs purely in CLI terminal without creating any window!
            return sapp_desc{};
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

