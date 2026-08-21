// ============================================================================
// src/app/main.cpp — Pebble Physics Sandbox v3: Liquids + Vibrant + Smart Sparks
// ============================================================================
// NEW in v3:
//  · 5 liquid pools (Mercury, Acid, Lava-Melt, Ink, Mana) — spring-cohesion
//    SPH-lite with soft glow + bridge rendering → looks like real liquid blobs
//  · Vibrant neon color palette — pure saturated hues on dark background
//  · Collision sparks = spectral::mix(A,B) boosted 2.2x → hyper-bright flare
//  · Liquid-solid interaction: blobs deflect off solid entities + splash bursts
//  · Liquid-liquid merging: different liquid types create chemical reactions
//
// Controls: [ESC] quit  [R] reset  [G] gravity  [SPC] explosion  [L] liquid wave
// ============================================================================
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

// ============================================================================
// Shaders
// ============================================================================
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
static const char* VS_GLSL =
    "#version 330\nlayout(location=0) in vec2 pos; layout(location=1) in vec2 uv;\n"
    "out vec2 v; void main(){ v=uv; gl_Position=vec4(pos,0,1); }\n";
static const char* FS_GLSL =
    "#version 330\nuniform sampler2D tex; in vec2 v; out vec4 c;\n"
    "void main(){ c=texture(tex,v); }\n";

// ============================================================================
// Window & timing
// ============================================================================
static constexpr int   W  = 1060;
static constexpr int   H  = 700;
static constexpr float FW = float(W);
static constexpr float FH = float(H);
static constexpr float DT = 1.0f / 60.0f;

// Panel width on the right side
static constexpr float PANEL_W  = 220.0f;
static constexpr float ARENA_W  = FW - PANEL_W - 2.0f;
static constexpr float ARENA_H  = FH - 52.0f;
static constexpr float ARENA_Y0 = 28.0f;

// ============================================================================
// Entity system (solid shapes)
// ============================================================================
enum class EType : std::uint8_t {
    Fire=0, Water, Earth, Ice, Lightning, Obsidian, Steam, Plasma, Count
};
enum class EShape : std::uint8_t { Circle, Triangle, Hexagon, Star, Diamond };

// Vibrant neon palette — pure hues, high saturation
static const kalpana::Color TYPE_FILL[(int)EType::Count] = {
    {1.00f, 0.22f, 0.00f, 1.0f}, // Fire      — pure deep orange-red
    {0.00f, 0.55f, 1.00f, 1.0f}, // Water     — pure cobalt blue
    {0.62f, 0.38f, 0.10f, 1.0f}, // Earth     — warm sienna
    {0.55f, 0.95f, 1.00f, 1.0f}, // Ice       — icy azure
    {1.00f, 0.98f, 0.00f, 1.0f}, // Lightning — electric yellow
    {0.48f, 0.00f, 0.80f, 1.0f}, // Obsidian  — deep violet
    {0.70f, 0.82f, 0.90f, 1.0f}, // Steam     — pale grey-blue
    {1.00f, 0.00f, 0.88f, 1.0f}, // Plasma    — hot magenta
};
static const kalpana::Color TYPE_RIM[(int)EType::Count] = {
    {1.00f, 0.75f, 0.10f, 1.0f}, // Fire rim      — amber
    {0.40f, 0.92f, 1.00f, 1.0f}, // Water rim     — cyan
    {0.90f, 0.72f, 0.30f, 1.0f}, // Earth rim     — gold
    {1.00f, 1.00f, 1.00f, 1.0f}, // Ice rim       — white
    {1.00f, 1.00f, 0.55f, 1.0f}, // Lightning rim — pale yellow
    {0.82f, 0.40f, 1.00f, 1.0f}, // Obsidian rim  — bright violet
    {1.00f, 1.00f, 1.00f, 0.9f}, // Steam rim     — white
    {1.00f, 0.55f, 1.00f, 1.0f}, // Plasma rim    — bright pink
};

// Elemental reaction table → -1 = no reaction
static int reaction_result(EType a, EType b) {
    int ia = int(a), ib = int(b);
    if (ia > ib) std::swap(ia, ib);
    if (ia==0&&ib==1) return int(EType::Steam);      // Fire + Water
    if (ia==0&&ib==3) return int(EType::Water);      // Fire + Ice
    if (ia==1&&ib==4) return int(EType::Plasma);     // Water + Lightning
    if (ia==2&&ib==4) return int(EType::Obsidian);   // Earth + Lightning
    if (ia==3&&ib==4) return int(EType::Steam);      // Ice + Lightning
    if (ia==0&&ib==6) return int(EType::Plasma);     // Fire + Steam
    if (ia==5&&ib==7) return int(EType::Fire);       // Obsidian + Plasma → Fire
    if (ia==1&&ib==7) return int(EType::Ice);        // Water + Plasma → Ice
    return -1;
}

struct Entity {
    float  x, y, vx, vy;
    float  radius;
    EType  type;
    EShape shape;
    float  spin, spin_rate, mass;
    float  glow = 0.0f;
    int    react_cd = 0;
    bool   alive = true;
};

static constexpr int MAX_ENTS = 56;

// ============================================================================
// Liquid simulation — SPH-lite blob physics
// ============================================================================
static constexpr int  LIQUID_TYPES   = 5;
static constexpr int  DROPS_PER_POOL = 14;

// Liquid color palette — vivid neon liquids
static const kalpana::Color LIQUID_FILL[LIQUID_TYPES] = {
    {0.85f, 0.85f, 0.90f, 0.72f}, // Mercury  — silver
    {0.10f, 1.00f, 0.20f, 0.70f}, // Acid     — neon green
    {1.00f, 0.28f, 0.00f, 0.70f}, // Lava     — molten orange
    {0.25f, 0.00f, 0.75f, 0.70f}, // Ink      — deep indigo
    {0.00f, 0.90f, 1.00f, 0.70f}, // Mana     — electric cyan
};
static const kalpana::Color LIQUID_GLOW[LIQUID_TYPES] = {
    {0.90f, 0.90f, 1.00f, 0.30f}, // Mercury glow
    {0.20f, 1.00f, 0.30f, 0.35f}, // Acid glow
    {1.00f, 0.55f, 0.10f, 0.35f}, // Lava glow
    {0.55f, 0.15f, 1.00f, 0.30f}, // Ink glow
    {0.10f, 0.95f, 1.00f, 0.35f}, // Mana glow
};

struct Droplet {
    float x, y, vx, vy;
    bool  alive = true;
};

struct LiquidPool {
    std::array<Droplet, DROPS_PER_POOL> drops{};
    float drop_radius = 9.0f;
    int   type_idx    = 0;  // index into LIQUID_FILL
    bool  active      = true;
};

// ============================================================================
// Particle
// ============================================================================
struct Particle {
    float x, y, vx, vy, life, max_life, radius;
    kalpana::Color col;
};

// ============================================================================
// Star
// ============================================================================
struct Star { float x, y, bri, phase; };

// ============================================================================
// Color helpers
// ============================================================================
static kalpana::Color brighten(kalpana::Color c, float factor) {
    return { std::min(1.0f, c.r * factor),
             std::min(1.0f, c.g * factor),
             std::min(1.0f, c.b * factor),
             c.a };
}

