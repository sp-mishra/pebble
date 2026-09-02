#pragma once
// akruti/fracture.hpp — geometric fracture/tear primitives. Pure geometry (returns polygons);
// no dynamics. Two operations:
//
//   voronoi_shatter(boundary, seeds) — partition a convex boundary polygon into per-seed
//     Voronoi cells by successive half-plane clipping (each cell = boundary clipped by the
//     perpendicular bisector against every other seed). Cells tile the boundary; areas sum to
//     the boundary area. This is the fragment set for a shatter effect.
//
//   clip_polygon(subject, clip) — Sutherland-Hodgman: clip a subject polygon by a convex clip
//     polygon. Used for tear/cut along a region and for boolean-subtract craters.
//   clip_halfplane(subject, normal, point) — single half-plane clip (building block; also the
//     tear-along-a-line operation).
//
// Vertex lists live in static_vector (fixed cap) / SmallVector (growable). Irregular data-
// dependent scans — plain C++, not kernelized.
#include "math.hpp"
#include "primitives.hpp"
#include "containers/static/static_vector.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include <vector>
#include <cmath>

namespace akruti {
    using Poly = containers::dynamic::SmallVector<Vec2<Scalar>>;

    // ── Single half-plane clip: keep the portion of `subject` on the side where
    //    normal·(p - point) <= 0 (the "inside" / solid side). ─────────────────────────
    [[nodiscard]] inline Poly clip_halfplane(const Poly& subject, Vec2<Scalar> normal, Vec2<Scalar> point) {
        Poly out;
        const std::size_t n = subject.size();
        if (n == 0) return out;
        auto side = [&](Vec2<Scalar> p) { return normal.dot(p - point); };
        for (std::size_t i = 0; i < n; ++i) {
            const Vec2<Scalar> cur = subject[i];
            const Vec2<Scalar> prev = subject[(i + n - 1) % n];
            const Scalar dc = side(cur), dp = side(prev);
            const bool cur_in = dc <= Scalar(0);
            const bool prev_in = dp <= Scalar(0);
            if (cur_in != prev_in) {
                const Scalar t = dp / (dp - dc); // crossing param on prev->cur
                out.push_back(prev + (cur - prev) * t);
            }
            if (cur_in) out.push_back(cur);
        }
        return out;
    }

    // ── Sutherland-Hodgman: clip subject by a CONVEX clip polygon (CCW). ───────────────
    [[nodiscard]] inline Poly clip_polygon(const Poly& subject, const Poly& clip) {
        Poly result = subject;
        const std::size_t m = clip.size();
        for (std::size_t i = 0; i < m && result.size() > 0; ++i) {
            const Vec2<Scalar> a = clip[i];
            const Vec2<Scalar> b = clip[(i + 1) % m];
            const Vec2<Scalar> edge = b - a;
            // Inward normal for CCW clip polygon points to the left of the edge.
            const Vec2<Scalar> inward{-edge.y, edge.x};
            // clip_halfplane keeps normal·(p-point)<=0; we want the inward (left) side => use -inward.
            result = clip_halfplane(result, inward * Scalar(-1), a);
        }
        return result;
    }

    // ── Polygon area (signed, CCW positive). ───────────────────────────────────────────
    [[nodiscard]] inline Scalar polygon_area(const Poly& p) noexcept {
        const std::size_t n = p.size();
        if (n < 3) return Scalar(0);
        Scalar a = 0;
        for (std::size_t i = 0; i < n; ++i) a += cross(p[i], p[(i + 1) % n]);
        return a * Scalar(0.5);
    }

    // ── Polygon centroid ───────────────────────────────────────────────────────────────
    [[nodiscard]] inline Vec2<Scalar> polygon_centroid(const Poly& p) noexcept {
        const std::size_t n = p.size();
        if (n == 0) return Vec2<Scalar>{0, 0};
        if (n == 1) return p[0];
        if (n == 2) return (p[0] + p[1]) * Scalar(0.5);
        const Scalar a = polygon_area(p);
        if (std::fabs(a) < Scalar(1e-12)) {
            Vec2<Scalar> c{0, 0};
            for (std::size_t i = 0; i < n; ++i) c += p[i];
            return c * (Scalar(1) / static_cast<Scalar>(n));
        }
        Vec2<Scalar> c{0, 0};
        for (std::size_t i = 0; i < n; ++i) {
            const auto& p0 = p[i];
            const auto& p1 = p[(i + 1) % n];
            const Scalar factor = cross(p0, p1);
            c.x += (p0.x + p1.x) * factor;
            c.y += (p0.y + p1.y) * factor;
        }
        return c / (Scalar(6) * a);
    }

    // ── Voronoi shatter: split a convex boundary polygon into one cell per seed. ────────
    //    Each cell = boundary clipped by the bisector half-plane against every other seed.
    //    Returns fragment polygons (some may be empty if a seed is dominated). Areas tile the
    //    boundary (sum == boundary area up to clipping precision).
    [[nodiscard]] inline std::vector<Poly> voronoi_shatter(const Poly& boundary,
                                                           const std::vector<Vec2<Scalar>>& seeds) {
        std::vector<Poly> cells;
        cells.reserve(seeds.size());
        for (std::size_t s = 0; s < seeds.size(); ++s) {
            Poly cell = boundary;
            const Vec2<Scalar> si = seeds[s];
            for (std::size_t o = 0; o < seeds.size() && cell.size() > 0; ++o) {
                if (o == s) continue;
                const Vec2<Scalar> so = seeds[o];
                // Bisector: points equidistant. Keep side closer to si => normal toward so, point at midpoint.
                const Vec2<Scalar> normal = so - si; // points from si to so
                const Vec2<Scalar> mid = (si + so) * Scalar(0.5);
                cell = clip_halfplane(cell, normal, mid); // keep normal·(p-mid)<=0 (si's side)
            }
            cells.push_back(std::move(cell));
        }
        return cells;
    }

    // Convenience: build a Poly from a ConvexPoly.
    template <std::size_t N>
    [[nodiscard]] inline Poly to_poly(const ConvexPoly<N>& c) {
        Poly p;
        for (std::size_t i = 0; i < c.verts.size(); ++i) p.push_back(c.verts[i]);
        return p;
    }

    // Convenience: build a rectangular boundary Poly (CCW).
    [[nodiscard]] inline Poly rect_poly(Vec2<Scalar> lo, Vec2<Scalar> hi) {
        Poly p;
        p.push_back({lo.x, lo.y});
        p.push_back({hi.x, lo.y});
        p.push_back({hi.x, hi.y});
        p.push_back({lo.x, hi.y});
        return p;
    }
} // namespace akruti
