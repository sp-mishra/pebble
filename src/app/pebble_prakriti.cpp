// ============================================================================
// src/app/pebble_prakriti.cpp — Prakriti Multiphysics & Continuum Stress Test
// ============================================================================
// Stress tests:
//  1. High-density PBF Fluid Column Dam Break (1,600+ liquid particles)
//  2. Multi-tier XPBD Elastic Mesh & Rope Bridges under continuous hydrodynamic loading
//  3. Akruti Obstacle SDF Collision & Restitution Pinwheels
//  4. Thermodynamic Phase Transitions (Ice melting, dry ice sublimation, superheated gas plume)
//  5. Interactive Thermal Injector / Gravity vortex with Real-Time HUD telemetry
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

#include "prakriti/prakriti.hpp"
#include "prakriti/solvers/obstacle.hpp"
#include "prakriti/solvers/joint.hpp"
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

// Custom Obstacle Collection satisfying ObstacleSolver requirements
struct ShowcaseObstacles {
    std::vector<akruti::Circle> circles;
    std::vector<akruti::Box>    boxes;
    std::vector<akruti::Capsule> capsules;

    template <typename Fn>
    void for_each_shape(Fn&& fn) const {
        for (const auto& c : circles) fn(c);
        for (const auto& b : boxes) fn(b);
        for (const auto& cap : capsules) fn(cap);
    }
};

using ShowcaseMechanics = prakriti::SolverStack<
    prakriti::XpbdSolver,
    prakriti::DensitySolver,
    prakriti::ObstacleSolver<ShowcaseObstacles>
>;

struct PrakritiStressApp {
    sg_pipeline pip{};
    sg_bindings bind{};
    sg_pass_action pass_action{};
    sg_buffer vbuf{};
    sg_buffer ibuf{};
    std::unique_ptr<kalpana::Canvas<kalpana::sokol_backend>> canvas;

    ShowcaseObstacles obstacles;
    std::unique_ptr<prakriti::World<prakriti::DefaultMaterialLaw, prakriti::ScalarBackend, ShowcaseMechanics>> world;
    prakriti::MaterialId mat_water = 0;
    prakriti::MaterialId mat_steel = 0;
    prakriti::MaterialId mat_dry_ice = 0;

    float t = 0.0f;
    int frame = 0;
    float fps = 60.0f;
    std::chrono::high_resolution_clock::time_point last_time;

    // Interactive controls
    float mouse_x = FW * 0.5f;
    float mouse_y = FH * 0.5f;
    bool mouse_down = false;
    bool heat_emitter = false;
    bool cold_emitter = false;
};

static PrakritiStressApp g_app;

