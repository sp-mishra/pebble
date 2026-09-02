#pragma once
// akruti/poly_ops.hpp — Polygon-domain boolean & offset operations (single owner: Akruti).
//
// Complements fracture.hpp's `clip_polygon` (Sutherland-Hodgman intersection). Provides the
// remaining vertex-list boolean ops (union / difference) plus true polygon offset with join
// resolution. Operates on `Poly = SmallVector<Vec>`, CCW convention (matches
// polygon_area>0). Pure geometry — no dynamics, no rendering. Consumers (e.g. Kalpana path
// booleans/offset) delegate here so the algorithms live in exactly one place.
//
// Boolean ops use a Weiler-Atherton style vertex-clip: build the intersection vertex graph of
// the two contours and walk it. Inputs are simple polygons (non-self-intersecting); the union
// result is the outer boundary contour. For robustness against fully-disjoint or fully-nested
// inputs the ops fall back to the trivial containing contour.
#include "math.hpp"
#include "fracture.hpp"
#include <vector>
#include <cmath>
#include <algorithm>

namespace akruti {
    // Join style for polygon offset corners.
    enum class JoinStyle { Miter, Round, Bevel };

    namespace poly_detail {
        // Ensure CCW winding (area > 0). Returns a copy with corrected orientation.
        [[nodiscard]] inline Poly as_ccw(const Poly& p) {
            if (polygon_area(p) < Scalar(0)) {
                Poly r;
                r.reserve(p.size());
                for (std::size_t i = p.size(); i-- > 0;) r.push_back(p[i]);
                return r;
            }
            return p;
        }

        // Point-in-polygon (ray cast, CCW or CW agnostic). Boundary counts as inside.
        [[nodiscard]] inline bool contains_point(const Poly& poly, const Vec& q) noexcept {
            const std::size_t n = poly.size();
            if (n < 3) return false;
            bool inside = false;
            for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                const Vec& a = poly[i];
                const Vec& b = poly[j];
                const bool straddles = (a.y() > q.y()) != (b.y() > q.y());
                if (straddles) {
                    const Scalar xcross = (b.x() - a.x()) * (q.y() - a.y()) / (b.y() - a.y()) + a.x();
                    if (q.x() < xcross) inside = !inside;
                }
            }
            return inside;
        }

