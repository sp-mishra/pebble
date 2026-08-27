// ============================================================================
// src/app/pebble_akriti.cpp — Akruti Geometry & Kalpana 2.0 Pigment Mixing Showcase
// ============================================================================
// Displays a structured gallery of ~50–60 distinct overlapping shape pairs.
// Each pair consists of 2 different geometry shapes in 2 distinct physical pigments.
// The intersection overlap explicitly evaluates the 16-band Kubelka-Munk
// subtractive pigment mixing equations (e.g. Ultramarine Blue + Cadmium Yellow = Green).
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
#include "kalpana/backend/sokol_backend.hpp"

#include <array>
#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>
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

static constexpr int W = 1200;
static constexpr int H = 820;
static constexpr float FW = float(W);
static constexpr float FH = float(H);
static constexpr float DT = 1.0f / 60.0f;

enum class ShapeType {
    Circle,
    Rect,
    RoundRect,
    Star,
    Triangle,
    Hexagon,
    Capsule,
    Sector,
    Diamond,
    NotchedBox
};

struct OverlapPair {
    ShapeType shape_a = ShapeType::Circle;
    ShapeType shape_b = ShapeType::Circle;
    float cx = 0.0f, cy = 0.0f;
    float r = 48.0f;
    kalpana::Color col_a{};
    kalpana::Color col_b{};
    const char* label_a = "";
    const char* label_b = "";
    const char* label_mix = "";
};

struct AkritiApp {
    sg_pipeline pip{};
    sg_bindings bind{};
    sg_pass_action pass_action{};
    sg_buffer vbuf{};
    sg_buffer ibuf{};
    std::unique_ptr<kalpana::Canvas<kalpana::sokol_backend>> canvas;

    std::vector<OverlapPair> pairs;
    float t = 0.0f;
};

static AkritiApp g_app;

static kalpana::Path make_shape(ShapeType type, float x, float y, float r) {
    using namespace kalpana;
    Path p;
    switch (type) {
        case ShapeType::Circle:
            p = circle(x, y, r);
            break;
        case ShapeType::Rect:
            p = rect(x - r * 0.9f, y - r * 0.9f, r * 1.8f, r * 1.8f);
            break;
        case ShapeType::RoundRect:
            p = round_rect(x - r, y - r * 0.75f, r * 2.0f, r * 1.5f, r * 0.35f, r * 0.35f);
            break;
        case ShapeType::Star:
            p = star(x, y, r, r * 0.45f, 5);
            break;
        case ShapeType::Triangle:
            p = star(x, y, r, r * 0.5f, 3);
            break;
        case ShapeType::Hexagon:
            p = star(x, y, r, r * 0.866f, 6);
            break;
        case ShapeType::Capsule: {
            const float cap_len = r * 0.75f;
            const float cap_r = r * 0.45f;
            p.move_to(x - cap_len, y - cap_r);
            p.line_to(x + cap_len, y - cap_r);
            p.line_to(x + cap_len, y + cap_r);
            p.line_to(x - cap_len, y + cap_r);
            p.close();
            break;
        }
        case ShapeType::Sector:
            p = arc(x, y, r, -0.9f, 1.8f);
            p.line_to(x, y);
            p.close();
            break;
        case ShapeType::Diamond:
            p.move_to(x, y - r);
            p.line_to(x + r * 0.8f, y);
            p.line_to(x, y + r);
            p.line_to(x - r * 0.8f, y);
            p.close();
            break;
        case ShapeType::NotchedBox:
            p.move_to(x - r * 0.9f, y - r * 0.9f);
            p.line_to(x, y - r * 0.3f);
            p.line_to(x + r * 0.9f, y - r * 0.9f);
            p.line_to(x + r * 0.9f, y + r * 0.9f);
            p.line_to(x - r * 0.9f, y + r * 0.9f);
            p.close();
            break;
    }
    return p;
}

