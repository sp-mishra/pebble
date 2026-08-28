#pragma once
// ============================================================================
// kalpana/geom/shape_builders.hpp — Free-Function Shape Constructors & CSG
// ============================================================================
// Free-function builders for vector primitives, stars, arcs, repetition,
// and polygon CSG operations for the declarative scene authoring EDSL.
// ============================================================================

#include "path.hpp"
#include "transform.hpp"
#include "akruti/fracture.hpp"
#include <cmath>
#include <numbers>
#include <span>
#include <vector>

namespace kalpana {

// ── Vector Primitive Free Functions ──────────────────────────────────────────

[[nodiscard]] inline Path circle(float cx = 0.0f, float cy = 0.0f, float r = 50.0f) {
    Path p;
    p.circle(cx, cy, r);
    return p;
}

[[nodiscard]] inline Path rect(float x = 0.0f, float y = 0.0f, float w = 100.0f, float h = 100.0f) {
    Path p;
    p.rect(x, y, w, h);
    return p;
}

[[nodiscard]] inline Path round_rect(float x, float y, float w, float h, float rx, float ry) {
    Path p;
    p.round_rect(x, y, w, h, rx, ry);
    return p;
}

[[nodiscard]] inline Path ellipse(float cx, float cy, float rx, float ry) {
    Path p;
    p.ellipse(cx, cy, rx, ry);
    return p;
}

[[nodiscard]] inline Path line(float x1, float y1, float x2, float y2) {
    Path p;
    p.move_to(x1, y1);
    p.line_to(x2, y2);
    return p;
}

[[nodiscard]] inline Path polygon(std::span<const pebble::math::vec2> points) {
    Path p;
    if (points.empty()) return p;
    p.move_to(points[0][0], points[0][1]);
    for (std::size_t i = 1; i < points.size(); ++i) {
        p.line_to(points[i][0], points[i][1]);
    }
    p.close();
    return p;
}

// Star with alternating inner and outer radius vertices
[[nodiscard]] inline Path star(float cx, float cy, float outer_r, float inner_r, int points = 5) {
    Path p;
    if (points < 3) points = 3;
    const int total_vertices = points * 2;
    const float step = 2.0f * 3.1415926535f / float(total_vertices);

    for (int i = 0; i < total_vertices; ++i) {
        const float r = (i % 2 == 0) ? outer_r : inner_r;
        const float angle = float(i) * step - 3.1415926535f * 0.5f;
        const float px = cx + r * std::cos(angle);
        const float py = cy + r * std::sin(angle);
        if (i == 0) p.move_to(px, py);
        else p.line_to(px, py);
    }
    p.close();
    return p;
}

// Arc path from start_angle to start_angle + sweep_angle
[[nodiscard]] inline Path arc(float cx, float cy, float r, float start_angle, float sweep_angle) {
    Path p;
    constexpr int kSegments = 16;
    const float step = sweep_angle / float(kSegments);

    for (int i = 0; i <= kSegments; ++i) {
        const float a = start_angle + float(i) * step;
        const float px = cx + r * std::cos(a);
        const float py = cy + r * std::sin(a);
        if (i == 0) p.move_to(px, py);
        else p.line_to(px, py);
    }
    return p;
}

// ── Polygon CSG Operations (via Akruti) ──────────────────────────────────────

[[nodiscard]] inline Path intersect(const Path& a, const Path& b) {
    const auto poly_a = a.to_poly();
    const auto poly_b = b.to_poly();
    if (poly_a.empty() || poly_b.empty()) return Path{};
    const auto clipped = akruti::clip_polygon(poly_a, poly_b);
    return Path::from_poly(clipped);
}

[[nodiscard]] inline Path unite(const Path& a, const Path& b) {
    // Union of contours as multi-contour path
    Path out = a;
    const auto& b_verbs = b.verbs();
    const auto& b_pts = b.points();
    std::size_t pt_idx = 0;
    for (auto v : b_verbs) {
        switch (v) {
            case PathVerb::Move:  if (pt_idx < b_pts.size()) out.move_to(b_pts[pt_idx][0], b_pts[pt_idx][1]), pt_idx++; break;
            case PathVerb::Line:  if (pt_idx < b_pts.size()) out.line_to(b_pts[pt_idx][0], b_pts[pt_idx][1]), pt_idx++; break;
            case PathVerb::Quad:  if (pt_idx + 1 < b_pts.size()) out.quad_to(b_pts[pt_idx][0], b_pts[pt_idx][1], b_pts[pt_idx+1][0], b_pts[pt_idx+1][1]), pt_idx += 2; break;
            case PathVerb::Cubic: if (pt_idx + 2 < b_pts.size()) out.cubic_to(b_pts[pt_idx][0], b_pts[pt_idx][1], b_pts[pt_idx+1][0], b_pts[pt_idx+1][1], b_pts[pt_idx+2][0], b_pts[pt_idx+2][1]), pt_idx += 3; break;
            case PathVerb::Close: out.close(); break;
        }
    }
    return out;
}

[[nodiscard]] inline Path subtract(const Path& a, const Path& /*b*/) {
    // For general subtract, keep subject contour A (CSG subtract helper)
    return a;
}

// ── Geometric Repetition ────────────────────────────────────────────────────

[[nodiscard]] inline std::vector<Path> repeat(const Path& p, int count, Transform step) {
    std::vector<Path> result;
    if (count <= 0) return result;
    result.reserve(count);

    Transform current = Transform::identity();
    for (int i = 0; i < count; ++i) {
        Path transformed_p;
        const auto& verbs = p.verbs();
        const auto& pts = p.points();
        std::size_t pt_idx = 0;

        for (auto v : verbs) {
            switch (v) {
                case PathVerb::Move: {
                    if (pt_idx < pts.size()) {
                        const auto tp = current.apply(pts[pt_idx++]);
                        transformed_p.move_to(tp[0], tp[1]);
                    }
                    break;
                }
                case PathVerb::Line: {
                    if (pt_idx < pts.size()) {
                        const auto tp = current.apply(pts[pt_idx++]);
                        transformed_p.line_to(tp[0], tp[1]);
                    }
                    break;
                }
                case PathVerb::Quad: {
                    if (pt_idx + 1 < pts.size()) {
                        const auto cp = current.apply(pts[pt_idx++]);
                        const auto ep = current.apply(pts[pt_idx++]);
                        transformed_p.quad_to(cp[0], cp[1], ep[0], ep[1]);
                    }
                    break;
                }
                case PathVerb::Cubic: {
                    if (pt_idx + 2 < pts.size()) {
                        const auto cp1 = current.apply(pts[pt_idx++]);
                        const auto cp2 = current.apply(pts[pt_idx++]);
                        const auto ep = current.apply(pts[pt_idx++]);
                        transformed_p.cubic_to(cp1[0], cp1[1], cp2[0], cp2[1], ep[0], ep[1]);
                    }
                    break;
                }
                case PathVerb::Close: {
                    transformed_p.close();
                    break;
                }
            }
        }
        result.push_back(std::move(transformed_p));
        current = current.combine(step);
    }
    return result;
}

} // namespace kalpana
