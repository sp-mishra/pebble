// ============================================================================
// src/app/main.cpp — Spandana-First Pebble App
// Rendering backend: Sokol (platform glue only)
// Simulation/animation/effects: Spandana + Pebble libraries
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

#include "spandana/spandana.hpp"
#include "akruti/spline.hpp"
#include "akruti/primitives.hpp"
#include "akruti/hull.hpp"
#include "akruti/csg.hpp"
#include "gati/gati.hpp"
#include "gati/material.hpp"
#include "gati/elemental.hpp"
#include "gati/material_reaction.hpp"
#include "kalpana/kalpana.hpp"
#include "kalpana/backend/capture_backend.hpp"

#include <array>
#include <vector>
#include <optional>
#include <memory>
#include <cmath>
#include <algorithm>
#include <cstdlib>

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

static constexpr int W = 1060;
static constexpr int H = 700;
static constexpr float FW = float(W);
static constexpr float FH = float(H);
static constexpr float DT = 1.0f / 60.0f;

static constexpr float PANEL_W  = 220.0f;
static constexpr float ARENA_W  = FW - PANEL_W - 2.0f;
static constexpr float ARENA_H  = FH - 52.0f;
static constexpr float ARENA_Y0 = 28.0f;

static float randf(float lo, float hi) {
    return lo + float(std::rand()) / float(RAND_MAX) * (hi - lo);
}

struct Star {
    float x;
    float y;
    float bri;
    float phase;
};

enum class ShapeKind : std::uint8_t { Circle, Box, Capsule, Triangle, RoundedBox, OrientedBox, Sector, ConvexBlob, StarPoly };

// Shape pool the simulation can spawn; extend to add new ShapeKinds to the mix.
static constexpr ShapeKind kSpawnKinds[] = {
    ShapeKind::Circle,     ShapeKind::Box,        ShapeKind::Capsule,
    ShapeKind::Triangle,   ShapeKind::RoundedBox, ShapeKind::OrientedBox,
    ShapeKind::Sector,     ShapeKind::ConvexBlob, ShapeKind::StarPoly,
};
static constexpr std::size_t kPolyMax = 12; // max verts for procedural polys

struct SimBody {
    pebble::ecs::Entity ent{};
    ShapeKind           kind = ShapeKind::Circle;
    pebble::math::vec2  pos{0.0f, 0.0f};
    pebble::math::vec2  vel{0.0f, 0.0f};
    float                size = 16.0f;
    float                rot = 0.0f;
    bool                 alive = true;
    // Cached local-space verts for procedural polygon shapes (ConvexBlob/StarPoly);
    // mapped to world space via poly_verts() using pos/rot each frame.
    std::array<akruti::Vec, kPolyMax> poly{};
    int poly_n = 0;
    float corner = 0.0f; // corner radius for RoundedPoly-based shapes
};

struct LiquidDrop {
    pebble::math::vec2 pos{0.0f, 0.0f};
    pebble::math::vec2 vel{0.0f, 0.0f};
    kalpana::Color     col{0.1f, 0.8f, 1.0f, 0.8f};
    bool               alive = true;
};

struct LiquidPool {
    std::vector<LiquidDrop> drops;
    float                   r = 8.0f;
    float                   viscosity = 0.985f;
    kalpana::Color          base{0.1f, 0.8f, 1.0f, 0.8f};
};

static kalpana::Color brighten(kalpana::Color c, float factor) {
    return {std::min(1.0f, c.r * factor), std::min(1.0f, c.g * factor), std::min(1.0f, c.b * factor), c.a};
}

// Builds cached *local*-space vertex rings for the procedural polygon shapes so the
// physics representation (ConvexPoly/RoundedPoly) and the Kalpana path stay in sync
// even as the body translates/rotates; poly_verts() maps local -> world on demand.
static void init_poly_verts(SimBody& b) {
    if (b.kind == ShapeKind::ConvexBlob) {
        // 2x-oversampled star points around a disc; convex_hull keeps the outer silhouette.
        constexpr int kSamples = kPolyMax;
        containers::static_vector<akruti::Vec, kPolyMax> pts;
        for (int i = 0; i < kSamples; ++i) {
            const float a = float(i) / float(kSamples) * 6.2831853f;
            const float r = randf(0.42f, 1.0f) * b.size;
            (void)pts.push_back({std::cos(a) * r, std::sin(a) * r});
        }
        auto hull = akruti::convex_hull<kPolyMax>(pts);
        b.poly_n = int(hull.verts.size());
        for (int i = 0; i < b.poly_n; ++i) b.poly[i] = hull.verts[i];
    } else { // StarPoly — flat-top hexagon inflated to a rounded star by `corner`.
        b.poly_n = 6;
        for (int i = 0; i < b.poly_n; ++i) {
            const float a = 3.14159265f / 6.0f + float(i) * 1.0471975f;
            b.poly[i] = {std::cos(a) * b.size, std::sin(a) * b.size};
        }
        b.corner = b.size * 0.25f;
    }
}

// Local -> world for the cached poly verts (rotation by b.rot, translation by b.pos).
static void poly_verts(const SimBody& b, std::array<akruti::Vec, kPolyMax>& out, int& n) {
    const float c = std::cos(b.rot), s = std::sin(b.rot);
    n = b.poly_n;
    for (int i = 0; i < n; ++i) {
        const auto& l = b.poly[i];
        out[i] = {b.pos[0] + l.x * c - l.y * s, b.pos[1] + l.x * s + l.y * c};
    }
}

struct AppState {
    sg_pipeline pip{};
    sg_bindings bind{};
    sg_pass_action pass_action{};
    sg_image tex_img{};
    sg_view tex_view{};
    sg_sampler smp{};
    std::vector<std::uint32_t> pixels;

    std::unique_ptr<kalpana::Canvas<kalpana::capture_backend>> canvas;
    std::array<Star, 90> stars{};