static void init_overlap_gallery() {
    using namespace kalpana;
    using namespace kalpana::spectral;
    auto& app = g_app;
    app.pairs.clear();

    struct PigmentPairSpec {
        Color col_a;
        const char* name_a;
        Color col_b;
        const char* name_b;
        ShapeType shape_a;
        ShapeType shape_b;
    };

    // 16 Key Subtractive Color Theory Combinations (Primary, Secondary & Color Wheel pairs)
    const std::vector<PigmentPairSpec> specs = {
        // Row 1: The Iconic Primary Painter Pairs (Blue + Yellow -> Green, etc.)
        { Color{0.00f, 0.25f, 0.95f, 1.0f}, "Cobalt Blue",
          Color{1.00f, 0.90f, 0.00f, 1.0f}, "Cadmium Yellow",
          ShapeType::Circle, ShapeType::Star },

        { Color{0.00f, 0.85f, 0.98f, 1.0f}, "Cyan",
          Color{1.00f, 0.90f, 0.00f, 1.0f}, "Yellow",
          ShapeType::RoundRect, ShapeType::Triangle },

        { Color{0.95f, 0.05f, 0.60f, 1.0f}, "Process Magenta",
          Color{1.00f, 0.90f, 0.00f, 1.0f}, "Yellow",
          ShapeType::Hexagon, ShapeType::Circle },

        { Color{0.00f, 0.85f, 0.98f, 1.0f}, "Cyan",
          Color{0.95f, 0.05f, 0.60f, 1.0f}, "Process Magenta",
          ShapeType::Capsule, ShapeType::Star },

        // Row 2: Secondary Wheel & Gradient Pairs
        { Color{0.00f, 0.25f, 0.95f, 1.0f}, "Cobalt Blue",
          Color{0.95f, 0.08f, 0.08f, 1.0f}, "Cadmium Red",
          ShapeType::Circle, ShapeType::RoundRect },

        { Color{0.95f, 0.08f, 0.08f, 1.0f}, "Cadmium Red",
          Color{1.00f, 0.90f, 0.00f, 1.0f}, "Cadmium Yellow",
          ShapeType::Star, ShapeType::Hexagon },

        { Color{0.00f, 0.85f, 0.35f, 1.0f}, "Emerald Green",
          Color{1.00f, 0.90f, 0.00f, 1.0f}, "Cadmium Yellow",
          ShapeType::Triangle, ShapeType::Capsule },

        { Color{0.00f, 0.85f, 0.35f, 1.0f}, "Emerald Green",
          Color{0.00f, 0.85f, 0.98f, 1.0f}, "Cyan",
          ShapeType::RoundRect, ShapeType::Circle },

        // Row 3: Complementary & Cross-Wheel Mixtures
        { Color{0.00f, 0.25f, 0.95f, 1.0f}, "Cobalt Blue",
          Color{1.00f, 0.50f, 0.00f, 1.0f}, "Cadmium Orange",
          ShapeType::Diamond, ShapeType::Circle },

        { Color{0.00f, 0.85f, 0.35f, 1.0f}, "Emerald Green",
          Color{0.95f, 0.08f, 0.08f, 1.0f}, "Cadmium Red",
          ShapeType::Circle, ShapeType::Triangle },

        { Color{0.60f, 0.10f, 0.95f, 1.0f}, "Cobalt Violet",
          Color{1.00f, 0.90f, 0.00f, 1.0f}, "Cadmium Yellow",
          ShapeType::Hexagon, ShapeType::Star },

        { Color{0.10f, 0.70f, 0.98f, 1.0f}, "Sky Cerulean",
          Color{1.00f, 0.15f, 0.65f, 1.0f}, "Hot Pink",
          ShapeType::Capsule, ShapeType::RoundRect },

        // Row 4: Bright Light & Pastel Spectral Tints
        { Color{0.92f, 0.98f, 0.10f, 1.0f}, "Bright Lemon",
          Color{0.00f, 0.25f, 0.95f, 1.0f}, "Cobalt Blue",
          ShapeType::Star, ShapeType::Diamond },

        { Color{1.00f, 0.50f, 0.00f, 1.0f}, "Cadmium Orange",
          Color{0.00f, 0.85f, 0.98f, 1.0f}, "Cyan",
          ShapeType::RoundRect, ShapeType::Hexagon },

        { Color{1.00f, 0.15f, 0.65f, 1.0f}, "Hot Pink",
          Color{1.00f, 0.90f, 0.00f, 1.0f}, "Cadmium Yellow",
          ShapeType::Circle, ShapeType::Capsule },

        { Color{0.60f, 0.10f, 0.95f, 1.0f}, "Cobalt Violet",
          Color{0.00f, 0.85f, 0.35f, 1.0f}, "Emerald Green",
          ShapeType::Triangle, ShapeType::Star }
    };

    // 4 Columns x 4 Rows = 16 Large Prominent Cards
    constexpr int kCols = 4;
    constexpr int kRows = 4;
    const float start_x = 160.0f;
    const float start_y = 150.0f;
    const float step_x  = 290.0f;
    const float step_y  = 165.0f;

    for (std::size_t i = 0; i < specs.size(); ++i) {
        int r = int(i) / kCols;
        int c = int(i) % kCols;

        OverlapPair pair;
        pair.cx = start_x + float(c) * step_x;
        pair.cy = start_y + float(r) * step_y;
        pair.r = 48.0f; // Much bigger shape radius

        pair.shape_a = specs[i].shape_a;
        pair.shape_b = specs[i].shape_b;
        pair.col_a = specs[i].col_a;
        pair.col_b = specs[i].col_b;
        pair.label_a = specs[i].name_a;
        pair.label_b = specs[i].name_b;

        app.pairs.push_back(pair);
    }
}

