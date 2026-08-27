// ============================================================================
// src/app/pebble_prakriti.cpp — Prakriti Continuum Physics & Fluids Showcase
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
#include "kalpana/kalpana.hpp"
#include "kalpana/backend/capture_backend.hpp"

#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>

static const char* VS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct In  { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };\n"
    "struct Out { float4 pos [[position]]; float2 uv; };\n"
    "vertex Out vs(In in [[stage_in]]) { Out o; o.pos=float4(in.pos,0,1); o.uv=in.uv; return o; }\n";
static const char* FS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct In { float2 uv; };\n"
    "fragment float4 fs(In in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "    return tex.sample(s, in.uv); }\n";

static constexpr int W = 1060;
static constexpr int H = 700;
static constexpr float FW = float(W);
static constexpr float FH = float(H);
static constexpr float DT = 1.0f / 60.0f;

struct PrakritiApp {
    sg_pipeline pip{};
    sg_bindings bind{};
    sg_pass_action pass_action{};
    sg_image tex_img{};
    sg_view tex_view{};
    sg_sampler smp{};
    std::vector<std::uint32_t> pixels;
    std::unique_ptr<kalpana::Canvas<kalpana::capture_backend>> canvas;

    std::unique_ptr<prakriti::World<prakriti::DefaultMaterialLaw, prakriti::ScalarBackend>> world;
    prakriti::MaterialId mat_water = 0;
    prakriti::MaterialId mat_steel = 0;
    prakriti::MaterialId mat_dry_ice = 0;

    float t = 0.0f;
    int frame = 0;
};

static PrakritiApp g_app;

static void init_prakriti_world() {
    auto& app = g_app;
    prakriti::WorldConfig cfg{};
    cfg.bounds = {{40.0f, 40.0f}, {FW - 40.0f, FH - 40.0f}};
    cfg.gravity = {0.0f, 220.0f};
    cfg.substeps = 4;
    cfg.solver_iters = 4;
    cfg.cell_size = 14.0f;

    app.world = std::make_unique<prakriti::World<prakriti::DefaultMaterialLaw, prakriti::ScalarBackend>>(cfg);
    app.mat_steel   = app.world->materials().add(prakriti::MaterialRegistry::steel());
    app.mat_water   = app.world->materials().add(prakriti::MaterialRegistry::water());
    app.mat_dry_ice = app.world->materials().add(prakriti::MaterialRegistry::dry_ice());

    // 1. Dual fluid columns for hydrodynamic sloshing and mixing
    constexpr int kCols = 16;
    constexpr int kRows = 14;
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            app.world->particles().add({
                .position = pebble::math::vec2(60.0f + float(c) * 12.0f, 60.0f + float(r) * 12.0f),
                .velocity = {0.0f, 0.0f},
                .mass = 1.0f,
                .temperature = 20.0f,
                .material = app.mat_water,
                .f_solid = 0.0f, .f_plastic = 0.0f, .f_liquid = 1.0f, .f_gas = 0.0f
            });
        }
    }

    // 2. XPBD Elastic Bridges
    constexpr int kChains = 2;
    for (int ch = 0; ch < kChains; ++ch) {
        float y_pos = 280.0f + float(ch) * 120.0f;
        constexpr int kNodes = 12;
        std::vector<prakriti::Index> node_indices;
        for (int i = 0; i < kNodes; ++i) {
            auto idx = app.world->particles().add({
                .position = pebble::math::vec2(200.0f + float(i) * 55.0f, y_pos),
                .velocity = {0.0f, 0.0f},
                .mass = (i == 0 || i == kNodes - 1) ? 0.0f : 1.5f,
                .temperature = 22.0f,
                .material = app.mat_steel,
                .f_solid = 1.0f, .f_plastic = 0.0f, .f_liquid = 0.0f, .f_gas = 0.0f
            });
            node_indices.push_back(idx);
        }
        for (std::size_t i = 1; i < node_indices.size(); ++i) {
            app.world->edges().add(node_indices[i - 1], node_indices[i], 55.0f);
        }
    }

    // 3. Sublimating Dry Ice Cluster
    for (int i = 0; i < 20; ++i) {
        app.world->particles().add({
            .position = pebble::math::vec2(FW - 160.0f + float(i % 5) * 16.0f, 80.0f + float(i / 5) * 16.0f),
            .velocity = {0.0f, 0.0f},
            .mass = 1.2f,
            .temperature = -50.0f + float(i) * 4.0f,
            .material = app.mat_dry_ice,
            .f_solid = 1.0f, .f_plastic = 0.0f, .f_liquid = 0.0f, .f_gas = 0.0f
        });
    }
}