    std::unique_ptr<pebble::spandana::ScreenShake2D> camera;
    std::unique_ptr<pebble::spandana::VerletCloth2D> cloth;
    pebble::spandana::Timeline timeline;

    pebble::ecs::World world;
    pebble::ecs::Entity actor{};

    pebble::math::vec2 actor_pos{140.0f, ARENA_Y0 + 120.0f};
    pebble::math::vec2 actor_prev{140.0f, ARENA_Y0 + 120.0f};
    float actor_rot = 0.0f;

    akruti::CubicBezierCurve path{};

    pebble::spandana::FlipbookClip idle_clip{"idle", 0, 6, 8.0f, pebble::spandana::LoopMode::Loop};
    pebble::spandana::FlipbookClip burst_clip{"burst", 6, 8, 14.0f, pebble::spandana::LoopMode::Once};
    pebble::spandana::SpriteAnimator animator{};

    pebble::spandana::BlendSpace2D blend_space{};
    pebble::spandana::BlendSpaceAnimator blend_anim{};

    pebble::spandana::Skeleton2D skeleton{};
    int bone_root = -1;
    int bone_arm = -1;

    std::optional<pebble::spandana::edsl::ParticleBurstAction> burst;
    float burst_age = 0.0f;
    float burst_life = 0.0f;

    std::vector<pebble::spandana::ShardSpawnDesc> shards;
    float shard_age = 0.0f;

    std::vector<SimBody> bodies;
    int reactions = 0;
    akruti::CsgPtr csg_field;
    float csg_heat = 0.0f;
    std::array<LiquidPool, 3> liquids{};

    float t = 0.0f;
    int frame = 0;
};

static AppState g_app;

static void rebuild_timeline();
static float bounding_radius(const SimBody& b);

static gati::MaterialComponent material_for_index(int i) {
    switch (i % 6) {
        case 0: return gati::MaterialComponent::Ice();
        case 1: return gati::MaterialComponent::Water();
        case 2: return gati::MaterialComponent::Glass();
        case 3: return gati::MaterialComponent::Steel();
        case 4: return gati::MaterialComponent::Wood();
        default: return gati::MaterialComponent::Lava();
    }
}

static gati::ElementType element_for_index(int i) {
    switch (i % 7) {
        case 0: return gati::ElementType::Water;
        case 1: return gati::ElementType::Lava;
        case 2: return gati::ElementType::Fire;
        case 3: return gati::ElementType::Wood;
        case 4: return gati::ElementType::Metal;
        case 5: return gati::ElementType::Acid;
        default: return gati::ElementType::Electricity;
    }
}

static kalpana::Color color_for_element(gati::ElementType e) {
    switch (e) {
        case gati::ElementType::Water: return {0.0f, 0.6f, 1.0f, 1.0f};
        case gati::ElementType::Lava: return {1.0f, 0.3f, 0.05f, 1.0f};
        case gati::ElementType::Fire: return {1.0f, 0.45f, 0.05f, 1.0f};
        case gati::ElementType::Wood: return {0.6f, 0.4f, 0.18f, 1.0f};
        case gati::ElementType::Metal: return {0.75f, 0.78f, 0.85f, 1.0f};
        case gati::ElementType::Acid: return {0.2f, 1.0f, 0.2f, 1.0f};
        case gati::ElementType::Electricity: return {1.0f, 1.0f, 0.25f, 1.0f};
        case gati::ElementType::Obsidian: return {0.45f, 0.1f, 0.65f, 1.0f};
        case gati::ElementType::Ice: return {0.7f, 0.95f, 1.0f, 1.0f};
        default: return {0.8f, 0.8f, 0.8f, 1.0f};
    }
}

static kalpana::Path body_shape_path(const SimBody& b, float sx, float sy) {
    kalpana::Path p;
    const float x = b.pos[0] + sx;
    const float y = b.pos[1] + sy;
    const float s = b.size;
    switch (b.kind) {
        case ShapeKind::Circle: p.circle(x, y, s); break;
        case ShapeKind::Box: p.rect(x - s, y - s, s * 2.0f, s * 2.0f); break;
        case ShapeKind::RoundedBox: p.round_rect(x - s, y - s, s * 2.0f, s * 2.0f, s * 0.35f, s * 0.35f); break;
        case ShapeKind::Capsule: p.round_rect(x - s * 1.3f, y - s * 0.6f, s * 2.6f, s * 1.2f, s * 0.6f, s * 0.6f); break;
        case ShapeKind::Triangle: {
            p.move_to(x + std::cos(b.rot) * s, y + std::sin(b.rot) * s);
            p.line_to(x + std::cos(b.rot + 2.0944f) * s, y + std::sin(b.rot + 2.0944f) * s);
            p.line_to(x + std::cos(b.rot + 4.1888f) * s, y + std::sin(b.rot + 4.1888f) * s);
            p.close();
            break;
        }
        case ShapeKind::OrientedBox: {
            const float c = std::cos(b.rot), sn = std::sin(b.rot);
            auto pt = [&](float lx, float ly) {
                return std::pair<float, float>{x + lx * c - ly * sn, y + lx * sn + ly * c};
            };
            auto [x0, y0] = pt(-s, -s * 0.75f);
            auto [x1, y1] = pt(s, -s * 0.75f);
            auto [x2, y2] = pt(s, s * 0.75f);
            auto [x3, y3] = pt(-s, s * 0.75f);
            p.move_to(x0, y0); p.line_to(x1, y1); p.line_to(x2, y2); p.line_to(x3, y3); p.close();
            break;
        }
        case ShapeKind::Sector: {
            p.move_to(x, y);
            const float a0 = b.rot - 0.7f;
            const float a1 = b.rot + 0.7f;
            constexpr int kArc = 12;
            for (int i = 0; i <= kArc; ++i) {
                float t = float(i) / float(kArc);
                float a = a0 + (a1 - a0) * t;
                p.line_to(x + std::cos(a) * s * 1.4f, y + std::sin(a) * s * 1.4f);
            }
            p.close();
            break;
        }
        case ShapeKind::ConvexBlob:
        case ShapeKind::StarPoly: {
            std::array<akruti::Vec, kPolyMax> wv{};
            int n = 0;
            poly_verts(b, wv, n);
            for (int i = 0; i < n; ++i) {
                if (i == 0) p.move_to(wv[i].x + sx, wv[i].y + sy);
                else p.line_to(wv[i].x + sx, wv[i].y + sy);
            }
            p.close();
            break;
        }
    }
    return p;
}

