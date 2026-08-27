// ============================================================================
// src/app/pebble_gati.cpp — Gati Realtime ECS, Elements & Material Reaction Showcase
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

#include "gati/gati.hpp"
#include "gati/material.hpp"
#include "gati/elemental.hpp"
#include "gati/material_reaction.hpp"
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

#include "akruti/akruti.hpp"
#include "akruti/primitives.hpp"
#include "akruti/narrowphase.hpp"
#include "akruti/gjk.hpp"

enum class ShapeKind : std::uint8_t {
    Circle,
    Box,
    RoundedBox,
    Capsule,
    Triangle,
    OrientedBox,
    Sector,
    Segment,
    Pentagon,
    Hexagon,
    Trapezoid,
    ConvexBlob,
    StarPoly
};

static constexpr std::size_t kPolyMax = 8;

struct GatiBody {
    pebble::ecs::Entity ent{};
    pebble::math::vec2  pos{0.0f, 0.0f};
    pebble::math::vec2  vel{0.0f, 0.0f};
    float               rot = 0.0f;
    float               rot_vel = 0.0f;
    float               size = 14.0f;
    ShapeKind           kind = ShapeKind::Circle;
    bool                alive = true;

    int                 poly_n = 0;
    float               corner = 0.0f;
    std::array<akruti::Vec, kPolyMax> poly{};
};

struct GatiApp {
    sg_pipeline pip{};
    sg_bindings bind{};
    sg_pass_action pass_action{};
    sg_image tex_img{};
    sg_view tex_view{};
    sg_sampler smp{};
    std::vector<std::uint32_t> pixels;
    std::unique_ptr<kalpana::Canvas<kalpana::capture_backend>> canvas;

    pebble::ecs::World world;
    std::vector<GatiBody> bodies;
    int reactions = 0;
    float t = 0.0f;
    int frame = 0;
};

static GatiApp g_app;

static inline float randf(float lo, float hi) {
    return lo + static_cast<float>(std::rand()) / (static_cast<float>(RAND_MAX) / (hi - lo));
}

static kalpana::Color brighten(kalpana::Color c, float factor) {
    return {std::min(1.0f, c.r * factor), std::min(1.0f, c.g * factor), std::min(1.0f, c.b * factor), c.a};
}

static kalpana::Color color_for_elem(gati::ElementType e) {
    switch (e) {
        case gati::ElementType::Water: return {0.0f, 0.7f, 1.0f, 1.0f};
        case gati::ElementType::Lava: return {1.0f, 0.3f, 0.05f, 1.0f};
        case gati::ElementType::Fire: return {1.0f, 0.5f, 0.05f, 1.0f};
        case gati::ElementType::Wood: return {0.6f, 0.4f, 0.2f, 1.0f};
        case gati::ElementType::Metal: return {0.75f, 0.8f, 0.88f, 1.0f};
        case gati::ElementType::Acid: return {0.2f, 1.0f, 0.2f, 1.0f};
        case gati::ElementType::Electricity: return {1.0f, 1.0f, 0.2f, 1.0f};
        case gati::ElementType::Ice: return {0.7f, 0.95f, 1.0f, 1.0f};
        case gati::ElementType::Obsidian: return {0.4f, 0.1f, 0.6f, 1.0f};
        default: return {0.8f, 0.8f, 0.8f, 1.0f};
    }
}

static void init_poly_verts(GatiBody& b) {
    if (b.kind == ShapeKind::ConvexBlob) {
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
    } else { // StarPoly
        b.poly_n = 6;
        for (int i = 0; i < b.poly_n; ++i) {
            const float a = float(i) / 6.0f * 6.2831853f;
            b.poly[i] = {std::cos(a) * (b.size * 0.85f), std::sin(a) * (b.size * 0.85f)};
        }
        b.corner = b.size * 0.25f;
    }
}

static void poly_verts(const GatiBody& b, std::array<akruti::Vec, kPolyMax>& out, int& n) {
    const float c = std::cos(b.rot), s = std::sin(b.rot);
    n = b.poly_n;
    for (int i = 0; i < n; ++i) {
        const auto& l = b.poly[i];
        out[i] = {b.pos[0] + l.x * c - l.y * s, b.pos[1] + l.x * s + l.y * c};
    }
}

