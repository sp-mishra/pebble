// ============================================================================
// src/app/pebble_rekha.cpp - Rekha + Kalpana windowed showcase
// ============================================================================
// Opens a native Sokol window and renders Rekha plots through Kalpana's
// sokol_backend tessellation path.
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

#include "kalpana/kalpana.hpp"
#include "rekha/rekha.hpp"

#include <cmath>
#include <memory>
#include <utility>

namespace {

constexpr int W = 1200;
constexpr int H = 760;
constexpr float DT = 1.0f / 60.0f;

const char* VS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct In  { float2 pos [[attribute(0)]]; float4 col [[attribute(1)]]; };\n"
    "struct Out { float4 pos [[position]]; float4 col; };\n"
    "vertex Out vs(In in [[stage_in]]) { Out o; o.pos=float4(in.pos,0,1); o.col=in.col; return o; }\n";

const char* FS_METAL =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct In { float4 col; };\n"
    "fragment float4 fs(In in [[stage_in]]) { return in.col; }\n";

const char* VS_GLSL =
    "#version 330\nlayout(location=0) in vec2 pos; layout(location=1) in vec4 col;\n"
    "out vec4 v_col; void main(){ v_col=col; gl_Position=vec4(pos,0,1); }\n";

const char* FS_GLSL =
    "#version 330\nin vec4 v_col; out vec4 c;\n"
    "void main(){ c=v_col; }\n";

struct RekhaApp {
    sg_pipeline pip{};
    sg_bindings bind{};
    sg_pass_action pass_action{};
    sg_buffer vbuf{};
    sg_buffer ibuf{};

    std::unique_ptr<kalpana::Canvas<kalpana::sokol_backend>> canvas;

    rekha::Graph graph{};
    rekha::GraphLayoutRuntime<> layout_runtime{gati::ClockConfig{.hz = 60.0f}};
    float t = 0.0f;

