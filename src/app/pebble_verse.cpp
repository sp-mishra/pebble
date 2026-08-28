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
#define BARNES_HUT_HAS_PRAVAHA 1

#include "pravaha/pravaha.hpp"
#include "containers/spatial/barnes_hut.hpp"
#include "containers/spatial/spatial_hash_grid.hpp"
#include "containers/dynamic/soa_vector.hpp"
#include "gati/stepper/block_stepper.hpp"
#include "containers/numeric/math_vector.hpp"
#include "prakriti/material/celestial.hpp"
#include "prakriti/celestial/sector_types.hpp"
#include "prakriti/celestial/sector_generator.hpp"
#include "prakriti/celestial/sector_multipole.hpp"
#include "prakriti/celestial/sector_cache_manager.hpp"
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
static constexpr float GRAVITATIONAL_G   = 18000.0f; // Vigorous mutual N-body attraction
static constexpr float PLUMMER_SOFTENING = 3.5f;     // Tight softening allows strong close-range pull
static constexpr float MAX_GRAV_FORCE    = 35000.0f; // Maximum acceleration clamp
static constexpr float MAX_SPEED_CAP     = 95.0f;    // Maximum dynamic orbital speed (px/s)
static constexpr float SIM_SPEED_FACTOR  = 0.85f;    // Dynamic observable physical time scaling
static constexpr int   INITIAL_DUST_COUNT = 650;     // Number of in-situ dust particles

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
    StrangeQuarkStar,
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
    bool                is_strange_star = false; // Deconfined Quark-Gluon Plasma state
    float               atmosphere_mass = 0.0f;  // Volatile gaseous envelope
    float               ocean_fraction = 0.0f;   // Liquid surface water coverage (0.0 to 1.0)
    float               crust_solid = 1.0f;      // Solid lithosphere plate coverage

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

    std::vector<PlanetBody>                     planets;
    std::vector<SparkParticle>                  sparks;
    std::vector<NebulaGasParticle>              nebulae;
    std::vector<RelativisticJetParticle>        jets;
    std::vector<GravitationalWaveRipple>        gw_ripples;
    std::vector<AccretionFlare>                 flares;
    std::vector<prakriti::celestial::SedovTaylorBlast> snr_blasts;
    std::vector<prakriti::celestial::MHDFluxTube>      mhd_tubes;
    pebble::spandana::ScreenShake2D             camera_shake;
    SlingshotLauncher                           slingshot;

    // Initial Simulation Startup Config Modal State
    bool in_startup_modal = true; // Displays startup config screen until ENTER is pressed
    int  config_selected_row = 0; // 0=Dust Count, 1=Gravitational G, 2=Initial Distribution, 3=Barnes-Hut Theta, 4=Show Overlays
    int  config_initial_dust_count = 650; // Range: 150 to 1500
    float config_grav_g = 18000.0f;       // Range: 5000 to 45000
    int  config_dist_mode = 0;            // 0=Uniform Cosmic Field, 1=Barycentric Cluster, 2=Dual Infall Cloud
    float config_bh_theta = 0.5f;         // 0.3 to 0.8
    bool show_analytics_overlays = false; // Background auxiliary overlays toggle (Grid, H-R, Jacobi, MHD)

    // Multi-Tier Sector Streaming & Out-of-Core Caching
    prakriti::celestial::SectorCacheManager sector_manager{128};
    prakriti::celestial::SectorKey          current_sector{0, 0};
    std::unordered_set<std::uint64_t>       visited_sectors;
    std::uint64_t                           cosmic_seed = 13371337ULL;

    // Multi-Spectral View & Cosmic Nucleosynthesis
    SpectralViewMode view_mode = SpectralViewMode::OpticalRGB;
    float cosmic_metallicity_z = 0.02f; // Current universe heavy element fraction (0.02 to 0.45)
    std::size_t supernova_count = 0;

    // Spawner & External Inflow Parameters
    float spawn_timer = 0.0f;
    float spawn_interval = 0.22f;      // Smooth periodic entry
    float inflow_timer = 0.0f;         // External galaxy/star/comet inflow timer
    float inflow_interval = 4.5f;      // Next cosmic entity ingress (seconds)
    std::size_t inflow_count = 0;      // Total external entities arrived
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

    // Dynamic Lagrangian Tracking & Open-World Camera
    int                tracked_planet_index = -1; // -1 = Free / Manual, >=0 = Target Body Index
    pebble::math::vec2 camera_pos{FW * 0.5f, FH * 0.5f};
    pebble::math::vec2 target_cam_pos{FW * 0.5f, FH * 0.5f};
    float              camera_zoom = 1.0f;
    float              target_zoom = 1.0f;
    bool               middle_mouse_down = false;
    bool               right_mouse_down = false;
    pebble::math::vec2 last_mouse_pos{0.0f, 0.0f};

    // Open Universe Radar Scale Mode (1x = Viewport, 2x = Neighborhood, 4x = Deep Cosmos)
    int radar_zoom_level = 1; // 0=1x, 1=2.5x, 2=5.0x cosmic radar reach

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

    // Check for Strange Quark Star (Deconfined Quark Matter)
    if (p.is_strange_star || p.type == CelestialType::StrangeQuarkStar) {
        base = kalpana::Color{0.75f, 0.15f, 1.0f, 1.0f}; // Deep Luminescent Violet
    } else if (p.temperature > 1200.0f || p.mass >= 140.0f) {
        // Check for Morgan-Keenan (MK) Stellar Spectral Classification for hot stellar cores
        const auto mk = prakriti::celestial::evaluate_stellar_spectral_class(p.mass, p.temperature);
        base = kalpana::Color{mk.r, mk.g, mk.b, 1.0f};
    } else {
        // High-luminance, high-contrast primary celestial spectral colors for terrestrial planetoids
        if (p.ocean_fraction > 0.3f) {
            // Terraformed Habitable Planet with Surface Oceans
            base = kalpana::Color{0.15f, 0.55f, 0.95f, 1.0f}; // Earth-like Deep Azure Ocean
        } else if (p.density < 1500.0f) {
            base = kalpana::Color{0.25f, 0.95f, 1.00f, 1.0f}; // Ultra-Bright Cyan / Ice Blue
        } else if (p.density < 4500.0f) {
            base = kalpana::Color{1.00f, 0.90f, 0.25f, 1.0f}; // Radiant Solar Gold / Yellow (Silicate Rock)
        } else if (p.density < 10000.0f) {
            base = kalpana::Color{1.00f, 0.60f, 0.15f, 1.0f}; // Intense Metallic Orange (Iron-Nickel)
        } else {
            base = kalpana::Color{0.95f, 0.30f, 1.00f, 1.0f}; // Vivid Hyperdense Singularity Magenta
        }
    }

    // Thermal incandescence shift for cooler bodies
    if (p.temperature < -20.0f) {
        const float t = std::clamp((-p.temperature) / 100.0f, 0.0f, 1.0f);
        base = kalpana::Color{
            base.r * (1.0f - t * 0.4f),
            base.g * (1.0f - t * 0.1f) + 0.3f * t,
            std::min(1.0f, base.b + 0.4f * t),
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
        // True Full-Grid Isotropic Matter Nucleation across all coordinates [12, FW - 12] x [12, FH - 12]
        p.pos = pebble::math::vec2{
            12.0f + dist01(app.rng) * (FW - 24.0f),
            12.0f + dist01(app.rng) * (FH - 24.0f)
        };
    }

    // Organic Primordial Kinematics:
    // Cold Jeans turbulence & thermal velocity dispersion (Maxwell-Boltzmann like)
    // Smooth velocity perturbation prevents artificial radial collapse and allows natural angular momentum development
    const float therm_speed = 1.2f + dist01(app.rng) * 2.8f;
    const float therm_angle = dist01(app.rng) * 6.2831853f;
    p.vel = pebble::math::vec2{std::cos(therm_angle), std::sin(therm_angle)} * therm_speed;
    p.prev_pos = p.pos;

    // Random micro-spin / angular velocity (radians/sec)
    p.angle = dist01(app.rng) * 6.2831853f;
    p.omega = (dist01(app.rng) - 0.5f) * 4.0f;

    // Randomize celestial material class influenced organically by current Universe Metallicity (Z)
    const float roll = dist01(app.rng);
    const float z_bias = app.cosmic_metallicity_z;
    if (roll < std::max(0.10f, 0.40f - z_bias * 0.4f)) {
        p.type = CelestialType::IceCrust;
        p.mat_params = prakriti::celestial::ice_crust();
        p.mass = 8.0f + dist01(app.rng) * 6.0f;
        p.temperature = -60.0f + dist01(app.rng) * 40.0f;
    } else if (roll < std::max(0.45f, 0.80f - z_bias * 0.2f)) {
        p.type = CelestialType::SilicateRock;
        p.mat_params = prakriti::celestial::silicate_rock();
        p.mass = 12.0f + dist01(app.rng) * 10.0f;
        p.temperature = 20.0f + dist01(app.rng) * 60.0f;
    } else if (roll < 0.96f) {
        p.type = CelestialType::IronCore;
        p.mat_params = prakriti::celestial::iron_nickel_core();
        p.mass = 20.0f + dist01(app.rng) * 15.0f;
        p.temperature = 80.0f + dist01(app.rng) * 100.0f;
    } else {
        p.type = CelestialType::MoltenMagma;
        p.mat_params = prakriti::celestial::molten_magma();
        p.mass = 35.0f + dist01(app.rng) * 25.0f;
        p.temperature = 1100.0f + dist01(app.rng) * 300.0f;
    }

    p.density = p.mat_params.rest_density;
    // Micro dust spec starts tiny (0.8px to 1.3px)
    p.radius = std::clamp(std::pow(p.mass, 0.333f) * 0.42f, 0.75f, 1.4f);

    // Initial angular momentum: L = I * omega = (0.5 * m * r^2) * omega
    const float moment_of_inertia = 0.5f * p.mass * (p.radius * p.radius);
    p.angular_momentum = moment_of_inertia * p.omega;

    app.planets.push_back(p);
}