// Boost to HDR-like brightness: desaturate toward white when factor > 1.5
static kalpana::Color spark_color(kalpana::Color a, kalpana::Color b) {
    // Spectral subtractive mix of the two entity colors
    kalpana::Color mixed = kalpana::spectral::mix(a, b, 0.5f);
    // Boost to a super-bright neon version
    float br = 2.4f;
    kalpana::Color bright = brighten(mixed, br);
    // Blend toward white at the bright end (bloom-like)
    float lum = (bright.r + bright.g + bright.b) / 3.0f;
    float bloom = std::max(0.0f, (lum - 0.7f) * 1.8f);
    bright.r = std::min(1.0f, bright.r + bloom * 0.6f);
    bright.g = std::min(1.0f, bright.g + bloom * 0.6f);
    bright.b = std::min(1.0f, bright.b + bloom * 0.6f);
    bright.a = 1.0f;
    return bright;
}

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

    // Entities
    std::array<Entity, MAX_ENTS> ents{};
    int num_ents = 0;

    // Liquid pools
    std::array<LiquidPool, LIQUID_TYPES> pools{};

    // Particles
    std::vector<Particle> particles;

    // Spandana
    std::unique_ptr<pebble::spandana::ScreenShake2D> camera;
    std::unique_ptr<pebble::spandana::VerletCloth2D> banner;

    // Kalpana
    std::unique_ptr<kalpana::Canvas<kalpana::capture_backend>> canvas;

    // Stars
    std::array<Star, 90> stars{};

    float t     = 0.0f;
    int   frame = 0;
    bool  gravity_on = false;

    int total_reactions = 0;

    // Precomputed spectral swatches
    kalpana::Color sw_blue{}, sw_yellow{}, sw_green{};
    kalpana::Color sw_red{},  sw_cyan{},   sw_orange{};
};

static AppState g_app;

// ============================================================================
// Math / random
// ============================================================================
static float randf(float lo, float hi) {
    return lo + float(std::rand()) / float(RAND_MAX) * (hi - lo);
}

// ============================================================================
// Particle emission
// ============================================================================
static void emit(float cx, float cy, int n, kalpana::Color col,
                 float slo=50.f, float shi=160.f,
                 float llo=0.3f, float lhi=0.8f, float r=3.5f) {
    for (int i = 0; i < n; ++i) {
        float a = randf(0, 6.2832f);
        float spd = randf(slo, shi);
        float life = randf(llo, lhi);
        g_app.particles.push_back({cx, cy, std::cos(a)*spd, std::sin(a)*spd,
            life, life, r, col});
    }
}

static void step_particles(float dt) {
    float grav = g_app.gravity_on ? 130.0f : 8.0f;
    for (auto& p : g_app.particles) {
        p.x += p.vx * dt; p.y += p.vy * dt;
        p.vy += grav * dt; p.vx *= 0.994f;
        p.life -= dt;
    }
    g_app.particles.erase(
        std::remove_if(g_app.particles.begin(), g_app.particles.end(),
                       [](const Particle& p){ return p.life <= 0.0f; }),
        g_app.particles.end());
}

// ============================================================================
// Entity management
// ============================================================================
static void spawn(float x, float y, float vx, float vy, float r, EType t, EShape s) {
    if (g_app.num_ents >= MAX_ENTS) return;
    auto& e = g_app.ents[g_app.num_ents++];
    e = {};
    e.x=x; e.y=y; e.vx=vx; e.vy=vy; e.radius=r;
    e.type=t; e.shape=s;
    e.spin      = randf(0, 6.28f);
    e.spin_rate = randf(-2.2f, 2.2f);
    e.mass      = r * r;
    e.alive     = true;
}

static void init_entities() {
    g_app.num_ents = 0;
    for (int t = 0; t < int(EType::Count); ++t) {
        for (int j = 0; j < 7; ++j) {
            float r   = randf(11.0f, 22.0f);
            float spd = randf(45.0f, 120.0f);
            float ang = randf(0, 6.28f);
            EShape sh = EShape(std::rand() % 5);

            // Rejection sampling: retry until the candidate position
            // doesn't overlap any already-placed entity
            float x = 0.0f, y = 0.0f;
            bool placed = false;
            for (int attempt = 0; attempt < 200; ++attempt) {
                float cx = randf(r + 4.0f, ARENA_W - r - 4.0f);
                float cy = randf(ARENA_Y0 + r + 4.0f, ARENA_H - r - 4.0f);
                bool overlap = false;
                for (int k = 0; k < g_app.num_ents; ++k) {
                    const auto& e = g_app.ents[k];
                    float dx = cx - e.x, dy = cy - e.y;
                    float min_d = r + e.radius + 2.0f; // 2px gap
                    if (dx*dx + dy*dy < min_d*min_d) { overlap = true; break; }
                }
                if (!overlap) { x = cx; y = cy; placed = true; break; }
            }
            if (!placed) {
                // Fallback: just place anywhere (very dense scenes)
                x = randf(r + 4.0f, ARENA_W - r - 4.0f);
                y = randf(ARENA_Y0 + r + 4.0f, ARENA_H - r - 4.0f);
            }
            spawn(x, y, std::cos(ang)*spd, std::sin(ang)*spd, r, EType(t), sh);
        }
    }
}

// ============================================================================
// Liquid pool management
// ============================================================================
static void init_liquid_pools() {
    float aw = ARENA_W, ah = ARENA_H;
    for (int p = 0; p < LIQUID_TYPES; ++p) {
        auto& pool = g_app.pools[p];
        pool.type_idx    = p;
        pool.active      = true;
        pool.drop_radius = 9.0f + float(p) * 0.8f;

        // Cluster the pool in a random area of the arena
        float cx = randf(80.0f, aw - 80.0f);
        float cy = randf(ARENA_Y0 + 80.0f, ah - 80.0f);
        for (int d = 0; d < DROPS_PER_POOL; ++d) {
            auto& drop = pool.drops[d];
            drop.x  = cx + randf(-30.0f, 30.0f);
            drop.y  = cy + randf(-20.0f, 20.0f);
            drop.vx = randf(-30.0f, 30.0f);
            drop.vy = randf(-20.0f, 20.0f);
            drop.alive = true;
        }
    }
}