static void init_sim_bodies() {
    auto& app = g_app;
    app.bodies.clear();
    app.reactions = 0;
    constexpr int kBodies = 20;
    for (int i = 0; i < kBodies; ++i) {
        SimBody b;
        b.ent = app.world.spawn();
        b.kind = kSpawnKinds[i % (sizeof(kSpawnKinds) / sizeof(kSpawnKinds[0]))];
        b.size = randf(11.0f, 18.0f);
        b.rot = randf(0.0f, 6.2832f);
        b.pos = pebble::math::vec2(randf(60.0f, ARENA_W - 60.0f), randf(ARENA_Y0 + 60.0f, ARENA_H - 60.0f));
        b.vel = pebble::math::vec2(randf(-90.0f, 90.0f), randf(-90.0f, 90.0f));
        if (b.kind == ShapeKind::ConvexBlob || b.kind == ShapeKind::StarPoly) init_poly_verts(b);
        app.world.add<gati::Transform>(b.ent, {.position = b.pos, .prev_position = b.pos});
        app.world.add<gati::MaterialComponent>(b.ent, material_for_index(i));
        app.world.add<gati::ElementalComponent>(b.ent, {.type = element_for_index(i)});
        app.bodies.push_back(b);
    }
}

static void init_liquids() {
    auto& app = g_app;
    std::array<kalpana::Color, 3> palette{{
        {0.0f, 0.92f, 1.0f, 0.82f},
        {0.15f, 1.0f, 0.25f, 0.82f},
        {1.0f, 0.35f, 0.06f, 0.82f}
    }};
    for (int i = 0; i < 3; ++i) {
        auto& p = app.liquids[i];
        p.drops.clear();
        p.base = palette[i];
        p.r = 7.0f + float(i) * 1.2f;
        p.viscosity = 0.987f - float(i) * 0.006f;
        constexpr int kDrops = 36;
        p.drops.reserve(kDrops);
        float cx = randf(100.0f, ARENA_W - 100.0f);
        float cy = randf(ARENA_Y0 + 100.0f, ARENA_H - 100.0f);
        for (int d = 0; d < kDrops; ++d) {
            LiquidDrop ld;
            ld.pos = pebble::math::vec2(cx + randf(-34.0f, 34.0f), cy + randf(-24.0f, 24.0f));
            ld.vel = pebble::math::vec2(randf(-20.0f, 20.0f), randf(-20.0f, 20.0f));
            ld.col = p.base;
            p.drops.push_back(ld);
        }
    }
}

static void spawn_break_shards(pebble::math::vec2 impact) {
    auto& app = g_app;
    akruti::Poly poly{
        akruti::Vec{impact[0] - 20.0f, impact[1] - 20.0f},
        akruti::Vec{impact[0] + 20.0f, impact[1] - 20.0f},
        akruti::Vec{impact[0] + 20.0f, impact[1] + 20.0f},
        akruti::Vec{impact[0] - 20.0f, impact[1] + 20.0f}
    };
    auto shards = pebble::spandana::DestructionEngine::shatter_polygon(poly, impact, 12, 220.0f);
    app.shards.insert(app.shards.end(), shards.begin(), shards.end());
    app.shard_age = 0.0f;
}

static void step_liquids(float dt) {
    auto& app = g_app;
    for (auto& pool : app.liquids) {
        const float r = pool.r;
        for (std::size_t i = 0; i < pool.drops.size(); ++i) {
            auto& a = pool.drops[i];
            if (!a.alive) continue;
            for (std::size_t j = i + 1; j < pool.drops.size(); ++j) {
                auto& b = pool.drops[j];
                if (!b.alive) continue;
                pebble::math::vec2 d = b.pos - a.pos;
                float dist = std::sqrt(d[0] * d[0] + d[1] * d[1]) + 1e-4f;
                float rest = r * 2.0f;
                float maxd = r * 4.0f;
                if (dist > maxd) continue;
                pebble::math::vec2 n = d * (1.0f / dist);
                float f = (dist < rest) ? -110.0f * (rest - dist) / rest : 65.0f * (dist - rest) / (maxd - rest);
                a.vel = a.vel - n * f * dt;
                b.vel = b.vel + n * f * dt;
            }
        }

        for (auto& d : pool.drops) {
            if (!d.alive) continue;
            d.vel[1] += 100.0f * dt;
            d.vel = d.vel * pool.viscosity;
            d.pos = d.pos + d.vel * dt;

            if (d.pos[0] < r) { d.pos[0] = r; d.vel[0] = std::abs(d.vel[0]) * 0.45f; }
            if (d.pos[0] > ARENA_W - r) { d.pos[0] = ARENA_W - r; d.vel[0] = -std::abs(d.vel[0]) * 0.45f; }
            if (d.pos[1] < ARENA_Y0 + r) { d.pos[1] = ARENA_Y0 + r; d.vel[1] = std::abs(d.vel[1]) * 0.45f; }
            if (d.pos[1] > ARENA_H - r) { d.pos[1] = ARENA_H - r; d.vel[1] = -std::abs(d.vel[1]) * 0.32f; }

            for (auto& b : app.bodies) {
                if (!b.alive || !app.world.alive(b.ent)) continue;
                float br = bounding_radius(b);
                pebble::math::vec2 dd = d.pos - b.pos;
                float dist = std::sqrt(dd[0] * dd[0] + dd[1] * dd[1]) + 1e-4f;
                float min_d = br + r;
                if (dist >= min_d) continue;
                pebble::math::vec2 n = dd * (1.0f / dist);
                float overlap = min_d - dist;
                d.pos = d.pos + n * overlap;
                float vn = d.vel[0] * n[0] + d.vel[1] * n[1];
                d.vel = d.vel - n * (1.6f * vn);
                b.vel = b.vel - n * (overlap * 9.0f);

                if (auto* mat = app.world.get<gati::MaterialComponent>(b.ent)) {
                    float liquid_temp = (pool.base.r > 0.8f) ? 800.0f : ((pool.base.g > 0.9f) ? 80.0f : 20.0f);
                    float delta = (liquid_temp - mat->temperature) * 0.0025f;
                    mat->update_thermodynamics(delta, dt);
                }
            }
        }
    }
}

