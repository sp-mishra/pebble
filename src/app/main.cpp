// ============================================================================
// src/app/main.cpp — Pebble Visual Engine Showcase v2: Physics Sandbox
// ============================================================================
// 50+ dynamic entities with full circle-circle collision physics, elemental
// chain reactions, spectral pigment mixing, Verlet cloth, and particle bursts.
//
// Controls:  [ESC] quit   [R] reset   [SPACE] spawn explosion   [G] gravity toggle
// ============================================================================

// Sokol (implementation in sokol_impl.cpp, defines chosen by CMake)
#define SOKOL_NO_DEPRECATED
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-function"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#pragma clang diagnostic pop

// Pebble subsystems
#include "ecs/ecs.hpp"
#include "gati/gati.hpp"
#include "gati/material.hpp"
#include "gati/elemental.hpp"
#include "spandana/spandana.hpp"
#include "kalpana/kalpana.hpp"
#include "kalpana/backend/capture_backend.hpp"
#include "dhvani/dhvani.hpp"

#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <numeric>

// ============================================================================
// Shader source
// ============================================================================
static const char* VS_METAL =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct In  { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };\n"
    "struct Out { float4 pos [[position]]; float2 uv; };\n"
    "vertex Out vs(In in [[stage_in]]) {\n"
    "    Out o; o.pos=float4(in.pos,0,1); o.uv=in.uv; return o; }\n";
static const char* FS_METAL =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct In { float2 uv; };\n"
    "fragment float4 fs(In in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "    return tex.sample(s, in.uv); }\n";
static const char* VS_GLSL =
    "#version 330\n"
    "layout(location=0) in vec2 pos; layout(location=1) in vec2 uv;\n"
    "out vec2 v; void main() { v=uv; gl_Position=vec4(pos,0,1); }\n";
static const char* FS_GLSL =
    "#version 330\n"
    "uniform sampler2D tex; in vec2 v; out vec4 c;\n"
    "void main() { c=texture(tex,v); }\n";

// ============================================================================
// Constants
// ============================================================================
static constexpr int   W   = 1024;
static constexpr int   H   = 680;
static constexpr float FW  = float(W);
static constexpr float FH  = float(H);
static constexpr float DT  = 1.0f / 60.0f;
static constexpr int   MAX_ENTS = 52;

// ============================================================================
// Entity types with visual and elemental identity
// ============================================================================
enum class EType : std::uint8_t {
    Fire=0, Water, Earth, Ice, Lightning, Obsidian, Steam, Plasma,
    Count
};

static const kalpana::Color TYPE_COLOR[(int)EType::Count] = {
    {1.00f,0.32f,0.05f,1.0f}, // Fire    — hot orange
    {0.10f,0.55f,0.95f,1.0f}, // Water   — cool blue
    {0.48f,0.35f,0.18f,1.0f}, // Earth   — warm brown
    {0.65f,0.92f,1.00f,1.0f}, // Ice     — icy pale
    {0.90f,0.90f,0.10f,1.0f}, // Lightning — electric yellow
    {0.25f,0.08f,0.42f,1.0f}, // Obsidian — deep purple
    {0.72f,0.82f,0.88f,1.0f}, // Steam   — grey-blue
    {0.90f,0.20f,0.90f,1.0f}, // Plasma  — magenta
};
static const kalpana::Color TYPE_RIM[(int)EType::Count] = {
    {1.00f,0.75f,0.15f,1.0f}, // Fire rim
    {0.50f,0.88f,1.00f,1.0f}, // Water rim
    {0.72f,0.60f,0.35f,1.0f}, // Earth rim
    {0.90f,1.00f,1.00f,1.0f}, // Ice rim
    {1.00f,1.00f,0.50f,1.0f}, // Lightning rim
    {0.62f,0.28f,0.98f,1.0f}, // Obsidian rim
    {0.92f,0.95f,1.00f,1.0f}, // Steam rim
    {1.00f,0.60f,1.00f,1.0f}, // Plasma rim
};

// Reaction table: what type emerges when typeA meets typeB
// -1 = no reaction / types unchanged
static int reaction_result(EType a, EType b) {
    // Encode as sorted pair
    int ia = int(a), ib = int(b);
    if (ia > ib) std::swap(ia, ib);
    // Fire + Water → Steam
    if (ia == int(EType::Fire) && ib == int(EType::Water))      return int(EType::Steam);
    // Fire + Ice → Water
    if (ia == int(EType::Fire) && ib == int(EType::Ice))        return int(EType::Water);
    // Water + Lightning → Plasma
    if (ia == int(EType::Water) && ib == int(EType::Lightning)) return int(EType::Plasma);
    // Earth + Lightning → Obsidian
    if (ia == int(EType::Earth) && ib == int(EType::Lightning)) return int(EType::Obsidian);
    // Ice + Lightning → Steam
    if (ia == int(EType::Ice) && ib == int(EType::Lightning))   return int(EType::Steam);
    // Steam + Fire → Plasma
    if (ia == int(EType::Fire) && ib == int(EType::Steam))      return int(EType::Plasma);
    return -1;
}

// ============================================================================
// Physics entity
// ============================================================================
enum class EShape : std::uint8_t { Circle=0, Triangle, Hexagon, Star, Diamond };