// SPH-lite liquid update
static void step_liquid(float dt) {
    float grav = g_app.gravity_on ? 160.0f : 15.0f;

    for (int pi = 0; pi < LIQUID_TYPES; ++pi) {
        auto& pool = g_app.pools[pi];
        if (!pool.active) continue;
        float dr = pool.drop_radius;

        // Spring forces between drops in same pool
        for (int a = 0; a < DROPS_PER_POOL; ++a) {
            auto& da = pool.drops[a];
            if (!da.alive) continue;
            for (int b = a + 1; b < DROPS_PER_POOL; ++b) {
                auto& db = pool.drops[b];
                if (!db.alive) continue;
                float dx = db.x - da.x, dy = db.y - da.y;
                float dist = std::sqrt(dx*dx + dy*dy) + 0.001f;
                float rest = dr * 1.8f;
                float max_dist = dr * 4.5f;
                if (dist > max_dist) continue;

                float nx = dx/dist, ny = dy/dist;
                float force;
                if (dist < rest * 0.5f) {
                    // Strong repulsion (incompressible)
                    force = -320.0f * (1.0f - dist / (rest * 0.5f));
                } else if (dist < rest) {
                    // Medium repulsion
                    force = -80.0f * (rest - dist) / rest;
                } else {
                    // Cohesion (surface tension)
                    force = 55.0f * (dist - rest) / (max_dist - rest);
                }
                da.vx -= nx * force * dt;
                da.vy -= ny * force * dt;
                db.vx += nx * force * dt;
                db.vy += ny * force * dt;
            }
        }

        for (int d = 0; d < DROPS_PER_POOL; ++d) {
            auto& drop = pool.drops[d];
            if (!drop.alive) continue;

            // Gravity + damping
            drop.vy += grav * dt;
            drop.vx *= 0.985f; drop.vy *= 0.985f;

            // Integrate
            drop.x += drop.vx * dt;
            drop.y += drop.vy * dt;

            // Arena boundary bounce (inelastic — liquid sticks to walls)
            const float e = 0.35f;
            if (drop.x - dr < 2.0f)        { drop.x = 2.0f + dr; drop.vx =  std::abs(drop.vx)*e; }
            if (drop.x + dr > ARENA_W - 2.0f){ drop.x = ARENA_W - 2.0f - dr; drop.vx = -std::abs(drop.vx)*e; }
            if (drop.y - dr < ARENA_Y0 + 2.0f){ drop.y = ARENA_Y0 + 2.0f + dr; drop.vy =  std::abs(drop.vy)*e; }
            if (drop.y + dr > ARENA_H - 2.0f){ drop.y = ARENA_H - 2.0f - dr;   drop.vy = -std::abs(drop.vy)*e*0.6f; }

            // Collision with solid entities
            for (int ei = 0; ei < g_app.num_ents; ++ei) {
                auto& ent = g_app.ents[ei];
                if (!ent.alive) continue;
                float ddx = drop.x - ent.x, ddy = drop.y - ent.y;
                float dist = std::sqrt(ddx*ddx + ddy*ddy);
                float min_d = dr + ent.radius;
                if (dist >= min_d) continue;
                // Push droplet out
                float nx = ddx / dist, ny = ddy / dist;
                float overlap = min_d - dist;
                drop.x += nx * overlap * 0.9f;
                drop.y += ny * overlap * 0.9f;
                // Reflect velocity
                float dot = drop.vx * nx + drop.vy * ny;
                drop.vx -= 2.0f * dot * nx * 0.6f;
                drop.vy -= 2.0f * dot * ny * 0.6f;
                // Splash burst every ~60 frames
                if ((g_app.frame + d + ei) % 62 == 0) {
                    kalpana::Color splash = LIQUID_FILL[pi];
                    splash.a = 1.0f;
                    emit(drop.x, drop.y, 3, splash, 30.0f, 90.0f, 0.15f, 0.4f, 2.0f);
                }
            }
        }

        // Liquid-liquid interactions between different pools
        for (int pj = pi + 1; pj < LIQUID_TYPES; ++pj) {
            auto& pool2 = g_app.pools[pj];
            if (!pool2.active) continue;
            for (int a = 0; a < DROPS_PER_POOL; ++a) {
                auto& da = pool.drops[a];
                if (!da.alive) continue;
                for (int b = 0; b < DROPS_PER_POOL; ++b) {
                    auto& db = pool2.drops[b];
                    if (!db.alive) continue;
                    float dx = db.x - da.x, dy = db.y - da.y;
                    float dist = std::sqrt(dx*dx+dy*dy) + 0.001f;
                    float min_d = pool.drop_radius + pool2.drop_radius;
                    if (dist >= min_d * 0.9f) continue;
                    // Repel different liquids (immiscible)
                    float nx = dx/dist, ny = dy/dist;
                    float force = 140.0f * (1.0f - dist / (min_d * 0.9f));
                    da.vx -= nx * force * dt;
                    da.vy -= ny * force * dt;
                    db.vx += nx * force * dt;
                    db.vy += ny * force * dt;
                    // Chemical burst on interface
                    if ((g_app.frame + a + b) % 75 == 0) {
                        float cx2 = (da.x + db.x) * 0.5f;
                        float cy2 = (da.y + db.y) * 0.5f;
                        kalpana::Color ca = LIQUID_FILL[pi], cb = LIQUID_FILL[pj];
                        kalpana::Color sc = spark_color(ca, cb);
                        emit(cx2, cy2, 4, sc, 25.0f, 70.0f, 0.2f, 0.5f, 2.5f);
                    }
                }
            }
        }
    }
}

// ============================================================================
// Kalpana path helpers
// ============================================================================
static kalpana::Path ngon(float cx, float cy, float r, int n, float ang) {
    kalpana::Path p;
    for (int i = 0; i < n; ++i) {
        float a = ang + float(i)/float(n)*6.2832f;
        float x = cx + std::cos(a)*r, y = cy + std::sin(a)*r;
        if (i==0) p.move_to(x,y); else p.line_to(x,y);
    }
    return p.close(), p;
}

static kalpana::Path star_path(float cx, float cy, float ro, float ri, int n, float ang) {
    kalpana::Path p;
    for (int i = 0; i < n*2; ++i) {
        float a = ang + float(i)/float(n*2)*6.2832f;
        float r = (i%2==0) ? ro : ri;
        float x = cx + std::cos(a)*r, y = cy + std::sin(a)*r;
        if (i==0) p.move_to(x,y); else p.line_to(x,y);
    }
    return p.close(), p;
}

static kalpana::Path entity_path(const Entity& e, float ox=0.f, float oy=0.f) {
    float cx = e.x+ox, cy = e.y+oy;
    switch (e.shape) {
    case EShape::Circle:   { kalpana::Path p; p.circle(cx, cy, e.radius); return p; }
    case EShape::Triangle: return ngon(cx, cy, e.radius, 3, e.spin);
    case EShape::Hexagon:  return ngon(cx, cy, e.radius, 6, e.spin);
    case EShape::Diamond:  return ngon(cx, cy, e.radius, 4, e.spin);
    case EShape::Star:     return star_path(cx, cy, e.radius, e.radius*0.42f, 5, e.spin);
    }
    kalpana::Path p; p.circle(cx,cy,e.radius); return p;
}

// ============================================================================
// Physics: solid entities
// ============================================================================
// ── Phase 1: enforce arena walls (also re-applied after each solver iter) ──────
static void enforce_boundaries(float bounce) {
    for (int i = 0; i < g_app.num_ents; ++i) {
        auto& e = g_app.ents[i]; if (!e.alive) continue;
        const float r = e.radius;
        if (e.x - r < 1.0f)          { e.x = 1.0f  + r;           e.vx =  std::abs(e.vx) * bounce; }
        if (e.x + r > ARENA_W - 1.0f){ e.x = ARENA_W - 1.0f - r;  e.vx = -std::abs(e.vx) * bounce; }
        if (e.y - r < ARENA_Y0 + 1.0f){ e.y = ARENA_Y0 + 1.0f + r; e.vy =  std::abs(e.vy) * bounce; }
        if (e.y + r > ARENA_H - 1.0f){ e.y = ARENA_H - 1.0f - r;  e.vy = -std::abs(e.vy) * bounce; }
    }
}