static float bounding_radius(const SimBody& b) {
    switch (b.kind) {
        case ShapeKind::Circle: return b.size;
        case ShapeKind::Capsule: return b.size * 1.3f;
        case ShapeKind::Sector: return b.size * 1.4f;
        case ShapeKind::ConvexBlob:
        case ShapeKind::StarPoly: {
            float m2 = 0.0f;
            for (int i = 0; i < b.poly_n; ++i) m2 = std::max(m2, b.poly[i].x * b.poly[i].x + b.poly[i].y * b.poly[i].y);
            return std::sqrt(m2) + b.corner;
        }
        default: return b.size * 1.2f;
    }
}

using AkrutiShapeVar = std::variant<akruti::Circle, akruti::Box, akruti::OrientedBox, akruti::Capsule, akruti::Triangle, akruti::RoundedBox, akruti::Sector, akruti::ConvexPoly<kPolyMax>, akruti::RoundedPoly<kPolyMax>>;

static AkrutiShapeVar get_akruti_shape(const SimBody& b) {
    const akruti::Vec pos{b.pos[0], b.pos[1]};
    const float s = b.size;
    switch (b.kind) {
        case ShapeKind::Circle:
            return akruti::Circle{pos, s};
        case ShapeKind::Box:
            return akruti::Box{pos, akruti::Vec{s, s}};
        case ShapeKind::RoundedBox:
            return akruti::RoundedBox{pos, akruti::Vec{s, s}, s * 0.35f};
        case ShapeKind::Capsule: {
            return akruti::Capsule{
                akruti::Vec{pos.x - s * 0.7f, pos.y},
                akruti::Vec{pos.x + s * 0.7f, pos.y},
                s * 0.6f
            };
        }
        case ShapeKind::Triangle: {
            return akruti::Triangle{
                akruti::Vec{pos.x + std::cos(b.rot) * s, pos.y + std::sin(b.rot) * s},
                akruti::Vec{pos.x + std::cos(b.rot + 2.0944f) * s, pos.y + std::sin(b.rot + 2.0944f) * s},
                akruti::Vec{pos.x + std::cos(b.rot + 4.1888f) * s, pos.y + std::sin(b.rot + 4.1888f) * s}
            };
        }
        case ShapeKind::OrientedBox: {
            return akruti::OrientedBox::from_angle(pos, akruti::Vec{s, s * 0.75f}, b.rot);
        }
        case ShapeKind::Sector: {
            const akruti::Vec dir{std::cos(b.rot), std::sin(b.rot)};
            return akruti::Sector::from_direction(pos, s * 1.4f, 0.7f, dir);
        }
        case ShapeKind::ConvexBlob:
        case ShapeKind::StarPoly: {
            std::array<akruti::Vec, kPolyMax> wv{};
            int n = 0;
            poly_verts(b, wv, n);
            if (b.kind == ShapeKind::ConvexBlob) {
                akruti::ConvexPoly<kPolyMax> cp;
                for (int i = 0; i < n; ++i) (void)cp.verts.push_back(wv[i]);
                return cp;
            }
            akruti::RoundedPoly<kPolyMax> rp;
            rp.radius = b.corner;
            for (int i = 0; i < n; ++i) (void)rp.base.verts.push_back(wv[i]);
            return rp;
        }
    }
    return akruti::Circle{pos, s};
}

static akruti::Manifold test_body_collision(const SimBody& a, const SimBody& b) {
    auto sa = get_akruti_shape(a);
    auto sb = get_akruti_shape(b);
    return std::visit([&](const auto& shape_a) -> akruti::Manifold {
        return std::visit([&](const auto& shape_b) -> akruti::Manifold {
            using TypeA = std::decay_t<decltype(shape_a)>;
            using TypeB = std::decay_t<decltype(shape_b)>;
            if constexpr (std::is_same_v<TypeA, akruti::Circle> && std::is_same_v<TypeB, akruti::Circle>) {
                return akruti::collide_circle_circle(shape_a, shape_b);
            } else if constexpr (std::is_same_v<TypeA, akruti::Circle> && std::is_same_v<TypeB, akruti::Capsule>) {
                return akruti::collide_circle_capsule(shape_a, shape_b);
            } else if constexpr (std::is_same_v<TypeA, akruti::Capsule> && std::is_same_v<TypeB, akruti::Circle>) {
                auto m = akruti::collide_circle_capsule(shape_b, shape_a);
                if (m.hit) m.normal = -m.normal;
                return m;
            } else if constexpr (std::is_same_v<TypeA, akruti::Circle> && std::is_same_v<TypeB, akruti::Box>) {
                return akruti::collide_circle_box(shape_a, shape_b);
            } else if constexpr (std::is_same_v<TypeA, akruti::Box> && std::is_same_v<TypeB, akruti::Circle>) {
                auto m = akruti::collide_circle_box(shape_b, shape_a);
                if (m.hit) m.normal = -m.normal;
                return m;
            } else if constexpr (std::is_same_v<TypeA, akruti::OrientedBox> && std::is_same_v<TypeB, akruti::OrientedBox>) {
                return akruti::collide_obb_obb(shape_a, shape_b);
            } else if constexpr (std::is_same_v<TypeA, akruti::Box> && std::is_same_v<TypeB, akruti::Box>) {
                return akruti::collide_box_box(shape_a, shape_b);
            } else {
                return akruti::collide_gjk_warm_started(shape_a, shape_b);
            }
        }, sb);
    }, sa);
}

