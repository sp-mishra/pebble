// ============================================================================
// src/app/pebble_akriti.cpp — Akruti Geometry & Kalpana Vector FX Showcase
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

#include "akruti/akruti.hpp"
#include "akruti/primitives.hpp"
#include "akruti/spline.hpp"
#include "akruti/hull.hpp"
#include "akruti/csg.hpp"
#include "akruti/fracture.hpp"
#include "kalpana/kalpana.hpp"
#include "kalpana/backend/capture_backend.hpp"

#include <array>
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

static constexpr int W = 1100;
static constexpr int H = 720;
static constexpr float FW = float(W);
static constexpr float FH = float(H);
static constexpr float DT = 1.0f / 60.0f;

struct AkritiApp {
    sg_pipeline pip{};
    sg_bindings bind{};
    sg_pass_action pass_action{};
    sg_image tex_img{};
    sg_view tex_view{};
    sg_sampler smp{};
    std::vector<std::uint32_t> pixels;
    std::unique_ptr<kalpana::Canvas<kalpana::capture_backend>> canvas;

    float t = 0.0f;
    int frame = 0;
    akruti::CsgPtr csg_field;
    akruti::CubicBezierCurve bezier{};
};

static AkritiApp g_app;

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

    // CSG Smooth Union
    auto c = akruti::csg_leaf(akruti::Circle{{0.0f, 0.0f}, 35.0f});
    auto b = akruti::csg_leaf(akruti::RoundedBox{{0.0f, 0.0f}, {28.0f, 20.0f}, 6.0f});
    app.csg_field = akruti::csg_smooth_union(std::move(c), std::move(b), 12.0f);

    app.bezier = akruti::CubicBezierCurve{
        .p0 = {60.0f, 620.0f},
        .p1 = {300.0f, 480.0f},
        .p2 = {800.0f, 700.0f},
        .p3 = {1040.0f, 540.0f},
        .radius = 3.0f
    };
}