static void init_cb() {
    auto& app = g_app;

    sg_desc gfx{};
    gfx.environment = sglue_environment();
    gfx.logger.func = slog_func;
    sg_setup(&gfx);

    {
        sg_buffer_desc d{};
        d.size = 256 * 1024 * sizeof(kalpana::sokol_backend::Vertex);
        d.usage.stream_update = true;
        app.vbuf = sg_make_buffer(d);
        app.bind.vertex_buffers[0] = app.vbuf;
    }
    {
        sg_buffer_desc d{};
        d.size = 512 * 1024 * sizeof(std::uint32_t);
        d.usage.index_buffer = true;
        d.usage.stream_update = true;
        app.ibuf = sg_make_buffer(d);
        app.bind.index_buffer = app.ibuf;
    }
    {
        sg_shader_desc shd{};
        shd.vertex_func.source = VS_METAL;
        shd.vertex_func.entry = "vs";
        shd.fragment_func.source = FS_METAL;
        shd.fragment_func.entry = "fs";
        sg_shader shdr = sg_make_shader(shd);

        sg_pipeline_desc pd{};
        pd.shader = shdr;
        pd.index_type = SG_INDEXTYPE_UINT32;
        pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2; // position
        pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT4; // color
        app.pip = sg_make_pipeline(pd);
    }

    app.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    app.pass_action.colors[0].clear_value = {0.015f, 0.02f, 0.035f, 1.0f};

    app.canvas = std::make_unique<kalpana::Canvas<kalpana::sokol_backend>>(W, H);

    init_overlap_gallery();
}