struct Entity {
    float  x, y;          // position
    float  vx, vy;        // velocity
    float  radius;        // collision radius
    EType  type;
    EShape shape;
    float  spin = 0.0f;   // current angle (rad)
    float  spin_rate;     // rad/s
    float  mass;
    bool   alive = true;
    int    react_cooldown = 0; // frames before next reaction
    float  glow = 0.0f;   // flash after reaction
};

// ============================================================================
// Particle
// ============================================================================
struct Particle {
    float x, y, vx, vy, life, max_life, radius;
    kalpana::Color col;
};

// ============================================================================
// Star field
// ============================================================================
struct Star { float x, y, brightness, twinkle_phase; };

// ============================================================================
// App State
// ============================================================================
struct AppState {
    // Sokol
    sg_pipeline    pip{};
    sg_bindings    bind{};
    sg_pass_action pass_action{};
    sg_image       tex_img{};
    sg_view        tex_view{};
    sg_sampler     smp{};
    std::vector<std::uint32_t> pixels;

    // Physics
    std::array<Entity, MAX_ENTS> ents{};
    int num_ents = 0;
    bool gravity_on = false;

    // Particles
    std::vector<Particle> particles;

    // Camera shake
    std::unique_ptr<pebble::spandana::ScreenShake2D> camera;

    // Cloth banner
    std::unique_ptr<pebble::spandana::VerletCloth2D> banner;

    // Kalpana
    std::unique_ptr<kalpana::Canvas<kalpana::capture_backend>> canvas;

    // Star field
    std::array<Star, 80> stars{};

    // Time
    float t = 0.0f;
    int   frame = 0;

    // Stats
    int total_reactions = 0;
    int total_collisions = 0;

    // Spectral swatches (precomputed)
    kalpana::Color sw_blue{}, sw_yellow{}, sw_green{};
    kalpana::Color sw_red{}, sw_cyan{}, sw_orange{};
};

static AppState g_app;

// ============================================================================
// Math helpers
// ============================================================================
static float randf(float lo, float hi) {
    return lo + float(std::rand()) / float(RAND_MAX) * (hi - lo);
}
static float randi(int lo, int hi) {
    return lo + std::rand() % (hi - lo + 1);
}

// ============================================================================
// Particle helpers
// ============================================================================
static void emit(float cx, float cy, int n, kalpana::Color col,
                 float speed_lo = 50.f, float speed_hi = 160.f,
                 float life_lo = 0.3f, float life_hi = 0.8f,
                 float radius = 3.5f) {
    for (int i = 0; i < n; ++i) {
        float angle = randf(0, 6.2832f);
        float spd   = randf(speed_lo, speed_hi);
        float life  = randf(life_lo, life_hi);
        g_app.particles.push_back({cx, cy,
            std::cos(angle)*spd, std::sin(angle)*spd,
            life, life, radius, col});
    }
}

static void step_particles(float dt) {
    float grav = g_app.gravity_on ? 120.0f : 12.0f;
    for (auto& p : g_app.particles) {
        p.x    += p.vx * dt;
        p.y    += p.vy * dt;
        p.vy   += grav * dt;
        p.vx   *= 0.995f;
        p.life -= dt;
    }
    g_app.particles.erase(
        std::remove_if(g_app.particles.begin(), g_app.particles.end(),
                       [](const Particle& p){ return p.life <= 0.0f; }),
        g_app.particles.end());
}

// ============================================================================
// Entity spawning
// ============================================================================
static void spawn_entity(float x, float y, float vx, float vy,
                         float radius, EType type, EShape shape) {
    if (g_app.num_ents >= MAX_ENTS) return;
    auto& e = g_app.ents[g_app.num_ents++];
    e = {};
    e.x = x; e.y = y;
    e.vx = vx; e.vy = vy;
    e.radius = radius;
    e.type = type;
    e.shape = shape;
    e.spin = randf(0, 6.28f);
    e.spin_rate = randf(-2.0f, 2.0f);
    e.mass = radius * radius;
    e.alive = true;
}

static void init_entities() {
    g_app.num_ents = 0;
    // Spread entities across the arena leaving room for the panel (right side)
    const float arena_w = FW - 220.0f;
    const float arena_h = FH - 60.0f;
    const float margin  = 60.0f;

    // 8 types × 6-7 entities each ≈ 50 entities
    int per_type = 6;
    for (int t = 0; t < int(EType::Count); ++t) {
        for (int j = 0; j < per_type; ++j) {
            float x = randf(margin, arena_w - margin);
            float y = randf(margin + 30.0f, arena_h - margin);
            float spd = randf(40.0f, 120.0f);
            float angle = randf(0, 6.2832f);
            float r = randf(12.0f, 26.0f);
            EShape sh = EShape(std::rand() % 5);
            spawn_entity(x, y, std::cos(angle)*spd, std::sin(angle)*spd,
                         r, EType(t), sh);
        }
    }
}

// ============================================================================
// Kalpana path helpers for shapes
// ============================================================================
static kalpana::Path make_ngon(float cx, float cy, float r, int n, float ang) {
    kalpana::Path p;
    for (int i = 0; i < n; ++i) {
        float a = ang + float(i) / float(n) * 6.2832f;
        float x = cx + std::cos(a) * r;
        float y = cy + std::sin(a) * r;
        if (i == 0) p.move_to(x, y); else p.line_to(x, y);
    }
    p.close();
    return p;
}