static void init_cb() {
    auto& app = g_app;

    sg_desc gfx{};
    gfx.environment = sglue_environment();
    gfx.logger.func = slog_func;
    sg_setup(&gfx);

    app.pixels.assign(W * H, 0xFF060610u);

    struct Vert { float x, y, u, v; };
    static const Vert verts[] = {{-1, -1, 0, 1}, {1, -1, 1, 1}, {1, 1, 1, 0}, {-1, 1, 0, 0}};
    static const uint16_t idx[] = {0, 1, 2, 0, 2, 3};

    {
        sg_buffer_desc d{};
        d.data = SG_RANGE(verts);
        app.bind.vertex_buffers[0] = sg_make_buffer(d);
    }
    {
        sg_buffer_desc d{};
        d.usage.index_buffer = true;
        d.data = SG_RANGE(idx);
        app.bind.index_buffer = sg_make_buffer(d);
    }
    {
        sg_image_desc d{};
        d.width = W;
        d.height = H;
        d.pixel_format = SG_PIXELFORMAT_RGBA8;
        d.usage.stream_update = true;
        app.tex_img = sg_make_image(d);
    }
    {
        sg_view_desc d{};
        d.texture.image = app.tex_img;
        app.tex_view = sg_make_view(d);
    }
    {
        sg_sampler_desc d{};
        d.min_filter = SG_FILTER_NEAREST;
        d.mag_filter = SG_FILTER_NEAREST;
        app.smp = sg_make_sampler(d);
    }
    app.bind.views[0] = app.tex_view;
    app.bind.samplers[0] = app.smp;

    sg_shader_desc shd{};
#if defined(SOKOL_METAL)
    shd.vertex_func.source = VS_METAL;
    shd.vertex_func.entry = "vs";
    shd.fragment_func.source = FS_METAL;
    shd.fragment_func.entry = "fs";
#endif
    shd.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    shd.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    shd.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    shd.texture_sampler_pairs[0].view_slot = 0;
    shd.texture_sampler_pairs[0].sampler_slot = 0;

    sg_shader shdr = sg_make_shader(shd);

    sg_pipeline_desc pd{};
    pd.shader = shdr;
    pd.index_type = SG_INDEXTYPE_UINT16;
    pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
    app.pip = sg_make_pipeline(pd);

    app.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    app.pass_action.colors[0].clear_value = {0.0f, 0.0f, 0.0f, 1.0f};
    app.canvas = std::make_unique<kalpana::Canvas<kalpana::capture_backend>>(W, H);

    init_prakriti_world();
}

static void build_scene(kalpana::Scene& scene) {
    auto& app = g_app;
    scene.clear_color(kalpana::Color{0.02f, 0.03f, 0.06f, 1.0f});

    if (!app.world) return;
    auto& pw = *app.world;
    const auto& P = pw.particles();
    const auto& E = pw.edges();

    // 1. Render XPBD Elastic Bonds
    for (std::size_t e = 0; e < E.size(); ++e) {
        if (!E.active[e]) continue;
        auto ia = E.a[e];
        auto ib = E.b[e];
        float x0 = P.pos_x[ia];
        float y0 = P.pos_y[ia];
        float x1 = P.pos_x[ib];
        float y1 = P.pos_y[ib];

        float strain_val = std::clamp(std::abs(E.strain[e]) * 20.0f, 0.0f, 1.0f);
        kalpana::Color bond_col = kalpana::spectral::mix(
            kalpana::Color{0.6f, 0.8f, 1.0f, 0.85f},
            kalpana::Color{1.0f, 0.2f, 0.2f, 0.95f},
            strain_val
        );

        kalpana::Path bond;
        bond.move_to(x0, y0);
        bond.line_to(x1, y1);
        scene.add(kalpana::Node::shape(bond, kalpana::Paint::stroke(bond_col, 4.0f)));
    }

    // 2. Render Continuum Particles (Fluids & Phases)
    for (prakriti::Index i = 0; i < P.size(); ++i) {
        float x = P.pos_x[i];
        float y = P.pos_y[i];

        kalpana::Color pcol;
        float pr = 5.5f;

        if (P.f_gas[i] > 0.3f) {
            pcol = kalpana::Color{0.85f, 0.9f, 1.0f, 0.4f * P.f_gas[i]};
            pr = 8.0f;
        } else if (P.f_liquid[i] > 0.5f) {
            float heat = std::clamp((P.temperature[i] - 20.0f) / 100.0f, 0.0f, 1.0f);
            pcol = kalpana::spectral::mix(kalpana::Color{0.0f, 0.7f, 1.0f, 0.85f}, kalpana::Color{1.0f, 0.35f, 0.05f, 0.9f}, heat);
            pr = 6.0f;
        } else {
            if (P.material[i] == app.mat_dry_ice) {
                pcol = kalpana::Color{0.92f, 0.95f, 1.0f, 0.95f};
                pr = 6.5f;
            } else {
                pcol = kalpana::Color{0.8f, 0.85f, 0.95f, 1.0f};
                pr = 5.0f;
            }
        }

        kalpana::Path pdot;
        pdot.circle(x, y, pr);
        scene.add(kalpana::Node::shape(pdot, kalpana::Paint::fill(pcol)));
    }
}

static void frame_cb() {
    auto& app = g_app;
    app.frame++;
    app.t += DT;

    if (app.world) {
        app.world->step();
    }

    kalpana::Scene scene;
    build_scene(scene);
    app.canvas->render(scene);
    auto snap = app.canvas->snapshot();

    for (std::size_t i = 0; i < snap.size(); ++i) {
        std::uint32_t argb = snap[i];
        std::uint8_t a = (argb >> 24) & 0xFF;
        std::uint8_t r = (argb >> 16) & 0xFF;
        std::uint8_t g = (argb >> 8) & 0xFF;
        std::uint8_t b = argb & 0xFF;
        app.pixels[i] = (std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) | (std::uint32_t(g) << 8) | r;
    }

    sg_image_data imgd{};
    imgd.mip_levels[0] = {app.pixels.data(), app.pixels.size() * sizeof(std::uint32_t)};
    sg_update_image(app.tex_img, imgd);

    sg_pass pass{};
    pass.action = app.pass_action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(pass);
    sg_apply_pipeline(app.pip);
    sg_apply_bindings(app.bind);
    sg_draw(0, 6, 1);
    sg_end_pass();
    sg_commit();
}

static void event_cb(const sapp_event* ev) {
    if (ev->type == SAPP_EVENTTYPE_KEY_DOWN) {
        switch (ev->key_code) {
            case SAPP_KEYCODE_ESCAPE:
                sapp_quit();
                break;
            case SAPP_KEYCODE_R:
            case SAPP_KEYCODE_SPACE:
                init_prakriti_world();
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
    d.window_title = "Pebble Prakriti Multiphysics & Fluids Showcase [R]/[SPC] reset";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}