static float bounding_radius(const GatiBody& b) {
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

static AkrutiShapeVar get_akruti_shape(const GatiBody& b) {
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

static akruti::Manifold test_body_collision(const GatiBody& a, const GatiBody& b) {
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

static kalpana::Path body_shape_path(const GatiBody& b) {
    kalpana::Path p;
    const float x = b.pos[0];
    const float y = b.pos[1];
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
                float frac = float(i) / float(kArc);
                float a = a0 + (a1 - a0) * frac;
                p.line_to(x + std::cos(a) * s * 1.4f, y + std::sin(a) * s * 1.4f);
            }
            p.close();
            break;
        }
        case ShapeKind::Segment: {
            const float ax = x - s * c, ay = y - s * sn;
            const float bx = x + s * c, by = y + s * sn;
            const float nx = -sn * 2.5f, ny = c * 2.5f;
            p.move_to(ax + nx, ay + ny);
            p.line_to(bx + nx, by + ny);
            p.line_to(bx - nx, by - ny);
            p.line_to(ax - nx, ay - ny);
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
                if (i == 0) p.move_to(wv[i].x, wv[i].y);
                else p.line_to(wv[i].x, wv[i].y);
            }
            p.close();
            break;
        }
    }
    return p;
}

static constexpr ShapeKind kAllKinds[] = {
    ShapeKind::Circle,
    ShapeKind::Box,
    ShapeKind::RoundedBox,
    ShapeKind::Capsule,
    ShapeKind::Triangle,
    ShapeKind::OrientedBox,
    ShapeKind::Sector,
    ShapeKind::Segment,
    ShapeKind::Pentagon,
    ShapeKind::Hexagon,
    ShapeKind::Trapezoid,
    ShapeKind::ConvexBlob,
    ShapeKind::StarPoly
};

static void init_gati_simulation() {
    auto& app = g_app;
    app.bodies.clear();
    app.reactions = 0;

    // 1000 interactive multi-shape bodies in a 40x25 stress-test layout
    constexpr int kCols = 40;
    constexpr int kRows = 25;
    const float cell_w = (FW - 60.0f) / float(kCols);
    const float cell_h = (FH - 100.0f) / float(kRows);

    int idx = 0;
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c, ++idx) {
            GatiBody b;
            b.ent = app.world.spawn();
            b.kind = kAllKinds[idx % (sizeof(kAllKinds) / sizeof(kAllKinds[0]))];
            b.size = randf(4.5f, 6.5f);
            b.rot = randf(0.0f, 6.2832f);
            b.rot_vel = randf(-2.5f, 2.5f);

            float cx = 30.0f + (float(c) + 0.5f) * cell_w + randf(-2.0f, 2.0f);
            float cy = 50.0f + (float(r) + 0.5f) * cell_h + randf(-2.0f, 2.0f);
            b.pos = pebble::math::vec2(cx, cy);

            float ang = randf(0.0f, 6.2832f);
            float spd = randf(25.0f, 70.0f);
            b.vel = pebble::math::vec2(std::cos(ang) * spd, std::sin(ang) * spd);

            if (b.kind == ShapeKind::ConvexBlob || b.kind == ShapeKind::StarPoly ||
                b.kind == ShapeKind::Pentagon || b.kind == ShapeKind::Hexagon ||
                b.kind == ShapeKind::Trapezoid) {
                init_poly_verts(b);
            }

            app.world.add<gati::Transform>(b.ent, {.position = b.pos, .prev_position = b.pos});

            gati::ElementType et = static_cast<gati::ElementType>(idx % 7);
            app.world.add<gati::ElementalComponent>(b.ent, {.type = et});

            switch (idx % 6) {
                case 0: app.world.add<gati::MaterialComponent>(b.ent, gati::MaterialComponent::Ice()); break;
                case 1: app.world.add<gati::MaterialComponent>(b.ent, gati::MaterialComponent::Water()); break;
                case 2: app.world.add<gati::MaterialComponent>(b.ent, gati::MaterialComponent::Glass()); break;
                case 3: app.world.add<gati::MaterialComponent>(b.ent, gati::MaterialComponent::Steel()); break;
                case 4: app.world.add<gati::MaterialComponent>(b.ent, gati::MaterialComponent::Wood()); break;
                default: app.world.add<gati::MaterialComponent>(b.ent, gati::MaterialComponent::Lava()); break;
            }

            app.bodies.push_back(b);
        }
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

    init_gati_simulation();
}