static kalpana::Path make_star(float cx, float cy, float ro, float ri, int n, float ang) {
    kalpana::Path p;
    for (int i = 0; i < n * 2; ++i) {
        float a = ang + float(i) / float(n * 2) * 6.2832f;
        float r = (i % 2 == 0) ? ro : ri;
        float x = cx + std::cos(a) * r;
        float y = cy + std::sin(a) * r;
        if (i == 0) p.move_to(x, y); else p.line_to(x, y);
    }
    p.close();
    return p;
}

static kalpana::Path make_entity_path(const Entity& e) {
    switch (e.shape) {
    case EShape::Circle:
        { kalpana::Path p; p.circle(e.x, e.y, e.radius); return p; }
    case EShape::Triangle:
        return make_ngon(e.x, e.y, e.radius, 3, e.spin);
    case EShape::Hexagon:
        return make_ngon(e.x, e.y, e.radius, 6, e.spin);
    case EShape::Star:
        return make_star(e.x, e.y, e.radius, e.radius * 0.42f, 5, e.spin);
    case EShape::Diamond:
        return make_ngon(e.x, e.y, e.radius, 4, e.spin);
    }
    kalpana::Path p; p.circle(e.x, e.y, e.radius); return p;
}

// ============================================================================
// Physics update
// ============================================================================
static void physics_step(float dt) {
    auto& app = g_app;
    const float arena_w = FW - 218.0f; // right boundary (before panel)
    const float arena_h = FH - 52.0f;
    const float margin  = 4.0f;
    float grav = app.gravity_on ? 200.0f : 0.0f;

    // Integrate
    for (int i = 0; i < app.num_ents; ++i) {
        auto& e = app.ents[i];
        if (!e.alive) continue;
        e.vy += grav * dt;
        e.vx *= 0.9985f; e.vy *= 0.9985f; // tiny air drag
        e.x  += e.vx * dt;
        e.y  += e.vy * dt;
        e.spin += e.spin_rate * dt;
        if (e.react_cooldown > 0) --e.react_cooldown;
        if (e.glow > 0.0f) e.glow -= dt * 2.5f;

        // Boundary bounce
        const float r = e.radius;
        if (e.x - r < margin)         { e.x = margin + r;       e.vx = std::abs(e.vx) * 0.85f; }
        if (e.x + r > arena_w - margin){ e.x = arena_w - margin - r; e.vx = -std::abs(e.vx) * 0.85f; }
        if (e.y - r < 28.0f)           { e.y = 28.0f + r;        e.vy = std::abs(e.vy) * 0.85f; }
        if (e.y + r > arena_h - margin){ e.y = arena_h - margin - r; e.vy = -std::abs(e.vy) * 0.85f; }
    }

    // Circle-circle collision detection & response
    for (int i = 0; i < app.num_ents; ++i) {
        auto& a = app.ents[i];
        if (!a.alive) continue;
        for (int j = i + 1; j < app.num_ents; ++j) {
            auto& b = app.ents[j];
            if (!b.alive) continue;

            float dx = b.x - a.x;
            float dy = b.y - a.y;
            float dist_sq = dx * dx + dy * dy;
            float min_dist = a.radius + b.radius;

            if (dist_sq >= min_dist * min_dist) continue;

            ++app.total_collisions;
            float dist = std::sqrt(dist_sq);
            if (dist < 0.001f) { dist = 0.001f; dx = 1.0f; dy = 0.0f; }

            // Separate
            float nx = dx / dist, ny = dy / dist;
            float overlap = min_dist - dist;
            float total_mass = a.mass + b.mass;
            a.x -= nx * overlap * (b.mass / total_mass);
            a.y -= ny * overlap * (b.mass / total_mass);
            b.x += nx * overlap * (a.mass / total_mass);
            b.y += ny * overlap * (a.mass / total_mass);

            // Impulse (elastic, e=0.75)
            float rel_vx = b.vx - a.vx;
            float rel_vy = b.vy - a.vy;
            float dot = rel_vx * nx + rel_vy * ny;
            if (dot > 0.0f) continue; // moving apart

            float restitution = 0.75f;
            float impulse = -(1.0f + restitution) * dot / (1.0f / a.mass + 1.0f / b.mass);
            a.vx -= impulse / a.mass * nx;
            a.vy -= impulse / a.mass * ny;
            b.vx += impulse / b.mass * nx;
            b.vy += impulse / b.mass * ny;

            // Speed cap
            auto cap = [](float& vx, float& vy, float maxv) {
                float spd = std::sqrt(vx*vx + vy*vy);
                if (spd > maxv) { vx = vx/spd*maxv; vy = vy/spd*maxv; }
            };
            cap(a.vx, a.vy, 320.0f);
            cap(b.vx, b.vy, 320.0f);

            // Collision spark
            float cx = (a.x + b.x) * 0.5f;
            float cy = (a.y + b.y) * 0.5f;
            kalpana::Color spark_col = TYPE_RIM[int(a.type)];
            emit(cx, cy, 6, spark_col, 40.0f, 120.0f, 0.2f, 0.5f, 2.5f);

            // Elemental reaction
            if (a.react_cooldown == 0 && b.react_cooldown == 0) {
                int result = reaction_result(a.type, b.type);
                if (result >= 0) {
                    ++app.total_reactions;
                    EType rt = EType(result);

                    // Big particle burst
                    emit(cx, cy, 32, TYPE_COLOR[result], 60.0f, 200.0f, 0.4f, 0.9f, 4.0f);
                    emit(cx, cy, 16, TYPE_RIM[result], 120.0f, 260.0f, 0.2f, 0.6f, 2.5f);

                    // Change types
                    a.type = rt; a.glow = 1.0f; a.react_cooldown = 90;
                    b.type = rt; b.glow = 1.0f; b.react_cooldown = 90;

                    // Camera trauma proportional to reaction size
                    float trauma = std::min(0.9f, 0.25f + float(std::abs(impulse)) * 0.00005f);
                    app.camera->add_trauma(trauma);
                }
            }
        }
    }
}