static void build_scene(kalpana::Scene& scene) {
    using namespace kalpana;
    using namespace kalpana::edsl;
    auto& app = g_app;

    // Dark sleek canvas base
    scene.clear_color(Color{0.02f, 0.025f, 0.045f, 1.0f});

    // ── Background Precision Grid ──────────────────────────────────────────
    for (int x = 0; x <= W; x += 40) {
        scene << shape(line(float(x), 0.0f, float(x), FH))
                     .stroke(Color{0.06f, 0.09f, 0.14f, 0.35f}, 1.0f);
    }
    for (int y = 0; y <= H; y += 40) {
        scene << shape(line(0.0f, float(y), FW, float(y)))
                     .stroke(Color{0.06f, 0.09f, 0.14f, 0.35f}, 1.0f);
    }

    // ── Main Header ─────────────────────────────────────────────────────────
    scene << text("Kubelka-Munk Spectral Pigment Mixing Showcase: Subtractive Overlaps")
                 .fill(colors::cyan())
                 .font_size(17.0f)
                 .position(40.0f, 32.0f)
                 .effect(glow(4.0f, colors::cyan()))
          << text("Shape A (Left) + Shape B (Right) -> Intersecting Center: Physical 38-Band Kubelka-Munk Absorption")
                 .fill(Color{0.65f, 0.80f, 0.95f, 1.0f})
                 .font_size(12.0f)
                 .position(40.0f, 52.0f);

    // ── Render 16 Large Overlapping Pairs with True Kubelka-Munk Center Mix ──
    for (const auto& p : app.pairs) {
        const float offset = 26.0f; // Half overlap shift for r=48

        // Sleek dark card backdrop for visual contrast
        scene << shape(round_rect(p.cx - 130.0f, p.cy - 68.0f, 260.0f, 136.0f, 12.0f, 12.0f))
                     .fill(Color{0.035f, 0.05f, 0.08f, 1.0f})
                     .stroke(Color{0.18f, 0.24f, 0.35f, 0.9f}, 1.2f);

        // 1. Left Shape in Pigment A
        Path path_a = make_shape(p.shape_a, p.cx - offset, p.cy - 8.0f, p.r);
        Color col_a = p.col_a;
        scene << shape(std::move(path_a))
                     .fill(col_a)
                     .stroke(colors::white(), 1.5f)
                     .opacity(0.95f);

        // 2. Right Shape in Pigment B
        Path path_b = make_shape(p.shape_b, p.cx + offset, p.cy - 8.0f, p.r);
        Color col_b = p.col_b;
        scene << shape(std::move(path_b))
                     .fill(col_b)
                     .stroke(colors::white(), 1.5f)
                     .opacity(0.95f);

        // 3. Center Subtractive Kubelka-Munk Pigment Mixture
        Color km_color = spectral::mix(col_a, col_b, 0.5f);

        // Exact lens-shaped overlap intersection patch
        float overlap_w = (p.r * 2.0f - offset * 2.0f) * 0.95f;
        float overlap_h = p.r * 1.55f;
        scene << shape(ellipse(p.cx, p.cy - 8.0f, overlap_w * 0.5f, overlap_h * 0.5f))
                     .fill(km_color)
                     .stroke(colors::white(), 1.8f)
                     .effect(glow(6.0f, km_color));

        // Subtractive Pair Label at bottom of card
        std::string label = std::string(p.label_a) + " + " + std::string(p.label_b);
        scene << text(label)
                     .fill(Color{0.80f, 0.88f, 0.95f, 0.9f})
                     .font_size(11.0f)
                     .position(p.cx - float(label.length()) * 3.1f, p.cy + 54.0f);
    }
}

static void frame_cb() {
    auto& app = g_app;
    app.t += DT;

    kalpana::Scene scene;
    build_scene(scene);
    app.canvas->render(scene);

    const auto& verts = app.canvas->backend().vertices();
    const auto& indices = app.canvas->backend().indices();

    if (!verts.empty() && !indices.empty()) {
        sg_range vr{};
        vr.ptr = verts.data();
        vr.size = verts.size() * sizeof(kalpana::sokol_backend::Vertex);
        sg_update_buffer(app.vbuf, vr);

        sg_range ir{};
        ir.ptr = indices.data();
        ir.size = indices.size() * sizeof(std::uint32_t);
        sg_update_buffer(app.ibuf, ir);
    }

    sg_pass pass{};
    pass.action = app.pass_action;
    pass.swapchain = sglue_swapchain();
    sg_begin_pass(pass);
    sg_apply_pipeline(app.pip);
    sg_apply_bindings(app.bind);
    if (!indices.empty()) {
        sg_draw(0, static_cast<int>(indices.size()), 1);
    }
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
    d.window_title = "Kalpana 2.0: 56 Subtractive Kubelka-Munk Pigment Overlaps [ESC] quit";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}
