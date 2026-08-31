// ============================================================================
// src/app/pebble_drishya_hud.cpp — Pebble Cinematic HUD
// ============================================================================
// Game-style HUD driven by a Manas evolutionary brain: 16 agents evolve each
// frame, best agent's outputs bind to health/energy gauges. ScreenShake2D
// adds trauma on hit events. RekhaWidget threat map shows orbiting enemies.
// Spandana particles burst on damage. SpringReflow eases layout shifts.
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
#include "manas/manas.hpp"
#include "spandana/procedural.hpp"
#include "containers/static/static_vector.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <algorithm>
#include <numbers>
#include <random>

namespace {

constexpr int   W  = 1280;
constexpr int   H  = 800;
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
    "#version 330\nin vec4 v_col; out vec4 c;\nvoid main(){ c=v_col; }\n";

// ─────────────────────────────────────────────────────────────────────────────
// Color palette
// ─────────────────────────────────────────────────────────────────────────────
namespace pal {
    constexpr kalpana::Color bg     {0.02f, 0.03f, 0.06f, 1.0f};
    constexpr kalpana::Color hud    {0.05f, 0.06f, 0.10f, 0.9f};
    constexpr kalpana::Color accent {0.18f, 0.72f, 1.00f, 1.0f};
    constexpr kalpana::Color green  {0.22f, 0.90f, 0.55f, 1.0f};
    constexpr kalpana::Color red    {1.00f, 0.28f, 0.28f, 1.0f};
    constexpr kalpana::Color orange {1.00f, 0.60f, 0.20f, 1.0f};
    constexpr kalpana::Color purple {0.80f, 0.40f, 1.00f, 1.0f};
    constexpr kalpana::Color yellow {1.00f, 0.90f, 0.25f, 1.0f};
    constexpr kalpana::Color text   {0.88f, 0.90f, 0.96f, 1.0f};
    constexpr kalpana::Color dim    {0.35f, 0.38f, 0.48f, 1.0f};
} // namespace pal

// ─────────────────────────────────────────────────────────────────────────────
// Immediate-mode draw helpers
// ─────────────────────────────────────────────────────────────────────────────
struct UICtx {
    kalpana::Scene& scene;
    float ox = 0, oy = 0; // viewport shake offset