// Spatial hash grid broadphase for high-performance 1000-body neighbor queries
struct SpatialHashGrid {
    static constexpr float kCellSize = 18.0f;
    static constexpr int kGridCols = static_cast<int>(FW / kCellSize) + 2;
    static constexpr int kGridRows = static_cast<int>(FH / kCellSize) + 2;
    static constexpr int kTotalCells = kGridCols * kGridRows;

    std::vector<int> head;
    std::vector<int> next;

    SpatialHashGrid() {
        head.resize(kTotalCells, -1);
    }

    void build(const std::vector<GatiBody>& bodies) {
        std::fill(head.begin(), head.end(), -1);
        next.resize(bodies.size(), -1);

        for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
            int cx = std::clamp(static_cast<int>(bodies[i].pos[0] / kCellSize), 0, kGridCols - 1);
            int cy = std::clamp(static_cast<int>(bodies[i].pos[1] / kCellSize), 0, kGridRows - 1);
            int cell = cy * kGridCols + cx;
            next[i] = head[cell];
            head[cell] = i;
        }
    }
};

static SpatialHashGrid g_grid;

static void step_gati(float dt) {
    auto& app = g_app;

    constexpr int kSubsteps = 4;
    const float sub_dt = dt / float(kSubsteps);

    for (int step = 0; step < kSubsteps; ++step) {
        // 1. Position and rotation integration
        for (auto& b : app.bodies) {
            b.pos = b.pos + b.vel * sub_dt;
            b.rot += b.rot_vel * sub_dt;
            float r = bounding_radius(b);
            if (b.pos[0] < r) { b.pos[0] = r; b.vel[0] = std::abs(b.vel[0]); }
            if (b.pos[0] > FW - r) { b.pos[0] = FW - r; b.vel[0] = -std::abs(b.vel[0]); }
            if (b.pos[1] < r + 20.0f) { b.pos[1] = r + 20.0f; b.vel[1] = std::abs(b.vel[1]); }
            if (b.pos[1] > FH - r - 20.0f) { b.pos[1] = FH - r - 20.0f; b.vel[1] = -std::abs(b.vel[1]); }
        }

        // 2. Spatial Broadphase Acceleration
        g_grid.build(app.bodies);

        // 3. Iterative non-penetration solver
        constexpr int kPbdIterations = 4;
        for (int pbd = 0; pbd < kPbdIterations; ++pbd) {
            for (int i = 0; i < static_cast<int>(app.bodies.size()); ++i) {
                auto& a = app.bodies[i];
                const float ra = bounding_radius(a);

                int cx = std::clamp(static_cast<int>(a.pos[0] / SpatialHashGrid::kCellSize), 0, SpatialHashGrid::kGridCols - 1);
                int cy = std::clamp(static_cast<int>(a.pos[1] / SpatialHashGrid::kCellSize), 0, SpatialHashGrid::kGridRows - 1);

                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = cy + dy;
                    if (ny < 0 || ny >= SpatialHashGrid::kGridRows) continue;

                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = cx + dx;
                        if (nx < 0 || nx >= SpatialHashGrid::kGridCols) continue;

                        int cell = ny * SpatialHashGrid::kGridCols + nx;
                        for (int j = g_grid.head[cell]; j != -1; j = g_grid.next[j]) {
                            if (j <= i) continue; // avoid duplicates and self-collision

                            auto& b = app.bodies[j];
                            const float rb = bounding_radius(b);

                            pebble::math::vec2 d = b.pos - a.pos;
                            float dist_sq = d[0] * d[0] + d[1] * d[1];
                            float min_d = ra + rb;
                            if (dist_sq >= min_d * min_d) continue;

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

                            float separation = (manifold.depth + 0.05f) * 0.5f;
                            a.pos = a.pos - n * separation;
                            b.pos = b.pos + n * separation;

                            float ra_bound = bounding_radius(a);
                            float rb_bound = bounding_radius(b);
                            a.pos[0] = std::clamp(a.pos[0], ra_bound, FW - ra_bound);
                            a.pos[1] = std::clamp(a.pos[1], ra_bound + 20.0f, FH - ra_bound - 20.0f);
                            b.pos[0] = std::clamp(b.pos[0], rb_bound, FW - rb_bound);
                            b.pos[1] = std::clamp(b.pos[1], rb_bound + 20.0f, FH - rb_bound - 20.0f);

                            if (pbd == 0) {
                                float va = a.vel[0] * n[0] + a.vel[1] * n[1];
                                float vb = b.vel[0] * n[0] + b.vel[1] * n[1];
                                float rel_vel = vb - va;
                                if (rel_vel < 0.0f) {
                                    float restitution = 0.35f;
                                    float impulse = -(1.0f + restitution) * rel_vel * 0.5f;
                                    a.vel = a.vel - n * impulse;
                                    b.vel = b.vel + n * impulse;
                                }

                                if (step == 0) {
                                    gati::ContactEvent ce{};
                                    ce.a = a.ent.index;
                                    ce.b = b.ent.index;
                                    ce.normal = n;
                                    ce.depth = manifold.depth;
                                    ce.point = (a.pos + b.pos) * 0.5f;

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
        }
    }

    gati::MaterialReactionSystem::step_thermodynamics(app.world, dt, 24.0f);
}

static void build_scene(kalpana::Scene& scene) {
    auto& app = g_app;
    scene.clear_color(kalpana::Color{0.02f, 0.03f, 0.06f, 1.0f});

    // Background decorative grid
    {
        kalpana::Color gc{0.07f, 0.09f, 0.14f, 1.0f};
        for (int i = 0; i <= 20; ++i) {
            float x = float(i) / 20.0f * FW;
            kalpana::Path l;
            l.move_to(x, 20.0f); l.line_to(x, FH - 20.0f);
            scene.add(kalpana::Node::shape(l, kalpana::Paint::stroke(gc, 1.0f)));
        }
        for (int j = 0; j <= 12; ++j) {
            float y = 20.0f + float(j) / 12.0f * (FH - 40.0f);
            kalpana::Path l;
            l.move_to(0.0f, y); l.line_to(FW, y);
            scene.add(kalpana::Node::shape(l, kalpana::Paint::stroke(gc, 1.0f)));
        }
        kalpana::Path border;
        border.rect(2.0f, 20.0f, FW - 4.0f, FH - 40.0f);
        scene.add(kalpana::Node::shape(border, kalpana::Paint::stroke(kalpana::Color{0.18f, 0.25f, 0.42f, 1.0f}, 2.0f)));
    }

    // Render all simulated bodies with elemental/thermodynamic pigment blending
    for (const auto& b : app.bodies) {
        auto* elem = app.world.get<gati::ElementalComponent>(b.ent);
        auto* mat = app.world.get<gati::MaterialComponent>(b.ent);

        kalpana::Color base = color_for_elem(elem ? elem->type : gati::ElementType::Neutral);
        float t_heat = mat ? std::clamp((mat->temperature + 20.0f) / 800.0f, 0.0f, 1.0f) : 0.0f;
        kalpana::Color fill = kalpana::spectral::mix(base, kalpana::Color{1.0f, 0.2f, 0.9f, 1.0f}, t_heat);
        kalpana::Color rim = brighten(fill, 1.35f);

        auto path = body_shape_path(b);
        scene.add(kalpana::Node::shape(path, kalpana::Paint::filled_outlined(fill, rim, 1.5f)));
    }

    // UI Overlay Header
    {
        kalpana::Path ui_box;
        ui_box.round_rect(10.0f, 25.0f, 360.0f, 32.0f, 4.0f, 4.0f);
        scene.add(kalpana::Node::shape(ui_box, kalpana::Paint::filled_outlined(
            kalpana::Color{0.04f, 0.06f, 0.12f, 0.85f},
            kalpana::Color{0.2f, 0.3f, 0.5f, 1.0f}, 1.0f)));
    }
}

static void frame_cb() {
    auto& app = g_app;
    app.frame++;
    app.t += DT;

    step_gati(DT);

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
                init_gati_simulation();
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
    d.window_title = "Pebble Gati ECS & Elemental Reactions Showcase [R]/[SPC] reset";
    d.icon.sokol_default = true;
    d.logger.func = slog_func;
    return d;
}