// ── Phase 2: multi-iteration position-only correction (no velocity change) ────
// Runs SOLVER_ITERS times so that a chain of 50 touching circles all
// propagate the push — eliminating visual overlap completely.
static constexpr int SOLVER_ITERS = 10;
static void solve_positions() {
    for (int iter = 0; iter < SOLVER_ITERS; ++iter) {
        for (int i = 0; i < g_app.num_ents; ++i) {
            auto& a = g_app.ents[i]; if (!a.alive) continue;
            for (int j = i + 1; j < g_app.num_ents; ++j) {
                auto& b = g_app.ents[j]; if (!b.alive) continue;
                float dx = b.x - a.x, dy = b.y - a.y;
                float dist_sq = dx * dx + dy * dy;
                float min_d = a.radius + b.radius;
                if (dist_sq >= min_d * min_d) continue;

                float dist = std::sqrt(dist_sq);
                if (dist < 1e-4f) { dx = 1.0f; dy = 0.0f; dist = 1.0f; }
                float nx = dx / dist, ny = dy / dist;
                // Full positional correction — no slop, push completely apart
                float correction = (min_d - dist);
                float tm = a.mass + b.mass;
                float ka = b.mass / tm, kb = a.mass / tm;
                a.x -= nx * correction * ka;
                a.y -= ny * correction * ka;
                b.x += nx * correction * kb;
                b.y += ny * correction * kb;
            }
        }
        // Re-enforce boundaries after each position-solver pass
        // (position corrections can push entities out of bounds)
        for (int i = 0; i < g_app.num_ents; ++i) {
            auto& e = g_app.ents[i]; if (!e.alive) continue;
            const float r = e.radius;
            if (e.x - r < 1.0f)          e.x = 1.0f  + r;
            if (e.x + r > ARENA_W - 1.0f) e.x = ARENA_W - 1.0f - r;
            if (e.y - r < ARENA_Y0 + 1.0f) e.y = ARENA_Y0 + 1.0f + r;
            if (e.y + r > ARENA_H - 1.0f) e.y = ARENA_H - 1.0f - r;
        }
    }
}

// ── Phase 3: velocity impulse (elastic bounce) — runs once per collision ──────
static void apply_velocity_impulses() {
    for (int i = 0; i < g_app.num_ents; ++i) {
        auto& a = g_app.ents[i]; if (!a.alive) continue;
        for (int j = i + 1; j < g_app.num_ents; ++j) {
            auto& b = g_app.ents[j]; if (!b.alive) continue;
            float dx = b.x - a.x, dy = b.y - a.y;
            float dist_sq = dx * dx + dy * dy;
            float min_d = a.radius + b.radius;
            // Allow a tiny tolerance so we catch touching-but-solved pairs too
            if (dist_sq > (min_d + 0.5f) * (min_d + 0.5f)) continue;

            float dist = std::sqrt(dist_sq); if (dist < 1e-4f) { dx=1.f; dy=0.f; dist=1.f; }
            float nx = dx / dist, ny = dy / dist;

            float rvx = b.vx - a.vx, rvy = b.vy - a.vy;
            float dot = rvx * nx + rvy * ny;
            if (dot > 0.0f) continue; // already separating

            const float restitution = 0.72f;
            float imp = -(1.0f + restitution) * dot / (1.0f / a.mass + 1.0f / b.mass);
            a.vx -= imp / a.mass * nx;  a.vy -= imp / a.mass * ny;
            b.vx += imp / b.mass * nx;  b.vy += imp / b.mass * ny;

            // Speed cap
            auto cap = [](float& vx, float& vy, float maxv) {
                float s = std::sqrt(vx*vx + vy*vy);
                if (s > maxv) { vx = vx/s*maxv; vy = vy/s*maxv; }
            };
            cap(a.vx, a.vy, 300.0f);
            cap(b.vx, b.vy, 300.0f);

            // Collision point for FX
            float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f;

            // ★ SMART SPARK: spectral mix of both entity fill colors, boosted bright
            kalpana::Color sc = spark_color(TYPE_FILL[int(a.type)], TYPE_FILL[int(b.type)]);
            emit(cx, cy,  8, sc, 50.0f, 180.0f, 0.2f, 0.6f, 3.5f);
            emit(cx, cy,  4, kalpana::colors::white(), 80.0f, 220.0f, 0.1f, 0.3f, 2.0f);

            // Elemental reaction
            if (a.react_cd == 0 && b.react_cd == 0) {
                int res = reaction_result(a.type, b.type);
                if (res >= 0) {
                    ++g_app.total_reactions;
                    EType rt = EType(res);
                    kalpana::Color rc = spark_color(TYPE_FILL[int(a.type)], TYPE_FILL[int(b.type)]);
                    emit(cx, cy, 38, rc, 60.0f, 220.0f, 0.45f, 1.0f, 4.5f);
                    emit(cx, cy, 18, kalpana::colors::white(), 120.0f, 280.0f, 0.15f, 0.5f, 2.5f);
                    kalpana::Color nc = TYPE_FILL[res]; nc.a = 1.0f;
                    emit(cx, cy, 20, nc, 100.0f, 240.0f, 0.3f, 0.7f, 3.5f);
                    a.type = rt; a.glow = 1.0f; a.react_cd = 90;
                    b.type = rt; b.glow = 1.0f; b.react_cd = 90;
                    g_app.camera->add_trauma(std::min(0.85f, 0.25f + std::abs(imp) * 0.00004f));
                }
            }
        }
    }
}

// ── Main entity step: integrate → solve positions → apply impulses ────────────
static void step_entities(float dt) {
    // Run 2 sub-steps so fast-moving entities don't tunnel
    const int   SUBSTEPS = 2;
    const float sdt      = dt / float(SUBSTEPS);
    const float bounce   = 0.75f;
    const float grav     = g_app.gravity_on ? 210.0f : 0.0f;

    for (int sub = 0; sub < SUBSTEPS; ++sub) {
        // ① Integrate velocity → position
        for (int i = 0; i < g_app.num_ents; ++i) {
            auto& e = g_app.ents[i]; if (!e.alive) continue;
            e.vy += grav * sdt;
            e.vx *= 0.9992f; e.vy *= 0.9992f;
            e.x  += e.vx * sdt;
            e.y  += e.vy * sdt;
            e.spin += e.spin_rate * sdt;
            if (e.react_cd > 0) --e.react_cd;
            if (e.glow > 0.0f) e.glow -= sdt * 2.0f;
        }

        // ② Boundary bounce (velocity flipped, then position clamped)
        enforce_boundaries(bounce);

        // ③ Multi-iteration position solver — eliminates all overlap
        solve_positions();

        // ④ Velocity impulses for elastic response (once per sub-step)
        apply_velocity_impulses();
    }
}