static void init_prakriti_world() {
    auto& app = g_app;
    prakriti::WorldConfig cfg{};
    cfg.bounds = {{30.0f, 30.0f}, {FW - 30.0f, FH - 30.0f}};
    cfg.gravity = {0.0f, 260.0f};
    cfg.substeps = 2;       // 2 high-rate substeps
    cfg.solver_iters = 3;   // 3 solver iterations for snappy performance
    cfg.cell_size = 14.0f;

    // 1. Setup Static Obstacles — Plinko field & angled sluice ramps
    app.obstacles.circles.clear();
    app.obstacles.boxes.clear();
    app.obstacles.capsules.clear();

    // Multi-tier Galton board / Plinko deflector grid
    constexpr int kPegRows = 3;
    for (int pr = 0; pr < kPegRows; ++pr) {
        int count = 5 + (pr % 2);
        float spacing = FW / float(count + 1);
        float y = 240.0f + float(pr) * 75.0f;
        for (int pc = 1; pc <= count; ++pc) {
            float x = float(pc) * spacing + ((pr % 2) ? (spacing * 0.5f) : 0.0f);
            if (x > 80.0f && x < FW - 80.0f) {
                app.obstacles.circles.push_back(akruti::Circle{pebble::math::vec2(x, y), 14.0f});
            }
        }
    }

    // Angled Funnel Guides & Center Bouncer
    app.obstacles.circles.push_back(akruti::Circle{pebble::math::vec2(FW * 0.50f, 500.0f), 32.0f});
    app.obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(80.0f, 160.0f),
        pebble::math::vec2(280.0f, 220.0f),
        10.0f
    });
    app.obstacles.capsules.push_back(akruti::Capsule{
        pebble::math::vec2(FW - 80.0f, 160.0f),
        pebble::math::vec2(FW - 280.0f, 220.0f),
        10.0f
    });

    prakriti::ObstacleConfig obs_cfg{};
    obs_cfg.contact_stiffness = 1.0f;
    obs_cfg.restitution = 0.45f;
    obs_cfg.friction = 0.12f;

    ShowcaseMechanics mechanics_stack{
        std::make_tuple(
            prakriti::XpbdSolver{},
            prakriti::DensitySolver{},
            prakriti::ObstacleSolver<ShowcaseObstacles>{app.obstacles, obs_cfg}
        )
    };

    app.world = std::make_unique<prakriti::World<prakriti::DefaultMaterialLaw, prakriti::ScalarBackend, ShowcaseMechanics>>(
        cfg, std::move(mechanics_stack)
    );

    app.mat_steel   = app.world->materials().add(prakriti::MaterialRegistry::steel());
    app.mat_water   = app.world->materials().add(prakriti::MaterialRegistry::water());
    app.mat_dry_ice = app.world->materials().add(prakriti::MaterialRegistry::dry_ice());

    // Custom Magma / Molten Lava Material
    prakriti::MaterialParams lava_params;
    lava_params.rest_density = 2400.0f;
    lava_params.heat_capacity = 1.8f;
    lava_params.conductivity = 2.5f;
    lava_params.melt_temp = 600.0f;
    lava_params.boil_temp = 1800.0f;
    lava_params.latent_heat_fusion = 400.0f;
    lava_params.alpha = {1e-7f, 1e-4f, 5e-2f, 1.0f};
    lava_params.visc = {0.0f, 0.4f, 0.85f, 0.02f};
    lava_params.eos_B = 1.5e5f;
    prakriti::MaterialId mat_lava = app.world->materials().add(lava_params);

    // Custom Viscoelastic Jelly Material
    prakriti::MaterialParams jelly_params;
    jelly_params.rest_density = 1100.0f;
    jelly_params.heat_capacity = 2.2f;
    jelly_params.conductivity = 0.8f;
    jelly_params.melt_temp = 85.0f;
    jelly_params.boil_temp = 140.0f;
    jelly_params.yield_strain = 0.35f;
    jelly_params.ultimate_strain = 0.85f;
    jelly_params.alpha = {5e-4f, 1e-3f, 1e-1f, 1.0f};
    jelly_params.visc = {0.1f, 0.25f, 0.5f, 0.01f};
    prakriti::MaterialId mat_jelly = app.world->materials().add(jelly_params);

    // 2. High-Density Dual Fluid Dam Breaks (Water on left, Superheated Magma on right)
    // Left: Cool Water Reservoir (450 particles)
    for (int r = 0; r < 18; ++r) {
        for (int c = 0; c < 25; ++c) {
            float px = 45.0f + float(c) * 9.0f;
            float py = 45.0f + float(r) * 9.0f;
            app.world->particles().add({
                .position = pebble::math::vec2(px, py),
                .velocity = {10.0f, 0.0f},
                .mass = 1.0f,
                .temperature = 16.0f,
                .material = app.mat_water,
                .f_solid = 0.0f, .f_plastic = 0.0f, .f_liquid = 1.0f, .f_gas = 0.0f
            });
        }
    }

    // Right: Superheated Molten Lava Column (450 particles)
    for (int r = 0; r < 18; ++r) {
        for (int c = 0; c < 25; ++c) {
            float px = FW - 270.0f + float(c) * 9.0f;
            float py = 45.0f + float(r) * 9.0f;
            app.world->particles().add({
                .position = pebble::math::vec2(px, py),
                .velocity = {-10.0f, 0.0f},
                .mass = 2.2f,
                .temperature = 950.0f, // Glowing hot magma!
                .material = mat_lava,
                .f_solid = 0.0f, .f_plastic = 0.1f, .f_liquid = 0.9f, .f_gas = 0.0f
            });
        }
    }

    // 3. Multi-Tier XPBD Elastic Lattice & Suspension Bridges
    constexpr int kChains = 2;
    for (int ch = 0; ch < kChains; ++ch) {
        float y_base = 350.0f + float(ch) * 130.0f;
        constexpr int kNodes = 20;
        std::vector<prakriti::Index> node_indices;
        for (int i = 0; i < kNodes; ++i) {
            float px = 270.0f + float(i) * 32.0f;
            float py = y_base + std::sin(float(i) * 0.32f) * 12.0f;
            auto idx = app.world->particles().add({
                .position = pebble::math::vec2(px, py),
                .velocity = {0.0f, 0.0f},
                .mass = (i == 0 || i == kNodes - 1) ? 0.0f : 1.8f,
                .temperature = 22.0f,
                .material = app.mat_steel,
                .f_solid = 1.0f, .f_plastic = 0.0f, .f_liquid = 0.0f, .f_gas = 0.0f
            });
            node_indices.push_back(idx);
        }
        for (std::size_t i = 1; i < node_indices.size(); ++i) {
            app.world->edges().add(node_indices[i - 1], node_indices[i], 32.0f);
        }
    }

    // 4. Deformable XPBD Jelly Soft-Body Mesh (5x5 Cross-Braced Lattice)
    {
        constexpr int j_rows = 5, j_cols = 5;
        prakriti::Index j_grid[j_rows][j_cols];
        for (int r = 0; r < j_rows; ++r) {
            for (int c = 0; c < j_cols; ++c) {
                float jx = FW * 0.5f - 40.0f + float(c) * 20.0f;
                float jy = 70.0f + float(r) * 20.0f;
                j_grid[r][c] = app.world->particles().add({
                    .position = pebble::math::vec2(jx, jy),
                    .velocity = {0.0f, 50.0f},
                    .mass = 1.2f,
                    .temperature = 24.0f,
                    .material = mat_jelly,
                    .f_solid = 0.85f, .f_plastic = 0.15f, .f_liquid = 0.0f, .f_gas = 0.0f
                });
            }
        }
        // Add horizontal, vertical, and cross-braced shear springs
        for (int r = 0; r < j_rows; ++r) {
            for (int c = 0; c < j_cols; ++c) {
                if (c + 1 < j_cols) app.world->edges().add(j_grid[r][c], j_grid[r][c + 1], 20.0f);
                if (r + 1 < j_rows) app.world->edges().add(j_grid[r][c], j_grid[r + 1][c], 20.0f);
                if (r + 1 < j_rows && c + 1 < j_cols) {
                    app.world->edges().add(j_grid[r][c], j_grid[r + 1][c + 1], 28.28f);
                    app.world->edges().add(j_grid[r + 1][c], j_grid[r][c + 1], 28.28f);
                }
            }
        }
    }

    // 5. Cryogenic Sublimating Dry Ice Cluster (100 particles)
    for (int r = 0; r < 10; ++r) {
        for (int c = 0; c < 10; ++c) {
            app.world->particles().add({
                .position = pebble::math::vec2(FW * 0.5f - 60.0f + float(c) * 12.0f, 180.0f + float(r) * 12.0f),
                .velocity = {0.0f, 20.0f},
                .mass = 1.4f,
                .temperature = -85.0f + float(r + c) * 1.5f,
                .material = app.mat_dry_ice,
                .f_solid = 1.0f, .f_plastic = 0.0f, .f_liquid = 0.0f, .f_gas = 0.0f
            });
        }
    }
}

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
    app.pass_action.colors[0].clear_value = {0.02f, 0.03f, 0.06f, 1.0f};
    app.canvas = std::make_unique<kalpana::Canvas<kalpana::sokol_backend>>(W, H);
    app.last_time = std::chrono::high_resolution_clock::now();

    init_prakriti_world();
}