// ----------------------------------------------------------------------------
// External Inflow Spawner: Form galaxies, stars, comets outside and inject into space
// ----------------------------------------------------------------------------
static void spawn_external_inflow(PebbleVerseApp& app) {
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    
    // Choose entity type
    const float roll = dist01(app.rng);
    prakriti::celestial::InflowEntityType type;
    if (roll < 0.35f) {
        type = prakriti::celestial::InflowEntityType::RogueProtogalaxy;
    } else if (roll < 0.65f) {
        type = prakriti::celestial::InflowEntityType::InterstellarComet;
    } else if (roll < 0.88f) {
        type = prakriti::celestial::InflowEntityType::HypervelocityStar;
    } else {
        type = prakriti::celestial::InflowEntityType::RoguePulsarMagnetar;
    }

    const float rand_angle = dist01(app.rng) * 6.2831853f;
    const float speed_factor = 0.5f + dist01(app.rng) * 0.8f;
    const auto cfg = prakriti::celestial::generate_random_inflow(FW, FH, 280.0f, rand_angle, speed_factor, type);

    app.inflow_count++;

    // 1. Core / Primary Body
    PlanetBody core;
    core.ent = app.world.spawn();
    core.pos = cfg.spawn_pos;
    core.prev_pos = core.pos;
    core.vel = cfg.ingress_vel;
    core.mass = cfg.core_mass;

    if (type == prakriti::celestial::InflowEntityType::RoguePulsarMagnetar) {
        core.is_neutron_star = true;
        core.type = CelestialType::NeutronStar;
        core.mat_params = prakriti::celestial::neutron_star();
        core.density = core.mat_params.rest_density;
        core.radius = 2.6f;
        core.temperature = 6000.0f;
        core.omega = 45.0f;
    } else if (type == prakriti::celestial::InflowEntityType::HypervelocityStar) {
        core.type = CelestialType::SuperheatedPlasma;
        core.mat_params = prakriti::celestial::superheated_plasma();
        core.density = core.mat_params.rest_density;
        core.radius = std::clamp(std::cbrt(core.mass) * 0.75f, 3.5f, 9.0f);
        core.temperature = 2800.0f + dist01(app.rng) * 2200.0f;
        core.omega = (dist01(app.rng) - 0.5f) * 6.0f;
    } else if (type == prakriti::celestial::InflowEntityType::InterstellarComet) {
        core.type = CelestialType::IceCrust;
        core.mat_params = prakriti::celestial::ice_crust();
        core.density = core.mat_params.rest_density;
        core.radius = std::clamp(std::cbrt(core.mass) * 0.6f, 1.4f, 2.8f);
        core.temperature = -120.0f + dist01(app.rng) * 40.0f; // Cryogenic deep space ice
        core.omega = (dist01(app.rng) - 0.5f) * 8.0f;
    } else { // RogueProtogalaxy
        core.type = CelestialType::MoltenMagma;
        core.mat_params = prakriti::celestial::molten_magma();
        core.density = core.mat_params.rest_density;
        core.radius = std::clamp(std::cbrt(core.mass) * 0.7f, 4.0f, 11.0f);
        core.temperature = 1400.0f + dist01(app.rng) * 800.0f;
        core.omega = 2.5f;
    }

    core.angular_momentum = 0.5f * core.mass * (core.radius * core.radius) * core.omega;
    app.planets.push_back(core);

    // 2. Swarm of Bound Satellite Moons / Orbiting Stars / Comet Shards
    for (int k = 0; k < cfg.satellite_count; ++k) {
        PlanetBody sat;
        sat.ent = app.world.spawn();
        
        const float sat_angle = dist01(app.rng) * 6.2831853f;
        const float sat_r = (type == prakriti::celestial::InflowEntityType::InterstellarComet)
            ? (6.0f + dist01(app.rng) * 16.0f)
            : (18.0f + dist01(app.rng) * 75.0f);

        sat.pos = core.pos + pebble::math::vec2{std::cos(sat_angle), std::sin(sat_angle)} * sat_r;
        sat.prev_pos = sat.pos;

        // Circular orbital velocity around core: v = \sqrt{G * M_core / r}
        const float v_circ = std::sqrt(std::max(1.0f, (app.gravity_policy.G * core.mass) / std::max(sat_r, 10.0f))) * 0.75f;
        const pebble::math::vec2 sat_tangent{-std::sin(sat_angle), std::cos(sat_angle)};
        sat.vel = core.vel + sat_tangent * v_circ + pebble::math::vec2{dist01(app.rng) - 0.5f, dist01(app.rng) - 0.5f} * 2.0f;

        if (type == prakriti::celestial::InflowEntityType::InterstellarComet) {
            sat.type = CelestialType::IceCrust;
            sat.mat_params = prakriti::celestial::ice_crust();
            sat.mass = 4.0f + dist01(app.rng) * 8.0f;
            sat.temperature = -130.0f + dist01(app.rng) * 30.0f;
        } else {
            const float m_roll = dist01(app.rng);
            if (m_roll < 0.4f) {
                sat.type = CelestialType::SilicateRock;
                sat.mat_params = prakriti::celestial::silicate_rock();
                sat.mass = 12.0f + dist01(app.rng) * 18.0f;
                sat.temperature = 20.0f + dist01(app.rng) * 60.0f;
            } else if (m_roll < 0.75f) {
                sat.type = CelestialType::IronCore;
                sat.mat_params = prakriti::celestial::iron_nickel_core();
                sat.mass = 18.0f + dist01(app.rng) * 25.0f;
                sat.temperature = 100.0f + dist01(app.rng) * 120.0f;
            } else {
                sat.type = CelestialType::IceCrust;
                sat.mat_params = prakriti::celestial::ice_crust();
                sat.mass = 8.0f + dist01(app.rng) * 10.0f;
                sat.temperature = -60.0f + dist01(app.rng) * 40.0f;
            }
        }

        sat.density = sat.mat_params.rest_density;
        sat.radius = std::clamp(std::cbrt(sat.mass) * 0.55f, 0.85f, 2.2f);
        sat.omega = (dist01(app.rng) - 0.5f) * 5.0f;
        sat.angular_momentum = 0.5f * sat.mass * (sat.radius * sat.radius) * sat.omega;

        app.planets.push_back(sat);
    }
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

    // 1. Dynamic Open-World Spatial Sector Discovery & Freezing (Strict Viewport Window Only)
    // Active simulation window is strictly the visible screen view:
    const float active_min_x = app.camera_pos[0] - FW * 0.5f;
    const float active_max_x = app.camera_pos[0] + FW * 0.5f;
    const float active_min_y = app.camera_pos[1] - FH * 0.5f;
    const float active_max_y = app.camera_pos[1] + FH * 0.5f;

    // Identify all spatial tiles (320x200 px) covering the active viewport
    const std::int32_t min_tile_x = static_cast<std::int32_t>(std::floor(active_min_x / prakriti::celestial::kSectorWidth));
    const std::int32_t max_tile_x = static_cast<std::int32_t>(std::floor(active_max_x / prakriti::celestial::kSectorWidth));
    const std::int32_t min_tile_y = static_cast<std::int32_t>(std::floor(active_min_y / prakriti::celestial::kSectorHeight));
    const std::int32_t max_tile_y = static_cast<std::int32_t>(std::floor(active_max_y / prakriti::celestial::kSectorHeight));

    // A. Freezing: Freeze any bodies that have slid outside the active simulation window into their sector tiles
    std::unordered_map<std::uint64_t, prakriti::celestial::SectorData> freezing_sectors;
    std::vector<PlanetBody> remaining_active_planets;
    remaining_active_planets.reserve(app.planets.size());

    for (const auto& p : app.planets) {
        if (!p.alive) continue;
        const bool inside_active = (p.pos[0] >= active_min_x && p.pos[0] <= active_max_x &&
                                    p.pos[1] >= active_min_y && p.pos[1] <= active_max_y);
        if (inside_active) {
            remaining_active_planets.push_back(p);
        } else {
            // Compress into dormant sector record
            const std::int32_t tx = static_cast<std::int32_t>(std::floor(p.pos[0] / prakriti::celestial::kSectorWidth));
            const std::int32_t ty = static_cast<std::int32_t>(std::floor(p.pos[1] / prakriti::celestial::kSectorHeight));
            const prakriti::celestial::SectorKey sk{tx, ty};
            const std::uint64_t hid = prakriti::celestial::hash_sector_key(sk);

            if (!freezing_sectors.contains(hid)) {
                freezing_sectors[hid] = app.sector_manager.get_or_generate_sector(sk, app.cosmic_seed);
            }
            auto& sec = freezing_sectors[hid];
            sec.key = sk;
            prakriti::celestial::CompactBodyRecord b;
            b.x = p.pos[0]; b.y = p.pos[1];
            b.vx = p.vel[0]; b.vy = p.vel[1];
            b.mass = p.mass; b.radius = p.radius;
            b.temperature = p.temperature; b.omega = p.omega;
            b.type = static_cast<std::uint8_t>(p.type);
            sec.bodies.push_back(b);
        }
    }
    app.planets = std::move(remaining_active_planets);

    // Save frozen sectors into dormant macro nodes
    for (auto& [hid, sec] : freezing_sectors) {
        sec.total_mass = 0.0f;
        sec.barycenter_x = 0.0f;
        sec.barycenter_y = 0.0f;
        sec.quadrupole = prakriti::celestial::SectorQuadrupole{};

        for (const auto& b : sec.bodies) {
            sec.total_mass += b.mass;
            sec.barycenter_x += b.mass * b.x;
            sec.barycenter_y += b.mass * b.y;
        }

        if (sec.total_mass > 0.0f) {
            sec.barycenter_x /= sec.total_mass;
            sec.barycenter_y /= sec.total_mass;
            for (const auto& b : sec.bodies) {
                const float rx = b.x - sec.barycenter_x;
                const float ry = b.y - sec.barycenter_y;
                const float r2 = rx * rx + ry * ry;
                sec.quadrupole.qxx += b.mass * (3.0f * rx * rx - r2);
                sec.quadrupole.qxy += b.mass * (3.0f * rx * ry);
                sec.quadrupole.qyy += b.mass * (3.0f * ry * ry - r2);
            }
        }
        app.sector_manager.freeze_sector(sec);
    }

    // B. Discovery & Awakening: Populate newly uncovered tiles or wake up returning tiles
    for (std::int32_t tx = min_tile_x; tx <= max_tile_x; ++tx) {
        for (std::int32_t ty = min_tile_y; ty <= max_tile_y; ++ty) {
            const prakriti::celestial::SectorKey sk{tx, ty};
            const std::uint64_t hid = prakriti::celestial::hash_sector_key(sk);

            if (!app.visited_sectors.contains(hid)) {
                // Brand new undiscovered space: seed primordial dust field across this 320x200 tile
                app.visited_sectors.insert(hid);
                const float base_x = static_cast<float>(tx) * prakriti::celestial::kSectorWidth;
                const float base_y = static_cast<float>(ty) * prakriti::celestial::kSectorHeight;

                // Density scaled per 320x200 tile (matches ~650 specs per 1280x800 screen)
                const int tile_count = std::max(20, app.config_initial_dust_count / 16);
                for (int s = 0; s < tile_count; ++s) {
                    const float px = base_x + 8.0f + dist01(app.rng) * (prakriti::celestial::kSectorWidth - 16.0f);
                    const float py = base_y + 8.0f + dist01(app.rng) * (prakriti::celestial::kSectorHeight - 16.0f);
                    spawn_dust_particle(app, true, px, py);
                }

                auto sec_data = app.sector_manager.get_or_generate_sector(sk, app.cosmic_seed);
                sec_data.discovery_tick = static_cast<std::uint64_t>(app.frame + 1);
                app.sector_manager.mark_sector_active(sk);
            } else if (app.sector_manager.dormant_macro_nodes().contains(hid)) {
                // Re-entering previously visited tile: wake up dormant frozen bodies!
                auto sec_data = app.sector_manager.get_or_generate_sector(sk, app.cosmic_seed);
                app.sector_manager.mark_sector_active(sk);

                for (const auto& b : sec_data.bodies) {
                    PlanetBody p;
                    p.ent = app.world.spawn();
                    p.pos = pebble::math::vec2{b.x, b.y};
                    p.prev_pos = p.pos;
                    p.vel = pebble::math::vec2{b.vx, b.vy};
                    p.mass = b.mass;
                    p.temperature = b.temperature;
                    p.omega = b.omega;
                    p.type = static_cast<CelestialType>(b.type);
                    if (p.type == CelestialType::IceCrust) {
                        p.mat_params = prakriti::celestial::ice_crust();
                    } else if (p.type == CelestialType::SilicateRock) {
                        p.mat_params = prakriti::celestial::silicate_rock();
                    } else if (p.type == CelestialType::IronCore) {
                        p.mat_params = prakriti::celestial::iron_nickel_core();
                    } else {
                        p.mat_params = prakriti::celestial::molten_magma();
                    }
                    p.density = p.mat_params.rest_density;
                    p.radius = std::clamp(std::pow(p.mass, 0.333f) * 0.45f, 0.85f, 2.8f);
                    p.angular_momentum = 0.5f * p.mass * (p.radius * p.radius) * p.omega;
                    app.planets.push_back(p);
                }
                // Cleared from dormant storage now that bodies are live in app.planets
                sec_data.bodies.clear();
                sec_data.total_mass = 0.0f;
                app.sector_manager.freeze_sector(sec_data);
            }
        }
    }

    // 2. Prepare Barnes-Hut input bodies array (Strictly only live active bodies in viewport)
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

    // 3. Build QuadTree & Compute N-Body Gravitational Forces for Active Window
    app.bh_tree.build(bh_bodies);
    std::vector<pebble::math::vec2> forces(bh_bodies.size());
    containers::spatial::compute_all_forces(app.bh_tree, bh_bodies, forces, app.gravity_policy);

    // Cache hot fusion stars once for fast O(K) stellar wind evaluations
    std::vector<const PlanetBody*> active_fusion_stars;
    for (const auto& p : app.planets) {
        if (p.alive && p.temperature >= 1500.0f) {
            active_fusion_stars.push_back(&p);
        }
    }

    // Map forces back and apply collective gravity from all dormant macro nodes
    std::size_t bh_idx = 0;
    for (std::size_t i = 0; i < app.planets.size(); ++i) {
        if (!app.planets[i].alive) continue;
        const pebble::math::vec2 f_grav = forces[bh_idx++];
        app.planets[i].acc = f_grav * (1.0f / app.planets[i].mass);

        // Ultra-Fast Collective Gravitational Pull from all Dormant Out-of-View Sectors
        for (const auto& [node_hid, macro_node] : app.sector_manager.dormant_macro_nodes()) {
            const pebble::math::vec2 a_coll = prakriti::celestial::compute_collective_macro_gravity(
                app.planets[i].pos, macro_node, app.config_grav_g
            );
            app.planets[i].acc = app.planets[i].acc + a_coll;
        }

        // Stellar Wind & Radiation Pressure (Evaluated only against genuine fusion stars)
        for (const auto* star : active_fusion_stars) {
            if (star == &app.planets[i]) continue;
            const auto wind = prakriti::celestial::evaluate_stellar_wind_radiation_pressure(
                star->pos, star->mass, star->temperature, star->radius, app.planets[i].pos, dist01(app.rng)
            );
            app.planets[i].acc = app.planets[i].acc + wind.force * (1.0f / app.planets[i].mass);

            // Coronal Mass Ejection (CME) Solar Flare Loop Eruption
            if (wind.triggers_cme && app.sparks.size() < 400) {
                const float flare_a = dist01(app.rng) * 6.2831853f;
                for (int s = 0; s < 8; ++s) {
                    const float arc = flare_a + (static_cast<float>(s) - 4.0f) * 0.12f;
                    const float flare_spd = 55.0f + dist01(app.rng) * 45.0f;
                    app.sparks.push_back(SparkParticle{
                        .pos = star->pos + pebble::math::vec2{std::cos(arc), std::sin(arc)} * (star->radius + 1.0f),
                        .vel = star->vel + pebble::math::vec2{std::cos(arc), std::sin(arc)} * flare_spd,
                        .radius = 1.4f + dist01(app.rng) * 1.0f,
                        .life = 0.45f + dist01(app.rng) * 0.25f,
                        .max_life = 0.70f,
                        .color = kalpana::Color{1.0f, 0.55f, 0.1f, 1.0f} // Brilliant solar flare orange
                    });
                }
            }
        }

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

    // 4. Symplectic Velocity Verlet Integration, Cosmological Metric Expansion & Radiative Cooling
    const pebble::math::vec2 cosmic_center{FW * 0.5f, FH * 0.5f};
    constexpr float kHubbleConstant = 0.00035f; // Gentle cosmic background metric expansion

    for (std::size_t i = 0; i < app.planets.size(); ++i) {
        auto& p = app.planets[i];
        if (!p.alive) continue;

        // ── 2.5PN Gravitational Radiation Reaction Drag between Binary Black Holes ──
        if (p.is_singularity || p.type == CelestialType::BlackHoleSingularity) {
            for (std::size_t j = i + 1; j < app.planets.size(); ++j) {
                auto& other = app.planets[j];
                if (!other.alive || (!other.is_singularity && other.type != CelestialType::BlackHoleSingularity)) continue;
                const pebble::math::vec2 d = other.pos - p.pos;
                const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1]);
                const float r_isco = (p.radius + other.radius) * 1.5f;
                const auto smbh = prakriti::celestial::evaluate_smbh_inspiral(p.mass, other.mass, dist, r_isco);
                if (smbh.is_in_inspiral) {
                    const pebble::math::vec2 drag_dir = pebble::math::normalize(d);
                    // Gravitational radiation reaction accelerates inspiral decay
                    p.vel = p.vel + drag_dir * (smbh.post_newtonian_drag * sim_dt * 0.5f);
                    other.vel = other.vel - drag_dir * (smbh.post_newtonian_drag * sim_dt * 0.5f);
                }
            }
        }

        // ── Roche Lobe Overflow (RLOF) & Tidal Spin-Orbit Locking ──
        if (p.mass > 40.0f) {
            for (std::size_t j = 0; j < app.planets.size(); ++j) {
                if (i == j) continue;
                auto& host = app.planets[j];
                if (!host.alive || host.mass <= p.mass * 1.5f) continue;

                const pebble::math::vec2 d = host.pos - p.pos;
                const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1]);
                if (dist > 180.0f || dist <= p.radius + host.radius) continue;

                // 1. Roche Lobe Overflow Mass Siphoning
                const auto rlof = prakriti::celestial::evaluate_roche_lobe_overflow(p.mass, p.radius, host.mass, dist, sim_dt);
                if (rlof.is_overflowing && p.mass > 5.0f) {
                    p.mass -= rlof.mass_transfer_rate;
                    host.mass += rlof.mass_transfer_rate * 0.85f; // Mass accretion efficiency

                    // Emit subtle siphoned accretion stream particles
                    if (app.sparks.size() < 400 && dist01(app.rng) < 0.35f) {
                        const pebble::math::vec2 stream_dir = pebble::math::normalize(d);
                        app.sparks.push_back(SparkParticle{
                            .pos = p.pos + stream_dir * (p.radius + 1.0f),
                            .vel = host.vel + stream_dir * 30.0f,
                            .radius = 1.0f,
                            .life = 0.25f,
                            .max_life = 0.25f,
                            .color = kalpana::Color{1.0f, 0.75f, 0.25f, 0.75f}
                        });
                    }
                }

                // 2. Tidal Dissipation & Spin-Orbit Locking
                const pebble::math::vec2 dv = p.vel - host.vel;
                const float v_rel = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1]);
                const auto tidal = prakriti::celestial::evaluate_tidal_locking_torque(
                    p.mass, p.radius, p.omega, host.mass, dist, v_rel, sim_dt
                );
                p.omega += tidal.spin_torque;
            }
        }

        // Apply gentle Cosmological Hubble Metric Drift (v_H = H0 * (r - r_center))
        const pebble::math::vec2 hubble_drift = prakriti::celestial::apply_hubble_metric_drift(
            p.pos, cosmic_center, kHubbleConstant, sim_dt
        );

        p.prev_pos = p.pos;
        p.pos = p.pos + p.vel * sim_dt + p.acc * (0.5f * sim_dt * sim_dt) + hubble_drift;
        p.vel = p.vel + p.acc * sim_dt;

        // Angular spin integration
        p.angle += p.omega * sim_dt;

        // ── Contact Binary Dumbbell Coalescence & Inward Orbital Spiral ──
        // Secondary lobe rotates with angular velocity around primary core and slowly sinks inward
        if (p.is_merging) {
            // Hotter/molten bodies relax faster into a circle; colder bodies hold dumbbell shape longer
            const float viscosity_rate = (p.temperature > 900.0f) ? 0.65f : 0.30f;
            p.merge_progress += (sim_dt / p.merge_duration) * viscosity_rate;
            
            // Orbit the secondary lobe around the core while pulling it in
            const float d_theta = p.omega * sim_dt;
            const float cos_t = std::cos(d_theta);
            const float sin_t = std::sin(d_theta);
            const pebble::math::vec2 rotated_offset{
                p.lobe2_offset[0] * cos_t - p.lobe2_offset[1] * sin_t,
                p.lobe2_offset[0] * sin_t + p.lobe2_offset[1] * cos_t
            };

            if (p.merge_progress >= 1.0f) {
                p.merge_progress = 1.0f;
                p.is_merging = false;
                p.lobe2_offset = pebble::math::vec2{0.0f, 0.0f};
                p.lobe2_radius = 0.0f;
                p.lobe2_mass = 0.0f;
            } else {
                // Inward orbital decay towards center of mass
                p.lobe2_offset = rotated_offset * (1.0f - sim_dt * 1.8f * viscosity_rate);
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
        if (p.is_singularity || p.type == CelestialType::BlackHoleSingularity) {
            // Hawking Radiation Quantum Mass Loss: dM/dt \propto -1 / M^2
            const auto hawk = prakriti::celestial::evaluate_hawking_radiation(p.mass, sim_dt);
            p.mass = std::max(0.0f, p.mass - hawk.mass_loss_rate);
            p.temperature = hawk.hawking_temp;

            if (hawk.triggers_gamma_flash || p.mass <= 10.0f) {
                // Final High-Energy Quantum Gamma-Ray Flash & Complete Evaporation
                p.alive = false;
                for (int s = 0; s < 32; ++s) {
                    const float a = static_cast<float>(s) * (6.2831853f / 32.0f);
                    const float spd = 120.0f + dist01(app.rng) * 90.0f;
                    app.sparks.push_back(SparkParticle{
                        .pos = p.pos,
                        .vel = p.vel + pebble::math::vec2{std::cos(a), std::sin(a)} * spd,
                        .radius = 1.6f + dist01(app.rng) * 1.2f,
                        .life = 0.5f + dist01(app.rng) * 0.3f,
                        .max_life = 0.8f,
                        .color = kalpana::Color{0.4f, 0.95f, 1.0f, 1.0f} // Brilliant Hawking gamma flash
                    });
                }
                app.camera_shake.add_trauma(0.35f);
            }
        } else {
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

                // ── Supernova Remnant (SNR) Sedov-Taylor Blast Wave ──
                app.snr_blasts.push_back(prakriti::celestial::SedovTaylorBlast{
                    .center = p.pos,
                    .radius = 2.0f,
                    .max_radius = 480.0f,
                    .energy = p.mass * 25.0f,
                    .age = 0.0f,
                    .max_age = 3.2f,
                    .density_compression = 4.0f
                });

                // ── Spectacular Core-Collapse Supernova Blast (Triple-Shell Relativistic Ejecta) ──
                // Shell 1: Hyper-velocity relativistic plasma blast
                for (int s = 0; s < 64; ++s) {
                    const float a = static_cast<float>(s) * (6.2831853f / 64.0f) + (dist01(app.rng) - 0.5f) * 0.1f;
                    const float spd = 90.0f + dist01(app.rng) * 140.0f;
                    app.sparks.push_back(SparkParticle{
                        .pos = p.pos,
                        .vel = p.vel + pebble::math::vec2{std::cos(a), std::sin(a)} * spd,
                        .radius = 2.4f + dist01(app.rng) * 2.2f,
                        .life = 1.2f + dist01(app.rng) * 0.6f,
                        .max_life = 1.8f,
                        .color = (s % 2 == 0) ? kalpana::Color{1.0f, 0.95f, 0.5f, 1.0f} : kalpana::Color{0.4f, 0.85f, 1.0f, 1.0f}
                    });
                }
                // Shell 2: Dense expanding fireball nebula cloud
                for (int s = 0; s < 32; ++s) {
                    const float a = dist01(app.rng) * 6.2831853f;
                    const float spd = 20.0f + dist01(app.rng) * 45.0f;
                    app.nebulae.push_back(NebulaGasParticle{
                        .pos = p.pos,
                        .vel = p.vel * 0.3f + pebble::math::vec2{std::cos(a), std::sin(a)} * spd,
                        .radius = 3.5f + dist01(app.rng) * 3.0f,
                        .life = 1.6f + dist01(app.rng) * 0.8f,
                        .max_life = 2.4f,
                        .color = kalpana::Color{1.0f, 0.35f, 0.1f, 0.65f}
                    });
                }
                app.camera_shake.add_trauma(0.85f); // Visceral cosmic camera recoil
            } else if (evol.phase == prakriti::celestial::StellarPhase::NeutronStar && !p.is_neutron_star) {
                p.is_neutron_star = true;
                p.type = CelestialType::NeutronStar;
                p.mat_params = prakriti::celestial::neutron_star();
                p.density = p.mat_params.rest_density;
                p.radius = evol.event_horizon_radius;
                p.temperature = 5500.0f;
                
                // ── Pulsar Conservation of Angular Momentum ($I_1 \omega_1 = I_2 \omega_2$) ──
                // As the star compresses from $R \approx 12\text{px}$ down to $2.2\text{px}$, $\omega \propto (R_1/R_2)^2 \approx 30\times$ spinup!
                const float prev_r = std::max(p.radius, 4.0f);
                const float spin_factor = (prev_r * prev_r) / (evol.event_horizon_radius * evol.event_horizon_radius);
                p.omega = std::clamp(p.omega * spin_factor * 1.5f + 35.0f, 35.0f, 75.0f); // High-speed millisecond pulsar spin (rad/s)

                // ── Radiant Pulsar Magnetar Core-Collapse Flash ──
                for (int s = 0; s < 36; ++s) {
                    const float a = static_cast<float>(s) * (6.2831853f / 36.0f);
                    const float spd = 50.0f + dist01(app.rng) * 80.0f;
                    app.sparks.push_back(SparkParticle{
                        .pos = p.pos,
                        .vel = p.vel + pebble::math::vec2{std::cos(a), std::sin(a)} * spd,
                        .radius = 1.8f + dist01(app.rng) * 1.6f,
                        .life = 0.8f + dist01(app.rng) * 0.4f,
                        .max_life = 1.2f,
                        .color = kalpana::Color{0.3f, 0.95f, 1.0f, 1.0f} // Electric cyan magnetar sparks
                    });
                }
                app.camera_shake.add_trauma(0.40f);
            } else if (evol.phase == prakriti::celestial::StellarPhase::MainSequenceStar) {
                p.type = CelestialType::SuperheatedPlasma;
            }
        }

        // ── Pulsar / Kerr Black Hole Lense-Thirring Jet Precession & Relativistic Beams ──
        if (p.is_neutron_star && app.jets.size() < 300 && dist01(app.rng) < 0.65f) {
            // Lense-Thirring Precession: The spin axis precesses around the angular momentum vector
            const float prec_rate = 0.85f; // Frame-dragging precession angular frequency
            const float prec_wobble = std::sin(app.time * prec_rate + p.mass * 0.05f) * 0.22f; // Precession cone angle
            const float b_angle = p.angle + prec_wobble;

            const pebble::math::vec2 beam_dir1{std::cos(b_angle), std::sin(b_angle)};
            const pebble::math::vec2 beam_dir2{-std::cos(b_angle), -std::sin(b_angle)};
            const float b_spd = 140.0f + dist01(app.rng) * 60.0f;

            // Relativistic Doppler Beaming: Jet approaching viewer is blue-boosted, receding jet is dimmed
            const float doppler_blue = 1.0f + std::max(0.0f, beam_dir1[0]) * 0.35f;
            const float doppler_red  = 1.0f - std::max(0.0f, beam_dir2[0]) * 0.25f;

            app.jets.push_back(RelativisticJetParticle{
                .pos = p.pos + beam_dir1 * (p.radius + 1.0f),
                .vel = p.vel + beam_dir1 * b_spd,
                .radius = 1.2f * doppler_blue,
                .life = 0.35f,
                .max_life = 0.35f,
                .color = kalpana::Color{0.3f * doppler_blue, 0.95f * doppler_blue, 1.0f, 0.85f} // Doppler Blue-Shifted Jet
            });
            app.jets.push_back(RelativisticJetParticle{
                .pos = p.pos + beam_dir2 * (p.radius + 1.0f),
                .vel = p.vel + beam_dir2 * b_spd,
                .radius = 1.0f * doppler_red,
                .life = 0.35f,
                .max_life = 0.35f,
                .color = kalpana::Color{0.5f * doppler_red, 0.75f * doppler_red, 0.95f, 0.60f}  // Doppler Red-Shifted Jet
            });
        }

        // ── SPH Gaseous Planet / Star Roche Lobe Stripping & Fluid Stream Dynamics ──
        if (p.mass > 5.0f && !p.is_singularity && app.nebulae.size() < 450) {
            for (const auto& other : app.planets) {
                if (!other.alive || &other == &p || other.mass < p.mass * 2.0f) continue;
                const pebble::math::vec2 d = other.pos - p.pos;
                const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1]);
                if (dist > 1.0f && dist < 120.0f) {
                    const auto sph_strip = prakriti::celestial::evaluate_sph_roche_lobe_stripping(
                        p.pos, p.mass, p.radius, other.pos, other.mass, dist
                    );
                    if (sph_strip.is_stripping && dist01(app.rng) < 0.35f) {
                        const pebble::math::vec2 dir_to_host = (other.pos - p.pos) * (1.0f / dist);
                        const pebble::math::vec2 tangent{-dir_to_host[1], dir_to_host[0]};
                        const pebble::math::vec2 stream_vel = dir_to_host * sph_strip.stream_velocity + tangent * (p.omega * 0.5f);

                        // SPH Fluid Siphoned Gas Particle
                        app.nebulae.push_back(NebulaGasParticle{
                            .pos = sph_strip.l1_lagrange_point,
                            .vel = p.vel * 0.3f + stream_vel,
                            .radius = 1.2f + dist01(app.rng) * 0.8f,
                            .life = 1.2f,
                            .max_life = 1.2f,
                            .color = kalpana::Color{0.45f, 0.85f, 1.0f, 0.40f} // SPH Siphoned accretion stream
                        });
                        break;
                    }
                }
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

        // Strange Quark Star Phase Transition (Witten Strange Matter Hypothesis)
        if (p.is_neutron_star && !p.is_strange_star) {
            const auto strange_trans = prakriti::celestial::evaluate_strange_star_transition(p.mass, p.is_neutron_star, 220.0f);
            if (strange_trans.triggers_strange_star) {
                p.is_strange_star = true;
                p.type = CelestialType::StrangeQuarkStar;
                p.mat_params = prakriti::celestial::strange_quark_star();
                p.density = p.mat_params.rest_density;
                p.radius = strange_trans.strange_radius;
                p.temperature = 9500.0f;

                // Deconfined Quark Matter Flash
                for (int s = 0; s < 28; ++s) {
                    const float a = static_cast<float>(s) * (6.2831853f / 28.0f);
                    const float spd = 60.0f + dist01(app.rng) * 70.0f;
                    app.sparks.push_back(SparkParticle{
                        .pos = p.pos,
                        .vel = p.vel + pebble::math::vec2{std::cos(a), std::sin(a)} * spd,
                        .radius = 2.0f,
                        .life = 0.6f,
                        .max_life = 0.6f,
                        .color = kalpana::Color{0.75f, 0.15f, 1.0f, 1.0f} // Violet QGP sparks
                    });
                }
            }
        }

        // ── Planetary Atmosphere Thermodynamics, Jeans Escape & Hydrology ──
        if (p.mass >= 20.0f && p.mass <= 350.0f && !p.is_singularity && !p.is_neutron_star && !p.is_strange_star) {
            // Find nearest stellar wind source
            float wind_flux = 0.0f;
            for (const auto& star : app.planets) {
                if (!star.alive || (&star == &p) || star.temperature < 1200.0f) continue;
                const pebble::math::vec2 d = p.pos - star.pos;
                const float dist2 = d[0] * d[0] + d[1] * d[1] + 100.0f;
                wind_flux += (star.radius * star.radius * 20.0f) / dist2;
            }

            // 1. Jeans Thermal Escape & Atmosphere Retention
            const auto escape = prakriti::celestial::evaluate_jeans_atmospheric_escape(
                p.mass, p.radius, p.temperature, wind_flux, sim_dt
            );
            if (escape.retains_atmosphere) {
                p.atmosphere_mass = std::min(p.atmosphere_mass + 0.05f * sim_dt, p.mass * 0.08f);
            } else {
                p.atmosphere_mass = std::max(0.0f, p.atmosphere_mass - (escape.jeans_loss_rate + escape.wind_stripping_rate));
            }

            // 2. Surface Hydrology & Ocean Condensation
            const float water_volatile_fraction = (p.type == CelestialType::IceCrust) ? 0.8f : (p.atmosphere_mass / std::max(p.mass, 1.0f));
            const auto hydro = prakriti::celestial::evaluate_surface_hydrology_phase(
                p.temperature, water_volatile_fraction, p.mass
            );
            p.ocean_fraction = hydro.ocean_coverage;
            p.crust_solid = hydro.crust_solidification;
        }

        // Recompute organic physical radius based on mass and material class:
        // Pure dust is ~0.8-1.2px, accumulating asteroids ~2-3px, large planets ~4-6px, stars and giants ~8-16px!
        if (p.is_singularity || p.type == CelestialType::BlackHoleSingularity) {
            p.radius = std::clamp(p.mass * 0.0035f + 2.8f, 3.0f, 12.0f);
        } else if (p.is_strange_star || p.type == CelestialType::StrangeQuarkStar) {
            p.radius = std::clamp(std::cbrt(p.mass) * 0.45f, 1.8f, 3.2f); // Ultra-dense Quark star
        } else if (p.is_neutron_star || p.type == CelestialType::NeutronStar) {
            p.radius = std::clamp(std::cbrt(p.mass) * 0.55f, 2.2f, 4.5f); // Compact degenerate core
        } else {
            // General planetary & stellar volume scaling (r \propto (M/\rho)^{1/3})
            p.radius = std::clamp(std::cbrt(p.mass) * 0.65f, 0.75f, 18.0f);
        }

        // ── Cometary Sublimation Tail Outgassing (When icy bodies approach hot stars) ──
        if (p.type == CelestialType::IceCrust && app.nebulae.size() < 500) {
            for (const auto& star : app.planets) {
                if (!star.alive || (&star == &p) || star.temperature < 1200.0f) continue;
                const auto comet_sub = prakriti::celestial::evaluate_comet_tail_sublimation(
                    p.pos, p.temperature, star.pos, star.temperature, star.radius, sim_dt
                );
                if (comet_sub.is_sublimating && dist01(app.rng) < 0.35f) {
                    p.mass = std::max(0.2f, p.mass - comet_sub.mass_loss_rate);
                    const float tail_spd = comet_sub.tail_velocity_mag + (dist01(app.rng) - 0.5f) * 15.0f;
                    const float spread = (dist01(app.rng) - 0.5f) * 0.25f;
                    const pebble::math::vec2 t_dir = pebble::math::normalize(
                        comet_sub.tail_direction + pebble::math::vec2{-comet_sub.tail_direction[1], comet_sub.tail_direction[0]} * spread
                    );

                    app.nebulae.push_back(NebulaGasParticle{
                        .pos = p.pos + t_dir * (p.radius + 1.0f),
                        .vel = p.vel * 0.35f + t_dir * tail_spd,
                        .radius = 1.2f + dist01(app.rng) * 0.8f,
                        .life = 0.65f + dist01(app.rng) * 0.45f,
                        .max_life = 1.1f,
                        .color = comet_sub.is_ion_plasma
                            ? kalpana::Color{0.3f, 0.9f, 1.0f, 0.65f}   // Radiant Cyan Ion Plasma Tail (Type I)
                            : kalpana::Color{0.9f, 0.85f, 0.7f, 0.45f}  // Diffuse Golden Dust Tail (Type II)
                    });
                    break;
                }
            }
        }

        // ── Open World Boundless Universe: Recycle bodies that escape far beyond active camera horizon ──
        const auto cull = prakriti::celestial::evaluate_open_world_bounds(p.pos, app.camera_pos, 10000.0f, 8000.0f);
        if (cull.should_recycle) {
            p.alive = false; // Graceful cosmological recycling beyond active horizon
        }
    }

    // 5. Collision Broadphase & Narrowphase via O(N) SpatialHashGrid
    containers::spatial::SpatialHashGrid<std::uint32_t, 36.0f, 2048> spatial_grid(app.planets.size());
    const std::size_t num_planets = app.planets.size();

    // Populate O(N) spatial grid
    for (std::size_t i = 0; i < num_planets; ++i) {
        if (app.planets[i].alive) {
            spatial_grid.insert(static_cast<std::uint32_t>(i), app.planets[i].pos[0], app.planets[i].pos[1]);
        }
    }

    for (std::size_t i = 0; i < num_planets; ++i) {
        if (!app.planets[i].alive) continue;
        const float ri = app.planets[i].radius;
        const float xi = app.planets[i].pos[0];
        const float yi = app.planets[i].pos[1];

        // O(1) local 3x3 neighborhood search via SpatialHashGrid
        spatial_grid.for_each_neighbor(xi, yi, [&](std::uint32_t neighbor_idx, float nx, float ny) {
            const std::size_t j = static_cast<std::size_t>(neighbor_idx);
            if (j <= i || !app.planets[j].alive) return;

            const float min_dist = ri + app.planets[j].radius;
            const float dx = nx - xi;
            if (std::abs(dx) > min_dist) return;
            const float dy = ny - yi;
            if (std::abs(dy) > min_dist) return;

            const float dist2 = dx * dx + dy * dy;

            if (dist2 < min_dist * min_dist && dist2 > 1e-4f) {
                app.collisions_count++;
                const pebble::math::vec2 dr{dx, dy};

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

                    // Update spin rate and organic radius for merged body: omega = L / I
                    constexpr float kRadiusScale = 0.65f;
                    app.planets[i].radius = std::max(0.6f, kRadiusScale * std::cbrt(m_total * 3000.0f / d_mixed));
                    const float new_inertia = 0.5f * m_total * (app.planets[i].radius * app.planets[i].radius);
                    app.planets[i].angular_momentum = l_total;
                    app.planets[i].omega = (new_inertia > 1e-4f) ? (l_total / new_inertia) : 0.0f;

                    // ── Kilonova Explosion, r-Process Synthesis & Gamma-Ray Burst (GRB) ──
                    const auto kn = prakriti::celestial::evaluate_kilonova_merger(
                        app.planets[i].mass, app.planets[j].mass,
                        app.planets[i].is_neutron_star, app.planets[j].is_neutron_star,
                        app.planets[i].is_singularity, app.planets[j].is_singularity
                    );

                    if (kn.triggers_kilonova) {
                        // Cosmic metallicity jump from r-process gold/platinum nucleosynthesis
                        app.cosmic_metallicity_z = std::min(0.55f, app.cosmic_metallicity_z + kn.gold_platinum_yield);
                        app.supernova_count++;

                        // Expanding radioactive gold & violet kilonova nebula debris
                        for (int k_idx = 0; k_idx < 40; ++k_idx) {
                            const float a = dist01(app.rng) * 6.2831853f;
                            const float spd = 35.0f + dist01(app.rng) * 65.0f;
                            app.nebulae.push_back(NebulaGasParticle{
                                .pos = p_cm,
                                .vel = v_cm * 0.2f + pebble::math::vec2{std::cos(a), std::sin(a)} * spd,
                                .radius = 3.0f + dist01(app.rng) * 2.5f,
                                .life = 2.0f + dist01(app.rng) * 1.0f,
                                .max_life = 3.0f,
                                .color = (k_idx % 2 == 0) ? kalpana::Color{1.0f, 0.85f, 0.2f, 0.75f} : kalpana::Color{0.7f, 0.3f, 1.0f, 0.70f}
                            });
                        }

                        // Collimated Relativistic Gamma-Ray Burst (GRB) twin jets
                        const float grb_angle = app.planets[i].angle + 1.5707963f;
                        const pebble::math::vec2 grb1{std::cos(grb_angle), std::sin(grb_angle)};
                        const pebble::math::vec2 grb2{-std::cos(grb_angle), -std::sin(grb_angle)};
                        for (int g = 0; g < 20; ++g) {
                            const float g_spd = 220.0f + dist01(app.rng) * 140.0f;
                            app.jets.push_back(RelativisticJetParticle{
                                .pos = p_cm + grb1 * (app.planets[i].radius + 2.0f),
                                .vel = v_cm + grb1 * g_spd,
                                .radius = 2.2f,
                                .life = 0.65f,
                                .max_life = 0.65f,
                                .color = kalpana::Color{1.0f, 0.95f, 0.4f, 1.0f} // Blinding white-gold GRB beam
                            });
                            app.jets.push_back(RelativisticJetParticle{
                                .pos = p_cm + grb2 * (app.planets[i].radius + 2.0f),
                                .vel = v_cm + grb2 * g_spd,
                                .radius = 2.2f,
                                .life = 0.65f,
                                .max_life = 0.65f,
                                .color = kalpana::Color{1.0f, 0.95f, 0.4f, 1.0f}
                            });
                        }
                        app.camera_shake.add_trauma(0.70f);

                        if (kn.forms_black_hole) {
                            app.planets[i].is_singularity = true;
                            app.planets[i].is_neutron_star = false;
                            app.planets[i].type = CelestialType::BlackHoleSingularity;
                            app.planets[i].mat_params = prakriti::celestial::black_hole_singularity();
                            app.planets[i].density = app.planets[i].mat_params.rest_density;
                        }
                    }

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
        });
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

    // 11. Update Supernova Remnant (SNR) Sedov-Taylor Blast Waves & Secondary Protostar Compression
    for (auto& snr : app.snr_blasts) {
        snr.age += dt;
        snr.radius = prakriti::celestial::compute_sedov_taylor_radius(snr.energy, 1.0f, snr.age);

        // Compress passing interstellar dust at the shock front -> triggers secondary protostellar nucleation
        for (auto& dust : app.planets) {
            if (!dust.alive || dust.mass > 40.0f) continue;
            const pebble::math::vec2 d = dust.pos - snr.center;
            const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1]);
            if (std::abs(dist - snr.radius) < 14.0f && dist > 1.0f) {
                // Radial shock compression force
                const pebble::math::vec2 s_norm = d * (1.0f / dist);
                dust.vel = dust.vel + s_norm * (25.0f * (1.0f - snr.age / snr.max_age));
                dust.temperature += 180.0f * dt;
                dust.density = std::min(dust.density * 1.002f, 12000.0f);
            }
        }
    }
    std::erase_if(app.snr_blasts, [](const prakriti::celestial::SedovTaylorBlast& snr) { return snr.age >= snr.max_age; });

    // 12. Evaluate Active Magnetohydrodynamic (MHD) Magnetic Flux Tubes between Binary Stars/Pulsars
    app.mhd_tubes.clear();
    for (std::size_t i = 0; i < app.planets.size(); ++i) {
        const auto& p1 = app.planets[i];
        if (!p1.alive || p1.mass < 140.0f) continue;
        for (std::size_t j = i + 1; j < app.planets.size(); ++j) {
            const auto& p2 = app.planets[j];
            if (!p2.alive || p2.mass < 140.0f) continue;
            const auto tube = prakriti::celestial::compute_mhd_flux_tube(p1.pos, p1.mass, p1.omega, p2.pos, p2.mass, p2.omega);
            if (tube.field_strength > 0.05f) {
                app.mhd_tubes.push_back(tube);
            }
        }
    }

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

    // ── 0. Dynamic Lagrangian Tracking & Open-World Camera Update ──
    if (app.tracked_planet_index >= 0 && app.tracked_planet_index < static_cast<int>(app.planets.size())) {
        const auto& tp = app.planets[app.tracked_planet_index];
        if (tp.alive) {
            app.target_cam_pos = tp.pos;
            app.target_zoom = std::clamp(2.4f - (tp.radius / 10.0f), 0.6f, 3.5f);
        } else {
            app.tracked_planet_index = -1; // Reset if target merged or evaporated
        }
    }

    // Smooth exponential damping interpolation
    constexpr float cam_lerp = 0.10f;
    app.camera_pos = app.camera_pos + (app.target_cam_pos - app.camera_pos) * cam_lerp;
    app.camera_zoom = app.camera_zoom + (app.target_zoom - app.camera_zoom) * cam_lerp;

    // Find top massive gravitational lenses for real-time Einstein micro-lensing
    std::vector<const PlanetBody*> massive_lenses;
    for (const auto& p : app.planets) {
        if (p.alive && (p.is_singularity || p.mass > 300.0f)) {
            massive_lenses.push_back(&p);
            if (massive_lenses.size() >= 4) break;
        }
    }

    auto w2s = [&](const pebble::math::vec2& w_pos, bool is_lens = false) -> pebble::math::vec2 {
        (void)is_lens;
        return pebble::math::vec2{
            (w_pos[0] - app.camera_pos[0]) * app.camera_zoom + FW * 0.5f,
            (w_pos[1] - app.camera_pos[1]) * app.camera_zoom + FH * 0.5f
        };
    };

    // ── 0. Sub-Layer: Spacetime Curvature & Geodesic Grid (Faint, High-Res Background Layer) ──
    if (app.show_analytics_overlays) {
        constexpr int grid_cols = 64;
        constexpr int grid_rows = 40;
        const float dx = FW / static_cast<float>(grid_cols);
        const float dy = FH / static_cast<float>(grid_rows);

        auto warp_vertex = [&](float gx, float gy) -> pebble::math::vec2 {
            pebble::math::vec2 pt{gx, gy};
            for (const auto* lens : massive_lenses) {
                const auto defl = prakriti::celestial::compute_spacetime_geodesic_deflection(
                    pt, lens->pos, lens->mass, 18.0f
                );
                pt = pt + defl;
            }
            return w2s(pt, true);
        };

        // Horizontal geodesic lines (Ultra-faint, sparse spacing)
        for (int r = 0; r <= grid_rows; r += 2) {
            const float gy = static_cast<float>(r) * dy;
            kalpana::Path h_line;
            const pebble::math::vec2 start_pt = warp_vertex(0.0f, gy);
            h_line.move_to(start_pt[0], start_pt[1]);
            for (int c = 1; c <= grid_cols; ++c) {
                const float gx = static_cast<float>(c) * dx;
                const pebble::math::vec2 p_w = warp_vertex(gx, gy);
                h_line.line_to(p_w[0], p_w[1]);
            }
            scene.add(kalpana::Node::shape(h_line, kalpana::Paint::stroke(kalpana::Color{0.10f, 0.22f, 0.45f, 0.035f}, 0.5f)));
        }

        // Vertical geodesic lines (Ultra-faint, sparse spacing)
        for (int c = 0; c <= grid_cols; c += 2) {
            const float gx = static_cast<float>(c) * dx;
            kalpana::Path v_line;
            const pebble::math::vec2 start_pt = warp_vertex(gx, 0.0f);
            v_line.move_to(start_pt[0], start_pt[1]);
            for (int r = 1; r <= grid_rows; ++r) {
                const float gy = static_cast<float>(r) * dy;
                const pebble::math::vec2 p_w = warp_vertex(gx, gy);
                v_line.line_to(p_w[0], p_w[1]);
            }
            scene.add(kalpana::Node::shape(v_line, kalpana::Paint::stroke(kalpana::Color{0.10f, 0.22f, 0.45f, 0.035f}, 0.5f)));
        }
    }

    // 1. Batch Orbital Motion Trails
    for (const auto& p : app.planets) {
        if (!p.alive) continue;

        // Render subtle forward orbit trajectory ONLY for the actively tracked Lagrangian target
        if (app.tracked_planet_index >= 0 && &p == &app.planets[app.tracked_planet_index] && p.mass < 250.0f) {
            for (const auto* host : massive_lenses) {
                if (host != &p && host->mass > p.mass * 3.0f) {
                    const auto orbit_pts = prakriti::celestial::compute_osculating_orbit_points(
                        p.pos, p.vel, host->pos, host->mass, 24, 0.06f
                    );
                    if (!orbit_pts.empty()) {
                        kalpana::Path orbit_path;
                        const pebble::math::vec2 s_start = w2s(orbit_pts[0]);
                        orbit_path.move_to(s_start[0], s_start[1]);
                        for (std::size_t pt_i = 1; pt_i < orbit_pts.size(); ++pt_i) {
                            const pebble::math::vec2 s_pt = w2s(orbit_pts[pt_i]);
                            orbit_path.line_to(s_pt[0], s_pt[1]);
                        }
                        scene.add(kalpana::Node::shape(orbit_path, kalpana::Paint::stroke(kalpana::Color{0.3f, 0.85f, 1.0f, 0.20f}, 0.8f)));
                    }
                }
            }
        }

        if (p.trail_count > 1) {
            const kalpana::Color col = get_celestial_color(p, app.view_mode);
            for (int t = 0; t < p.trail_count; ++t) {
                const int idx = (p.trail_head - 1 - t + PlanetBody::kMaxTrail) % PlanetBody::kMaxTrail;
                const float fade = 1.0f - static_cast<float>(t) / static_cast<float>(p.trail_count);
                kalpana::Color t_col = col;
                t_col.a = fade * 0.40f;
                const pebble::math::vec2 s_trail = w2s(p.trail_history[idx]);
                app.instanced_planets.add_instance(s_trail[0], s_trail[1], p.radius * fade * 0.85f * app.camera_zoom, t_col);
            }
        }
        // Celestial Body Core & Contact Binary Lobe (Dumbbell shape)
        const kalpana::Color c = get_celestial_color(p, app.view_mode);
        const pebble::math::vec2 s_pos = w2s(p.pos);

        // ── Relativistic Gravitational Lensing & Photon Sphere Halo Shader Simulation ──
        if (p.is_singularity || p.type == CelestialType::BlackHoleSingularity || p.is_neutron_star) {
            // 1. Photon Sphere & Outer Einstein Lensing Ring ($r_{\text{photon}} = 1.5 R_s$, $r_{\text{Einstein}} \propto \sqrt{M}$) - Ultra-Subtle & Faint
            const float r_einstein = (p.radius * 1.5f + std::sqrt(p.mass) * 0.45f) * app.camera_zoom;
            kalpana::Color einstein_col = p.is_singularity 
                ? kalpana::Color{0.40f, 0.70f, 1.0f, 0.05f} 
                : kalpana::Color{0.25f, 0.85f, 1.0f, 0.07f};
            app.instanced_planets.add_instance(s_pos[0], s_pos[1], r_einstein, einstein_col);

            // 2. Innermost Stable Circular Orbit (ISCO) Plasma Glow - Delicate & Thin
            const float r_isco = (p.radius * 1.25f) * app.camera_zoom;
            app.instanced_planets.add_instance(s_pos[0], s_pos[1], r_isco, kalpana::Color{1.0f, 0.60f, 0.20f, 0.10f});
        }

        app.instanced_planets.add_instance(s_pos[0], s_pos[1], p.radius * app.camera_zoom, c);

        // If in gradual coalescence, render the rotating dumbbell lobes and smooth bridging neck
        if (p.is_merging && p.lobe2_radius > 0.3f) {
            const pebble::math::vec2 lobe_pos = w2s(p.pos + p.lobe2_offset);
            const float lobe_r = p.lobe2_radius * (1.0f - p.merge_progress * 0.45f) * app.camera_zoom;
            app.instanced_planets.add_instance(lobe_pos[0], lobe_pos[1], lobe_r, c);

            // Connecting fluid neck specks (Roche Lobe matter bridge)
            for (float f : {0.33f, 0.66f}) {
                const pebble::math::vec2 neck_pos = w2s(p.pos + p.lobe2_offset * f);
                const float neck_r = (p.radius * (1.0f - f) + lobe_r * f) * 0.75f * (1.0f - p.merge_progress * 0.3f);
                app.instanced_planets.add_instance(neck_pos[0], neck_pos[1], neck_r, c);
            }
        }
    }

    // 2. Batch Evaporated Nebula Gas Clouds
    for (const auto& n : app.nebulae) {
        const pebble::math::vec2 s_pos = w2s(n.pos);
        app.instanced_planets.add_instance(s_pos[0], s_pos[1], n.radius * app.camera_zoom, n.color);
    }

    // 3. Batch Fire Sparks
    for (const auto& s : app.sparks) {
        const pebble::math::vec2 s_pos = w2s(s.pos);
        app.instanced_planets.add_instance(s_pos[0], s_pos[1], s.radius * app.camera_zoom, s.color);
    }

    // 4. Batch Relativistic Polar Matter Jets
    for (const auto& j : app.jets) {
        const pebble::math::vec2 s_pos = w2s(j.pos);
        app.instanced_planets.add_instance(s_pos[0], s_pos[1], j.radius * app.camera_zoom, j.color);
    }

    // 5. Batch Relativistic ISCO Accretion Flares with Doppler Asymmetry
    for (const auto& f : app.flares) {
        const pebble::math::vec2 s_pos = w2s(f.pos);
        // Doppler boost based on tangential orbital position relative to camera center
        const float dx_rel = f.pos[0] - app.camera_pos[0];
        const float doppler_flare = 1.0f + std::clamp(dx_rel * 0.015f, -0.3f, 0.4f);
        kalpana::Color f_col = f.color;
        f_col.r = std::clamp(f_col.r * (2.0f - doppler_flare), 0.0f, 1.0f);
        f_col.b = std::clamp(f_col.b * doppler_flare, 0.0f, 1.0f);
        f_col.a = std::clamp(f_col.a * (0.7f + doppler_flare * 0.3f), 0.0f, 1.0f);
        app.instanced_planets.add_instance(s_pos[0], s_pos[1], f.radius * app.camera_zoom, f_col);
    }

    // 6. Relativistic Gravitational Wave Space-time Ripples (Ultra-Subtle & Faint)
    for (const auto& gw : app.gw_ripples) {
        const float alpha = std::clamp(gw.life / gw.max_life, 0.0f, 1.0f) * 0.08f;
        const pebble::math::vec2 s_center = w2s(gw.center);
        kalpana::Path gw_ring;
        gw_ring.circle(s_center[0], s_center[1], gw.radius * app.camera_zoom);
        scene.add(kalpana::Node::shape(gw_ring, kalpana::Paint::stroke(kalpana::Color{0.35f, 0.7f, 1.0f, alpha}, 0.6f)));
    }

    // 6b. Supernova Remnant (SNR) Sedov-Taylor Blast Shockwaves (Ultra-faint expanding compression fronts)
    for (const auto& snr : app.snr_blasts) {
        const float alpha = std::clamp(1.0f - (snr.age / snr.max_age), 0.0f, 1.0f) * 0.08f;
        const pebble::math::vec2 s_center = w2s(snr.center);
        kalpana::Path snr_ring;
        snr_ring.circle(s_center[0], s_center[1], snr.radius * app.camera_zoom);
        scene.add(kalpana::Node::shape(snr_ring, kalpana::Paint::stroke(kalpana::Color{1.0f, 0.45f, 0.15f, alpha}, 0.8f)));
    }

    // 6c. Magnetohydrodynamic (MHD) Magnetic Flux Tubes (Ultra-faint synchrotron flux arcs between binary stars/pulsars)
    if (app.show_analytics_overlays) {
        for (const auto& tube : app.mhd_tubes) {
            const pebble::math::vec2 s1 = w2s(tube.p1);
            const pebble::math::vec2 s2 = w2s(tube.p2);
            const pebble::math::vec2 smid = w2s(tube.midpoint);
            kalpana::Path flux_path;
            flux_path.move_to(s1[0], s1[1]);
            flux_path.quad_to(smid[0], smid[1], s2[0], s2[1]);
            const float alpha = tube.field_strength * 0.04f;
            scene.add(kalpana::Node::shape(flux_path, kalpana::Paint::stroke(kalpana::Color{0.35f, 0.95f, 1.0f, alpha}, 0.6f)));
        }

        // 6e. Planetary Atmospheric Haze & Strange Quark Star Deconfined Gluon Halos (Ultra-faint Sub-layer)
        for (const auto& p : app.planets) {
            if (!p.alive) continue;
            const pebble::math::vec2 s_pos = w2s(p.pos);

            // Planetary Atmosphere Glow
            if (p.atmosphere_mass > 2.0f && p.mass < 400.0f) {
                kalpana::Path atmo_ring;
                atmo_ring.circle(s_pos[0], s_pos[1], (p.radius + 2.5f) * app.camera_zoom);
                const float atmo_alpha = std::clamp(p.atmosphere_mass / (p.mass * 0.08f), 0.02f, 0.08f);
                scene.add(kalpana::Node::shape(atmo_ring, kalpana::Paint::stroke(kalpana::Color{0.3f, 0.75f, 1.0f, atmo_alpha}, 1.0f)));
            }

            // Strange Quark Star Deconfined Gluon Aura
            if (p.is_strange_star || p.type == CelestialType::StrangeQuarkStar) {
                kalpana::Path gluon_aura;
                gluon_aura.circle(s_pos[0], s_pos[1], p.radius * 2.2f * app.camera_zoom);
                scene.add(kalpana::Node::shape(gluon_aura, kalpana::Paint::stroke(kalpana::Color{0.8f, 0.2f, 1.0f, 0.12f}, 1.2f)));
            }

            // Pulsar Timing Array (PTA) GW Modulation Beacon
            if (p.is_neutron_star && !app.gw_ripples.empty()) {
                const float gw_strain = app.gw_ripples[0].amplitude;
                const auto pta = prakriti::celestial::evaluate_pulsar_gw_timing_residual(p.omega, gw_strain, 1.2f, app.time);
                if (std::abs(pta.timing_shift_ns) > 0.01f) {
                    kalpana::Path pta_ray;
                    const pebble::math::vec2 beam_dir{std::cos(p.angle), std::sin(p.angle)};
                    const pebble::math::vec2 ray_end = s_pos + beam_dir * (22.0f * app.camera_zoom);
                    pta_ray.move_to(s_pos[0], s_pos[1]);
                    pta_ray.line_to(ray_end[0], ray_end[1]);
                    scene.add(kalpana::Node::shape(pta_ray, kalpana::Paint::stroke(kalpana::Color{0.4f, 0.95f, 1.0f, 0.08f}, 0.8f)));
                }
            }
        }
    }

    // 7. Relativistic Event Horizons, Photon Rings & ISCO (For Actual Compact Singularities & Stars)
    for (std::size_t idx = 0; idx < app.planets.size(); ++idx) {
        const auto& p = app.planets[idx];
        if (!p.alive) continue;
        const pebble::math::vec2 s_pos = w2s(p.pos, p.is_singularity || p.mass > 300.0f);
        if (p.is_singularity || p.type == CelestialType::BlackHoleSingularity) {
            // ── Binary Black Hole Tidal Distortion & Shared Accretion Bridge ──
            for (std::size_t o_idx = idx + 1; o_idx < app.planets.size(); ++o_idx) {
                const auto& other = app.planets[o_idx];
                if (!other.alive || (!other.is_singularity && other.type != CelestialType::BlackHoleSingularity)) continue;
                const pebble::math::vec2 d = other.pos - p.pos;
                const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1]);
                const float mutual_isco = (p.radius + other.radius) * 3.2f;
                if (dist < mutual_isco) {
                    // Mutual ISCO plasma bridge linking both event horizons
                    const pebble::math::vec2 s_other = w2s(other.pos, true);
                    kalpana::Path isco_bridge;
                    isco_bridge.move_to(s_pos[0], s_pos[1]);
                    isco_bridge.line_to(s_other[0], s_other[1]);
                    const float bridge_thick = std::max(2.0f, (p.radius + other.radius) * 1.2f * app.camera_zoom);
                    scene.add(kalpana::Node::shape(isco_bridge, kalpana::Paint::stroke(kalpana::Color{1.0f, 0.65f, 0.2f, 0.85f}, bridge_thick)));
                    
                    // Shared Common Ergosphere Envelope
                    const pebble::math::vec2 s_mid = (s_pos + s_other) * 0.5f;
                    app.instanced_planets.add_instance(s_mid[0], s_mid[1], dist * 1.1f * app.camera_zoom, kalpana::Color{0.6f, 0.25f, 1.0f, 0.25f});
                }
            }

            // ── Relativistic Kerr Black Hole Anatomy ──
            // Layer 1: Relativistic Ergosphere Frame-Dragging Swirl Aura
            const float ergo_r = p.radius * 4.2f * app.camera_zoom;
            app.instanced_planets.add_instance(s_pos[0], s_pos[1], ergo_r, kalpana::Color{0.5f, 0.2f, 0.95f, 0.16f});

            // Layer 2: Glowing Superheated Accretion Disk (ISCO - Innermost Stable Circular Orbit)
            const float isco_r = p.radius * 2.8f * app.camera_zoom;
            app.instanced_planets.add_instance(s_pos[0], s_pos[1], isco_r, kalpana::Color{1.0f, 0.55f, 0.15f, 0.75f});

            // Layer 3: Razor-Sharp Relativistic Photon Sphere / Einstein Ring
            const float photon_ring_r = p.radius * 1.6f * app.camera_zoom;
            app.instanced_planets.add_instance(s_pos[0], s_pos[1], photon_ring_r, kalpana::Color{1.0f, 0.92f, 0.65f, 0.95f});

            // Layer 4: Absolute Pitch-Black Event Horizon ($R_s = \frac{2GM}{c^2}$) Void Core
            // Renders on top with complete opacity so zero light escapes from within
            app.instanced_planets.add_instance(s_pos[0], s_pos[1], p.radius * app.camera_zoom, kalpana::Color{0.005f, 0.005f, 0.012f, 1.0f});
        } else if (p.is_neutron_star || p.type == CelestialType::NeutronStar) {
            // Radiant Pulsar Magnetosphere Halo
            const float pulsar_halo_r = p.radius * 2.5f * app.camera_zoom;
            app.instanced_planets.add_instance(s_pos[0], s_pos[1], pulsar_halo_r, kalpana::Color{0.3f, 0.9f, 1.0f, 0.25f});

            // High-Speed Pulsar Magnetic Dipole Sweeping Beams
            const pebble::math::vec2 beam_vec{std::cos(p.angle), std::sin(p.angle)};
            kalpana::Path dipole_beam;
            const pebble::math::vec2 b1 = s_pos - beam_vec * (p.radius * 3.6f * app.camera_zoom);
            const pebble::math::vec2 b2 = s_pos + beam_vec * (p.radius * 3.6f * app.camera_zoom);
            dipole_beam.move_to(b1[0], b1[1]);
            dipole_beam.line_to(b2[0], b2[1]);
            scene.add(kalpana::Node::shape(dipole_beam, kalpana::Paint::stroke(kalpana::Color{0.4f, 0.95f, 1.0f, 0.85f}, 1.5f)));
        } else if (p.temperature > 1800.0f && p.mass >= 220.0f) {
            // Incandescent Fusion Star Corona (Only for genuine fusion stars!)
            const float corona_r = p.radius * 1.8f * app.camera_zoom;
            app.instanced_planets.add_instance(s_pos[0], s_pos[1], corona_r, kalpana::Color{1.0f, 0.6f, 0.1f, 0.20f});
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

    // 6. Real-Time Open-World Cosmic Radar Inset (Bottom-Right with Adaptive Scale)
    {
        constexpr float radar_w = 140.0f;
        constexpr float radar_h = 90.0f;
        const float radar_x = FW - radar_w - 12.0f;
        const float radar_y = FH - radar_h - 12.0f;

        // Radar background & frame border
        kalpana::Path r_box;
        r_box.rect(radar_x, radar_y, radar_w, radar_h);
        scene.add(kalpana::Node::shape(r_box, kalpana::Paint::fill(kalpana::Color{0.015f, 0.03f, 0.07f, 0.88f})));
        scene.add(kalpana::Node::shape(r_box, kalpana::Paint::stroke(kalpana::Color{0.2f, 0.55f, 0.9f, 0.75f}, 1.2f)));

        // Center crosshair
        kalpana::Path r_cross;
        const float cx = radar_x + radar_w * 0.5f;
        const float cy = radar_y + radar_h * 0.5f;
        r_cross.move_to(cx - 6.0f, cy); r_cross.line_to(cx + 6.0f, cy);
        r_cross.move_to(cx, cy - 6.0f); r_cross.line_to(cx, cy + 6.0f);
        scene.add(kalpana::Node::shape(r_cross, kalpana::Paint::stroke(kalpana::Color{0.25f, 0.6f, 0.9f, 0.4f}, 0.8f)));

        // Viewport bounds rectangle on radar
        const float radar_world_span = (app.radar_zoom_level == 0) ? (FW * 1.5f) : ((app.radar_zoom_level == 1) ? (FW * 3.5f) : (FW * 7.0f));
        const float vp_rw = (FW / radar_world_span) * radar_w;
        const float vp_rh = (FH / radar_world_span) * radar_h;
        const float vp_rx = cx + ((app.camera_pos[0] - FW * 0.5f) / radar_world_span) * radar_w - vp_rw * 0.5f;
        const float vp_ry = cy + ((app.camera_pos[1] - FH * 0.5f) / radar_world_span) * radar_h - vp_rh * 0.5f;

        kalpana::Path vp_rect;
        vp_rect.rect(vp_rx, vp_ry, vp_rw, vp_rh);
        scene.add(kalpana::Node::shape(vp_rect, kalpana::Paint::stroke(kalpana::Color{0.3f, 0.85f, 1.0f, 0.45f}, 0.8f)));

        // Map all bodies across open universe onto radar coordinates
        for (const auto& p : app.planets) {
            if (!p.alive) continue;
            const float rx = cx + ((p.pos[0] - FW * 0.5f) / radar_world_span) * radar_w;
            const float ry = cy + ((p.pos[1] - FH * 0.5f) / radar_world_span) * radar_h;
            if (rx >= radar_x + 1.0f && rx <= radar_x + radar_w - 1.0f && ry >= radar_y + 1.0f && ry <= radar_y + radar_h - 1.0f) {
                kalpana::Path p_dot;
                const float dot_r = (p.mass > 500.0f) ? 2.2f : ((p.mass > 100.0f) ? 1.5f : 0.85f);
                p_dot.circle(rx, ry, dot_r);
                scene.add(kalpana::Node::shape(p_dot, kalpana::Paint::fill(get_celestial_color(p))));
            }
        }
    }

    // 6e. Real-Time Hertzsprung-Russell (H-R) Diagram Inset (Bottom-Left, Ultra-Faint)
    if (app.show_analytics_overlays) {
        constexpr float hr_w = 110.0f;
        constexpr float hr_h = 75.0f;
        constexpr float hr_x = 12.0f;
        const float hr_y = FH - hr_h - 12.0f;

        // Frame
        kalpana::Path hr_box;
        hr_box.rect(hr_x, hr_y, hr_w, hr_h);
        scene.add(kalpana::Node::shape(hr_box, kalpana::Paint::fill(kalpana::Color{0.015f, 0.03f, 0.06f, 0.65f})));
        scene.add(kalpana::Node::shape(hr_box, kalpana::Paint::stroke(kalpana::Color{0.25f, 0.45f, 0.75f, 0.35f}, 0.8f)));

        // Axis line
        kalpana::Path hr_axes;
        hr_axes.move_to(hr_x + 6.0f, hr_y + 6.0f); hr_axes.line_to(hr_x + 6.0f, hr_y + hr_h - 6.0f);
        hr_axes.line_to(hr_x + hr_w - 6.0f, hr_y + hr_h - 6.0f);
        scene.add(kalpana::Node::shape(hr_axes, kalpana::Paint::stroke(kalpana::Color{0.3f, 0.5f, 0.8f, 0.25f}, 0.6f)));

        // Plot stars on H-R diagram: X = Temperature (reversed: Hot -> Cold), Y = Mass/Luminosity
        for (const auto& p : app.planets) {
            if (!p.alive || p.mass < 60.0f) continue;
            // Temp range [2000 K to 35000 K] mapped to X
            const float t_norm = std::clamp((p.temperature + 273.15f - 2000.0f) / 33000.0f, 0.0f, 1.0f);
            const float px = hr_x + hr_w - 10.0f - t_norm * (hr_w - 20.0f); // Hot on left, cold on right
            // Mass range [60 to 1200] mapped to Y (high mass on top)
            const float m_norm = std::clamp((p.mass - 60.0f) / 1000.0f, 0.0f, 1.0f);
            const float py = hr_y + hr_h - 10.0f - m_norm * (hr_h - 20.0f);

            kalpana::Path star_dot;
            star_dot.circle(px, py, 1.1f);
            kalpana::Color dot_c = get_celestial_color(p);
            dot_c.a = 0.55f; // Keep ultra-faint
            scene.add(kalpana::Node::shape(star_dot, kalpana::Paint::fill(dot_c)));
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

        // Sector Coordinates Display (e.g. SEC:[0, 0])
        const std::string sec_str = "SEC:[" + std::to_string(app.current_sector.x) + "," + std::to_string(app.current_sector.y) + "]";
        draw_text(1075.0f, 10.0f, sec_str, kalpana::Color{0.3f, 0.95f, 0.85f, 0.95f}, 6.0f, 11.0f);

        // Time Dilation Speed
        const int speed_pct = static_cast<int>(app.time_dilation * 100.0f);
        std::string time_str = "TIME:" + std::to_string(speed_pct / 100) + "." + std::to_string((speed_pct % 100) / 10) + "X";
        draw_text(1175.0f, 10.0f, time_str, kalpana::Color{0.6f, 0.95f, 0.7f, 0.95f}, 6.0f, 11.0f);

        // Overlays Layer Toggle Status
        std::string over_hud = app.show_analytics_overlays ? "OVERLAYS:ON" : "OVERLAYS:OFF";
        kalpana::Color over_col = app.show_analytics_overlays ? kalpana::Color{0.4f, 0.95f, 0.5f, 0.95f} : kalpana::Color{0.6f, 0.6f, 0.6f, 0.75f};
        draw_text(1250.0f, 10.0f, over_hud, over_col, 5.5f, 10.0f);

        // 6) NADI Real-Time Compute Sparkline Waveform (Ultra-Faint Mini Wave in Top Right)
        if (app.show_analytics_overlays) {
            kalpana::Path spark_line;
            const float sx_base = 545.0f;
            const float sy_base = 24.0f;
            const float comp_h = std::clamp(app.compute_ms * 2.0f, 1.0f, 12.0f);
            spark_line.move_to(sx_base, sy_base);
            spark_line.line_to(sx_base + 15.0f, sy_base - comp_h * 0.4f);
            spark_line.line_to(sx_base + 30.0f, sy_base - comp_h * 0.8f);
            spark_line.line_to(sx_base + 45.0f, sy_base - comp_h * 0.5f);
            spark_line.line_to(sx_base + 60.0f, sy_base);
            scene.add(kalpana::Node::shape(spark_line, kalpana::Paint::stroke(kalpana::Color{0.85f, 0.45f, 1.0f, 0.40f}, 0.8f)));
        }
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
        constexpr float mw = 590.0f;
        constexpr float mh = 445.0f;
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
        const float start_y = my + 60.0f;
        constexpr float row_step = 52.0f;

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

        // Row 4: Analytics Overlays Layer Toggle (Grid, Jacobi Links, MHD Arcs, H-R Inset)
        {
            const float ry = start_y + row_step * 4.0f;
            const bool sel = (app.config_selected_row == 4);
            const kalpana::Color row_col = sel ? kalpana::Color{1.0f, 0.85f, 0.2f, 1.0f} : kalpana::Color{0.7f, 0.8f, 0.9f, 0.85f};
            if (sel) {
                kalpana::Path sel_box;
                sel_box.rect(mx + 15.0f, ry - 4.0f, mw - 30.0f, 44.0f);
                scene.add(kalpana::Node::shape(sel_box, kalpana::Paint::stroke(kalpana::Color{1.0f, 0.85f, 0.2f, 0.7f}, 1.2f)));
            }
            const std::string over_str = app.show_analytics_overlays ? "ENABLED" : "DISABLED";
            const kalpana::Color val_col = app.show_analytics_overlays ? kalpana::Color{0.25f, 0.95f, 0.45f, 1.0f} : kalpana::Color{0.75f, 0.4f, 0.4f, 1.0f};
            draw_modal_text(mx + 30.0f, ry + 4.0f, "[5] OVERLAYS LAYER", row_col, 7.0f, 11.0f);
            draw_modal_text(mx + 340.0f, ry + 4.0f, "< " + over_str + " >", sel ? val_col : row_col, 7.0f, 11.0f);
            draw_modal_text(mx + 30.0f, ry + 22.0f, "SEPARATE SUB-LAYER: GRID, JACOBI, MHD & H-R INSET", kalpana::Color{0.45f, 0.6f, 0.75f, 0.75f}, 5.0f, 9.0f);
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
        if (app.middle_mouse_down || app.right_mouse_down) {
            // Smooth small-step spatial panning across the open world
            constexpr float kPanSpeed = 1.0f;
            const float dx = (e->mouse_x - app.last_mouse_pos[0]) * kPanSpeed;
            const float dy = (e->mouse_y - app.last_mouse_pos[1]) * kPanSpeed;
            app.target_cam_pos[0] -= dx;
            app.target_cam_pos[1] -= dy;
            app.tracked_planet_index = -1; // Unlink tracking on manual pan
        }
        app.last_mouse_pos = pebble::math::vec2{e->mouse_x, e->mouse_y};
        app.mouse_x = e->mouse_x;
        app.mouse_y = e->mouse_y;
        if (app.slingshot.active) {
            app.slingshot.current_x = e->mouse_x;
            app.slingshot.current_y = e->mouse_y;
        }
    } else if (e->type == SAPP_EVENTTYPE_MOUSE_SCROLL) {
        // Smooth Multi-Scale Logarithmic Cosmic Zoom: Range 0.25x (Deep Space) to 3.5x (Close Surface)
        if (e->scroll_y > 0.0f) {
            app.target_zoom = std::min(3.5f, app.target_zoom * 1.15f);
        } else if (e->scroll_y < 0.0f) {
            app.target_zoom = std::max(0.25f, app.target_zoom * 0.87f);
        }
    } else if (e->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
        if (e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) {
            app.middle_mouse_down = true;
            app.last_mouse_pos = pebble::math::vec2{e->mouse_x, e->mouse_y};
        } else if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
            app.right_mouse_down = true;
            app.last_mouse_pos = pebble::math::vec2{e->mouse_x, e->mouse_y};
        } else if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && !app.gravity_vortex && !app.heat_ray && !app.freeze_ray) {
            app.slingshot.active = true;
            app.slingshot.start_x = e->mouse_x;
            app.slingshot.start_y = e->mouse_y;
            app.slingshot.current_x = e->mouse_x;
            app.slingshot.current_y = e->mouse_y;
        } else {
            app.mouse_down = true;
        }
    } else if (e->type == SAPP_EVENTTYPE_MOUSE_UP) {
        if (e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) {
            app.middle_mouse_down = false;
        } else if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
            app.right_mouse_down = false;
        } else if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && app.slingshot.active) {
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
                    app.config_selected_row = (app.config_selected_row == 0) ? 4 : app.config_selected_row - 1;
                    break;
                case SAPP_KEYCODE_DOWN:
                    app.config_selected_row = (app.config_selected_row + 1) % 5;
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
                    } else if (app.config_selected_row == 4) {
                        app.show_analytics_overlays = !app.show_analytics_overlays;
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
                    } else if (app.config_selected_row == 4) {
                        app.show_analytics_overlays = !app.show_analytics_overlays;
                    }
                    break;
                case SAPP_KEYCODE_ENTER:
                case SAPP_KEYCODE_KP_ENTER:
                case SAPP_KEYCODE_SPACE: {
                    // Apply parameters and launch pure autonomous physical simulation
                    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
                    app.in_startup_modal = false;
                    app.gravity_policy.G = app.config_grav_g;
                    app.gravity_policy.theta = app.config_bh_theta;

                    app.planets.clear();
                    app.sparks.clear();
                    app.nebulae.clear();
                    app.jets.clear();

                    // Seed initial viewport spatial tiles (4x4 tiles of 320x200 covering [0, FW] x [0, FH])
                    app.visited_sectors.clear();
                    for (int tx = 0; tx < 4; ++tx) {
                        for (int ty = 0; ty < 4; ++ty) {
                            const prakriti::celestial::SectorKey sk{tx, ty};
                            app.visited_sectors.insert(prakriti::celestial::hash_sector_key(sk));
                        }
                    }

                    for (int i = 0; i < app.config_initial_dust_count; ++i) {
                        if (app.config_dist_mode == 0) {
                            // Mode 0: True Isotropic Uniform Field with Thermal Dispersion across [0, FW] x [0, FH]
                            spawn_dust_particle(app, true, 12.0f + dist01(app.rng) * (FW - 24.0f), 12.0f + dist01(app.rng) * (FH - 24.0f));
                        } else if (app.config_dist_mode == 1) {
                            // Mode 1: Rotating Barycentric Protogalactic Disk
                            const float cx = FW * 0.5f;
                            const float cy = FH * 0.5f;
                            std::normal_distribution<float> norm_r(0.0f, FW * 0.18f);
                            const float r = std::clamp(std::abs(norm_r(app.rng)) + 12.0f, 15.0f, FW * 0.42f);
                            const float theta = dist01(app.rng) * 6.2831853f;
                            const float px = cx + std::cos(theta) * r;
                            const float py = cy + std::sin(theta) * r;

                            spawn_dust_particle(app, true, px, py);

                            // Organic Keplerian / Virial orbital speed: v \approx \sqrt{G * M(r) / r}
                            auto& p_new = app.planets.back();
                            const float expected_interior_mass = 15.0f * (r / 20.0f) * 12.0f;
                            const float v_mag = std::sqrt((app.config_grav_g * expected_interior_mass) / std::max(r, 20.0f)) * 0.08f;
                            const pebble::math::vec2 tangent{-std::sin(theta), std::cos(theta)};
                            p_new.vel = tangent * v_mag + p_new.vel * 0.25f;
                        } else {
                            // Mode 2: Dual Infall Colliding Protogalactic Clouds with natural relative orbital velocity
                            const bool cloud2 = (i % 2 == 0);
                            const float cx = cloud2 ? (FW * 0.65f) : (FW * 0.35f);
                            const float cy = cloud2 ? (FH * 0.60f) : (FH * 0.40f);
                            std::normal_distribution<float> norm_r(0.0f, 65.0f);
                            const float r = std::clamp(std::abs(norm_r(app.rng)), 5.0f, 120.0f);
                            const float theta = dist01(app.rng) * 6.2831853f;
                            const float px = std::clamp(cx + std::cos(theta) * r, 20.0f, FW - 20.0f);
                            const float py = std::clamp(cy + std::sin(theta) * r, 20.0f, FH - 20.0f);

                            spawn_dust_particle(app, true, px, py);
                            auto& p_new = app.planets.back();
                            // Infall drift speed between the two clusters
                            const pebble::math::vec2 drift = cloud2 ? pebble::math::vec2{-6.0f, -3.5f} : pebble::math::vec2{6.0f, 3.5f};
                            const pebble::math::vec2 spin_v{-std::sin(theta) * 4.5f, std::cos(theta) * 4.5f};
                            p_new.vel = drift + spin_v + p_new.vel * 0.2f;
                        }
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
            case SAPP_KEYCODE_I:
                // Instantly spawn an external system inflow
                spawn_external_inflow(app);
                break;
            case SAPP_KEYCODE_M:
                // Cycle Radar Reach (1.5x -> 3.5x -> 7.0x)
                app.radar_zoom_level = (app.radar_zoom_level + 1) % 3;
                break;
            case SAPP_KEYCODE_UP:
                app.target_cam_pos[1] -= 240.0f;
                app.tracked_planet_index = -1;
                break;
            case SAPP_KEYCODE_DOWN:
                app.target_cam_pos[1] += 240.0f;
                app.tracked_planet_index = -1;
                break;
            case SAPP_KEYCODE_LEFT:
                app.target_cam_pos[0] -= 240.0f;
                app.tracked_planet_index = -1;
                break;
            case SAPP_KEYCODE_RIGHT:
                app.target_cam_pos[0] += 240.0f;
                app.tracked_planet_index = -1;
                break;
            case SAPP_KEYCODE_TAB: {
                // Cycle tracking to next massive body (or reset if at end)
                std::vector<int> heavy_indices;
                for (int i = 0; i < static_cast<int>(app.planets.size()); ++i) {
                    if (app.planets[i].alive && app.planets[i].mass > 70.0f) {
                        heavy_indices.push_back(i);
                    }
                }
                if (heavy_indices.empty()) {
                    app.tracked_planet_index = -1;
                } else {
                    int curr_pos = -1;
                    for (int k = 0; k < static_cast<int>(heavy_indices.size()); ++k) {
                        if (heavy_indices[k] == app.tracked_planet_index) {
                            curr_pos = k;
                            break;
                        }
                    }
                    if (curr_pos == -1 || curr_pos + 1 >= static_cast<int>(heavy_indices.size())) {
                        app.tracked_planet_index = (curr_pos == -1) ? heavy_indices[0] : -1;
                    } else {
                        app.tracked_planet_index = heavy_indices[curr_pos + 1];
                    }
                }
                break;
            }
            case SAPP_KEYCODE_ESCAPE:
                app.tracked_planet_index = -1;
                app.target_cam_pos = pebble::math::vec2{FW * 0.5f, FH * 0.5f};
                app.target_zoom = 1.0f;
                break;
            case SAPP_KEYCODE_O:
                app.show_analytics_overlays = !app.show_analytics_overlays;
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
            case SAPP_KEYCODE_B: {
                // Spawn Inspiral Binary Black Holes at cursor
                const pebble::math::vec2 spawn_pos = app.camera_pos;
                PlanetBody bh1, bh2;
                bh1.ent = app.world.spawn();
                bh1.pos = spawn_pos + pebble::math::vec2{-25.0f, 0.0f};
                bh1.prev_pos = bh1.pos;
                bh1.vel = pebble::math::vec2{0.0f, -22.0f};
                bh1.mass = 450.0f;
                bh1.radius = 3.5f;
                bh1.type = CelestialType::BlackHoleSingularity;
                bh1.mat_params = prakriti::celestial::black_hole_singularity();
                bh1.is_singularity = true;

                bh2.ent = app.world.spawn();
                bh2.pos = spawn_pos + pebble::math::vec2{25.0f, 0.0f};
                bh2.prev_pos = bh2.pos;
                bh2.vel = pebble::math::vec2{0.0f, 22.0f};
                bh2.mass = 450.0f;
                bh2.radius = 3.5f;
                bh2.type = CelestialType::BlackHoleSingularity;
                bh2.mat_params = prakriti::celestial::black_hole_singularity();
                bh2.is_singularity = true;

                app.planets.push_back(bh1);
                app.planets.push_back(bh2);
                break;
            }
            case SAPP_KEYCODE_P: {
                // Spawn Millisecond Pulsar with Magnetosphere
                PlanetBody pulsar;
                pulsar.ent = app.world.spawn();
                pulsar.pos = app.camera_pos;
                pulsar.prev_pos = pulsar.pos;
                pulsar.vel = pebble::math::vec2{0.0f, 0.0f};
                pulsar.mass = 350.0f;
                pulsar.radius = 2.4f;
                pulsar.temperature = 7500.0f;
                pulsar.omega = 45.0f;
                pulsar.is_neutron_star = true;
                pulsar.type = CelestialType::NeutronStar;
                pulsar.mat_params = prakriti::celestial::neutron_star();
                app.planets.push_back(pulsar);
                break;
            }
            case SAPP_KEYCODE_S: {
                // Spawn Glowing Protostar with Protoplanetary Disk
                PlanetBody star;
                star.ent = app.world.spawn();
                star.pos = app.camera_pos;
                star.prev_pos = star.pos;
                star.vel = pebble::math::vec2{0.0f, 0.0f};
                star.mass = 650.0f;
                star.radius = 6.0f;
                star.temperature = 4200.0f;
                star.type = CelestialType::SuperheatedPlasma;
                star.mat_params = prakriti::celestial::superheated_plasma();
                app.planets.push_back(star);

                // Surrounding Protoplanetary dust ring
                std::uniform_real_distribution<float> d01(0.0f, 1.0f);
                for (int d = 0; d < 28; ++d) {
                    const float a = (static_cast<float>(d) / 28.0f) * 6.2831853f;
                    const float r = 35.0f + d01(app.rng) * 45.0f;
                    const float spd = std::sqrt((app.config_grav_g * star.mass) / r);
                    PlanetBody dust;
                    dust.ent = app.world.spawn();
                    dust.pos = star.pos + pebble::math::vec2{std::cos(a), std::sin(a)} * r;
                    dust.prev_pos = dust.pos;
                    dust.vel = pebble::math::vec2{-std::sin(a), std::cos(a)} * spd;
                    dust.mass = 0.5f;
                    dust.radius = 0.9f;
                    dust.type = CelestialType::SilicateRock;
                    dust.mat_params = prakriti::celestial::silicate_rock();
                    app.planets.push_back(dust);
                }
                break;
            }
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