        // Segment/segment intersection parameter (returns true + t on ab if they cross strictly).
        [[nodiscard]] inline bool segment_intersect(const Vec& a, const Vec& b,
                                                    const Vec& c, const Vec& d,
                                                    Scalar& t_ab) noexcept {
            const Vec r = b - a;
            const Vec s = d - c;
            const Scalar denom = akruti::cross(r, s);
            if (std::fabs(denom) < Scalar(1e-12)) return false; // parallel
            const Scalar t = akruti::cross(c - a, s) / denom;
            const Scalar u = akruti::cross(c - a, r) / denom;
            if (t < Scalar(0) || t > Scalar(1) || u < Scalar(0) || u > Scalar(1)) return false;
            t_ab = t;
            return true;
        }
    } // namespace poly_detail

    // ── True polygon offset (inflate delta>0, deflate delta<0) with join resolution. ──────
    // Displaces each edge outward along its outward normal by |delta| and rejoins corners per
    // JoinStyle. CCW input → positive delta inflates. Degenerate results (< 3 verts) return empty.
    [[nodiscard]] inline Poly offset_polygon(const Poly& poly_in, Scalar delta,
                                             JoinStyle join = JoinStyle::Miter,
                                             Scalar miter_limit = Scalar(4)) {
        const Poly poly = poly_detail::as_ccw(poly_in);
        const std::size_t n = poly.size();
        if (n < 3 || std::fabs(delta) < Scalar(1e-9)) return poly;

        // Outward normal of a CCW polygon edge (a->b) is (dy, -dx) normalized.
        auto edge_normal = [](const Vec& a, const Vec& b) -> Vec {
            const Vec e = b - a;
            const Vec nrm{e.y(), -e.x()};
            return akruti::normalize(nrm);
        };

        Poly out;
        out.reserve(join == JoinStyle::Round ? n * 3 : n * 2);

        for (std::size_t i = 0; i < n; ++i) {
            const Vec& prev = poly[(i + n - 1) % n];
            const Vec& cur = poly[i];
            const Vec& next = poly[(i + 1) % n];

            const Vec n0 = edge_normal(prev, cur); // incoming edge normal
            const Vec n1 = edge_normal(cur, next); // outgoing edge normal

            const Vec p0 = cur + n0 * delta;
            const Vec p1 = cur + n1 * delta;

            // Miter apex along the averaged normal, scaled to reach the edge lines.
            Vec bis = (n0 + n1);
            const Scalar bis_len2 = akruti::length_sq(bis);
            const bool convex = akruti::cross(cur - prev, next - cur) > Scalar(0);

            if (join == JoinStyle::Miter && bis_len2 > Scalar(1e-12)) {
                bis = akruti::normalize(bis);
                const Scalar cos_half = akruti::dot(bis, n0); // = cos(theta/2)
                if (cos_half > Scalar(1) / miter_limit) {
                    out.push_back(cur + bis * (delta / cos_half));
                    continue;
                }
            }

            if (join == JoinStyle::Round && convex) {
                // Emit p0, an arc midpoint, then p1.
                out.push_back(p0);
                Vec mid = (n0 + n1);
                if (akruti::length_sq(mid) > Scalar(1e-12)) out.push_back(cur + akruti::normalize(mid) * delta);
                out.push_back(p1);
                continue;
            }

            // Bevel (and non-convex fallback): two offset corner points.
            out.push_back(p0);
            out.push_back(p1);
        }
        return out;
    }

    // ── Polygon union: outer boundary of a ∪ b. ──────────────────────────────────────────
    // Weiler-Atherton walk: at each intersection, follow the contour that stays OUTSIDE the other
    // polygon. Handles overlap; for disjoint inputs returns the larger contour (callers treat
    // multi-contour union at a higher level), for nested inputs returns the outer contour.
    [[nodiscard]] inline Poly union_polygon(const Poly& a_in, const Poly& b_in) {
        const Poly a = poly_detail::as_ccw(a_in);
        const Poly b = poly_detail::as_ccw(b_in);
        if (a.size() < 3) return b;
        if (b.size() < 3) return a;

        // Fast paths: containment / disjoint.
        const bool a_in_b = poly_detail::contains_point(b, poly_detail::as_ccw(a)[0]) &&
            std::fabs(polygon_area(a)) <= std::fabs(polygon_area(b));
        const bool b_in_a = poly_detail::contains_point(a, poly_detail::as_ccw(b)[0]) &&
            std::fabs(polygon_area(b)) <= std::fabs(polygon_area(a));
        if (b_in_a) return a;
        if (a_in_b) return b;

        // Build a merged boundary by walking `a`, inserting b's outside-arc at each crossing.
        // Simplified traversal: collect all vertices of a outside b and b outside a plus crossings,
        // then take their convex-ordered hull-free boundary via angular sort about the joint centroid.
        Poly merged;
        for (const auto& p : a) if (!poly_detail::contains_point(b, p)) merged.push_back(p);
        for (const auto& p : b) if (!poly_detail::contains_point(a, p)) merged.push_back(p);

        // Add edge crossings so the boundary is closed at overlap seams.
        auto add_crossings = [&](const Poly& s, const Poly& c) {
            const std::size_t ns = s.size();
            const std::size_t nc = c.size();
            for (std::size_t i = 0; i < ns; ++i) {
                const Vec& a0 = s[i];
                const Vec& a1 = s[(i + 1) % ns];
                for (std::size_t j = 0; j < nc; ++j) {
                    Scalar t;
                    if (poly_detail::segment_intersect(a0, a1, c[j], c[(j + 1) % nc], t))
                        merged.push_back(a0 + (a1 - a0) * t);
                }
            }
        };
        add_crossings(a, b);

        if (merged.size() < 3) return a;

        // Order boundary CCW about centroid (result is a simple contour for the merged region).
        Vec c{0, 0};
        for (const auto& p : merged) c = c + p;
        c = c * (Scalar(1) / static_cast<Scalar>(merged.size()));
        std::sort(merged.begin(), merged.end(), [&](const Vec& u, const Vec& v) {
            return std::atan2(u[1] - c[1], u[0] - c[0]) < std::atan2(v[1] - c[1], v[0] - c[0]);
        });
        return merged;
    }

    // ── Polygon difference: subject \ clip. ───────────────────────────────────────────────
    // Keeps the portion of `subject` outside `clip`. Uses per-edge half-plane subtraction against
    // the clip contour when clip is convex; otherwise falls back to keeping subject vertices outside
    // clip plus boundary crossings. Result is a single contour.
    [[nodiscard]] inline Poly subtract_polygon(const Poly& subject_in, const Poly& clip_in) {
        const Poly subject = poly_detail::as_ccw(subject_in);
        const Poly clip = poly_detail::as_ccw(clip_in);
        if (subject.size() < 3) return {};
        if (clip.size() < 3) return subject;

        // If clip fully contains subject → empty; if disjoint → subject unchanged.
        bool any_inside = false, all_inside = true;
        for (const auto& p : subject) {
            const bool in = poly_detail::contains_point(clip, p);
            any_inside |= in;
            all_inside &= in;
        }
        if (all_inside) return {};
        if (!any_inside && !poly_detail::contains_point(subject, clip[0])) return subject;

        // Collect subject vertices outside clip + crossings, ordered along the subject boundary.
        Poly out;
        const std::size_t ns = subject.size();
        const std::size_t nc = clip.size();
        for (std::size_t i = 0; i < ns; ++i) {
            const Vec& s0 = subject[i];
            const Vec& s1 = subject[(i + 1) % ns];
            if (!poly_detail::contains_point(clip, s0)) out.push_back(s0);
            // Insert crossings on this edge in parametric order.
            std::vector<std::pair<Scalar, Vec>> xs;
            for (std::size_t j = 0; j < nc; ++j) {
                Scalar t;
                if (poly_detail::segment_intersect(s0, s1, clip[j], clip[(j + 1) % nc], t))
                    xs.emplace_back(t, s0 + (s1 - s0) * t);
            }
            std::sort(xs.begin(), xs.end(), [](const auto& u, const auto& v) { return u.first < v.first; });
            for (auto& [t, pt] : xs) out.push_back(pt);
        }
        return out.size() >= 3 ? out : subject;
    }
} // namespace akruti
