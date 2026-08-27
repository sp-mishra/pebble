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
#include "prakriti/prakriti.hpp"
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

enum class ShapeKind : std::uint8_t {
    Circle,
    Box,
    Capsule,
    Triangle,
    RoundedBox,
    OrientedBox,
    Sector,
    Segment,
    Pentagon,
    Hexagon,
    Trapezoid,
    ConvexBlob,
    StarPoly
};

// Shape pool the simulation can spawn; all supported Akruti shapes.
static constexpr ShapeKind kSpawnKinds[] = {
    ShapeKind::Circle,     ShapeKind::Box,        ShapeKind::Capsule,
    ShapeKind::Triangle,   ShapeKind::RoundedBox, ShapeKind::OrientedBox,
    ShapeKind::Sector,     ShapeKind::Segment,    ShapeKind::Pentagon,
    ShapeKind::Hexagon,    ShapeKind::Trapezoid,  ShapeKind::ConvexBlob,
    ShapeKind::StarPoly
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
    // Cached local-space verts for procedural polygon shapes;
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
        // Regular pentagon/hexagon with slight jitter passed through Andrew's monotone chain convex hull
        containers::static_vector<akruti::Vec, kPolyMax> pts;
        constexpr int kSamples = 6;
        for (int i = 0; i < kSamples; ++i) {
            const float a = float(i) / float(kSamples) * 6.2831853f;
            const float r = randf(0.75f, 1.0f) * b.size;
            (void)pts.push_back({std::cos(a) * r, std::sin(a) * r});
        }
        auto hull = akruti::convex_hull<kPolyMax>(pts);
        b.poly_n = int(hull.verts.size());
        for (int i = 0; i < b.poly_n; ++i) b.poly[i] = hull.verts[i];
    } else if (b.kind == ShapeKind::Pentagon) {
        b.poly_n = 5;
        for (int i = 0; i < 5; ++i) {
            const float a = float(i) / 5.0f * 6.2831853f - 1.5707963f;
            b.poly[i] = {std::cos(a) * b.size, std::sin(a) * b.size};
        }
    } else if (b.kind == ShapeKind::Hexagon) {
        b.poly_n = 6;
        for (int i = 0; i < 6; ++i) {
            const float a = float(i) / 6.0f * 6.2831853f;
            b.poly[i] = {std::cos(a) * b.size, std::sin(a) * b.size};
        }
    } else if (b.kind == ShapeKind::Trapezoid) {
        b.poly_n = 4;
        const float s = b.size;
        b.poly[0] = {-s * 1.1f,  s * 0.7f};
        b.poly[1] = { s * 1.1f,  s * 0.7f};
        b.poly[2] = { s * 0.6f, -s * 0.7f};
        b.poly[3] = {-s * 0.6f, -s * 0.7f};
    } else { // StarPoly — convex regular hexagon base with corner rounding
        b.poly_n = 6;
        for (int i = 0; i < b.poly_n; ++i) {
            const float a = float(i) / 6.0f * 6.2831853f;
            b.poly[i] = {std::cos(a) * (b.size * 0.85f), std::sin(a) * (b.size * 0.85f)};
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

    // Prakriti continuum simulator integration
    std::unique_ptr<prakriti::World<prakriti::DefaultMaterialLaw, prakriti::ScalarBackend>> prakriti_world;
    prakriti::MaterialId mat_water = 0;
    prakriti::MaterialId mat_steel = 0;
    prakriti::MaterialId mat_dry_ice = 0;

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
    const float c = std::cos(b.rot);
    const float sn = std::sin(b.rot);

    switch (b.kind) {
        case ShapeKind::Circle: p.circle(x, y, s); break;
        case ShapeKind::Box: {
            auto pt = [&](float lx, float ly) {
                return std::pair<float, float>{x + lx * c - ly * sn, y + lx * sn + ly * c};
            };
            auto [x0, y0] = pt(-s, -s);
            auto [x1, y1] = pt(s, -s);
            auto [x2, y2] = pt(s, s);
            auto [x3, y3] = pt(-s, s);
            p.move_to(x0, y0); p.line_to(x1, y1); p.line_to(x2, y2); p.line_to(x3, y3); p.close();
            break;
        }
        case ShapeKind::RoundedBox: {
            p.round_rect(x - s, y - s, s * 2.0f, s * 2.0f, s * 0.35f, s * 0.35f);
            break;
        }
        case ShapeKind::Capsule: {
            // Symmetrical capsule oriented with rotation
            const float cap_len = s * 0.7f;
            const float cap_r = s * 0.6f;
            const float ax = x - cap_len * c, ay = y - cap_len * sn;
            const float bx = x + cap_len * c, by = y + cap_len * sn;
            const float nx = -sn * cap_r, ny = c * cap_r;

            p.move_to(ax + nx, ay + ny);
            p.line_to(bx + nx, by + ny);
            p.line_to(bx - nx, by - ny);
            p.line_to(ax - nx, ay - ny);
            p.close();
            break;
        }
        case ShapeKind::Triangle: {
            p.move_to(x + std::cos(b.rot) * s, y + std::sin(b.rot) * s);
            p.line_to(x + std::cos(b.rot + 2.0944f) * s, y + std::sin(b.rot + 2.0944f) * s);
            p.line_to(x + std::cos(b.rot + 4.1888f) * s, y + std::sin(b.rot + 4.1888f) * s);
            p.close();
            break;
        }
        case ShapeKind::OrientedBox: {
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
        case ShapeKind::Segment: {
            const float ax = x - s * c, ay = y - s * sn;
            const float bx = x + s * c, by = y + s * sn;
            const float nx = -sn * 2.5f, ny = c * 2.5f;
            p.move_to(ax + nx + sx, ay + ny + sy);
            p.line_to(bx + nx + sx, by + ny + sy);
            p.line_to(bx - nx + sx, by - ny + sy);
            p.line_to(ax - nx + sx, ay - ny + sy);
            p.close();
            break;
        }
        case ShapeKind::Pentagon:
        case ShapeKind::Hexagon:
        case ShapeKind::Trapezoid:
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
    constexpr int kCols = 6;
    constexpr int kRows = 4;
    constexpr int kBodies = kCols * kRows; // 24 well-spaced multi-shape bodies

    const float cell_w = (ARENA_W - 100.0f) / float(kCols);
    const float cell_h = (ARENA_H - ARENA_Y0 - 100.0f) / float(kRows);

    int idx = 0;
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c, ++idx) {
            SimBody b;
            b.ent = app.world.spawn();
            b.kind = kSpawnKinds[idx % (sizeof(kSpawnKinds) / sizeof(kSpawnKinds[0]))];
            b.size = randf(12.0f, 16.0f);
            b.rot = randf(0.0f, 6.2832f);

            float cx = 50.0f + (float(c) + 0.5f) * cell_w + randf(-6.0f, 6.0f);
            float cy = ARENA_Y0 + 50.0f + (float(r) + 0.5f) * cell_h + randf(-6.0f, 6.0f);

            b.pos = pebble::math::vec2(cx, cy);
            b.vel = pebble::math::vec2(randf(-60.0f, 60.0f), randf(-60.0f, 60.0f));
            if (b.kind == ShapeKind::ConvexBlob || b.kind == ShapeKind::StarPoly ||
                b.kind == ShapeKind::Pentagon || b.kind == ShapeKind::Hexagon ||
                b.kind == ShapeKind::Trapezoid) {
                init_poly_verts(b);
            }
            app.world.add<gati::Transform>(b.ent, {.position = b.pos, .prev_position = b.pos});
            app.world.add<gati::MaterialComponent>(b.ent, material_for_index(idx));
            app.world.add<gati::ElementalComponent>(b.ent, {.type = element_for_index(idx)});
            app.bodies.push_back(b);
        }
    }
}

static void init_prakriti_continuum() {
    auto& app = g_app;
    prakriti::WorldConfig pcfg{};
    pcfg.bounds = {{20.0f, ARENA_Y0 + 20.0f}, {ARENA_W - 20.0f, ARENA_H - 20.0f}};
    pcfg.gravity = {0.0f, 250.0f};
    pcfg.substeps = 4;
    pcfg.solver_iters = 4;
    pcfg.cell_size = 14.0f;

    app.prakriti_world = std::make_unique<prakriti::World<prakriti::DefaultMaterialLaw, prakriti::ScalarBackend>>(pcfg);
    app.mat_steel   = app.prakriti_world->materials().add(prakriti::MaterialRegistry::steel());
    app.mat_water   = app.prakriti_world->materials().add(prakriti::MaterialRegistry::water());
    app.mat_dry_ice = app.prakriti_world->materials().add(prakriti::MaterialRegistry::dry_ice());

    // 1. Continuum Fluid Block (Macklin-Muller PBF + Modified Tait EOS)
    constexpr int kFluidCols = 10;
    constexpr int kFluidRows = 7;
    const float start_x = 80.0f;
    const float start_y = ARENA_Y0 + 80.0f;
    const float spacing = 11.0f;

    for (int r = 0; r < kFluidRows; ++r) {
        for (int c = 0; c < kFluidCols; ++c) {
            app.prakriti_world->particles().add({
                .position = pebble::math::vec2(start_x + float(c) * spacing, start_y + float(r) * spacing),
                .velocity = pebble::math::vec2(randf(-5.0f, 5.0f), randf(-5.0f, 5.0f)),
                .mass = 1.0f,
                .temperature = 22.0f,
                .material = app.mat_water,
                .f_solid = 0.0f, .f_plastic = 0.0f, .f_liquid = 1.0f, .f_gas = 0.0f
            });
        }
    }

    // 2. Dry Ice Sublimating Cluster (Direct Solid -> Gas Phase Transition)
    for (int i = 0; i < 10; ++i) {
        app.prakriti_world->particles().add({
            .position = pebble::math::vec2(ARENA_W - 140.0f + randf(-20.0f, 20.0f), ARENA_Y0 + 120.0f + randf(-15.0f, 15.0f)),
            .velocity = pebble::math::vec2(randf(-10.0f, 10.0f), randf(-10.0f, 10.0f)),
            .mass = 1.2f,
            .temperature = -50.0f + float(i) * 5.0f,
            .material = app.mat_dry_ice,
            .f_solid = 1.0f, .f_plastic = 0.0f, .f_liquid = 0.0f, .f_gas = 0.0f
        });
    }

    // 3. XPBD Elastic Bonded Chain / Beam
    constexpr int kBeamNodes = 7;
    const float beam_x = ARENA_W * 0.5f - 70.0f;
    const float beam_y = ARENA_Y0 + 200.0f;
    std::vector<prakriti::Index> beam_indices;

    for (int i = 0; i < kBeamNodes; ++i) {
        auto idx = app.prakriti_world->particles().add({
            .position = pebble::math::vec2(beam_x + float(i) * 22.0f, beam_y),
            .velocity = {0.0f, 0.0f},
            .mass = (i == 0 || i == kBeamNodes - 1) ? 0.0f : 2.0f, // Pinned endpoints
            .temperature = 20.0f,
            .material = app.mat_steel,
            .f_solid = 1.0f, .f_plastic = 0.0f, .f_liquid = 0.0f, .f_gas = 0.0f
        });
        beam_indices.push_back(idx);
    }

    for (std::size_t i = 1; i < beam_indices.size(); ++i) {
        app.prakriti_world->edges().add(beam_indices[i - 1], beam_indices[i], 22.0f);
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
        constexpr int kDrops = 20;
        p.drops.reserve(kDrops);
        float cx = randf(100.0f, ARENA_W - 100.0f);
        float cy = randf(ARENA_Y0 + 100.0f, ARENA_H - 100.0f);
        for (int d = 0; d < kDrops; ++d) {
            LiquidDrop ld;
            ld.pos = pebble::math::vec2(cx + randf(-25.0f, 25.0f), cy + randf(-20.0f, 20.0f));
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
        case ShapeKind::Segment: return b.size * 1.1f;
        case ShapeKind::Capsule: return b.size * 1.3f;
        case ShapeKind::Sector: return b.size * 1.4f;
        case ShapeKind::ConvexBlob:
        case ShapeKind::StarPoly:
        case ShapeKind::Pentagon:
        case ShapeKind::Hexagon:
        case ShapeKind::Trapezoid: {
            float m2 = 0.0f;
            for (int i = 0; i < b.poly_n; ++i) m2 = std::max(m2, b.poly[i].x * b.poly[i].x + b.poly[i].y * b.poly[i].y);
            return std::sqrt(m2) + b.corner;
        }
        default: return b.size * 1.2f;
    }
}

using AkrutiShapeVar = std::variant<akruti::Circle, akruti::Box, akruti::OrientedBox, akruti::Capsule, akruti::Triangle, akruti::RoundedBox, akruti::Sector, akruti::Segment, akruti::ConvexPoly<kPolyMax>, akruti::RoundedPoly<kPolyMax>>;

static AkrutiShapeVar get_akruti_shape(const SimBody& b) {
    const akruti::Vec pos{b.pos[0], b.pos[1]};
    const float s = b.size;
    switch (b.kind) {
        case ShapeKind::Circle:
            return akruti::Circle{pos, s};
        case ShapeKind::Segment: {
            const float c = std::cos(b.rot), sn = std::sin(b.rot);
            return akruti::Segment{
                akruti::Vec{pos.x - s * c, pos.y - s * sn},
                akruti::Vec{pos.x + s * c, pos.y + s * sn}
            };
        }
        case ShapeKind::Box:
            return akruti::Box{pos, akruti::Vec{s, s}};
        case ShapeKind::RoundedBox:
            return akruti::RoundedBox{pos, akruti::Vec{s, s}, s * 0.35f};
        case ShapeKind::Capsule: {
            const float c = std::cos(b.rot);
            const float sn = std::sin(b.rot);
            const float cap_len = s * 0.7f;
            return akruti::Capsule{
                akruti::Vec{pos.x - cap_len * c, pos.y - cap_len * sn},
                akruti::Vec{pos.x + cap_len * c, pos.y + cap_len * sn},
                s * 0.6f
            };
        }
        case ShapeKind::Triangle: {
            const float c = std::cos(b.rot), sn = std::sin(b.rot);
            const float c1 = std::cos(b.rot + 2.0943951f), s1 = std::sin(b.rot + 2.0943951f);
            const float c2 = std::cos(b.rot + 4.1887902f), s2 = std::sin(b.rot + 4.1887902f);
            // Ensure CCW winding in world space
            akruti::Vec v0{pos.x + c * s, pos.y + sn * s};
            akruti::Vec v1{pos.x + c1 * s, pos.y + s1 * s};
            akruti::Vec v2{pos.x + c2 * s, pos.y + s2 * s};
            if ((v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x) < 0) {
                std::swap(v1, v2);
            }
            return akruti::Triangle{v0, v1, v2};
        }
        case ShapeKind::OrientedBox: {
            return akruti::OrientedBox::from_angle(pos, akruti::Vec{s, s * 0.75f}, b.rot);
        }
        case ShapeKind::Sector: {
            const akruti::Vec dir{std::cos(b.rot), std::sin(b.rot)};
            return akruti::Sector::from_direction(pos, s * 1.4f, 0.7f, dir);
        }
        case ShapeKind::Pentagon:
        case ShapeKind::Hexagon:
        case ShapeKind::Trapezoid:
        case ShapeKind::ConvexBlob: {
            std::array<akruti::Vec, kPolyMax> wv{};
            int n = 0;
            poly_verts(b, wv, n);
            akruti::ConvexPoly<kPolyMax> cp;
            for (int i = 0; i < n; ++i) (void)cp.verts.push_back(wv[i]);
            return cp;
        }
        case ShapeKind::StarPoly: {
            std::array<akruti::Vec, kPolyMax> wv{};
            int n = 0;
            poly_verts(b, wv, n);
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
            } else if constexpr (std::is_same_v<TypeA, akruti::Capsule> && std::is_same_v<TypeB, akruti::Capsule>) {
                return akruti::collide_capsule_capsule(shape_a, shape_b);
            } else if constexpr (std::is_same_v<TypeA, akruti::Capsule> && std::is_same_v<TypeB, akruti::OrientedBox>) {
                return akruti::collide_capsule_obb(shape_a, shape_b);
            } else if constexpr (std::is_same_v<TypeA, akruti::OrientedBox> && std::is_same_v<TypeB, akruti::Capsule>) {
                auto m = akruti::collide_capsule_obb(shape_b, shape_a);
                if (m.hit) m.normal = -m.normal;
                return m;
            } else {
                auto m = akruti::collide_gjk_warm_started(shape_a, shape_b);
                if (m.hit && m.depth > 0.0f) {
                    // Ensure normal is oriented from shape_a toward shape_b
                    pebble::math::vec2 delta = b.pos - a.pos;
                    if (m.normal.x * delta[0] + m.normal.y * delta[1] < 0.0f) {
                        m.normal = -m.normal;
                    }
                }
                return m;
            }
        }, sb);
    }, sa);
}