    bool panel_signal = true;
    bool panel_graph = true;
    bool panel_heatmap = true;
    bool panel_dist = true;
    bool legend_enabled = false;
    bool legend_auto = true;
};

RekhaApp g_app{};

rekha::Graph remap_graph_to_panel(const rekha::Graph& in,
                                  float x0, float y0, float x1, float y1) {
    rekha::Graph out = in;
    if (out.nodes.empty()) return out;

    float min_x = out.nodes[0].x;
    float max_x = out.nodes[0].x;
    float min_y = out.nodes[0].y;
    float max_y = out.nodes[0].y;
    for (const auto& n : out.nodes) {
        min_x = std::min(min_x, n.x);
        max_x = std::max(max_x, n.x);
        min_y = std::min(min_y, n.y);
        max_y = std::max(max_y, n.y);
    }

    const float sx = (max_x > min_x) ? ((x1 - x0) / (max_x - min_x)) : 1.0f;
    const float sy = (max_y > min_y) ? ((y1 - y0) / (max_y - min_y)) : 1.0f;
    for (auto& n : out.nodes) {
        n.x = x0 + (n.x - min_x) * sx;
        n.y = y0 + (n.y - min_y) * sy;
    }
    return out;
}

rekha::Figure build_figure(const RekhaApp& app) {
    const float t = app.t;
    const auto& graph = app.graph;

    rekha::XYSeries wave("latency_wave");
    wave.stroke({kalpana::colors::cyan(), 2.0f});
    wave.marker({kalpana::colors::white(), 2.0f});
    for (int i = 0; i < 64; ++i) {
        const float x = static_cast<float>(i) * 1.05f + 4.0f;
        const float y = 62.0f + std::sin(x * 0.12f + t * 2.0f) * 12.0f;
        wave.add(x, y);
    }

    rekha::XYSeries bars("throughput");
    bars.stroke({kalpana::colors::coral(), 1.0f});
    for (int i = 0; i < 12; ++i) {
        const auto fi = static_cast<float>(i);
        const float x = 8.0f + fi * 6.2f;
        const float y = 16.0f + std::fabs(std::sin(t + fi * 0.37f)) * 20.0f;
        bars.add(x, y);
    }

    rekha::XYSeries step("state");
    step.stroke({kalpana::Color{0.95f, 0.85f, 0.25f, 1.0f}, 1.8f});
    for (int i = 0; i < 10; ++i) {
        const auto fi = static_cast<float>(i);
        step.add(6.0f + fi * 7.0f, 40.0f + std::fmod(fi * 11.0f, 28.0f));
    }

    rekha::XYSeries stems("events");
    stems.stroke({kalpana::Color{0.95f, 0.35f, 0.8f, 0.95f}, 1.0f});
    stems.marker({kalpana::Color{0.95f, 0.35f, 0.8f, 1.0f}, 1.4f});
    for (int i = 0; i < 9; ++i) {
        const auto fi = static_cast<float>(i);
        stems.add(10.0f + fi * 8.0f, 12.0f + std::fabs(std::sin(t * 0.6f + fi)) * 14.0f);
    }

    rekha::BubblePlot bubbles;
    for (int i = 0; i < 8; ++i) {
        const auto fi = static_cast<float>(i);
        bubbles.points.push_back(rekha::BubblePoint{
            .x = 12.0f + fi * 10.0f,
            .y = 74.0f + std::sin(t * 0.9f + fi * 0.8f) * 10.0f,
            .r = 2.5f + std::fabs(std::sin(t * 1.3f + fi)) * 3.5f,
            .color = kalpana::Color{0.25f + 0.07f * fi, 0.55f, 1.0f - 0.08f * fi, 1.0f}
        });
    }

    const rekha::Graph graph_panel = remap_graph_to_panel(graph, 8.0f, 8.0f, 92.0f, 92.0f);

    rekha::ErrorBarPlot err;
    for (int i = 0; i < 8; ++i) {
        const auto fi = static_cast<float>(i);
        const float x = 8.0f + fi * 11.0f;
        const float y = 35.0f + std::sin(t + fi * 0.5f) * 16.0f;
        err.points.push_back({x, y, 2.0f + std::fmod(fi, 3.0f), 3.0f + std::fmod(fi, 2.0f)});
    }
    err.stroke = {kalpana::Color{0.95f, 0.9f, 0.25f, 1.0f}, 1.2f};
    err.marker = {kalpana::colors::white(), 2.0f};

    rekha::HeatmapPlot heat;
    heat.rows = 10;
    heat.cols = 14;
    heat.x_extent = {0.0f, 100.0f};
    heat.y_extent = {0.0f, 100.0f};
    heat.values.resize(heat.rows * heat.cols);
    for (std::size_t r = 0; r < heat.rows; ++r) {
        for (std::size_t c = 0; c < heat.cols; ++c) {
            const float rf = static_cast<float>(r) / static_cast<float>(heat.rows);
            const float cf = static_cast<float>(c) / static_cast<float>(heat.cols);
            heat.values[r * heat.cols + c] = 0.5f + 0.5f * std::sin(t * 1.7f + rf * 4.0f + cf * 6.0f);
        }
    }

    rekha::PiePlot pie;
    pie.cx = 78.0f;
    pie.cy = 78.0f;
    pie.radius = 16.0f;
    pie.inner_radius = 6.0f;
    pie.slices = {
        {3.0f, kalpana::Color{0.18f, 0.72f, 1.0f, 1.0f}, "A"},
        {2.0f, kalpana::Color{1.0f, 0.55f, 0.22f, 1.0f}, "B"},
        {2.5f, kalpana::Color{0.78f, 0.35f, 0.98f, 1.0f}, "C"},
        {1.5f, kalpana::Color{0.45f, 0.9f, 0.45f, 1.0f}, "D"}
    };

    rekha::Figure fig;
    fig.viewport({W, H, {68.0f, 24.0f, 24.0f, 54.0f}})
       .theme(rekha::Figure::theme_dark_neon())
       .subplots(2, 2)
       .constrained_layout(true)
       .subplot_gap(26.0f, 30.0f)
       .legend(app.legend_enabled)
       .legend_auto(app.legend_auto);

    fig.select_subplot(0, 0)
       .axes({
           "time", "signal", 6, 0, 0, 1, 1, true, false, false,
           true, true, {0.0f, 100.0f}, {0.0f, 100.0f}
       });
    if (app.panel_signal) {
        fig.add(rekha::AreaPlot{wave, 50.0f, 0.16f})
           .add(rekha::LinePlot{std::move(wave)})
           .add(rekha::StepPlot{std::move(step)})
           .add(rekha::StemPlot{std::move(stems), 10.0f});
    } else {
        fig.add(rekha::LinePlot{rekha::XYSeries{"off"}.add(10.0f, 50.0f).add(90.0f, 50.0f)});
    }

    fig.select_subplot(0, 1)
       .axes({
           "graph-x", "graph-y", 5, 0, 0, 0, 0, true, false, false,
           true, true, {0.0f, 100.0f}, {0.0f, 100.0f}
       });
    if (app.panel_graph) {
        fig.add(std::move(bubbles))
           .add(rekha::GraphPlot{graph_panel,
                                 rekha::StrokeStyle{kalpana::Color{0.75f, 0.8f, 1.0f, 0.82f}, 1.0f},
                                 rekha::MarkerStyle{kalpana::colors::yellow(), 3.0f},
                                 true,
                                 3.2f});
    } else {
        fig.add(rekha::ScatterPlot{rekha::XYSeries{"off"}.add(50.0f, 50.0f).marker({kalpana::colors::white(), 2.0f})});
    }

    fig.select_subplot(1, 0)
       .axes({
           "grid-x", "grid-y", 5, 0, 0, 0, 0, true, false, false,
           true, true, {0.0f, 100.0f}, {0.0f, 100.0f}
       });
    if (app.panel_heatmap) {
        fig.add(std::move(heat));
    } else {
        fig.add(rekha::ScatterPlot{rekha::XYSeries{"off"}.add(50.0f, 50.0f).marker({kalpana::colors::white(), 2.0f})});
    }

    fig.select_subplot(1, 1)
       .axes({
           "sample", "distribution", 5, 0, 0, 1, 1, true, false, false,
           true, true, {0.0f, 100.0f}, {0.0f, 100.0f}
       });
    if (app.panel_dist) {
        fig.add(rekha::BarPlot{std::move(bars), 0.9f})
           .add(std::move(err))
           .add(std::move(pie));
    } else {
        fig.add(rekha::ScatterPlot{rekha::XYSeries{"off"}.add(50.0f, 50.0f).marker({kalpana::colors::white(), 2.0f})});
    }

    return fig;
}

void init_graph() {
    auto& g = g_app.graph;
    g.edges = {
        {0, 1, 1.0f}, {1, 2, 1.0f}, {2, 3, 1.0f}, {3, 4, 1.0f}, {4, 5, 1.0f},
        {5, 0, 1.0f}, {0, 3, 0.8f}, {1, 4, 0.8f}, {2, 5, 0.8f}
    };

    auto& cfg = g_app.layout_runtime.layout().config();
    cfg.iterations = 3;
    cfg.temperature = 9.0f;
    cfg.cooling = 0.97f;
    g_app.layout_runtime.layout().initialize(g, 260, 180, 17);
}

void init_cb() {
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

    sg_shader_desc shd{};
#if defined(SOKOL_METAL)
    shd.vertex_func.source = VS_METAL;
    shd.vertex_func.entry = "vs";
    shd.fragment_func.source = FS_METAL;
    shd.fragment_func.entry = "fs";
#else
    shd.vertex_func.source = VS_GLSL;
    shd.fragment_func.source = FS_GLSL;
#endif

    sg_shader shdr = sg_make_shader(shd);

    sg_pipeline_desc pd{};
    pd.shader = shdr;
    pd.index_type = SG_INDEXTYPE_UINT32;
    pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT4;
    app.pip = sg_make_pipeline(pd);

    app.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    app.pass_action.colors[0].clear_value = {0.03f, 0.04f, 0.07f, 1.0f};

    app.canvas = std::make_unique<kalpana::Canvas<kalpana::sokol_backend>>(W, H);
    init_graph();
}

void frame_cb() {
    auto& app = g_app;
    app.t += DT;

    app.layout_runtime.update(app.graph, 260, 180, DT);

    const rekha::Figure fig = build_figure(app);
    rekha::KalpanaBackend rekha_backend;
    fig.render(rekha_backend);
    app.canvas->render(rekha_backend.scene());

    const auto& verts = app.canvas->backend().vertices();
    const auto& indices = app.canvas->backend().indices();

    if (!verts.empty() && !indices.empty()) {
        sg_range vr{verts.data(), verts.size() * sizeof(kalpana::sokol_backend::Vertex)};
        sg_update_buffer(app.vbuf, vr);

        sg_range ir{indices.data(), indices.size() * sizeof(std::uint32_t)};
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

void event_cb(const sapp_event* ev) {
    if (ev->type != SAPP_EVENTTYPE_KEY_DOWN) return;

    switch (ev->key_code) {
        case SAPP_KEYCODE_ESCAPE:
            sapp_quit();
            break;
        case SAPP_KEYCODE_1:
            g_app.panel_signal = !g_app.panel_signal;
            break;
        case SAPP_KEYCODE_2:
            g_app.panel_graph = !g_app.panel_graph;
            break;
        case SAPP_KEYCODE_3:
            g_app.panel_heatmap = !g_app.panel_heatmap;
            break;
        case SAPP_KEYCODE_4:
            g_app.panel_dist = !g_app.panel_dist;
            break;
        case SAPP_KEYCODE_0:
            g_app.panel_signal = true;
            g_app.panel_graph = true;
            g_app.panel_heatmap = true;
            g_app.panel_dist = true;
            break;
        case SAPP_KEYCODE_L:
            g_app.legend_enabled = !g_app.legend_enabled;
            break;
        case SAPP_KEYCODE_A:
            g_app.legend_auto = !g_app.legend_auto;
            break;
        case SAPP_KEYCODE_R:
            init_graph();
            break;
        default:
            break;
    }
}

void cleanup_cb() {
    sg_shutdown();
}

} // namespace

sapp_desc sokol_main(int /*argc*/, char** /*argv*/) {
    sapp_desc d{};
    d.init_cb = init_cb;
    d.frame_cb = frame_cb;
    d.event_cb = event_cb;
    d.cleanup_cb = cleanup_cb;
    d.width = W;
    d.height = H;
    d.window_title = "Rekha Dashboard [1..4 toggle panels | 0 all | L legend | A auto-legend | R reset graph | ESC quit]";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}