// ============================================================================
// Scene builder
// ============================================================================
static void build_scene(kalpana::Scene& scene) {
    auto& app = g_app;
    scene.clear_color(kalpana::Color{0.025f, 0.028f, 0.06f, 1.0f});

    // ── Star field ────────────────────────────────────────────────────────────
    for (auto& s : app.stars) {
        float b = s.bri * (0.55f + 0.45f*std::sin(app.t*1.6f + s.phase));
        kalpana::Path p; p.circle(s.x, s.y, b*2.2f);
        scene.add(kalpana::Node::shape(p, kalpana::Paint::fill(
            kalpana::Color{b*0.85f, b*0.88f, b, b})));
    }

    // ── Grid ─────────────────────────────────────────────────────────────────
    {
        kalpana::Color gc{0.08f,0.10f,0.16f,1.0f};
        for (int i=0; i<=24; ++i) {
            float x = float(i)/24.0f * ARENA_W;
            kalpana::Path l; l.move_to(x, ARENA_Y0); l.line_to(x, ARENA_H);
            scene.add(kalpana::Node::shape(l, kalpana::Paint::stroke(gc, 1.0f)));
        }
        for (int j=0; j<=14; ++j) {
            float y = ARENA_Y0 + float(j)/14.0f*(ARENA_H-ARENA_Y0);
            kalpana::Path l; l.move_to(0.0f, y); l.line_to(ARENA_W, y);
            scene.add(kalpana::Node::shape(l, kalpana::Paint::stroke(gc, 1.0f)));
        }
        // Arena border
        kalpana::Path border; border.rect(1.0f, ARENA_Y0, ARENA_W-2.0f, ARENA_H-ARENA_Y0-1.0f);
        scene.add(kalpana::Node::shape(border,
            kalpana::Paint::stroke(kalpana::Color{0.20f,0.28f,0.50f,1.0f}, 2.0f)));
    }

    // ── Camera shake offset for entities ─────────────────────────────────────
    auto shk = app.camera->offset();
    float sx = shk[0], sy = shk[1];

    // ── Liquid pools ──────────────────────────────────────────────────────────
    for (int pi = 0; pi < LIQUID_TYPES; ++pi) {
        const auto& pool = app.pools[pi];
        if (!pool.active) continue;
        float dr = pool.drop_radius;

        kalpana::Color fill = LIQUID_FILL[pi];
        kalpana::Color glow = LIQUID_GLOW[pi];

        for (int a = 0; a < DROPS_PER_POOL; ++a) {
            const auto& da = pool.drops[a];
            if (!da.alive) continue;

            // ── Liquid bridge to nearby drops in same pool ────────────────
            for (int b = a+1; b < DROPS_PER_POOL; ++b) {
                const auto& db = pool.drops[b];
                if (!db.alive) continue;
                float ddx = db.x-da.x, ddy = db.y-da.y;
                float dist = std::sqrt(ddx*ddx+ddy*ddy);
                if (dist > dr*3.8f) continue;

                // Capsule bridge: draw a connecting oval between the two drops
                float t2 = 1.0f - dist/(dr*3.8f);
                float br2 = dr * (0.55f + 0.35f * t2); // bridge half-width
                float mx = (da.x+db.x)*0.5f + sx;
                float my = (da.y+db.y)*0.5f + sy;
                float len2 = (dist + dr*1.2f) * 0.5f;

                // Ellipse stretched along the connection axis
                // Using a path approximation: oval centered at midpoint
                kalpana::Path bridge;
                // Rotation: atan2 of the direction
                float ang = std::atan2(ddy, ddx);
                // Draw rotated ellipse as 4-point cubic approx
                const float k = 0.5523f;
                float rx = len2, ry = br2;
                float ca2 = std::cos(ang), sa2 = std::sin(ang);
                auto rot = [&](float lx, float ly) -> std::pair<float,float> {
                    return {mx + lx*ca2 - ly*sa2, my + lx*sa2 + ly*ca2};
                };
                auto [rx0, ry0] = rot(rx, 0);      bridge.move_to(rx0, ry0);
                auto [cx1, cy1] = rot(rx, k*ry);
                auto [cx2, cy2] = rot(k*rx, ry);
                auto [tx, ty]   = rot(0, ry);
                bridge.cubic_to(cx1,cy1, cx2,cy2, tx,ty);
                auto [cx3, cy3] = rot(-k*rx, ry);
                auto [cx4, cy4] = rot(-rx, k*ry);
                auto [tx2, ty2] = rot(-rx, 0);
                bridge.cubic_to(cx3,cy3, cx4,cy4, tx2,ty2);
                auto [cx5, cy5] = rot(-rx, -k*ry);
                auto [cx6, cy6] = rot(-k*rx, -ry);
                auto [tx3, ty3] = rot(0, -ry);
                bridge.cubic_to(cx5,cy5, cx6,cy6, tx3,ty3);
                auto [cx7, cy7] = rot(k*rx, -ry);
                auto [cx8, cy8] = rot(rx, -k*ry);
                bridge.cubic_to(cx7,cy7, cx8,cy8, rx0,ry0);
                bridge.close();

                kalpana::Color bc = fill; bc.a *= (0.4f + 0.4f * t2);
                scene.add(kalpana::Node::shape(bridge, kalpana::Paint::fill(bc)));
            }

            float dx2 = da.x + sx, dy2 = da.y + sy;

            // Soft outer glow
            kalpana::Path outer; outer.circle(dx2, dy2, dr * 1.7f);
            scene.add(kalpana::Node::shape(outer, kalpana::Paint::fill(glow)));

            // Mid body
            kalpana::Path mid; mid.circle(dx2, dy2, dr);
            scene.add(kalpana::Node::shape(mid, kalpana::Paint::fill(fill)));

            // Specular highlight
            kalpana::Path hi; hi.circle(dx2 - dr*0.3f, dy2 - dr*0.32f, dr*0.28f);
            kalpana::Color hc{1.0f,1.0f,1.0f, fill.a * 0.55f};
            scene.add(kalpana::Node::shape(hi, kalpana::Paint::fill(hc)));
        }
    }

    // ── Solid entities ────────────────────────────────────────────────────────
    for (int i = 0; i < app.num_ents; ++i) {
        const auto& e = app.ents[i]; if (!e.alive) continue;

        kalpana::Color fill = TYPE_FILL[int(e.type)];
        kalpana::Color rim  = TYPE_RIM[int(e.type)];

        // Reaction glow ring
        if (e.glow > 0.0f) {
            kalpana::Path gr; gr.circle(e.x+sx, e.y+sy, e.radius*(1.6f+e.glow*1.0f));
            kalpana::Color gc = rim; gc.a = e.glow * 0.55f;
            scene.add(kalpana::Node::shape(gr, kalpana::Paint::fill(gc)));
        }

        // Speed glow for fast entities
        float spd = std::sqrt(e.vx*e.vx + e.vy*e.vy);
        if (spd > 150.0f) {
            float sg = std::min(0.35f, (spd-150.0f)*0.001f);
            kalpana::Path sgg; sgg.circle(e.x+sx - e.vx*0.01f, e.y+sy - e.vy*0.01f, e.radius*1.3f);
            kalpana::Color sgc = fill; sgc.a = sg;
            scene.add(kalpana::Node::shape(sgg, kalpana::Paint::fill(sgc)));
        }

        // Main body
        scene.add(kalpana::Node::shape(entity_path(e, sx, sy),
            kalpana::Paint::filled_outlined(fill, rim, 2.0f)));

        // Type-specific inner detail
        switch (e.type) {
        case EType::Fire: {
            float fl = 0.65f + 0.35f*std::sin(app.t*14.0f + e.x*0.08f);
            kalpana::Path fi; fi.circle(e.x+sx, e.y+sy, e.radius*fl*0.5f);
            scene.add(kalpana::Node::shape(fi,
                kalpana::Paint::fill(kalpana::Color{1.0f,1.0f,0.7f,0.8f})));
            break;
        }
        case EType::Lightning: {
            // Electric cross
            float zig = 3.0f*std::sin(app.t*22.0f + float(i));
            kalpana::Path l1, l2;
            l1.move_to(e.x+sx - e.radius*0.6f, e.y+sy+zig);
            l1.line_to(e.x+sx + e.radius*0.6f, e.y+sy-zig);
            l2.move_to(e.x+sx+zig, e.y+sy - e.radius*0.6f);
            l2.line_to(e.x+sx-zig, e.y+sy + e.radius*0.6f);
            scene.add(kalpana::Node::shape(l1,
                kalpana::Paint::stroke(kalpana::Color{1.0f,1.0f,0.7f,0.9f}, 2.5f)));
            scene.add(kalpana::Node::shape(l2,
                kalpana::Paint::stroke(kalpana::Color{1.0f,1.0f,0.7f,0.9f}, 2.5f)));
            break;
        }
        case EType::Ice: {
            kalpana::Path c1, c2;
            c1.move_to(e.x+sx-e.radius*0.55f, e.y+sy); c1.line_to(e.x+sx+e.radius*0.55f, e.y+sy);
            c2.move_to(e.x+sx, e.y+sy-e.radius*0.55f); c2.line_to(e.x+sx, e.y+sy+e.radius*0.55f);
            scene.add(kalpana::Node::shape(c1, kalpana::Paint::stroke(kalpana::Color{1.0f,1.0f,1.0f,0.7f}, 2.0f)));
            scene.add(kalpana::Node::shape(c2, kalpana::Paint::stroke(kalpana::Color{1.0f,1.0f,1.0f,0.7f}, 2.0f)));
            break;
        }
        case EType::Plasma: {
            float oa = app.t*5.0f + float(i);
            kalpana::Path od; od.circle(e.x+sx+std::cos(oa)*e.radius*0.62f,
                                       e.y+sy+std::sin(oa)*e.radius*0.62f, 3.5f);
            kalpana::Path od2; od2.circle(e.x+sx+std::cos(oa+2.09f)*e.radius*0.62f,
                                          e.y+sy+std::sin(oa+2.09f)*e.radius*0.62f, 2.5f);
            scene.add(kalpana::Node::shape(od, kalpana::Paint::fill(kalpana::colors::white())));
            scene.add(kalpana::Node::shape(od2, kalpana::Paint::fill(kalpana::colors::white())));
            break;
        }
        case EType::Obsidian: {
            // Inner void
            kalpana::Path ov; ov.circle(e.x+sx, e.y+sy, e.radius*0.45f);
            scene.add(kalpana::Node::shape(ov,
                kalpana::Paint::fill(kalpana::Color{0.0f,0.0f,0.0f,0.85f})));
            break;
        }
        default: break;
        }
    }

    // ── Particles ─────────────────────────────────────────────────────────────
    for (const auto& p : app.particles) {
        float alpha = std::max(0.0f, p.life / p.max_life);
        kalpana::Color c = p.col; c.a = alpha;
        kalpana::Path dot; dot.circle(p.x, p.y, p.radius * alpha + 0.5f);
        scene.add(kalpana::Node::shape(dot, kalpana::Paint::fill(c)));
    }

    // ── Verlet cloth banner ───────────────────────────────────────────────────
    {
        const auto& pts = app.banner->particles();
        float bx = ARENA_W * 0.5f - 55.0f;
        for (std::size_t i = 1; i < pts.size(); ++i) {
            float x0=bx+pts[i-1].pos[0]*3.0f, y0=ARENA_Y0+2.0f+pts[i-1].pos[1]*1.6f;
            float x1=bx+pts[i].pos[0]*3.0f,   y1=ARENA_Y0+2.0f+pts[i].pos[1]*1.6f;
            float t_ = 1.0f - float(i)/float(pts.size());
            kalpana::Color pc = TYPE_FILL[int(EType::Plasma)]; pc.a = t_*0.9f;
            kalpana::Path seg; seg.move_to(x0,y0); seg.line_to(x1,y1);
            scene.add(kalpana::Node::shape(seg, kalpana::Paint::stroke(pc, 2.8f*t_)));
        }
        kalpana::Path dot; dot.circle(bx, ARENA_Y0+2.0f, 4.0f);
        scene.add(kalpana::Node::shape(dot, kalpana::Paint::fill(kalpana::colors::white())));
    }

    // ── Right panel ───────────────────────────────────────────────────────────
    {
        const float px=FW-PANEL_W+2.0f, py=26.0f, pw=PANEL_W-4.0f, ph=FH-78.0f;
        kalpana::Path card; card.round_rect(px, py, pw, ph, 8.0f, 8.0f);
        scene.add(kalpana::Node::shape(card,
            kalpana::Paint::filled_outlined(
                kalpana::Color{0.04f,0.06f,0.12f,0.93f},
                kalpana::Color{0.20f,0.28f,0.50f,1.00f}, 1.5f)));

        float lx = px+10.0f, ly = py+12.0f;

        // Solid entity type bars
        for (int t=0; t<int(EType::Count); ++t) {
            int cnt=0;
            for (int i=0; i<app.num_ents; ++i)
                if (app.ents[i].alive && int(app.ents[i].type)==t) ++cnt;
            kalpana::Path dot; dot.circle(lx+7.0f, ly+7.0f, 7.0f);
            scene.add(kalpana::Node::shape(dot,
                kalpana::Paint::filled_outlined(TYPE_FILL[t], TYPE_RIM[t], 1.5f)));
            float bw = float(cnt)/float(app.num_ents)*(pw-30.0f);
            if (bw>0.5f) {
                kalpana::Path bar; bar.round_rect(lx+18.0f, ly+2.0f, bw, 10.0f, 2.0f, 2.0f);
                kalpana::Color bc=TYPE_FILL[t]; bc.a=0.7f;
                scene.add(kalpana::Node::shape(bar, kalpana::Paint::fill(bc)));
            }
            ly += 20.0f;
        }

        // Liquid pool indicators
        ly += 6.0f;
        kalpana::Path sepl; sepl.move_to(px+6.0f,ly); sepl.line_to(px+pw-6.0f,ly);
        scene.add(kalpana::Node::shape(sepl, kalpana::Paint::stroke(kalpana::Color{0.25f,0.30f,0.48f,0.7f},1.0f)));
        ly += 10.0f;

        for (int p=0; p<LIQUID_TYPES; ++p) {
            float pulse = 0.7f + 0.3f*std::sin(app.t*2.5f + float(p)*1.2f);
            kalpana::Color lc = LIQUID_FILL[p]; lc.a = pulse;
            kalpana::Path ld; ld.circle(lx+7.0f, ly+7.0f, 8.0f*pulse);
            scene.add(kalpana::Node::shape(ld, kalpana::Paint::fill(lc)));
            ly += 20.0f;
        }

        // Spectral mix slider
        ly += 8.0f;
        kalpana::Path sep2; sep2.move_to(px+6.0f,ly); sep2.line_to(px+pw-6.0f,ly);
        scene.add(kalpana::Node::shape(sep2, kalpana::Paint::stroke(kalpana::Color{0.25f,0.30f,0.48f,0.7f},1.0f)));
        ly += 10.0f;

        float mix_t = (std::sin(app.t*0.7f)+1.0f)*0.5f;
        kalpana::Color lv = kalpana::spectral::mix(kalpana::colors::blue(), kalpana::colors::yellow(), mix_t);
        float bw2 = pw - 18.0f;
        kalpana::Path sbar; sbar.round_rect(lx, ly, bw2, 18.0f, 4.0f, 4.0f);
        scene.add(kalpana::Node::shape(sbar,
            kalpana::Paint::filled_outlined(lv, kalpana::Color{0.5f,0.5f,0.5f,0.5f}, 1.0f)));
        kalpana::Path sthumb; sthumb.circle(lx+mix_t*bw2, ly+9.0f, 6.5f);
        scene.add(kalpana::Node::shape(sthumb,
            kalpana::Paint::filled_outlined(kalpana::colors::white(),
                kalpana::Color{0.35f,0.35f,0.35f,1.0f}, 1.0f)));
        ly += 28.0f;

        // Reactions counter bar
        float rp = float(app.total_reactions) / float(std::max(1, app.total_reactions + 15));
        kalpana::Path rbar; rbar.round_rect(lx, ly, rp*(bw2), 12.0f, 3.0f, 3.0f);
        scene.add(kalpana::Node::shape(rbar,
            kalpana::Paint::fill(TYPE_FILL[int(EType::Plasma)])));
    }

    // ── HUD bar ───────────────────────────────────────────────────────────────
    {
        const float bx=1.0f, by=FH-50.0f, bw=ARENA_W, bh=46.0f;
        kalpana::Path bg; bg.round_rect(bx, by, bw, bh, 4.0f, 4.0f);
        scene.add(kalpana::Node::shape(bg,
            kalpana::Paint::filled_outlined(
                kalpana::Color{0.04f,0.06f,0.12f,0.90f},
                kalpana::Color{0.18f,0.26f,0.48f,1.00f}, 1.0f)));

        // Trauma bar
        float trauma = app.camera->trauma();
        kalpana::Path tbg; tbg.round_rect(bx+8.0f, by+27.0f, bw-16.0f, 10.0f, 3.0f, 3.0f);
        scene.add(kalpana::Node::shape(tbg,
            kalpana::Paint::stroke(kalpana::Color{0.22f,0.25f,0.38f,1.0f}, 1.0f)));
        if (trauma > 0.01f) {
            // Trauma bar color is the spectral mix of two random entity type colors
            kalpana::Color tc = brighten(TYPE_FILL[int(EType::Fire)], 1.5f + trauma);
            kalpana::Path fill; fill.round_rect(bx+8.0f, by+27.0f, (bw-16.0f)*trauma, 10.0f, 3.0f, 3.0f);
            scene.add(kalpana::Node::shape(fill, kalpana::Paint::fill(tc)));
        }

        // LEDs row
        kalpana::Color led_beat = (app.frame%60)<30
            ? kalpana::Color{0.15f,1.0f,0.40f,1.0f}
            : kalpana::Color{0.06f,0.32f,0.16f,1.0f};
        kalpana::Path hb; hb.circle(bx+16.0f, by+13.0f, 5.0f);
        scene.add(kalpana::Node::shape(hb, kalpana::Paint::fill(led_beat)));

        kalpana::Color grav_col = app.gravity_on
            ? kalpana::Color{1.0f,0.8f,0.0f,1.0f}
            : kalpana::Color{0.25f,0.25f,0.25f,1.0f};
        kalpana::Path gd; gd.circle(bx+44.0f, by+13.0f, 5.0f);
        scene.add(kalpana::Node::shape(gd, kalpana::Paint::fill(grav_col)));

        // Type color dots
        float ex=bx+72.0f;
        for (int t=0; t<int(EType::Count); ++t) {
            kalpana::Color ec=TYPE_FILL[t];
            kalpana::Path ed; ed.circle(ex, by+13.0f, 5.0f);
            scene.add(kalpana::Node::shape(ed, kalpana::Paint::fill(ec)));
            ex += 14.0f;
        }
        // Liquid dots
        ex += 4.0f;
        for (int p=0; p<LIQUID_TYPES; ++p) {
            kalpana::Color lc = LIQUID_FILL[p]; lc.a = 1.0f;
            kalpana::Path ld; ld.circle(ex, by+13.0f, 5.0f);
            scene.add(kalpana::Node::shape(ld, kalpana::Paint::fill(lc)));
            ex += 14.0f;
        }
    }

    // ── Top bar ───────────────────────────────────────────────────────────────
    {
        kalpana::Path top; top.rect(0.0f, 0.0f, FW, 28.0f);
        scene.add(kalpana::Node::shape(top,
            kalpana::Paint::fill(kalpana::Color{0.025f,0.030f,0.065f,0.96f})));
        kalpana::Path tl; tl.move_to(0.0f,28.0f); tl.line_to(FW,28.0f);
        scene.add(kalpana::Node::shape(tl,
            kalpana::Paint::stroke(kalpana::Color{0.22f,0.30f,0.55f,1.0f}, 1.5f)));

        // Heartbeat bar — 30 dots cycling
        int fps_pos = app.frame % 60;
        for (int d=0; d<45; ++d) {
            bool lit = d < fps_pos/2;
            kalpana::Color dc = lit
                ? TYPE_FILL[d % int(EType::Count)]
                : kalpana::Color{0.12f,0.14f,0.22f,1.0f};
            kalpana::Path dp; dp.circle(9.0f + float(d)*14.0f, 14.0f, 4.0f);
            scene.add(kalpana::Node::shape(dp, kalpana::Paint::fill(dc)));
        }
    }
}