static void step_sim_bodies(float dt) {
    auto& app = g_app;

    // Substepping physics integration for absolute non-penetration stability
    constexpr int kSubsteps = 8;
    const float sub_dt = dt / float(kSubsteps);

    for (int step = 0; step < kSubsteps; ++step) {
        // 1. Integrate velocities and predict positions
        for (auto& b : app.bodies) {
            b.alive = app.world.alive(b.ent);
            if (!b.alive) continue;
            b.pos = b.pos + b.vel * sub_dt;
            b.rot += sub_dt * 0.7f;
            float r = bounding_radius(b);
            if (b.pos[0] < r) { b.pos[0] = r; b.vel[0] = std::abs(b.vel[0]); }
            if (b.pos[0] > ARENA_W - r) { b.pos[0] = ARENA_W - r; b.vel[0] = -std::abs(b.vel[0]); }
            if (b.pos[1] < ARENA_Y0 + r) { b.pos[1] = ARENA_Y0 + r; b.vel[1] = std::abs(b.vel[1]); }
            if (b.pos[1] > ARENA_H - r) { b.pos[1] = ARENA_H - r; b.vel[1] = -std::abs(b.vel[1]); }
        }

        // 2. Iterative non-penetration solver (Position-Based Dynamics loop)
        constexpr int kPbdIterations = 12;
        for (int pbd = 0; pbd < kPbdIterations; ++pbd) {
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

                    // Exact Akruti narrowphase (Analytic SAT / GJK-EPA)
                    auto manifold = test_body_collision(a, b);
                    if (!manifold.hit || manifold.depth <= 0.0f) continue;

                    pebble::math::vec2 n{manifold.normal.x, manifold.normal.y};
                    float len_n = std::sqrt(n[0] * n[0] + n[1] * n[1]);
                    if (len_n > 1e-5f) {
                        n = n * (1.0f / len_n);
                    } else {
                        float dist = std::sqrt(dist_sq);
                        n = (dist > 1e-4f) ? d * (1.0f / dist) : pebble::math::vec2(1.0f, 0.0f);
                    }

                    // Strict non-penetration projection (100% split equally between bodies)
                    float separation = (manifold.depth + 0.1f) * 0.5f;
                    a.pos = a.pos - n * separation;
                    b.pos = b.pos + n * separation;

                    // Boundary clamping with strict radius clearance
                    float ra_bound = bounding_radius(a);
                    float rb_bound = bounding_radius(b);
                    a.pos[0] = std::clamp(a.pos[0], ra_bound, ARENA_W - ra_bound);
                    a.pos[1] = std::clamp(a.pos[1], ARENA_Y0 + ra_bound, ARENA_H - ra_bound);
                    b.pos[0] = std::clamp(b.pos[0], rb_bound, ARENA_W - rb_bound);
                    b.pos[1] = std::clamp(b.pos[1], ARENA_Y0 + rb_bound, ARENA_H - rb_bound);

                    // Update ECS transform immediately
                    if (auto* tr_a = app.world.get<gati::Transform>(a.ent)) tr_a->position = a.pos;
                    if (auto* tr_b = app.world.get<gati::Transform>(b.ent)) tr_b->position = b.pos;

                    // Apply velocity reflection on first relaxation pass
                    if (pbd == 0) {
                        float va = a.vel[0] * n[0] + a.vel[1] * n[1];
                        float vb = b.vel[0] * n[0] + b.vel[1] * n[1];
                        float rel_vel = vb - va;
                        if (rel_vel < 0.0f) {
                            float restitution = 0.4f;
                            float impulse = -(1.0f + restitution) * rel_vel * 0.5f;
                            a.vel = a.vel - n * impulse;
                            b.vel = b.vel + n * impulse;
                        }

                        if (step == 0) {
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
        }
    }

    for (auto& b : app.bodies) {
        if (!b.alive) continue;
        if (auto* tr = app.world.get<gati::Transform>(b.ent)) {
            tr->prev_position = tr->position;
            tr->position = b.pos;
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

    // Render liquid blobs
    for (const auto& pool : app.liquids) {
        for (std::size_t i = 0; i < pool.drops.size(); ++i) {
            const auto& a = pool.drops[i];
            if (!a.alive) continue;
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

    // ── Prakriti Continuum Particles, Phases & XPBD Elastic Bonds ────────────
    if (app.prakriti_world) {
        auto& pw = *app.prakriti_world;
        const auto& P = pw.particles();
        const auto& E = pw.edges();

        // 1. Render XPBD structural elastic bonds
        for (std::size_t e = 0; e < E.size(); ++e) {
            if (!E.active[e]) continue;
            auto ia = E.a[e];
            auto ib = E.b[e];
            float x0 = P.pos_x[ia] + sx;
            float y0 = P.pos_y[ia] + sy;
            float x1 = P.pos_x[ib] + sx;
            float y1 = P.pos_y[ib] + sy;

            float strain_val = std::clamp(std::abs(E.strain[e]) * 20.0f, 0.0f, 1.0f);
            kalpana::Color bond_col = kalpana::spectral::mix(
                kalpana::Color{0.7f, 0.85f, 1.0f, 0.85f},
                kalpana::Color{1.0f, 0.2f, 0.2f, 0.95f},
                strain_val
            );

            kalpana::Path bond_line;
            bond_line.move_to(x0, y0);
            bond_line.line_to(x1, y1);
            scene.add(kalpana::Node::shape(bond_line, kalpana::Paint::stroke(bond_col, 3.5f)));
        }

        // 2. Render Continuum Particles with continuous 4-fraction spectral pigment blending
        for (prakriti::Index i = 0; i < P.size(); ++i) {
            float x = P.pos_x[i] + sx;
            float y = P.pos_y[i] + sy;
            float temp = P.temperature[i];

            kalpana::Color pcol;
            float pr = 5.0f;

            if (P.f_gas[i] > 0.4f) {
                // Vapor / gas expansion
                pcol = kalpana::Color{0.8f, 0.85f, 1.0f, 0.4f * P.f_gas[i]};
                pr = 7.5f;
            } else if (P.f_liquid[i] > 0.5f) {
                // Liquid droplets (Macklin-Müller PBF)
                float heat_frac = std::clamp((temp - 20.0f) / 100.0f, 0.0f, 1.0f);
                pcol = kalpana::spectral::mix(kalpana::Color{0.0f, 0.75f, 1.0f, 0.85f}, kalpana::Color{1.0f, 0.4f, 0.1f, 0.9f}, heat_frac);
                pr = 5.5f;
            } else {
                // Solid / plastic structural matter (XPBD)
                if (P.material[i] == app.mat_dry_ice) {
                    pcol = kalpana::Color{0.92f, 0.96f, 1.0f, 0.95f}; // Dry ice frosty solid
                    pr = 6.0f;
                } else {
                    pcol = kalpana::Color{0.8f, 0.82f, 0.9f, 1.0f}; // Steel
                    pr = 5.0f;
                }
            }

            kalpana::Path pdot;
            pdot.circle(x, y, pr);
            scene.add(kalpana::Node::shape(pdot, kalpana::Paint::fill(pcol)));
        }
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
    init_prakriti_continuum();

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

    // Step Prakriti Continuum Multiphysics World
    if (app.prakriti_world) {
        app.prakriti_world->step();
    }

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
                init_prakriti_continuum();
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
    d.window_title = "Pebble Multiphysics & Continuum Showcase (Prakriti, Akruti, Gati, Spandana, Kalpana) [R]/[SPC] reset";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}
