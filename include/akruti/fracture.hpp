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
#include "containers/dynamic/SmallVector.hpp"
#include <vector>
#include <span>

namespace akruti {
    using Poly = containers::dynamic::SmallVector<Vec>;

    // ── Single half-plane clip: keep the portion of `subject` on the side where
    //    normal·(p - point) <= 0 (the "inside" / solid side). ─────────────────────────
    [[nodiscard]] inline Poly clip_halfplane(const Poly& subject, const Vec normal, const Vec point) {
        Poly out;
        const std::size_t n = subject.size();
        if (n == 0) return out;
        auto side = [&](const Vec p) { return akruti::dot(normal, p - point); };
        for (std::size_t i = 0; i < n; ++i) {
            const Vec cur = subject[i];
            const Vec prev = subject[(i + n - 1) % n];
            const Scalar dc = side(cur), dp = side(prev);
            const bool cur_in = dc <= static_cast<Scalar>(0);
            if (const bool prev_in = dp <= static_cast<Scalar>(0); cur_in != prev_in) {
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
        for (std::size_t i = 0; i < m && !result.empty(); ++i) {
            const Vec a = clip[i];
            const Vec b = clip[(i + 1) % m];
            const Vec edge = b - a;
            // Inward normal for CCW clip polygon points to the left of the edge.
            const Vec inward{-edge.y(), edge.x()};
            // clip_halfplane keeps normal·(p-point)<=0; we want the inward (left) side => use -inward.
            result = clip_halfplane(result, inward * static_cast<Scalar>(-1), a);
        }
        return result;
    }

    // ── Polygon area (signed, CCW positive). ───────────────────────────────────────────
    [[nodiscard]] inline Scalar polygon_area(const Poly& p) noexcept {
        const std::size_t n = p.size();
        if (n < 3) return static_cast<Scalar>(0);
        Scalar a = 0;
        for (std::size_t i = 0; i < n; ++i) a += akruti::cross(p[i], p[(i + 1) % n]);
        return a * static_cast<Scalar>(0.5);
    }

    // ── Polygon centroid ───────────────────────────────────────────────────────────────
    [[nodiscard]] inline Vec polygon_centroid(const Poly& p) noexcept {
        const std::size_t n = p.size();
        if (n == 0) return Vec{0, 0};
        if (n == 1) return p[0];
        if (n == 2) return (p[0] + p[1]) * static_cast<Scalar>(0.5);
        const Scalar a = polygon_area(p);
        if (std::fabs(a) < static_cast<Scalar>(1e-12)) {
            Vec c{0, 0};
            for (std::size_t i = 0; i < n; ++i) c = c + p[i];
            return c * (static_cast<Scalar>(1) / static_cast<Scalar>(n));
        }
        Vec c{0, 0};
        for (std::size_t i = 0; i < n; ++i) {
            const auto& p0 = p[i];
            const auto& p1 = p[(i + 1) % n];
            const Scalar factor = akruti::cross(p0, p1);
            c.x() += (p0.x() + p1.x()) * factor;
            c.y() += (p0.y() + p1.y()) * factor;
        }
        return c / (static_cast<Scalar>(6) * a);
    }

    // ── Voronoi shatter: split a convex boundary polygon into one cell per seed. ────────
    //    Each cell = boundary clipped by the bisector half-plane against every other seed.
    //    Returns fragment polygons (some may be empty if a seed is dominated). Areas tile the
    //    boundary (sum == boundary area up to clipping precision).
    template <typename OutContainer>
    inline void voronoi_shatter_into(const Poly& boundary,
                                     const std::span<const Vec> seeds,
                                     OutContainer& cells) {
        cells.reserve(cells.size() + seeds.size());
        for (std::size_t s = 0; s < seeds.size(); ++s) {
            Poly cell = boundary;
            const Vec si = seeds[s];
            for (std::size_t o = 0; o < seeds.size() && !cell.empty(); ++o) {
                if (o == s) continue;
                const Vec so = seeds[o];
                // Bisector: points equidistant. Keep side closer to si => normal toward so, point at midpoint.
                const Vec normal = so - si; // points from si to so
                const Vec mid = (si + so) * static_cast<Scalar>(0.5);
                cell = clip_halfplane(cell, normal, mid); // keep normal·(p-mid)<=0 (si's side)
            }
            cells.push_back(std::move(cell));
        }
    }

    [[nodiscard]] inline containers::dynamic::SmallVector<Poly, 16 * sizeof(Poly)>
    voronoi_shatter(const Poly& boundary, const std::span<const Vec> seeds) {
        containers::dynamic::SmallVector<Poly, 16 * sizeof(Poly)> cells;
        voronoi_shatter_into(boundary, seeds, cells);
        return cells;
    }

    [[nodiscard]] inline std::vector<Poly>
    voronoi_shatter(const Poly& boundary, const std::vector<Vec>& seeds) {
        std::vector<Poly> cells;
        voronoi_shatter_into(boundary, std::span<const Vec>(seeds.data(), seeds.size()), cells);
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
    [[nodiscard]] inline Poly rect_poly(Vec lo, Vec hi) {
        Poly p;
        p.push_back({lo.x(), lo.y()});
        p.push_back({hi.x(), lo.y()});
        p.push_back({hi.x(), hi.y()});
        p.push_back({lo.x(), hi.y()});
        return p;
    }
} // namespace akruti