// ============================================================================
// Sokol callbacks
// ============================================================================
static void init_cb() {
    auto& app = g_app;
    sg_desc gfx{}; gfx.environment = sglue_environment(); gfx.logger.func = slog_func;
    sg_setup(&gfx);

    app.pixels.assign(W * H, 0xFF060610u);

    struct Vert { float x, y, u, v; };
    static const Vert verts[] = {{-1,-1,0,1},{1,-1,1,1},{1,1,1,0},{-1,1,0,0}};
    static const uint16_t idx[]={0,1,2,0,2,3};

    { sg_buffer_desc d{}; d.data=SG_RANGE(verts); app.bind.vertex_buffers[0]=sg_make_buffer(d); }
    { sg_buffer_desc d{}; d.usage.index_buffer=true; d.data=SG_RANGE(idx); app.bind.index_buffer=sg_make_buffer(d); }
    { sg_image_desc d{}; d.width=W; d.height=H; d.pixel_format=SG_PIXELFORMAT_RGBA8;
      d.usage.stream_update=true; app.tex_img=sg_make_image(d); }
    { sg_view_desc d{}; d.texture.image=app.tex_img; app.tex_view=sg_make_view(d); }
    { sg_sampler_desc d{}; d.min_filter=SG_FILTER_NEAREST; d.mag_filter=SG_FILTER_NEAREST; app.smp=sg_make_sampler(d); }

    app.bind.views[0]=app.tex_view; app.bind.samplers[0]=app.smp;

    sg_shader_desc shd{};
#if defined(SOKOL_METAL)
    shd.vertex_func.source=VS_METAL; shd.vertex_func.entry="vs";
    shd.fragment_func.source=FS_METAL; shd.fragment_func.entry="fs";
#else
    shd.vertex_func.source=VS_GLSL; shd.fragment_func.source=FS_GLSL;
    shd.texture_sampler_pairs[0].glsl_name="tex";
#endif
    shd.views[0].texture.stage=SG_SHADERSTAGE_FRAGMENT;
    shd.samplers[0].stage=SG_SHADERSTAGE_FRAGMENT;
    shd.texture_sampler_pairs[0].stage=SG_SHADERSTAGE_FRAGMENT;
    shd.texture_sampler_pairs[0].view_slot=0;
    shd.texture_sampler_pairs[0].sampler_slot=0;
    sg_shader shdr=sg_make_shader(shd);

    sg_pipeline_desc pd{};
    pd.shader=shdr; pd.index_type=SG_INDEXTYPE_UINT16;
    pd.layout.attrs[0].format=SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[1].format=SG_VERTEXFORMAT_FLOAT2;
    app.pip=sg_make_pipeline(pd);

    app.pass_action.colors[0].load_action=SG_LOADACTION_CLEAR;
    app.pass_action.colors[0].clear_value={0.0f,0.0f,0.0f,1.0f};

    // Pebble subsystems
    app.camera = std::make_unique<pebble::spandana::ScreenShake2D>(16.0f, 0.07f);
    app.banner = std::make_unique<pebble::spandana::VerletCloth2D>(10, 8.0f,
        pebble::math::vec2{0.7f, -18.0f}, 0.012f);
    app.canvas = std::make_unique<kalpana::Canvas<kalpana::capture_backend>>(W, H);

    // Stars
    for (auto& s : app.stars) {
        s.x = randf(0.0f, ARENA_W);
        s.y = randf(ARENA_Y0, ARENA_H);
        s.bri = randf(0.12f, 0.60f);
        s.phase = randf(0, 6.28f);
    }

    init_entities();
    init_liquid_pools();

    // Spectral swatches
    app.sw_blue   = kalpana::colors::blue();
    app.sw_yellow = kalpana::colors::yellow();
    app.sw_green  = kalpana::spectral::mix(kalpana::colors::blue(), kalpana::colors::yellow(), 0.5f);
    app.sw_red    = kalpana::colors::red();
    app.sw_cyan   = kalpana::Color{0.0f,1.0f,1.0f,1.0f};
    app.sw_orange = kalpana::spectral::mix(kalpana::colors::red(), kalpana::Color{0.0f,1.0f,1.0f,1.0f}, 0.5f);
}