static void step_sim_bodies(float dt) {
    auto& app = g_app;
    for (auto& b : app.bodies) {
        b.alive = app.world.alive(b.ent);
        if (!b.alive) continue;
        b.pos = b.pos + b.vel * dt;
        b.rot += dt * 0.9f;
        float r = bounding_radius(b);
        if (b.pos[0] < r) { b.pos[0] = r; b.vel[0] = std::abs(b.vel[0]); }
        if (b.pos[0] > ARENA_W - r) { b.pos[0] = ARENA_W - r; b.vel[0] = -std::abs(b.vel[0]); }
        if (b.pos[1] < ARENA_Y0 + r) { b.pos[1] = ARENA_Y0 + r; b.vel[1] = std::abs(b.vel[1]); }
        if (b.pos[1] > ARENA_H - r) { b.pos[1] = ARENA_H - r; b.vel[1] = -std::abs(b.vel[1]); }
        if (auto* tr = app.world.get<gati::Transform>(b.ent)) {
            tr->prev_position = tr->position;
            tr->position = b.pos;
        }
    }

    // 4 iterative position/velocity constraint relaxation passes using Akruti SAT/GJK
    constexpr int kSolverIterations = 4;
    for (int iter = 0; iter < kSolverIterations; ++iter) {
        for (std::size_t i = 0; i < app.bodies.size(); ++i) {
            auto& a = app.bodies[i];
            if (!a.alive) continue;
            for (std::size_t j = i + 1; j < app.bodies.size(); ++j) {
                auto& b = app.bodies[j];
                if (!b.alive) continue;

                // Broadphase bounding circle rejection
                const float ra = bounding_radius(a);
                const float rb = bounding_radius(b);
                pebble::math::vec2 d = b.pos - a.pos;
                float dist_sq = d[0] * d[0] + d[1] * d[1];
                float min_d = ra + rb;
                if (dist_sq >= min_d * min_d) continue;

                // Exact Akruti narrowphase (SAT 2-point / analytic / warm-started GJK-EPA)
                auto manifold = test_body_collision(a, b);
                if (!manifold.hit || manifold.depth <= 0.0f) continue;

                pebble::math::vec2 n{manifold.normal.x, manifold.normal.y};
                float len_n = std::sqrt(n[0] * n[0] + n[1] * n[1]);
                if (len_n > 1e-5f) {
                    n = n * (1.0f / len_n);
                } else {
                    n = pebble::math::vec2(1.0f, 0.0f);
                }

                // Positional separation (hard anti-penetration projection)
                float separation = manifold.depth * 0.55f;
                a.pos = a.pos - n * separation;
                b.pos = b.pos + n * separation;

                // Boundary clamping after separation
                float ra_bound = bounding_radius(a);
                float rb_bound = bounding_radius(b);
                a.pos[0] = std::clamp(a.pos[0], ra_bound, ARENA_W - ra_bound);
                a.pos[1] = std::clamp(a.pos[1], ARENA_Y0 + ra_bound, ARENA_H - ra_bound);
                b.pos[0] = std::clamp(b.pos[0], rb_bound, ARENA_W - rb_bound);
                b.pos[1] = std::clamp(b.pos[1], ARENA_Y0 + rb_bound, ARENA_H - rb_bound);

                // Velocity impulse reflection along contact normal
                float va = a.vel[0] * n[0] + a.vel[1] * n[1];
                float vb = b.vel[0] * n[0] + b.vel[1] * n[1];
                float rel_vel = vb - va;
                if (rel_vel < 0.0f) {
                    float restitution = 0.85f;
                    float impulse = -(1.0f + restitution) * rel_vel * 0.5f;
                    a.vel = a.vel - n * impulse;
                    b.vel = b.vel + n * impulse;
                }

                if (iter == 0) {
                    float impact = std::abs(rel_vel);
                    if (impact > 170.0f) {
                        for (auto* body_ptr : std::array<SimBody*, 2>{&a, &b}) {
                            auto* mat = app.world.get<gati::MaterialComponent>(body_ptr->ent);
                            if (!mat || !app.world.alive(body_ptr->ent)) continue;
                            bool brittle = mat->phase_fractions.solid() > 0.7f && mat->params.ultimate_strain < 0.01f;
                            bool weakened = mat->damage > 0.55f;
                            if (brittle || weakened) {
                                spawn_break_shards(body_ptr->pos);
                                app.world.despawn(body_ptr->ent);
                                body_ptr->alive = false;
                            }
                        }
                    }

                    gati::ContactEvent ce{};
                    ce.a = a.ent.index;
                    ce.b = b.ent.index;
                    ce.normal = n;
                    ce.depth = manifold.depth;
                    if (!manifold.points.empty()) {
                        ce.point = pebble::math::vec2(manifold.points[0].point.x, manifold.points[0].point.y);
                    } else {
                        ce.point = (a.pos + b.pos) * 0.5f;
                    }

                    auto* ea = app.world.get<gati::ElementalComponent>(a.ent);
                    auto* eb = app.world.get<gati::ElementalComponent>(b.ent);
                    auto er = gati::ElementalReactionMatrix::evaluate(
                        ea ? ea->type : gati::ElementType::Neutral,
                        eb ? eb->type : gati::ElementType::Neutral);
                    if (er.reacted) ++app.reactions;

                    gati::ElementalReactionMatrix::process_contact(app.world, ce);
                    gati::MaterialReactionSystem::evaluate_reactions(app.world, ce);
                }
            }
        }
    }

    gati::MaterialReactionSystem::step_thermodynamics(app.world, dt, 22.0f);
}

static void start_burst(pebble::math::vec2 origin) {
    auto& app = g_app;
    app.burst = pebble::spandana::edsl::particle_burst()
        .at(origin)
        .count(48)
        .speed(70.0f, 210.0f)
        .lifetime(0.55f);
    app.burst_age = 0.0f;
    app.burst_life = 0.55f;
    if (app.burst) app.burst->on_start();
}