// ============================================================================
// Banner cloth
// ============================================================================
static void update_banner(float dt) {
    auto& app = g_app;
    pebble::math::vec2 anchor{FW * 0.5f - 60.0f, 18.0f};
    app.banner->update(anchor, dt);
}

// ============================================================================
// Scene builder
// ============================================================================
static void build_scene(kalpana::Scene& scene) {
    auto& app = g_app;

    scene.clear_color(kalpana::Color{0.03f,0.04f,0.09f,1.0f});

    // ── Star field ────────────────────────────────────────────────────────────
    for (auto& star : app.stars) {
        float bri = star.brightness * (0.6f + 0.4f * std::sin(app.t * 1.8f + star.twinkle_phase));
        kalpana::Path s; s.circle(star.x, star.y, bri * 2.0f);
        scene.add(kalpana::Node::shape(s, kalpana::Paint::fill(
            kalpana::Color{bri, bri, bri * 1.1f, bri})));
    }

    // ── Subtly animated grid ──────────────────────────────────────────────────
    {
        float arena_w = FW - 218.0f;
        kalpana::Color grid_col{0.09f,0.11f,0.18f,1.0f};
        for (int i = 0; i <= 20; ++i) {
            float x = i * arena_w / 20.0f;
            kalpana::Path l; l.move_to(x, 26.0f); l.line_to(x, FH - 50.0f);
            scene.add(kalpana::Node::shape(l, kalpana::Paint::stroke(grid_col, 1.0f)));
        }
        for (int j = 0; j <= 12; ++j) {
            float y = 26.0f + j * (FH - 76.0f) / 12.0f;
            kalpana::Path l; l.move_to(0.0f, y); l.line_to(arena_w, y);
            scene.add(kalpana::Node::shape(l, kalpana::Paint::stroke(grid_col, 1.0f)));
        }

        // Arena boundary frame
        kalpana::Path frame;
        frame.rect(1.0f, 26.0f, arena_w - 2.0f, FH - 78.0f);
        scene.add(kalpana::Node::shape(frame,
            kalpana::Paint::stroke(kalpana::Color{0.22f,0.28f,0.45f,1.0f}, 2.0f)));
    }

    // ── Cloth banner at top ───────────────────────────────────────────────────
    {
        const auto& pts = app.banner->particles();
        float bx = FW * 0.5f - 60.0f;
        for (std::size_t i = 1; i < pts.size(); ++i) {
            float sx0 = bx + pts[i-1].pos[0] * 3.2f;
            float sy0 = 18.0f + pts[i-1].pos[1] * 1.8f;
            float sx1 = bx + pts[i].pos[0] * 3.2f;
            float sy1 = 18.0f + pts[i].pos[1] * 1.8f;
            float t_  = 1.0f - float(i) / float(pts.size());
            kalpana::Color col = TYPE_COLOR[int(EType::Plasma)];
            col.a = t_ * 0.85f;
            kalpana::Path seg; seg.move_to(sx0, sy0); seg.line_to(sx1, sy1);
            scene.add(kalpana::Node::shape(seg, kalpana::Paint::stroke(col, 2.5f * t_)));
        }
        // Banner attachment points
        kalpana::Path dot; dot.circle(bx, 18.0f, 4.0f);
        scene.add(kalpana::Node::shape(dot, kalpana::Paint::fill(kalpana::colors::white())));
    }

    // ── Camera shake offset ───────────────────────────────────────────────────
    auto shk = app.camera->offset();
    float sx = shk[0], sy = shk[1];
    (void)sx; (void)sy; // applied implicitly via scene root if needed

    // ── Entities ─────────────────────────────────────────────────────────────
    for (int i = 0; i < app.num_ents; ++i) {
        const auto& e = app.ents[i];
        if (!e.alive) continue;

        kalpana::Color fill = TYPE_COLOR[int(e.type)];
        kalpana::Color rim  = TYPE_RIM[int(e.type)];

        // Glow pulse on reaction
        if (e.glow > 0.0f) {
            kalpana::Path glow_ring;
            glow_ring.circle(e.x + sx, e.y + sy, e.radius * (1.5f + e.glow * 0.8f));
            kalpana::Color gc = rim; gc.a = e.glow * 0.5f;
            scene.add(kalpana::Node::shape(glow_ring, kalpana::Paint::fill(gc)));
        }

        // Main shape
        kalpana::Path path = make_entity_path(e);
        // Apply camera shake by offsetting path (rebuild path centered at shaken position)
        {
            Entity shifted = e;
            shifted.x += sx; shifted.y += sy;
            path = make_entity_path(shifted);
        }
        scene.add(kalpana::Node::shape(path,
            kalpana::Paint::filled_outlined(fill, rim, 1.8f)));

        // Type-specific detail
        switch (e.type) {
        case EType::Lightning: {
            // Inner electric arc
            kalpana::Path arc; arc.circle(e.x + sx, e.y + sy, e.radius * 0.45f);
            kalpana::Color lc = kalpana::colors::white(); lc.a = 0.6f;
            scene.add(kalpana::Node::shape(arc, kalpana::Paint::fill(lc)));
            break;
        }
        case EType::Ice: {
            // Snowflake cross
            kalpana::Path c1, c2;
            c1.move_to(e.x + sx - e.radius * 0.6f, e.y + sy);
            c1.line_to(e.x + sx + e.radius * 0.6f, e.y + sy);
            c2.move_to(e.x + sx, e.y + sy - e.radius * 0.6f);
            c2.line_to(e.x + sx, e.y + sy + e.radius * 0.6f);
            kalpana::Color ic{0.9f,1.0f,1.0f,0.7f};
            scene.add(kalpana::Node::shape(c1, kalpana::Paint::stroke(ic, 2.0f)));
            scene.add(kalpana::Node::shape(c2, kalpana::Paint::stroke(ic, 2.0f)));
            break;
        }
        case EType::Fire: {
            // Flicker inner glow
            float fl = 0.7f + 0.3f * std::sin(app.t * 12.0f + e.x);
            kalpana::Path fc; fc.circle(e.x + sx, e.y + sy, e.radius * fl * 0.55f);
            scene.add(kalpana::Node::shape(fc,
                kalpana::Paint::fill(kalpana::Color{1.0f,0.92f,0.55f,0.75f})));
            break;
        }
        case EType::Plasma: {
            // Orbiting dot
            float oa = app.t * 4.0f + float(i);
            kalpana::Path od; od.circle(
                e.x + sx + std::cos(oa) * e.radius * 0.7f,
                e.y + sy + std::sin(oa) * e.radius * 0.7f, 3.0f);
            scene.add(kalpana::Node::shape(od,
                kalpana::Paint::fill(kalpana::Color{1.0f,1.0f,1.0f,0.9f})));
            break;
        }
        default: break;
        }
    }

    // ── Particles ─────────────────────────────────────────────────────────────
    for (const auto& p : app.particles) {
        float alpha = std::max(0.0f, p.life / p.max_life);
        kalpana::Color c = p.col; c.a = alpha;
        kalpana::Path dot; dot.circle(p.x, p.y, p.radius * alpha);
        scene.add(kalpana::Node::shape(dot, kalpana::Paint::fill(c)));
    }

    // ── Right-side Info Panel ─────────────────────────────────────────────────
    {
        const float px = FW - 214.0f, py = 26.0f, pw = 210.0f, ph = FH - 78.0f;
        kalpana::Path card; card.round_rect(px, py, pw, ph, 8.0f, 8.0f);
        scene.add(kalpana::Node::shape(card,
            kalpana::Paint::filled_outlined(
                kalpana::Color{0.05f,0.07f,0.13f,0.92f},
                kalpana::Color{0.22f,0.30f,0.55f,1.00f}, 1.5f)));

        // Type legend
        float lx = px + 12.0f, ly = py + 14.0f;
        for (int t = 0; t < int(EType::Count); ++t) {
            // Count live entities of this type
            int cnt = 0;
            for (int i = 0; i < g_app.num_ents; ++i)
                if (g_app.ents[i].alive && int(g_app.ents[i].type) == t) ++cnt;

            kalpana::Path dot; dot.circle(lx + 7.0f, ly + 7.0f, 7.0f);
            scene.add(kalpana::Node::shape(dot,
                kalpana::Paint::filled_outlined(TYPE_COLOR[t], TYPE_RIM[t], 1.5f)));

            // Count bar
            float bar_w = float(cnt) / float(g_app.num_ents) * (pw - 50.0f);
            if (bar_w > 0.5f) {
                kalpana::Path bar; bar.round_rect(lx + 18.0f, ly + 2.0f, bar_w, 10.0f, 2.0f, 2.0f);
                kalpana::Color bc = TYPE_COLOR[t]; bc.a = 0.65f;
                scene.add(kalpana::Node::shape(bar, kalpana::Paint::fill(bc)));
            }
            ly += 20.0f;
        }

        // Separator
        float sep_y = ly + 4.0f;
        kalpana::Path sep; sep.move_to(px + 8.0f, sep_y); sep.line_to(px + pw - 8.0f, sep_y);
        scene.add(kalpana::Node::shape(sep,
            kalpana::Paint::stroke(kalpana::Color{0.25f,0.30f,0.50f,0.8f}, 1.0f)));
        ly = sep_y + 14.0f;

        // Spectral mixing swatches (Blue+Yellow=Green, Red+Cyan=mix)
        struct Row { kalpana::Color a, b, res; };
        Row rows[] = {
            {app.sw_blue,   app.sw_yellow, app.sw_green},
            {app.sw_red,    app.sw_cyan,   app.sw_orange},
        };
        for (auto& row : rows) {
            const float sw = 34.0f, sh = 24.0f, g = 4.0f;
            float rx = lx;

            kalpana::Path sa; sa.rect(rx, ly, sw, sh);
            scene.add(kalpana::Node::shape(sa,
                kalpana::Paint::filled_outlined(row.a, kalpana::Color{0.4f,0.4f,0.4f,0.5f}, 1.0f)));
            rx += sw + g;

            kalpana::Path ph_; ph_.rect(rx, ly + sh * 0.5f - 1.5f, 9.0f, 3.0f);
            kalpana::Path pv_; pv_.rect(rx + 3.0f, ly + sh * 0.5f - 6.0f, 3.0f, 12.0f);
            scene.add(kalpana::Node::shape(ph_, kalpana::Paint::fill(kalpana::colors::white())));
            scene.add(kalpana::Node::shape(pv_, kalpana::Paint::fill(kalpana::colors::white())));
            rx += 9.0f + g;

            kalpana::Path sb; sb.rect(rx, ly, sw, sh);
            scene.add(kalpana::Node::shape(sb,
                kalpana::Paint::filled_outlined(row.b, kalpana::Color{0.4f,0.4f,0.4f,0.5f}, 1.0f)));
            rx += sw + g;

            kalpana::Path arrow; arrow.move_to(rx, ly + sh*0.5f); arrow.line_to(rx+10.0f, ly+sh*0.5f);
            arrow.move_to(rx+6.0f, ly+sh*0.5f-4.0f); arrow.line_to(rx+10.0f, ly+sh*0.5f);
            arrow.line_to(rx+6.0f, ly+sh*0.5f+4.0f);
            scene.add(kalpana::Node::shape(arrow, kalpana::Paint::stroke(kalpana::colors::white(), 1.2f)));
            rx += 12.0f + g;

            kalpana::Path sr; sr.rect(rx, ly, sw, sh);
            scene.add(kalpana::Node::shape(sr,
                kalpana::Paint::filled_outlined(row.res, kalpana::Color{0.7f,0.7f,0.7f,0.8f}, 1.5f)));

            ly += sh + 11.0f;
        }

        // Live KM mixing slider
        float mix_t = (std::sin(g_app.t * 0.6f) + 1.0f) * 0.5f;
        kalpana::Color lv = kalpana::spectral::mix(kalpana::colors::blue(), kalpana::colors::yellow(), mix_t);
        float bw = pw - 20.0f;
        ly += 6.0f;
        kalpana::Path sbar; sbar.round_rect(lx, ly, bw, 18.0f, 4.0f, 4.0f);
        scene.add(kalpana::Node::shape(sbar,
            kalpana::Paint::filled_outlined(lv, kalpana::Color{0.5f,0.5f,0.5f,0.5f}, 1.0f)));
        kalpana::Path sthumb; sthumb.circle(lx + mix_t * bw, ly + 9.0f, 6.0f);
        scene.add(kalpana::Node::shape(sthumb,
            kalpana::Paint::filled_outlined(kalpana::colors::white(),
                kalpana::Color{0.35f,0.35f,0.35f,1.0f}, 1.0f)));
        ly += 28.0f;

        // Stats: reactions
        kalpana::Path r_dot; r_dot.circle(lx + 6.0f, ly + 6.0f, 5.0f);
        kalpana::Color rdc = (g_app.total_reactions > 0)
            ? TYPE_COLOR[int(EType::Plasma)]
            : kalpana::Color{0.35f,0.35f,0.35f,1.0f};
        scene.add(kalpana::Node::shape(r_dot, kalpana::Paint::fill(rdc)));

        // Reactions progress bar
        float rb = float(g_app.total_reactions) / float(std::max(1, g_app.total_reactions + 20));
        kalpana::Path rbar; rbar.round_rect(lx + 16.0f, ly + 1.0f, rb * (bw - 16.0f), 10.0f, 2.0f, 2.0f);
        scene.add(kalpana::Node::shape(rbar, kalpana::Paint::fill(TYPE_COLOR[int(EType::Plasma)])));
    }

    // ── HUD bar (bottom) ──────────────────────────────────────────────────────
    {
        const float bx = 1.0f, by = FH - 50.0f, bw = FW - 218.0f, bh = 46.0f;
        kalpana::Path bg; bg.round_rect(bx, by, bw, bh, 4.0f, 4.0f);
        scene.add(kalpana::Node::shape(bg,
            kalpana::Paint::filled_outlined(
                kalpana::Color{0.04f,0.06f,0.12f,0.90f},
                kalpana::Color{0.20f,0.28f,0.50f,1.00f}, 1.0f)));

        // Camera trauma bar
        float trauma = app.camera->trauma();
        kalpana::Path tbg; tbg.round_rect(bx + 8.0f, by + 28.0f, bw - 16.0f, 10.0f, 3.0f, 3.0f);
        scene.add(kalpana::Node::shape(tbg,
            kalpana::Paint::stroke(kalpana::Color{0.22f,0.26f,0.38f,1.0f}, 1.0f)));
        if (trauma > 0.01f) {
            kalpana::Color bar_col = trauma > 0.6f
                ? kalpana::Color{0.95f,0.18f,0.12f,1.0f}
                : kalpana::Color{0.22f,0.62f,0.95f,1.0f};
            kalpana::Path fill; fill.round_rect(bx + 8.0f, by + 28.0f, (bw - 16.0f) * trauma, 10.0f, 3.0f, 3.0f);
            scene.add(kalpana::Node::shape(fill, kalpana::Paint::fill(bar_col)));
        }

        // Heartbeat LED
        kalpana::Color led = (app.frame % 60) < 30
            ? kalpana::Color{0.18f,0.92f,0.42f,1.0f}
            : kalpana::Color{0.08f,0.35f,0.18f,1.0f};
        kalpana::Path hb; hb.circle(bx + 16.0f, by + 13.0f, 5.0f);
        scene.add(kalpana::Node::shape(hb, kalpana::Paint::fill(led)));

        // Gravity indicator
        kalpana::Color glc = app.gravity_on
            ? kalpana::Color{0.95f,0.75f,0.10f,1.0f}
            : kalpana::Color{0.28f,0.28f,0.28f,1.0f};
        kalpana::Path gd; gd.circle(bx + 44.0f, by + 13.0f, 5.0f);
        scene.add(kalpana::Node::shape(gd, kalpana::Paint::fill(glc)));

        // Entity count dots (one colored dot per entity type)
        float ex = bx + 70.0f;
        for (int t = 0; t < int(EType::Count); ++t) {
            int cnt = 0;
            for (int i = 0; i < app.num_ents; ++i)
                if (app.ents[i].alive && int(app.ents[i].type) == t) ++cnt;
            kalpana::Color ec = TYPE_COLOR[t]; ec.a = std::max(0.2f, float(cnt) / 8.0f);
            kalpana::Path ed; ed.circle(ex, by + 13.0f, 5.0f);
            scene.add(kalpana::Node::shape(ed, kalpana::Paint::fill(ec)));
            ex += 14.0f;
        }
    }

    // ── Top bar ───────────────────────────────────────────────────────────────
    {
        kalpana::Path top; top.rect(0.0f, 0.0f, FW, 26.0f);
        scene.add(kalpana::Node::shape(top,
            kalpana::Paint::fill(kalpana::Color{0.04f,0.06f,0.13f,0.95f})));
        kalpana::Path tline; tline.move_to(0.0f, 26.0f); tline.line_to(FW, 26.0f);
        scene.add(kalpana::Node::shape(tline,
            kalpana::Paint::stroke(kalpana::Color{0.22f,0.30f,0.55f,1.0f}, 1.5f)));

        // FPS indicator dots
        int fps_dots = std::min(30, app.frame % 60);
        for (int d = 0; d < 30; ++d) {
            kalpana::Color dc = d < fps_dots
                ? kalpana::Color{0.18f,0.92f,0.42f,0.9f}
                : kalpana::Color{0.15f,0.18f,0.25f,1.0f};
            kalpana::Path dp; dp.circle(10.0f + d * 13.0f, 13.0f, 4.0f);
            scene.add(kalpana::Node::shape(dp, kalpana::Paint::fill(dc)));
        }
    }
}