static void frame_cb() {
    auto& app = g_app;
    app.frame++;
    app.t += DT;

    step_entities(DT);
    step_liquid(DT);
    app.camera->update(DT);
    step_particles(DT);

    // Banner
    pebble::math::vec2 anchor{ARENA_W*0.5f - 55.0f, ARENA_Y0+2.0f};
    app.banner->update(anchor, DT);

    // Trail sparks from very fast entities
    for (int i=0; i<app.num_ents; ++i) {
        auto& e = app.ents[i];
        float spd = std::sqrt(e.vx*e.vx+e.vy*e.vy);
        if (spd > 200.0f && (app.frame+i)%6==0) {
            kalpana::Color tc = brighten(TYPE_RIM[int(e.type)], 1.4f); tc.a=0.8f;
            emit(e.x, e.y, 2, tc, 15.0f, 50.0f, 0.08f, 0.25f, 2.0f);
        }
    }

    kalpana::Scene scene;
    build_scene(scene);
    app.canvas->render(scene);
    auto snap = app.canvas->snapshot();

    for (std::size_t i=0; i<snap.size(); ++i) {
        std::uint32_t argb = snap[i];
        std::uint8_t a=(argb>>24)&0xFF, r=(argb>>16)&0xFF, g_=(argb>>8)&0xFF, b=argb&0xFF;
        app.pixels[i]=(std::uint32_t(a)<<24)|(std::uint32_t(b)<<16)|(std::uint32_t(g_)<<8)|r;
    }

    sg_image_data imgd{};
    imgd.mip_levels[0]={app.pixels.data(), app.pixels.size()*sizeof(std::uint32_t)};
    sg_update_image(app.tex_img, imgd);

    sg_pass pass{};
    pass.action=app.pass_action; pass.swapchain=sglue_swapchain();
    sg_begin_pass(pass);
    sg_apply_pipeline(app.pip); sg_apply_bindings(app.bind); sg_draw(0,6,1);
    sg_end_pass(); sg_commit();
}