static void rebuild_timeline() {
    using namespace pebble::spandana::edsl;
    using namespace pebble::spandana::ease;

    auto& app = g_app;
    app.timeline = pebble::spandana::Timeline{};
    app.actor_pos = pebble::math::vec2(140.0f, ARENA_Y0 + 120.0f);
    app.actor_prev = app.actor_pos;
    app.actor_rot = 0.0f;
    app.animator.play(&app.idle_clip);

    app.timeline.add(
        set_material(app.world, app.actor, gati::MaterialComponent::Ice()).temperature(-12.0f),
        follow_path(app.actor_pos, app.path).duration(2.2f).orient_to_tangent(app.actor_rot).ease(in_out_cubic),
        apply_heat(app.world).at({500.0f, ARENA_Y0 + 180.0f}).radius(220.0f).temperature(520.0f).duration(2.3f),
        shake_camera(*app.camera).trauma(0.45f).duration(0.3f),
        flipbook(app.animator).play(&app.burst_clip),
        callback([&]() {
            auto impact = app.actor_pos;
            start_burst(impact);

            akruti::Poly poly{
                akruti::Vec{impact[0] - 24.0f, impact[1] - 24.0f},
                akruti::Vec{impact[0] + 24.0f, impact[1] - 24.0f},
                akruti::Vec{impact[0] + 24.0f, impact[1] + 24.0f},
                akruti::Vec{impact[0] - 24.0f, impact[1] + 24.0f}
            };
            app.shards = pebble::spandana::DestructionEngine::shatter_polygon(poly, impact, 10, 180.0f);
            app.shard_age = 0.0f;
        }),
        spring(app.actor_pos).target(pebble::math::vec2(250.0f, ARENA_Y0 + 130.0f), 220.0f, 15.0f, 1.0f),
        tween(app.actor_rot).to(6.2832f, 0.8f).ease(in_out_sine),
        flipbook(app.animator).play(&app.idle_clip)
    );

    auto a = akruti::csg_leaf(akruti::Circle{{0.0f, 0.0f}, 32.0f});
    auto b = akruti::csg_leaf(akruti::RoundedBox{{0.0f, 0.0f}, {26.0f, 16.0f}, 6.0f});
    app.csg_field = akruti::csg_smooth_union(std::move(a), std::move(b), 10.0f);
}