static void build_scene(kalpana::Scene& scene) {
    auto& app = g_app;
    scene.clear_color(kalpana::Color{0.02f, 0.03f, 0.06f, 1.0f});

    if (!app.world) return;
    auto& pw = *app.world;
    const auto& P = pw.particles();
    const auto& E = pw.edges();

    // 1. Background Grid
    {
        kalpana::Color gc{0.07f, 0.09f, 0.14f, 1.0f};
        for (int i = 0; i <= 24; ++i) {
            float x = float(i) / 24.0f * FW;
            kalpana::Path line;
            line.move_to(x, 0.0f);
            line.line_to(x, FH);
            scene.add(kalpana::Node::shape(line, kalpana::Paint::stroke(gc, 1.0f)));
        }
        for (int j = 0; j <= 16; ++j) {
            float y = float(j) / 16.0f * FH;
            kalpana::Path line;
            line.move_to(0.0f, y);
            line.line_to(FW, y);
            scene.add(kalpana::Node::shape(line, kalpana::Paint::stroke(gc, 1.0f)));
        }
    }

    // 2. Render Static Obstacles (Circles & Capsules)
    for (const auto& c : app.obstacles.circles) {
        kalpana::Path p;
        p.circle(c.center.x, c.center.y, c.radius);
        scene.add(kalpana::Node::shape(p, kalpana::Paint::fill(kalpana::Color{0.12f, 0.16f, 0.24f, 0.95f})));
        scene.add(kalpana::Node::shape(p, kalpana::Paint::stroke(kalpana::Color{0.3f, 0.5f, 0.85f, 0.8f}, 3.0f)));
    }
    for (const auto& cap : app.obstacles.capsules) {
        kalpana::Path p;
        p.circle(cap.a.x, cap.a.y, cap.radius);
        p.circle(cap.b.x, cap.b.y, cap.radius);
        kalpana::Path seg;
        seg.move_to(cap.a.x, cap.a.y);
        seg.line_to(cap.b.x, cap.b.y);
        scene.add(kalpana::Node::shape(seg, kalpana::Paint::stroke(kalpana::Color{0.14f, 0.18f, 0.28f, 0.95f}, cap.radius * 2.0f)));
        scene.add(kalpana::Node::shape(seg, kalpana::Paint::stroke(kalpana::Color{0.4f, 0.6f, 0.95f, 0.8f}, 2.5f)));
        scene.add(kalpana::Node::shape(p, kalpana::Paint::fill(kalpana::Color{0.14f, 0.18f, 0.28f, 0.95f})));
        scene.add(kalpana::Node::shape(p, kalpana::Paint::stroke(kalpana::Color{0.4f, 0.6f, 0.95f, 0.8f}, 2.5f)));
    }

    // 3. Render XPBD Elastic Bonds with Strain-Spectral Color Mapping
    for (std::size_t e = 0; e < E.size(); ++e) {
        if (!E.active[e]) continue;
        auto ia = E.a[e];
        auto ib = E.b[e];
        float x0 = P.pos_x[ia];
        float y0 = P.pos_y[ia];
        float x1 = P.pos_x[ib];
        float y1 = P.pos_y[ib];

        float strain_val = std::clamp(std::abs(E.strain[e]) * 25.0f, 0.0f, 1.0f);
        kalpana::Color bond_col = kalpana::spectral::mix(
            kalpana::Color{0.3f, 0.8f, 1.0f, 0.9f},
            kalpana::Color{1.0f, 0.2f, 0.25f, 0.98f},
            strain_val
        );

        kalpana::Path bond;
        bond.move_to(x0, y0);
        bond.line_to(x1, y1);
        scene.add(kalpana::Node::shape(bond, kalpana::Paint::stroke(bond_col, 3.5f)));
    }

    // 4. Render Multiphysics Particles (Fluid, Solid, Gas & Thermal Glow)
    const std::size_t N = P.size();
    for (prakriti::Index i = 0; i < N; ++i) {
        float x = P.pos_x[i];
        float y = P.pos_y[i];

        kalpana::Color pcol;
        float pr = 4.5f;

        if (P.f_gas[i] > 0.25f) {
            // Rising vapor / sublimated plume
            float heat = std::clamp((P.temperature[i] - 20.0f) / 120.0f, 0.0f, 1.0f);
            pcol = kalpana::spectral::mix(
                kalpana::Color{0.8f, 0.88f, 1.0f, 0.35f * P.f_gas[i]},
                kalpana::Color{1.0f, 0.45f, 0.1f, 0.55f * P.f_gas[i]},
                heat
            );
            pr = 6.5f;
        } else if (P.f_liquid[i] > 0.35f) {
            // Hydrodynamic fluid particle: Cool water vs Molten magma
            if (P.temperature[i] > 200.0f) {
                // Glowing molten lava
                float lava_glow = std::clamp((P.temperature[i] - 200.0f) / 800.0f, 0.0f, 1.0f);
                pcol = kalpana::spectral::mix(
                    kalpana::Color{0.95f, 0.25f, 0.05f, 0.95f},
                    kalpana::Color{1.0f, 0.92f, 0.30f, 1.0f},
                    lava_glow
                );
                pr = 5.5f;
            } else {
                // Clear blue water
                float heat = std::clamp((P.temperature[i] - 10.0f) / 80.0f, 0.0f, 1.0f);
                pcol = kalpana::spectral::mix(
                    kalpana::Color{0.08f, 0.68f, 1.0f, 0.90f},
                    kalpana::Color{0.45f, 0.95f, 0.85f, 0.92f},
                    heat
                );
                pr = 4.8f;
            }
        } else {
            // Solid / Viscoelastic Soft-Body / Plastic
            if (P.material[i] == app.mat_dry_ice) {
                pcol = kalpana::Color{0.92f, 0.96f, 1.0f, 0.98f}; // Cryo frost white
                pr = 5.2f;
            } else if (P.f_solid[i] > 0.5f && P.f_plastic[i] > 0.05f) {
                pcol = kalpana::Color{0.15f, 0.92f, 0.55f, 0.92f}; // Bouncy jelly emerald green
                pr = 5.0f;
            } else {
                pcol = kalpana::Color{0.75f, 0.82f, 0.95f, 1.0f}; // Structural steel
                pr = 4.0f;
            }
        }

        kalpana::Path pdot;
        pdot.circle(x, y, pr);
        scene.add(kalpana::Node::shape(pdot, kalpana::Paint::fill(pcol)));
    }

    // 5. Interactive Mouse Emitter Feedback
    if (app.mouse_down || app.heat_emitter || app.cold_emitter) {
        kalpana::Path reticle;
        reticle.circle(app.mouse_x, app.mouse_y, 45.0f);
        kalpana::Color emit_col = app.heat_emitter ? kalpana::Color{1.0f, 0.3f, 0.1f, 0.4f} :
                                 (app.cold_emitter ? kalpana::Color{0.2f, 0.8f, 1.0f, 0.4f} :
                                  kalpana::Color{0.5f, 1.0f, 0.5f, 0.35f});
        scene.add(kalpana::Node::shape(reticle, kalpana::Paint::stroke(emit_col, 2.0f)));
    }

    // 6. Glassmorphic Real-Time Performance & Telemetry HUD
    {
        const float hud_x = 24.0f, hud_y = 24.0f;
        const float hud_w = 340.0f, hud_h = 135.0f;

        kalpana::Path hud_bg;
        hud_bg.round_rect(hud_x, hud_y, hud_w, hud_h, 12.0f, 12.0f);
        scene.add(kalpana::Node::shape(hud_bg, kalpana::Paint::fill(kalpana::Color{0.04f, 0.07f, 0.12f, 0.88f})));
        scene.add(kalpana::Node::shape(hud_bg, kalpana::Paint::stroke(kalpana::Color{0.22f, 0.32f, 0.50f, 0.85f}, 1.5f)));

        kalpana::Color bar_col = app.fps >= 55.0f ? kalpana::Color{0.18f, 0.85f, 0.35f, 0.95f} :
                                (app.fps >= 30.0f ? kalpana::Color{1.0f, 0.72f, 0.15f, 0.95f} :
                                 kalpana::Color{0.95f, 0.22f, 0.22f, 0.95f});

        float bar_w = std::clamp((app.fps / 60.0f) * (hud_w - 32.0f), 0.0f, hud_w - 32.0f);
        kalpana::Path fps_bar;
        fps_bar.round_rect(hud_x + 16.0f, hud_y + 42.0f, bar_w, 8.0f, 4.0f, 4.0f);
        scene.add(kalpana::Node::shape(fps_bar, kalpana::Paint::fill(bar_col)));

        kalpana::Path diag_p1;
        diag_p1.circle(hud_x + 22.0f, hud_y + 72.0f, 4.0f);
        scene.add(kalpana::Node::shape(diag_p1, kalpana::Paint::fill(kalpana::Color{0.05f, 0.65f, 1.0f, 0.9f})));

        kalpana::Path diag_p2;
        diag_p2.circle(hud_x + 22.0f, hud_y + 94.0f, 4.0f);
        scene.add(kalpana::Node::shape(diag_p2, kalpana::Paint::fill(kalpana::Color{0.3f, 0.8f, 1.0f, 0.9f})));

        kalpana::Path diag_p3;
        diag_p3.circle(hud_x + 22.0f, hud_y + 116.0f, 4.0f);
        scene.add(kalpana::Node::shape(diag_p3, kalpana::Paint::fill(kalpana::Color{0.92f, 0.96f, 1.0f, 0.95f})));
    }
}