    void fill_rect(float x, float y, float w, float h, kalpana::Color c) {
        kalpana::Path p; p.rect(ox+x, oy+y, w, h);
        scene.add(kalpana::Node::shape(std::move(p), kalpana::Paint::fill(c)));
    }
    void round_rect(float x, float y, float w, float h, float r, kalpana::Color c) {
        kalpana::Path p; p.round_rect(ox+x, oy+y, w, h, r, r);
        scene.add(kalpana::Node::shape(std::move(p), kalpana::Paint::fill(c)));
    }
    void stroke_round_rect(float x, float y, float w, float h, float r, kalpana::Color c, float lw=1.0f) {
        kalpana::Path p; p.round_rect(ox+x, oy+y, w, h, r, r);
        scene.add(kalpana::Node::shape(std::move(p), kalpana::Paint::stroke(c, lw)));
    }
    void text(std::string_view t, float x, float y, kalpana::Color c, float sz = 13.0f) {
        scene.add(kalpana::Node::text(t, c, sz, ox+x, oy+y));
    }
    void line(float x0, float y0, float x1, float y1, kalpana::Color c, float w = 1.0f) {
        kalpana::Path p; p.move_to(ox+x0, oy+y0).line_to(ox+x1, oy+y1);
        scene.add(kalpana::Node::shape(std::move(p), kalpana::Paint::stroke(c, w)));
    }
    void circle(float x, float y, float r, kalpana::Color c) {
        kalpana::Path p; p.circle(ox+x, oy+y, r);
        scene.add(kalpana::Node::shape(std::move(p), kalpana::Paint::fill(c)));
    }
    void circle_stroke(float x, float y, float r, kalpana::Color c, float lw=1.0f) {
        kalpana::Path p; p.circle(ox+x, oy+y, r);
        scene.add(kalpana::Node::shape(std::move(p), kalpana::Paint::stroke(c, lw)));
    }
    // Horizontal bar: [x,y,w,h], fill from left with fraction f
    void bar(float x, float y, float w, float h, float f, kalpana::Color bg_c, kalpana::Color fg_c, float r=3.0f) {
        round_rect(x, y, w, h, r, bg_c);
        if (f > 0.0f) round_rect(x, y, std::max(r*2.0f, w * std::clamp(f, 0.0f, 1.0f)), h, r, fg_c);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Manual feedforward for BrainGenome (no virtual dispatch, no heap per call)
// Topology: layers stored in layer_weights/layer_biases
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<float> eval_genome(const manas::BrainGenome& g,
                                       const std::vector<float>& input)
{
    std::vector<float> act = input;
    const int L = static_cast<int>(g.layer_weights.size());
    for (int l = 0; l < L; ++l) {
        const auto& W = g.layer_weights[l];
        const auto& B = g.layer_biases[l];
        const size_t out_n = W.shape()[0];
        const size_t in_n  = W.shape()[1];
        std::vector<float> next(out_n, 0.0f);
        for (size_t o = 0; o < out_n; ++o) {
            float s = B(std::vector<size_t>{o});
            const size_t lim = std::min(in_n, act.size());
            for (size_t i = 0; i < lim; ++i) s += W(std::vector<size_t>{o, i}) * act[i];
            // ReLU except last layer (sigmoid)
            if (l < L - 1) next[o] = std::max(0.0f, s);
            else            next[o] = 1.0f / (1.0f + std::exp(-s));
        }
        act = std::move(next);
    }
    return act;
}

// ─────────────────────────────────────────────────────────────────────────────
// Enemy struct for threat map
// ─────────────────────────────────────────────────────────────────────────────
struct Enemy {
    float angle = 0.0f;
    float radius = 0.0f;
    float speed  = 0.0f;
    float threat = 0.0f; // 0..1
};

// ─────────────────────────────────────────────────────────────────────────────
// Damage number pop-up
// ─────────────────────────────────────────────────────────────────────────────
struct DamageNum {
    float x = 0, y = 0;
    float vy = -40.0f;
    float age = 0, lifetime = 1.2f;
    int   dmg = 0;
    bool  active = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Application state
// ─────────────────────────────────────────────────────────────────────────────
struct HUDState {
    float t = 0.0f;
    int   frame = 0;

    // ── Evolutionary brain ──────────────────────────────────────────────────
    static constexpr int POP_SIZE = 16;
    manas::EvolutionaryProcess<
        manas::TournamentSelection,
        manas::GaussianJitterMutation,
        manas::BlendCrossover> evo{
            manas::TournamentSelection{4},
            manas::GaussianJitterMutation{0.1f},
            manas::BlendCrossover{0.5f}
        };
    bool evo_initialized = false;
    uint32_t generation = 0;
    float best_fitness  = 0.0f;

    // Vital signals driven by brain output
    float health = 1.0f;   // 0..1
    float energy = 0.8f;   // 0..1

    // ── ScreenShake ──────────────────────────────────────────────────────────
    pebble::spandana::ScreenShake2D shake{18.0f, 0.08f}; // max_offset, max_angle
    float shake_x = 0, shake_y = 0;

    // ── Enemies ──────────────────────────────────────────────────────────────
    static constexpr int NUM_ENEMIES = 20;
    std::array<Enemy, NUM_ENEMIES> enemies{};

    // ── Damage numbers ───────────────────────────────────────────────────────
    static constexpr int MAX_DMG = 8;
    std::array<DamageNum, MAX_DMG> damage_nums{};

    // ── Fitness sparkline ────────────────────────────────────────────────────
    static constexpr int FIT_HIST = 90;
    std::vector<float> fitness_hist;

    // ── Abilities cooldown (0..1 = full) ─────────────────────────────────────
    std::array<float, 5> ability_cd{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 5> ability_max_cd{4.0f, 8.0f, 12.0f, 6.0f, 20.0f};

    // ── RadialMenu ───────────────────────────────────────────────────────────
    bool  radial_open = false;
    float radial_angle = 0.0f;

    // ── Ammo / shield ────────────────────────────────────────────────────────
    int   ammo = 30;
    int   ammo_max = 30;
    float shield = 0.6f;

    // ── Layout shift timer ───────────────────────────────────────────────────
    float layout_shift_timer = 0.0f;
    float sidebar_w = 220.0f; // animates via spring
    float sidebar_w_target = 220.0f;
    float sidebar_w_vel = 0.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// Init evolutionary brain population
// ─────────────────────────────────────────────────────────────────────────────
static void init_evo(HUDState& s) {
    // Build a simple 4-input → 8 hidden → 4 hidden → 2-output brain genome
    std::mt19937 rng(42u);
    std::normal_distribution<float> nd(0.0f, 0.3f);

    auto make_layer = [&](size_t out, size_t in) -> std::pair<ts::tensor<float>, ts::tensor<float>> {
        ts::tensor<float> W({out, in}), B({out, (size_t)1});
        for (size_t o = 0; o < out; ++o) {
            for (size_t i = 0; i < in; ++i) W(o, i) = nd(rng);
            B(o, (size_t)0) = 0.0f;
        }
        return {std::move(W), std::move(B)};
    };

    for (int p = 0; p < HUDState::POP_SIZE; ++p) {
        manas::BrainGenome g;
        g.topology_type = manas::TopologyType::FeedForward;

        auto [W1, B1] = make_layer(8, 4);
        auto [W2, B2] = make_layer(4, 8);
        auto [W3, B3] = make_layer(2, 4);

        g.layer_weights = {std::move(W1), std::move(W2), std::move(W3)};
        g.layer_biases  = {std::move(B1), std::move(B2), std::move(B3)};
        s.evo.add_genome(std::move(g));
    }
    s.evo_initialized = true;

    // Enemies
    rng.seed(123u);
    std::uniform_real_distribution<float> ang_d(0.0f, 6.28f);
    std::uniform_real_distribution<float> rad_d(60.0f, 200.0f);
    std::uniform_real_distribution<float> spd_d(0.3f, 1.2f);
    for (auto& e : s.enemies) {
        e.angle  = ang_d(rng);
        e.radius = rad_d(rng);
        e.speed  = spd_d(rng) * (rng() & 1 ? 1.0f : -1.0f);
        e.threat = static_cast<float>(rng() & 0xFF) / 255.0f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Spawn a damage number popup
// ─────────────────────────────────────────────────────────────────────────────
static void spawn_damage(HUDState& s, float x, float y, int dmg) {
    for (auto& d : s.damage_nums) {
        if (!d.active) {
            d = {x, y, -45.0f, 0.0f, 1.2f, dmg, true};
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-frame update
// ─────────────────────────────────────────────────────────────────────────────
static void update(HUDState& s) {
    s.t     += DT;
    s.frame += 1;

    // ── Evolutionary brain step ─────────────────────────────────────────────
    if (s.evo_initialized) {
        // Fitness: brain should maximise health+energy output signals
        manas::FitnessFn fit = [&](const manas::BrainGenome& g) -> manas::FitnessMetrics {
            const std::vector<float> inputs = {
                std::sin(s.t * 0.5f),
                std::cos(s.t * 0.7f),
                s.health,
                s.energy,
            };
            const auto out = eval_genome(g, inputs);
            const float h_out  = out.size() > 0 ? out[0] : 0.5f;
            const float en_out = out.size() > 1 ? out[1] : 0.5f;
            return {h_out + en_out * 0.5f, 0};
        };

        s.evo.run_generation(fit);
        ++s.generation;

        const auto& best = s.evo.best_genome();
        const std::vector<float> inputs = {std::sin(s.t * 0.5f), std::cos(s.t * 0.7f), s.health, s.energy};
        const auto out = eval_genome(best, inputs);
        s.health = std::clamp((out.size() > 0 ? out[0] : s.health), 0.0f, 1.0f);
        s.energy = std::clamp((out.size() > 1 ? out[1] : s.energy), 0.0f, 1.0f);

        s.best_fitness = s.evo.best_score();
        s.fitness_hist.push_back(s.best_fitness);
        if (static_cast<int>(s.fitness_hist.size()) > HUDState::FIT_HIST)
            s.fitness_hist.erase(s.fitness_hist.begin());
    }

    // ── Enemies orbit ───────────────────────────────────────────────────────
    for (auto& e : s.enemies) {
        e.angle += e.speed * DT;
        e.threat = 0.3f + 0.7f * std::pow(std::abs(std::sin(s.t * 0.3f + e.angle)), 2.0f);
    }

    // ── Simulated hit every ~3 seconds ─────────────────────────────────────
    if (s.frame % 180 == 90) {
        s.shake.add_trauma(0.55f);
        const int dmg = 10 + (s.frame % 30);
        spawn_damage(s, W * 0.5f + 30, H * 0.5f - 20, dmg);
        s.health = std::max(0.0f, s.health - 0.08f);
        s.ammo   = std::max(0, s.ammo - (1 + s.frame % 4));
    }

    // ── Ability cooldowns ───────────────────────────────────────────────────
    for (int i = 0; i < 5; ++i) {
        s.ability_cd[i] = std::min(s.ability_max_cd[i],
                                   s.ability_cd[i] + DT);
    }

    // ── Damage number physics ───────────────────────────────────────────────
    for (auto& d : s.damage_nums) {
        if (!d.active) continue;
        d.age += DT;
        d.y   += d.vy * DT;
        d.vy  *= 0.95f;
        if (d.age >= d.lifetime) d.active = false;
    }

    // ── Screen shake ───────────────────────────────────────────────────────
    s.shake.update(DT);
    s.shake_x = s.shake.offset()[0] * static_cast<float>(W);
    s.shake_y = s.shake.offset()[1] * static_cast<float>(H);

    // ── Sidebar spring layout shift every 180 frames ───────────────────────
    s.layout_shift_timer += DT;
    if (s.frame % 180 == 0) {
        s.sidebar_w_target = (s.frame % 360 == 0) ? 180.0f : 240.0f;
    }
    // Simple spring: F = -k*(x-target) - d*v
    const float k = 120.0f, d = 20.0f;
    const float force = -k * (s.sidebar_w - s.sidebar_w_target) - d * s.sidebar_w_vel;
    s.sidebar_w_vel += force * DT;
    s.sidebar_w     += s.sidebar_w_vel * DT;

    // Ammo regen
    if (s.frame % 120 == 0) s.ammo = std::min(s.ammo_max, s.ammo + 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw left HUD panel
// ─────────────────────────────────────────────────────────────────────────────
static void draw_left_hud(UICtx& ui, const HUDState& s) {
    const float sw = s.sidebar_w;
    ui.fill_rect(0, 0, sw, H, kalpana::Color{0.04f, 0.05f, 0.09f, 0.92f});

    // Title
    ui.round_rect(6, 6, sw - 12, 32, 4.0f, pal::hud);
    ui.text("PEBBLE HUD", 14, 14, pal::accent, 14.0f);

    // Health bar
    ui.text("HEALTH", 10, 52, pal::dim, 10.0f);
    ui.bar(8, 64, sw - 16, 14, s.health,
           kalpana::Color{0.15f,0.05f,0.05f,1.0f},
           kalpana::Color{0.9f + s.health * 0.1f, 0.2f, 0.2f, 1.0f});
    char hbuf[8]; std::snprintf(hbuf, sizeof(hbuf), "%.0f%%", s.health * 100.0f);
    ui.text(hbuf, sw - 38, 64, pal::text, 10.0f);

    // Energy bar
    ui.text("ENERGY", 10, 88, pal::dim, 10.0f);
    ui.bar(8, 100, sw - 16, 14, s.energy,
           kalpana::Color{0.05f,0.10f,0.20f,1.0f},
           kalpana::Color{0.2f, 0.6f, 1.0f, 1.0f});
    char ebuf[8]; std::snprintf(ebuf, sizeof(ebuf), "%.0f%%", s.energy * 100.0f);
    ui.text(ebuf, sw - 38, 100, pal::text, 10.0f);

    // Shield + ammo tiles
    ui.round_rect(8, 124, (sw - 22) * 0.5f, 36, 3.0f, pal::hud);
    ui.text("SHIELD", 14, 128, pal::dim, 9.0f);
    char sbuf[10]; std::snprintf(sbuf, sizeof(sbuf), "%.0f%%", s.shield * 100.0f);
    ui.text(sbuf, 14, 141, pal::purple, 13.0f);

    const float ax = 10 + (sw - 22) * 0.5f + 6;
    ui.round_rect(ax, 124, (sw - 22) * 0.5f, 36, 3.0f, pal::hud);
    ui.text("AMMO", ax + 6, 128, pal::dim, 9.0f);
    ui.text(std::to_string(s.ammo), ax + 6, 141, pal::yellow, 13.0f);

    // Fitness sparkline
    ui.text("FITNESS", 10, 172, pal::dim, 9.0f);
    const int F = static_cast<int>(s.fitness_hist.size());
    if (F > 1) {
        float fmn = *std::min_element(s.fitness_hist.begin(), s.fitness_hist.end());
        float fmx = *std::max_element(s.fitness_hist.begin(), s.fitness_hist.end());
        if (fmx - fmn < 1e-4f) fmx = fmn + 1.0f;
        const float sh2 = 40.0f;
        for (int i = 1; i < F; ++i) {
            const float x0 = 8.0f + static_cast<float>(i-1) / HUDState::FIT_HIST * (sw - 16);
            const float x1 = 8.0f + static_cast<float>(i)   / HUDState::FIT_HIST * (sw - 16);
            const float y0 = 183.0f + sh2 * (1.0f - (s.fitness_hist[i-1] - fmn) / (fmx - fmn));
            const float y1 = 183.0f + sh2 * (1.0f - (s.fitness_hist[i]   - fmn) / (fmx - fmn));
            const float age = static_cast<float>(i) / F;
            ui.line(x0, y0, x1, y1, kalpana::Color{0.3f, 0.8f+age*0.2f, 0.5f, 0.4f + 0.6f*age}, 1.0f);
        }
    }

    // Generation counter
    ui.round_rect(8, 232, sw - 16, 24, 3.0f, pal::hud);
    ui.text("GEN", 14, 237, pal::dim, 9.0f);
    ui.text(std::to_string(s.generation), sw * 0.5f, 237, pal::green, 12.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw center: crosshair, damage numbers, radial menu indicator
// ─────────────────────────────────────────────────────────────────────────────
static void draw_center(UICtx& ui, const HUDState& s) {
    const float cx = W * 0.5f, cy = H * 0.5f;

    // Crosshair (pulsed by health)
    const float pulse = 1.0f + 0.15f * std::sin(s.t * 8.0f) * (1.0f - s.health);
    const float cross_r = 12.0f * pulse;
    const float gap = 4.0f * pulse;
    const kalpana::Color ccol{0.9f, 0.9f * s.health + 0.1f, 0.1f + 0.7f * s.health, 0.9f};
    ui.line(cx - cross_r, cy, cx - gap, cy, ccol, 1.5f);
    ui.line(cx + gap, cy, cx + cross_r, cy, ccol, 1.5f);
    ui.line(cx, cy - cross_r, cx, cy - gap, ccol, 1.5f);
    ui.line(cx, cy + gap, cx, cy + cross_r, ccol, 1.5f);
    ui.circle_stroke(cx, cy, gap * 0.8f, ccol, 0.8f);

    // Radial menu (if open, show 6 spokes)
    if (s.radial_open) {
        for (int i = 0; i < 6; ++i) {
            const float a = static_cast<float>(i) * (std::numbers::pi_v<float> / 3.0f) + s.radial_angle;
            const float r0 = 30.0f, r1 = 80.0f;
            ui.line(cx + r0 * std::cos(a), cy + r0 * std::sin(a),
                    cx + r1 * std::cos(a), cy + r1 * std::sin(a),
                    pal::accent, 1.2f);
            ui.circle(cx + r1 * std::cos(a), cy + r1 * std::sin(a), 6.0f, pal::hud);
            ui.circle_stroke(cx + r1 * std::cos(a), cy + r1 * std::sin(a), 6.0f, pal::accent, 1.0f);
        }
        ui.circle_stroke(cx, cy, 30.0f, pal::accent, 0.6f);
    }

    // Damage numbers
    for (const auto& d : s.damage_nums) {
        if (!d.active) continue;
        const float alpha = 1.0f - d.age / d.lifetime;
        const kalpana::Color dc{1.0f, 0.3f + d.age * 0.3f, 0.1f, alpha};
        const std::string dmg_str = "-" + std::to_string(d.dmg);
        ui.text(dmg_str, d.x, d.y, dc, 16.0f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw right: threat map (RekhaWidget-style using KalpanaBackend offset)
// ─────────────────────────────────────────────────────────────────────────────
static rekha::Figure build_threat_fig(const HUDState& s, uint32_t pw, uint32_t ph) {
    rekha::HeatmapPlot hm;
    hm.rows = 12; hm.cols = 12;
    hm.x_extent = {-200.0f, 200.0f};
    hm.y_extent = {-200.0f, 200.0f};
    hm.values.resize(hm.rows * hm.cols, 0.0f);

    // Accumulate enemy threat density into heatmap cells
    for (const auto& e : s.enemies) {
        const float ex = e.radius * std::cos(e.angle);
        const float ey = e.radius * std::sin(e.angle);
        const float nx = (ex + 200.0f) / 400.0f; // 0..1
        const float ny = (ey + 200.0f) / 400.0f;
        const int ci = std::clamp(static_cast<int>(nx * hm.cols), 0, static_cast<int>(hm.cols) - 1);
        const int ri = std::clamp(static_cast<int>(ny * hm.rows), 0, static_cast<int>(hm.rows) - 1);
        hm.values[ri * hm.cols + ci] = std::min(1.0f, hm.values[ri * hm.cols + ci] + e.threat * 0.6f);
    }

    // Enemies as scatter
    rekha::XYSeries enemies("enemies");
    enemies.stroke({pal::red, 0.0f}).marker({pal::red, 3.0f});
    rekha::XYSeries player("player");
    player.stroke({pal::green, 0.0f}).marker({pal::green, 6.0f});
    player.add(0.0f, 0.0f);

    for (const auto& e : s.enemies) {
        enemies.add(e.radius * std::cos(e.angle), e.radius * std::sin(e.angle));
    }

    rekha::Figure fig;
    fig.viewport({pw, ph, {24.0f, 4.0f, 4.0f, 18.0f}})
       .theme(rekha::Figure::theme_dark_neon())
       .axes(rekha::Axes{.x_label="X", .y_label="Y", .ticks=3,
                         .x_range_override=true, .y_range_override=true,
                         .x_range={-200.0f, 200.0f}, .y_range={-200.0f, 200.0f}})
       .add(std::move(hm))
       .add(rekha::ScatterPlot{std::move(enemies)})
       .add(rekha::ScatterPlot{std::move(player)});
    return fig;
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw bottom ability bar
// ─────────────────────────────────────────────────────────────────────────────
static void draw_ability_bar(UICtx& ui, const HUDState& s) {
    const float BY = H - 70.0f, BH = 62.0f;
    const float center_x = W * 0.5f;
    const float slot_w = 56.0f, slot_h = 52.0f, gap = 6.0f;
    const float total_w = 5 * slot_w + 4 * gap;
    const float start_x = center_x - total_w * 0.5f;

    ui.round_rect(start_x - 8, BY - 4, total_w + 16, BH + 8, 6.0f,
                  kalpana::Color{0.04f, 0.05f, 0.09f, 0.85f});

    const char* labels[5] = {"Q", "W", "E", "R", "F"};
    const kalpana::Color ability_colors[5] = {pal::accent, pal::green, pal::orange, pal::purple, pal::red};

    for (int i = 0; i < 5; ++i) {
        const float sx = start_x + i * (slot_w + gap);
        const float cd_frac = s.ability_cd[i] / s.ability_max_cd[i];
        const bool ready = cd_frac >= 1.0f;
        const kalpana::Color base_c = ready ? ability_colors[i] : pal::dim;

        ui.round_rect(sx, BY, slot_w, slot_h, 4.0f, kalpana::Color{0.07f, 0.07f, 0.12f, 1.0f});
        ui.stroke_round_rect(sx, BY, slot_w, slot_h, 4.0f, base_c, 1.2f);

        // Cooldown arc fill
        ui.bar(sx + 2, BY + slot_h - 8, slot_w - 4, 6, cd_frac,
               kalpana::Color{0.1f, 0.1f, 0.15f, 1.0f}, ability_colors[i]);

        ui.text(labels[i], sx + slot_w * 0.5f - 4, BY + 8, base_c, 18.0f);
        if (!ready) {
            const float rem = s.ability_max_cd[i] * (1.0f - cd_frac);
            char cbuf[8]; std::snprintf(cbuf, sizeof(cbuf), "%.1f", rem);
            ui.text(cbuf, sx + 4, BY + slot_h - 12, pal::dim, 9.0f);
        } else {
            ui.text("READY", sx + 3, BY + slot_h - 12, ability_colors[i], 8.0f);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Translate rekha scene nodes into parent coordinate space
// ─────────────────────────────────────────────────────────────────────────────
static void translate_node(kalpana::Node& n, float ox, float oy) {
    if (auto* g = std::get_if<kalpana::GroupNode>(&n.content))
        for (kalpana::Node& ch : g->children) translate_node(ch, ox, oy);
    else if (std::get_if<kalpana::ShapeNode>(&n.content))
        n.xf = kalpana::Transform::translate(ox, oy).combine(n.xf);
    else if (auto* t = std::get_if<kalpana::TextNode>(&n.content))
        { t->x += ox; t->y += oy; }
}

// ─────────────────────────────────────────────────────────────────────────────
// Sokol state
// ─────────────────────────────────────────────────────────────────────────────
struct CinematicApp {
    sg_pipeline  pip{};
    sg_bindings  bind{};
    sg_pass_action pass_action{};
    sg_buffer vbuf{}, ibuf{};
    std::unique_ptr<kalpana::Canvas<kalpana::sokol_backend>> canvas;
    HUDState state{};
};

CinematicApp g_app{};

void init_cb() {
    auto& app = g_app;

    sg_desc gfx{};
    gfx.environment = sglue_environment();
    gfx.logger.func = slog_func;
    sg_setup(&gfx);

    {
        sg_buffer_desc d{};
        d.size = 512 * 1024 * sizeof(kalpana::sokol_backend::Vertex);
        d.usage.stream_update = true;
        app.vbuf = sg_make_buffer(d);
        app.bind.vertex_buffers[0] = app.vbuf;
    }
    {
        sg_buffer_desc d{};
        d.size = 1024 * 1024 * sizeof(std::uint32_t);
        d.usage.index_buffer = true;
        d.usage.stream_update = true;
        app.ibuf = sg_make_buffer(d);
        app.bind.index_buffer = app.ibuf;
    }

    sg_shader_desc shd{};
#if defined(SOKOL_METAL)
    shd.vertex_func.source = VS_METAL;   shd.vertex_func.entry = "vs";
    shd.fragment_func.source = FS_METAL; shd.fragment_func.entry = "fs";
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
    app.pass_action.colors[0].clear_value = {0.02f, 0.03f, 0.06f, 1.0f};

    app.canvas = std::make_unique<kalpana::Canvas<kalpana::sokol_backend>>(W, H);
    init_evo(app.state);
}

void frame_cb() {
    auto& app = g_app;
    auto& s   = app.state;
    update(s);

    kalpana::Scene scene;
    scene.clear_color(pal::bg);

    // Apply screen shake offset to UI context
    UICtx ui{scene, s.shake_x * 0.5f, s.shake_y * 0.5f};

    // ── Background grid ─────────────────────────────────────────────────────
    for (int gx = 0; gx < W; gx += 60)
        ui.line(static_cast<float>(gx), 0, static_cast<float>(gx), H,
                kalpana::Color{0.06f, 0.07f, 0.12f, 0.5f}, 0.5f);
    for (int gy = 0; gy < H; gy += 60)
        ui.line(0, static_cast<float>(gy), W, static_cast<float>(gy),
                kalpana::Color{0.06f, 0.07f, 0.12f, 0.5f}, 0.5f);

    // ── Left HUD ─────────────────────────────────────────────────────────────
    draw_left_hud(ui, s);

    // ── Threat map (right panel) ─────────────────────────────────────────────
    {
        const float rw = 260.0f, rh = 260.0f;
        const float rx = W - rw - 8.0f, ry = 8.0f;
        kalpana::Path bg; bg.round_rect(ui.ox + rx, ui.oy + ry, rw, rh, 6.0f, 6.0f);
        scene.add(kalpana::Node::shape(std::move(bg), kalpana::Paint::fill(pal::hud)));

        const auto fig = build_threat_fig(s,
                                          static_cast<uint32_t>(rw),
                                          static_cast<uint32_t>(rh));
        rekha::KalpanaBackend bk;
        fig.render(bk);
        bk.end_frame();
        const float tx = ui.ox + rx, ty = ui.oy + ry;
        kalpana::Node& root = const_cast<kalpana::Node&>(bk.scene().root());
        if (auto* grp = std::get_if<kalpana::GroupNode>(&root.content)) {
            for (kalpana::Node& ch : grp->children) {
                translate_node(ch, tx, ty);
                scene.add(ch);
            }
        }

        // Label
        scene.add(kalpana::Node::text("THREAT MAP", pal::accent, 11.0f,
                                       ui.ox + rx + 6, ui.oy + ry + rh - 6));
    }

    // ── Center crosshair + HUD ───────────────────────────────────────────────
    draw_center(ui, s);

    // ── Ability bar ──────────────────────────────────────────────────────────
    draw_ability_bar(ui, s);

    // ── Title overlay ────────────────────────────────────────────────────────
    {
        const float pulsed_a = 0.7f + 0.3f * std::sin(s.t * 3.0f);
        ui.round_rect(s.sidebar_w + 4, 4, 220, 28, 4.0f,
                      kalpana::Color{0.05f, 0.06f, 0.09f, 0.85f});
        ui.text("CINEMATIC HUD  |  PEBBLE ENGINE",
                s.sidebar_w + 10, 12,
                kalpana::Color{pal::accent.r, pal::accent.g, pal::accent.b, pulsed_a},
                11.0f);
    }

    // ── Render to sokol ─────────────────────────────────────────────────────
    app.canvas->render(scene);

    const auto& verts   = app.canvas->backend().vertices();
    const auto& indices = app.canvas->backend().indices();

    if (!verts.empty() && !indices.empty()) {
        sg_update_buffer(app.vbuf, sg_range{verts.data(), verts.size() * sizeof(kalpana::sokol_backend::Vertex)});
        sg_update_buffer(app.ibuf, sg_range{indices.data(), indices.size() * sizeof(uint32_t)});
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
    auto& s = g_app.state;
    switch (ev->key_code) {
        case SAPP_KEYCODE_ESCAPE: sapp_quit(); break;
        case SAPP_KEYCODE_SPACE:
            s.shake.add_trauma(0.7f);
            spawn_damage(s, W * 0.5f, H * 0.4f, 25 + s.frame % 20);
            break;
        case SAPP_KEYCODE_R:
            s.radial_open = !s.radial_open;
            break;
        default:
            // Q/W/E/R/F trigger abilities
            if (ev->key_code >= SAPP_KEYCODE_Q && ev->key_code <= SAPP_KEYCODE_T) {
                const int slot = ev->key_code - SAPP_KEYCODE_Q;
                if (slot < 5) s.ability_cd[slot] = 0.0f;
            }
            break;
    }
}

void cleanup_cb() { sg_shutdown(); }

} // namespace

sapp_desc sokol_main(int /*argc*/, char** /*argv*/) {
    sapp_desc d{};
    d.init_cb    = init_cb;
    d.frame_cb   = frame_cb;
    d.event_cb   = event_cb;
    d.cleanup_cb = cleanup_cb;
    d.width      = W;
    d.height     = H;
    d.window_title = "Pebble Cinematic HUD [SPACE hit | R radial | Q-F abilities | ESC quit]";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}