static void build_scene(kalpana::Scene& scene) {
    auto& app = g_app;
    scene.clear_color(kalpana::Color{0.025f, 0.028f, 0.06f, 1.0f});

    for (const auto& s : app.stars) {
        float b = s.bri * (0.55f + 0.45f * std::sin(app.t * 1.6f + s.phase));
        kalpana::Path p;
        p.circle(s.x, s.y, b * 2.1f);
        scene.add(kalpana::Node::shape(p, kalpana::Paint::fill(kalpana::Color{b * 0.85f, b * 0.88f, b, b})));
    }

    {
        kalpana::Color gc{0.08f, 0.10f, 0.16f, 1.0f};
        for (int i = 0; i <= 24; ++i) {
            float x = float(i) / 24.0f * ARENA_W;
            kalpana::Path l;
            l.move_to(x, ARENA_Y0);
            l.line_to(x, ARENA_H);
            scene.add(kalpana::Node::shape(l, kalpana::Paint::stroke(gc, 1.0f)));
        }
        for (int j = 0; j <= 14; ++j) {
            float y = ARENA_Y0 + float(j) / 14.0f * (ARENA_H - ARENA_Y0);
            kalpana::Path l;
            l.move_to(0.0f, y);
            l.line_to(ARENA_W, y);
            scene.add(kalpana::Node::shape(l, kalpana::Paint::stroke(gc, 1.0f)));
        }
        kalpana::Path border;
        border.rect(1.0f, ARENA_Y0, ARENA_W - 2.0f, ARENA_H - ARENA_Y0 - 1.0f);
        scene.add(kalpana::Node::shape(border,
            kalpana::Paint::stroke(kalpana::Color{0.20f, 0.28f, 0.50f, 1.0f}, 2.0f)));
    }

    auto shk = app.camera->offset();
    float sx = shk[0];
    float sy = shk[1];

    {
        kalpana::Path curve;
        constexpr int kSamples = 40;
        for (int i = 0; i <= kSamples; ++i) {
            float t = float(i) / float(kSamples);
            auto p = app.path.evaluate(t);
            if (i == 0) curve.move_to(p.x + sx, p.y + sy);
            else curve.line_to(p.x + sx, p.y + sy);
        }
        scene.add(kalpana::Node::shape(curve,
            kalpana::Paint::stroke(kalpana::Color{0.22f, 0.8f, 1.0f, 0.6f}, 2.0f)));
    }

    auto* mat = app.world.get<gati::MaterialComponent>(app.actor);
    float heat = mat ? std::clamp((mat->temperature + 20.0f) / 520.0f, 0.0f, 1.0f) : 0.0f;
    kalpana::Color cold{0.55f, 0.95f, 1.0f, 1.0f};
    kalpana::Color hot{1.0f, 0.0f, 0.88f, 1.0f};
    kalpana::Color actor_col = kalpana::spectral::mix(cold, hot, heat);

    if (app.csg_field) {
        akruti::Vec lp{app.actor_pos[0] - 480.0f, app.actor_pos[1] - (ARENA_Y0 + 180.0f)};
        float sdf = app.csg_field->sdf(lp);
        app.csg_heat = std::clamp(1.0f - sdf / 60.0f, 0.0f, 1.0f);
        actor_col = kalpana::spectral::mix(actor_col, kalpana::Color{1.0f, 0.9f, 0.2f, 1.0f}, app.csg_heat * 0.5f);
    }

    float px = app.actor_pos[0] + sx;
    float py = app.actor_pos[1] + sy;
    float pulse = 0.92f + 0.08f * std::sin(app.t * 10.0f + float(app.animator.current_frame));
    float body_r = 19.0f * pulse;

    {
        kalpana::Path aura;
        aura.circle(px, py, body_r * 1.9f);
        kalpana::Color ac = actor_col;
        ac.a = 0.24f;
        scene.add(kalpana::Node::shape(aura, kalpana::Paint::fill(ac)));

        kalpana::Path body;
        body.circle(px, py, body_r);
        scene.add(kalpana::Node::shape(body,
            kalpana::Paint::filled_outlined(actor_col, brighten(actor_col, 1.35f), 2.0f)));
    }

    if (app.bone_root >= 0 && app.bone_arm >= 0) {
        const auto& bones = app.skeleton.bones();
        const auto& root = bones[app.bone_root].world_transform.position;
        const auto& arm = bones[app.bone_arm].world_transform.position;
        kalpana::Path limb;
        limb.move_to(px + root[0], py + root[1]);
        limb.line_to(px + arm[0], py + arm[1]);
        scene.add(kalpana::Node::shape(limb,
            kalpana::Paint::stroke(kalpana::Color{1.0f, 1.0f, 1.0f, 0.88f}, 2.0f)));
    }

    if (!app.shards.empty()) {
        float alpha = std::max(0.0f, 1.0f - app.shard_age * 1.2f);
        for (const auto& s : app.shards) {
            kalpana::Path dot;
            dot.circle(s.centroid[0] + sx, s.centroid[1] + sy, 2.5f + 6.0f * alpha);
            scene.add(kalpana::Node::shape(dot,
                kalpana::Paint::fill(kalpana::Color{1.0f, 0.8f, 1.0f, alpha * 0.7f})));
        }
    }

    if (app.burst) {
        const auto& particles = app.burst->particles();
        for (const auto& p : particles) {
            float life_t = std::max(0.0f, 1.0f - p.age / std::max(0.001f, p.lifetime));
            kalpana::Path dot;
            dot.circle(p.position[0] + sx, p.position[1] + sy, p.size * life_t * 0.35f + 0.5f);
            kalpana::Color c = kalpana::spectral::mix(cold, hot, 1.0f - life_t);
            c.a = life_t;
            scene.add(kalpana::Node::shape(dot, kalpana::Paint::fill(c)));
        }
    }

    // Render liquid blobs + bridges.
    for (const auto& pool : app.liquids) {
        for (std::size_t i = 0; i < pool.drops.size(); ++i) {
            const auto& a = pool.drops[i];
            if (!a.alive) continue;
            for (std::size_t j = i + 1; j < pool.drops.size(); ++j) {
                const auto& b = pool.drops[j];
                if (!b.alive) continue;
                float dx = b.pos[0] - a.pos[0];
                float dy = b.pos[1] - a.pos[1];
                float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > pool.r * 3.2f) continue;
                float mx = (a.pos[0] + b.pos[0]) * 0.5f + sx;
                float my = (a.pos[1] + b.pos[1]) * 0.5f + sy;
                kalpana::Path bridge;
                bridge.round_rect(mx - dist * 0.28f, my - pool.r * 0.42f, dist * 0.56f, pool.r * 0.84f, pool.r * 0.4f, pool.r * 0.4f);
                kalpana::Color bc = a.col;
                bc.a *= 0.35f;
                scene.add(kalpana::Node::shape(bridge, kalpana::Paint::fill(bc)));
            }
            kalpana::Path g;
            g.circle(a.pos[0] + sx, a.pos[1] + sy, pool.r * 1.45f);
            kalpana::Color gc = a.col;
            gc.a = 0.24f;
            scene.add(kalpana::Node::shape(g, kalpana::Paint::fill(gc)));

            kalpana::Path d;
            d.circle(a.pos[0] + sx, a.pos[1] + sy, pool.r);
            scene.add(kalpana::Node::shape(d, kalpana::Paint::fill(a.col)));
        }
    }

    // Multi-shape material/element simulation field
    for (const auto& b : app.bodies) {
        if (!b.alive || !app.world.alive(b.ent)) continue;
        auto* mat = app.world.get<gati::MaterialComponent>(b.ent);
        auto* elem = app.world.get<gati::ElementalComponent>(b.ent);
        kalpana::Color base = color_for_element(elem ? elem->type : gati::ElementType::Neutral);
        float t_heat = mat ? std::clamp((mat->temperature + 20.0f) / 1200.0f, 0.0f, 1.0f) : 0.0f;
        float dmg = mat ? std::clamp(mat->damage, 0.0f, 1.0f) : 0.0f;
        kalpana::Color fill = kalpana::spectral::mix(base, kalpana::Color{1.0f, 0.2f, 0.95f, 1.0f}, t_heat);
        fill = kalpana::spectral::mix(fill, kalpana::Color{0.05f, 0.05f, 0.05f, 1.0f}, dmg * 0.8f);
        kalpana::Color rim = brighten(fill, 1.25f);

        auto path = body_shape_path(b, sx, sy);
        scene.add(kalpana::Node::shape(path, kalpana::Paint::filled_outlined(fill, rim, 1.5f)));
    }

    {
        const auto& pts = app.cloth->particles();
        float bx = ARENA_W * 0.5f - 55.0f;
        for (std::size_t i = 1; i < pts.size(); ++i) {
            float x0 = bx + pts[i - 1].pos[0] * 3.0f;
            float y0 = ARENA_Y0 + 2.0f + pts[i - 1].pos[1] * 1.6f;
            float x1 = bx + pts[i].pos[0] * 3.0f;
            float y1 = ARENA_Y0 + 2.0f + pts[i].pos[1] * 1.6f;
            float t = 1.0f - float(i) / float(pts.size());
            kalpana::Path seg;
            seg.move_to(x0, y0);
            seg.line_to(x1, y1);
            scene.add(kalpana::Node::shape(seg,
                kalpana::Paint::stroke(kalpana::Color{1.0f, 0.2f, 0.9f, t * 0.9f}, 2.7f * t)));
        }
    }

    {
        const float bx = 1.0f;
        const float by = FH - 50.0f;
        const float bw = ARENA_W;
        const float bh = 46.0f;
        kalpana::Path bg;
        bg.round_rect(bx, by, bw, bh, 4.0f, 4.0f);
        scene.add(kalpana::Node::shape(bg,
            kalpana::Paint::filled_outlined(
                kalpana::Color{0.04f, 0.06f, 0.12f, 0.90f},
                kalpana::Color{0.18f, 0.26f, 0.48f, 1.00f}, 1.0f)));

        float trauma = app.camera->trauma();
        kalpana::Path tbg;
        tbg.round_rect(bx + 8.0f, by + 27.0f, bw - 16.0f, 10.0f, 3.0f, 3.0f);
        scene.add(kalpana::Node::shape(tbg,
            kalpana::Paint::stroke(kalpana::Color{0.22f, 0.25f, 0.38f, 1.0f}, 1.0f)));
        if (trauma > 0.01f) {
            kalpana::Path fill;
            fill.round_rect(bx + 8.0f, by + 27.0f, (bw - 16.0f) * trauma, 10.0f, 3.0f, 3.0f);
            scene.add(kalpana::Node::shape(fill,
                kalpana::Paint::fill(brighten(actor_col, 1.4f + trauma))));
        }

        float rp = float(std::min(app.reactions, 80)) / 80.0f;
        kalpana::Path rb;
        rb.round_rect(bx + 8.0f, by + 10.0f, (bw - 16.0f) * rp, 8.0f, 2.0f, 2.0f);
        scene.add(kalpana::Node::shape(rb,
            kalpana::Paint::fill(kalpana::Color{0.9f, 0.25f, 1.0f, 0.9f})));
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
#else
    shd.vertex_func.source = VS_GLSL;
    shd.fragment_func.source = FS_GLSL;
    shd.texture_sampler_pairs[0].glsl_name = "tex";
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

    app.camera = std::make_unique<pebble::spandana::ScreenShake2D>(16.0f, 0.07f);
    app.cloth = std::make_unique<pebble::spandana::VerletCloth2D>(10, 8.0f, pebble::math::vec2(0.7f, -18.0f), 0.012f);
    app.canvas = std::make_unique<kalpana::Canvas<kalpana::capture_backend>>(W, H);

    for (auto& s : app.stars) {
        s.x = randf(0.0f, ARENA_W);
        s.y = randf(ARENA_Y0, ARENA_H);
        s.bri = randf(0.12f, 0.6f);
        s.phase = randf(0.0f, 6.2832f);
    }

    app.actor = app.world.spawn();
    app.world.add<gati::Transform>(app.actor, {.position = app.actor_pos, .prev_position = app.actor_pos});
    app.world.add<gati::MaterialComponent>(app.actor, gati::MaterialComponent::Ice());

    app.path = akruti::CubicBezierCurve{
        .p0 = {140.0f, ARENA_Y0 + 120.0f},
        .p1 = {340.0f, ARENA_Y0 + 28.0f},
        .p2 = {520.0f, ARENA_H - 90.0f},
        .p3 = {710.0f, ARENA_Y0 + 145.0f},
        .radius = 2.0f
    };

    app.blend_space.add_sample(pebble::math::vec2(0.0f, 0.0f), &app.idle_clip);
    app.blend_space.add_sample(pebble::math::vec2(1.0f, 0.0f), &app.burst_clip);
    app.blend_anim.set_blend_space(&app.blend_space);

    app.bone_root = app.skeleton.add_bone("root", -1, {.position = {0.0f, 0.0f}, .rotation = 0.0f}, 12.0f);
    app.bone_arm = app.skeleton.add_bone("arm", app.bone_root, {.position = {18.0f, 0.0f}, .rotation = 0.0f}, 14.0f);

    init_sim_bodies();
    init_liquids();

    rebuild_timeline();
}