static void frame_cb() {
    auto& app = g_app;
    app.frame++;
    app.t += DT;

    auto now = std::chrono::high_resolution_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - app.last_time).count();
    app.last_time = now;
    if (frame_ms > 0.0f) {
        float current_fps = 1000.0f / frame_ms;
        app.fps = app.fps * 0.92f + current_fps * 0.08f;
    }

    if (app.world) {
        // Interactive Mouse Temperature / Force Injection
        if (app.mouse_down || app.heat_emitter || app.cold_emitter) {
            auto& P = app.world->particles();
            const std::size_t N = P.size();
            for (std::size_t i = 0; i < N; ++i) {
                float dx = P.pos_x[i] - app.mouse_x;
                float dy = P.pos_y[i] - app.mouse_y;
                float d2 = dx * dx + dy * dy;
                if (d2 < 55.0f * 55.0f) {
                    if (app.heat_emitter) {
                        P.temperature[i] += 12.0f;
                    } else if (app.cold_emitter) {
                        P.temperature[i] -= 12.0f;
                    } else if (app.mouse_down) {
                        float dist = std::sqrt(d2);
                        if (dist > 1.0f) {
                            P.vel_x[i] += (-dy / dist) * 180.0f * DT;
                            P.vel_y[i] += (dx / dist) * 180.0f * DT;
                        }
                    }
                }
            }
        }

        app.world->step();
    }

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
    if (!indices.empty()) {
        sg_apply_pipeline(app.pip);
        sg_apply_bindings(app.bind);
        sg_draw(0, static_cast<int>(indices.size()), 1);
    }
    sg_end_pass();
    sg_commit();
}