// ============================================================================
// Sokol callbacks
// ============================================================================
static void init_cb() {
    auto& app = g_app;

    sg_desc gfx{};
    gfx.environment = sglue_environment();
    gfx.logger.func = slog_func;
    sg_setup(&gfx);

    app.pixels.assign(W * H, 0xFF080810u);

    // Vertex + index buffers for fullscreen quad
    struct Vert { float x, y, u, v; };
    static const Vert verts[] = {
        {-1.f,-1.f,0.f,1.f},{1.f,-1.f,1.f,1.f},
        {1.f, 1.f,1.f,0.f},{-1.f,1.f,0.f,0.f}
    };
    static const uint16_t idx[] = {0,1,2,0,2,3};

    { sg_buffer_desc d{}; d.data = SG_RANGE(verts); app.bind.vertex_buffers[0] = sg_make_buffer(d); }
    { sg_buffer_desc d{}; d.usage.index_buffer = true; d.data = SG_RANGE(idx); app.bind.index_buffer = sg_make_buffer(d); }

    // Streaming texture
    { sg_image_desc d{}; d.width = W; d.height = H; d.pixel_format = SG_PIXELFORMAT_RGBA8;
      d.usage.stream_update = true; app.tex_img = sg_make_image(d); }

    // Texture view + sampler
    { sg_view_desc d{}; d.texture.image = app.tex_img; app.tex_view = sg_make_view(d); }
    { sg_sampler_desc d{}; d.min_filter = SG_FILTER_NEAREST; d.mag_filter = SG_FILTER_NEAREST;
      app.smp = sg_make_sampler(d); }

    app.bind.views[0]    = app.tex_view;
    app.bind.samplers[0] = app.smp;

    // Shader
    sg_shader_desc shd{};
#if defined(SOKOL_METAL)
    shd.vertex_func.source = VS_METAL; shd.vertex_func.entry = "vs";
    shd.fragment_func.source = FS_METAL; shd.fragment_func.entry = "fs";
#else
    shd.vertex_func.source = VS_GLSL; shd.fragment_func.source = FS_GLSL;
    shd.texture_sampler_pairs[0].glsl_name = "tex";
#endif
    shd.views[0].texture.stage               = SG_SHADERSTAGE_FRAGMENT;
    shd.samplers[0].stage                    = SG_SHADERSTAGE_FRAGMENT;
    shd.texture_sampler_pairs[0].stage        = SG_SHADERSTAGE_FRAGMENT;
    shd.texture_sampler_pairs[0].view_slot    = 0;
    shd.texture_sampler_pairs[0].sampler_slot = 0;
    sg_shader shdr = sg_make_shader(shd);

    // Pipeline
    sg_pipeline_desc pd{};
    pd.shader = shdr; pd.index_type = SG_INDEXTYPE_UINT16;
    pd.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[1].format = SG_VERTEXFORMAT_FLOAT2;
    app.pip = sg_make_pipeline(pd);

    app.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    app.pass_action.colors[0].clear_value = {0.0f,0.0f,0.0f,1.0f};

    // Pebble subsystems
    app.camera = std::make_unique<pebble::spandana::ScreenShake2D>(18.0f, 0.08f);
    app.banner = std::make_unique<pebble::spandana::VerletCloth2D>(10, 8.0f,
        pebble::math::vec2{0.8f, -20.0f}, 0.015f);
    app.canvas = std::make_unique<kalpana::Canvas<kalpana::capture_backend>>(W, H);

    // Star field
    for (auto& star : app.stars) {
        star.x = randf(0.0f, FW - 220.0f);
        star.y = randf(28.0f, FH - 52.0f);
        star.brightness    = randf(0.15f, 0.65f);
        star.twinkle_phase = randf(0.0f, 6.28f);
    }

    // Entities
    init_entities();

    // Precompute spectral swatches
    app.sw_blue   = kalpana::colors::blue();
    app.sw_yellow = kalpana::colors::yellow();
    app.sw_green  = kalpana::spectral::mix(kalpana::colors::blue(), kalpana::colors::yellow(), 0.5f);
    app.sw_red    = kalpana::colors::red();
    app.sw_cyan   = kalpana::Color{0.0f, 1.0f, 1.0f, 1.0f};
    app.sw_orange = kalpana::spectral::mix(kalpana::colors::red(), kalpana::Color{0.0f,1.0f,1.0f,1.0f}, 0.5f);
}