static void build_scene(kalpana::Scene& scene) {
    auto& app = g_app;
    scene.clear_color(kalpana::Color{0.03f, 0.04f, 0.08f, 1.0f});

    // 1. Grid Background
    kalpana::Color grid_col{0.08f, 0.11f, 0.18f, 1.0f};
    for (int x = 0; x <= W; x += 40) {
        kalpana::Path l;
        l.move_to(float(x), 0.0f);
        l.line_to(float(x), FH);
        scene.add(kalpana::Node::shape(l, kalpana::Paint::stroke(grid_col, 1.0f)));
    }
    for (int y = 0; y <= H; y += 40) {
        kalpana::Path l;
        l.move_to(0.0f, float(y));
        l.line_to(FW, float(y));
        scene.add(kalpana::Node::shape(l, kalpana::Paint::stroke(grid_col, 1.0f)));
    }

    // 2. Showcase Akruti Primitive Shapes across grid cards with Kalpana vector styling
    struct Card {
        const char* name;
        float cx, cy;
    };
    std::array<Card, 8> cards{{
        {"Circle (Analytic)", 160.0f, 130.0f},
        {"Box / OBB (SAT)", 420.0f, 130.0f},
        {"Capsule (Segment Inflated)", 680.0f, 130.0f},
        {"Rounded Box", 940.0f, 130.0f},
        {"Triangle (Barycentric)", 160.0f, 340.0f},
        {"Sector (Radar FOV)", 420.0f, 340.0f},
        {"Convex Hull Polygon", 680.0f, 340.0f},
        {"CSG Smooth Union SDF", 940.0f, 340.0f}
    }};

    const float rot = app.t * 0.8f;

    // Card 0: Circle with spectral glow
    {
        kalpana::Path p;
        p.circle(cards[0].cx, cards[0].cy, 45.0f);
        kalpana::Color c1{0.0f, 0.85f, 1.0f, 1.0f};
        kalpana::Color c2{0.0f, 0.2f, 0.9f, 0.3f};
        kalpana::Path glow;
        glow.circle(cards[0].cx, cards[0].cy, 60.0f);
        scene.add(kalpana::Node::shape(glow, kalpana::Paint::fill(c2)));
        scene.add(kalpana::Node::shape(p, kalpana::Paint::filled_outlined(c1, kalpana::Color{1,1,1,1}, 2.5f)));
    }

    // Card 1: Oriented Box (OBB)
    {
        const float s = 40.0f;
        const float c = std::cos(rot), sn = std::sin(rot);
        auto pt = [&](float lx, float ly) {
            return std::pair<float, float>{cards[1].cx + lx * c - ly * sn, cards[1].cy + lx * sn + ly * c};
        };
        auto [x0, y0] = pt(-s, -s * 0.7f);
        auto [x1, y1] = pt(s, -s * 0.7f);
        auto [x2, y2] = pt(s, s * 0.7f);
        auto [x3, y3] = pt(-s, s * 0.7f);

        kalpana::Path p;
        p.move_to(x0, y0); p.line_to(x1, y1); p.line_to(x2, y2); p.line_to(x3, y3); p.close();
        kalpana::Color col = kalpana::spectral::mix(kalpana::Color{1.0f, 0.2f, 0.6f, 0.9f}, kalpana::Color{1.0f, 0.8f, 0.2f, 0.9f}, 0.5f + 0.5f * std::sin(app.t));
        scene.add(kalpana::Node::shape(p, kalpana::Paint::filled_outlined(col, kalpana::Color{1,1,1,1}, 2.0f)));
    }

    // Card 2: Oriented Capsule
    {
        const float cap_len = 35.0f;
        const float cap_r = 22.0f;
        const float c = std::cos(rot + 0.5f), sn = std::sin(rot + 0.5f);
        const float ax = cards[2].cx - cap_len * c, ay = cards[2].cy - cap_len * sn;
        const float bx = cards[2].cx + cap_len * c, by = cards[2].cy + cap_len * sn;
        const float nx = -sn * cap_r, ny = c * cap_r;

        kalpana::Path p;
        p.move_to(ax + nx, ay + ny);
        p.line_to(bx + nx, by + ny);
        p.line_to(bx - nx, by - ny);
        p.line_to(ax - nx, ay - ny);
        p.close();

        kalpana::Color col{0.2f, 1.0f, 0.4f, 0.85f};
        scene.add(kalpana::Node::shape(p, kalpana::Paint::filled_outlined(col, kalpana::Color{0.9f,1,0.9f,1}, 2.0f)));
    }

    // Card 3: Rounded Box
    {
        kalpana::Path p;
        p.round_rect(cards[3].cx - 42.0f, cards[3].cy - 35.0f, 84.0f, 70.0f, 16.0f, 16.0f);
        kalpana::Color col{1.0f, 0.45f, 0.1f, 0.85f};
        scene.add(kalpana::Node::shape(p, kalpana::Paint::filled_outlined(col, kalpana::Color{1,0.8f,0.6f,1}, 2.5f)));
    }

    // Card 4: Rotating Triangle
    {
        const float s = 45.0f;
        kalpana::Path p;
        p.move_to(cards[4].cx + std::cos(rot) * s, cards[4].cy + std::sin(rot) * s);
        p.line_to(cards[4].cx + std::cos(rot + 2.0944f) * s, cards[4].cy + std::sin(rot + 2.0944f) * s);
        p.line_to(cards[4].cx + std::cos(rot + 4.1888f) * s, cards[4].cy + std::sin(rot + 4.1888f) * s);
        p.close();

        kalpana::Color col{0.9f, 0.1f, 0.9f, 0.85f};
        scene.add(kalpana::Node::shape(p, kalpana::Paint::filled_outlined(col, kalpana::Color{1,0.8f,1,1}, 2.0f)));
    }

    // Card 5: Radar Sector FOV
    {
        kalpana::Path p;
        p.move_to(cards[5].cx, cards[5].cy);
        const float a0 = rot - 0.75f;
        const float a1 = rot + 0.75f;
        constexpr int kArc = 16;
        for (int i = 0; i <= kArc; ++i) {
            float frac = float(i) / float(kArc);
            float a = a0 + (a1 - a0) * frac;
            p.line_to(cards[5].cx + std::cos(a) * 55.0f, cards[5].cy + std::sin(a) * 55.0f);
        }
        p.close();

        kalpana::Color col{0.1f, 0.8f, 1.0f, 0.75f};
        scene.add(kalpana::Node::shape(p, kalpana::Paint::filled_outlined(col, kalpana::Color{0.7f,1,1,1}, 2.0f)));
    }

    // Card 6: Procedural Convex Hull
    {
        constexpr int kVerts = 8;
        kalpana::Path p;
        for (int i = 0; i < kVerts; ++i) {
            float ang = float(i) / float(kVerts) * 6.28318f + rot * 0.5f;
            float rad = 35.0f + 12.0f * std::sin(float(i) * 2.4f + app.t * 2.0f);
            float vx = cards[6].cx + std::cos(ang) * rad;
            float vy = cards[6].cy + std::sin(ang) * rad;
            if (i == 0) p.move_to(vx, vy);
            else p.line_to(vx, vy);
        }
        p.close();

        kalpana::Color col{0.95f, 0.85f, 0.1f, 0.85f};
        scene.add(kalpana::Node::shape(p, kalpana::Paint::filled_outlined(col, kalpana::Color{1,1,0.7f,1}, 2.0f)));
    }

    // Card 7: CSG Smooth Union
    {
        kalpana::Path c1, c2;
        float shift = std::sin(app.t * 2.5f) * 18.0f;
        c1.circle(cards[7].cx - shift, cards[7].cy, 28.0f);
        c2.round_rect(cards[7].cx + shift - 25.0f, cards[7].cy - 20.0f, 50.0f, 40.0f, 8.0f, 8.0f);

        kalpana::Color col1{0.2f, 0.7f, 1.0f, 0.6f};
        kalpana::Color col2{1.0f, 0.3f, 0.5f, 0.6f};
        scene.add(kalpana::Node::shape(c1, kalpana::Paint::fill(col1)));
        scene.add(kalpana::Node::shape(c2, kalpana::Paint::fill(col2)));
    }

    // 3. Bottom Showcase: Cubic Bezier Spline with multi-width variable strokes
    {
        kalpana::Path curve;
        constexpr int kSamples = 60;
        for (int i = 0; i <= kSamples; ++i) {
            float t = float(i) / float(kSamples);
            auto pt = app.bezier.evaluate(t);
            if (i == 0) curve.move_to(pt.x, pt.y);
            else curve.line_to(pt.x, pt.y);
        }
        kalpana::Color spline_col = kalpana::spectral::mix(
            kalpana::Color{0.0f, 1.0f, 0.8f, 1.0f},
            kalpana::Color{1.0f, 0.1f, 0.9f, 1.0f},
            0.5f + 0.5f * std::sin(app.t * 1.5f)
        );
        scene.add(kalpana::Node::shape(curve, kalpana::Paint::stroke(spline_col, 4.0f)));
    }
}

static void frame_cb() {
    auto& app = g_app;
    app.frame++;
    app.t += DT;

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
        if (ev->key_code == SAPP_KEYCODE_ESCAPE) {
            sapp_quit();
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
    d.window_title = "Pebble Akruti & Kalpana Showcase [ESC] quit";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}