static void event_cb(const sapp_event* ev) {
    auto& app = g_app;
    if (ev->type == SAPP_EVENTTYPE_MOUSE_MOVE) {
        app.mouse_x = ev->mouse_x;
        app.mouse_y = ev->mouse_y;
    } else if (ev->type == SAPP_EVENTTYPE_MOUSE_DOWN) {
        if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT) app.mouse_down = true;
    } else if (ev->type == SAPP_EVENTTYPE_MOUSE_UP) {
        if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT) app.mouse_down = false;
    } else if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
        switch (ev->key_code) {
            case SAPP_KEYCODE_ESCAPE:
                sapp_quit();
                break;
            case SAPP_KEYCODE_R:
            case SAPP_KEYCODE_SPACE:
                init_prakriti_world();
                break;
            case SAPP_KEYCODE_H:
                app.heat_emitter = !app.heat_emitter;
                break;
            case SAPP_KEYCODE_C:
                app.cold_emitter = !app.cold_emitter;
                break;
            default:
                break;
        }
    }
}

static void cleanup_cb() {
    sg_shutdown();
}

sapp_desc sokol_main(int /*argc*/, char** /*argv*/) {
    sapp_desc d{};
    d.init_cb = init_cb;
    d.frame_cb = frame_cb;
    d.event_cb = event_cb;
    d.cleanup_cb = cleanup_cb;
    d.width = W;
    d.height = H;
    d.window_title = "Pebble Prakriti Stress Test — [L-Click] Vortex | [H] Heat Gun | [C] Cryo Gun | [R] Reset";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}