static void frame_cb() {
    auto& app = g_app;
    app.frame++;
    app.t += DT;

    if (app.timeline.is_finished()) {
        rebuild_timeline();
    }

    app.timeline.update(DT);
    step_sim_bodies(DT);
    step_liquids(DT);

    if (auto* tr = app.world.get<gati::Transform>(app.actor)) {
        tr->prev_position = tr->position;
        tr->position = app.actor_pos;
    }

    pebble::math::vec2 v = (app.actor_pos - app.actor_prev) * (1.0f / DT);
    app.actor_prev = app.actor_pos;
    float speed_n = std::clamp(std::sqrt(v[0] * v[0] + v[1] * v[1]) / 260.0f, 0.0f, 1.0f);
    app.blend_anim.set_velocity(pebble::math::vec2(speed_n, 0.0f));
    app.blend_anim.update(DT);

    auto ws = app.blend_anim.current_weights();
    if (!ws.empty() && ws.front().clip && app.animator.current_clip != ws.front().clip && !app.animator.playing) {
        app.animator.play(ws.front().clip);
    }
    app.animator.update(DT);

    app.skeleton.set_bone_rotation(app.bone_root, app.actor_rot);
    app.skeleton.set_bone_rotation(app.bone_arm, std::sin(app.t * 6.0f) * 0.65f);
    app.skeleton.update_fk();

    if (app.burst) {
        app.burst_age += DT;
        app.burst->update(0.0f, DT);
        if (app.burst_age >= app.burst_life) {
            app.burst.reset();
        }
    }

    if (!app.shards.empty()) {
        app.shard_age += DT;
        if (app.shard_age > 1.0f) {
            app.shards.clear();
            app.shard_age = 0.0f;
        }
    }

    app.camera->update(DT);
    app.cloth->update(pebble::math::vec2(ARENA_W * 0.5f - 55.0f, ARENA_Y0 + 2.0f), DT);

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
                rebuild_timeline();
                init_sim_bodies();
                init_liquids();
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
    d.window_title = "Pebble Spandana Demo  [R]/[SPC] restart  [ESC] quit";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}