static void frame_cb() {
    auto& app = g_app;
    app.frame++;
    app.t += DT;

    physics_step(DT);
    update_banner(DT);
    app.camera->update(DT);
    step_particles(DT);

    // Occasionally spawn a random tiny spark from fast entities
    for (int i = 0; i < app.num_ents; ++i) {
        auto& e = app.ents[i];
        float spd = std::sqrt(e.vx*e.vx + e.vy*e.vy);
        if (spd > 180.0f && (app.frame + i) % 8 == 0) {
            emit(e.x, e.y, 2, TYPE_RIM[int(e.type)], 20.0f, 60.0f, 0.1f, 0.3f, 1.5f);
        }
    }

    // Build & rasterize scene
    kalpana::Scene scene;
    build_scene(scene);
    app.canvas->render(scene);
    auto snap = app.canvas->snapshot();

    // Convert ARGB8888 → RGBA8 (Sokol little-endian)
    for (std::size_t i = 0; i < snap.size(); ++i) {
        std::uint32_t argb = snap[i];
        std::uint8_t a=(argb>>24)&0xFF, r=(argb>>16)&0xFF, g_=(argb>>8)&0xFF, b=argb&0xFF;
        app.pixels[i] = (std::uint32_t(a)<<24)|(std::uint32_t(b)<<16)|(std::uint32_t(g_)<<8)|r;
    }

    sg_image_data imgd{};
    imgd.mip_levels[0] = {app.pixels.data(), app.pixels.size()*sizeof(std::uint32_t)};
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
    if (ev->type != SAPP_EVENTTYPE_KEY_DOWN) return;
    auto& app = g_app;
    switch (ev->key_code) {
    case SAPP_KEYCODE_ESCAPE:
        sapp_quit();
        break;
    case SAPP_KEYCODE_R:
        app.total_reactions = 0;
        app.total_collisions = 0;
        app.particles.clear();
        init_entities();
        break;
    case SAPP_KEYCODE_G:
        app.gravity_on = !app.gravity_on;
        break;
    case SAPP_KEYCODE_SPACE: {
        // Explosion at screen center
        float cx = (FW - 218.0f) * 0.5f;
        float cy = (FH - 78.0f) * 0.5f;
        emit(cx, cy, 64, kalpana::Color{1.0f,0.85f,0.25f,1.0f}, 80.0f, 300.0f, 0.5f, 1.2f, 5.0f);
        emit(cx, cy, 32, kalpana::Color{1.0f,0.35f,0.05f,1.0f}, 150.0f, 360.0f, 0.3f, 0.8f, 3.5f);
        app.camera->add_trauma(0.85f);
        // Blast impulse
        for (int i = 0; i < app.num_ents; ++i) {
            auto& e = app.ents[i];
            float dx = e.x - cx, dy = e.y - cy;
            float d = std::sqrt(dx*dx + dy*dy) + 1.0f;
            float force = 25000.0f / (d * d);
            e.vx += dx / d * force;
            e.vy += dy / d * force;
        }
        break;
    }
    default: break;
    }
}

static void cleanup_cb() { sg_shutdown(); }

sapp_desc sokol_main(int /*argc*/, char** /*argv*/) {
    sapp_desc d{};
    d.init_cb    = init_cb;
    d.frame_cb   = frame_cb;
    d.event_cb   = event_cb;
    d.cleanup_cb = cleanup_cb;
    d.width      = W;
    d.height     = H;
    d.window_title  = "Pebble Physics Sandbox  [R] reset  [G] gravity  [SPC] explode  [ESC] quit";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}