static void event_cb(const sapp_event* ev) {
    if (ev->type != SAPP_EVENTTYPE_KEY_DOWN) return;
    auto& app = g_app;
    switch (ev->key_code) {
    case SAPP_KEYCODE_ESCAPE: sapp_quit(); break;
    case SAPP_KEYCODE_R:
        app.total_reactions = 0;
        app.particles.clear();
        init_entities();
        init_liquid_pools();
        break;
    case SAPP_KEYCODE_G:
        app.gravity_on = !app.gravity_on;
        break;
    case SAPP_KEYCODE_L: {
        // Liquid wave: scatter all pools upward
        for (int p=0; p<LIQUID_TYPES; ++p) {
            auto& pool = app.pools[p];
            for (auto& d : pool.drops) {
                d.vy -= randf(120.0f, 280.0f);
                d.vx += randf(-60.0f, 60.0f);
            }
        }
        break;
    }
    case SAPP_KEYCODE_SPACE: {
        float cx = ARENA_W*0.5f, cy = (ARENA_H-ARENA_Y0)*0.5f + ARENA_Y0;
        // Mixed-color mega explosion
        kalpana::Color e1 = spark_color(TYPE_FILL[int(EType::Fire)], TYPE_FILL[int(EType::Plasma)]);
        kalpana::Color e2 = spark_color(TYPE_FILL[int(EType::Lightning)], TYPE_FILL[int(EType::Ice)]);
        emit(cx, cy, 72, e1, 80.0f, 320.0f, 0.5f, 1.3f, 5.0f);
        emit(cx, cy, 36, e2, 150.0f, 380.0f, 0.3f, 0.9f, 3.5f);
        emit(cx, cy, 24, kalpana::colors::white(), 200.0f, 420.0f, 0.15f, 0.5f, 2.5f);
        app.camera->add_trauma(0.9f);
        // Blast impulse on all objects
        for (int i=0; i<app.num_ents; ++i) {
            auto& e = app.ents[i];
            float dx=e.x-cx, dy=e.y-cy, d=std::sqrt(dx*dx+dy*dy)+1.0f;
            float force = 28000.0f/(d*d);
            e.vx += dx/d*force; e.vy += dy/d*force;
        }
        for (int p=0; p<LIQUID_TYPES; ++p) {
            for (auto& d : app.pools[p].drops) {
                float ddx=d.x-cx, ddy=d.y-cy, dd=std::sqrt(ddx*ddx+ddy*ddy)+1.0f;
                float force = 12000.0f/(dd*dd);
                d.vx += ddx/dd*force; d.vy += ddy/dd*force;
            }
        }
        break;
    }
    default: break;
    }
}

static void cleanup_cb() { sg_shutdown(); }

sapp_desc sokol_main(int /*argc*/, char** /*argv*/) {
    sapp_desc d{};
    d.init_cb=init_cb; d.frame_cb=frame_cb;
    d.event_cb=event_cb; d.cleanup_cb=cleanup_cb;
    d.width=W; d.height=H;
    d.window_title="Pebble Sandbox v3  [R]eset [G]ravity [L]iquid-wave [SPC]explode [ESC]quit";
    d.icon.sokol_default=true; d.logger.func=slog_func;
    return d;
}
