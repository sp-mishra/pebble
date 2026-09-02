#pragma once
// akruti/query.hpp — geometric queries against Shapes and point sets. Plain C++ (irregular,
// data-dependent — same tier boundary as gjk.hpp). raycast marches an SDF sphere-trace;
// point_inside/closest_point use the SDF directly; winding_number is a robust point-in-polygon.
#include "shape.hpp"
#include "primitives.hpp"
#include <cmath>
#include <cstddef>

namespace akruti {
    // ── Point membership via SDF sign (negative = inside solid). ──────────────────────
    template <Shape S>
    [[nodiscard]] inline bool point_inside(const S& s, Vec2<Scalar> p) noexcept {
        return s.sdf(p) < Scalar(0);
    }

    // ── Closest surface point: single Newton step along the SDF gradient (finite-diff normal).
    //    Exact for primitives whose SDF is a true Euclidean distance; good enough elsewhere.
    template <Shape S>
    [[nodiscard]] inline Vec2<Scalar> closest_point(const S& s, Vec2<Scalar> p,
                                                    Scalar h = Scalar(1e-3)) noexcept {
        const Scalar d = s.sdf(p);
        const Scalar dx = s.sdf({p.x + h, p.y}) - s.sdf({p.x - h, p.y});
        const Scalar dy = s.sdf({p.x, p.y + h}) - s.sdf({p.x, p.y - h});
        Vec2<Scalar> g{dx, dy};
        g = g.normalized();
        return p - g * d; // step back to the zero level set
    }

    // ── Feasible projection onto a convex shape's solid interior (SDF ≤ 0). ────────────
    //    Returns p unchanged when already feasible (inside/on boundary); otherwise the
    //    nearest boundary point via closest_point. Unlike closest_point (which always
    //    lands on the surface), project is idempotent on feasible inputs — the operation
    //    an optimizer wants when clamping an iterate back into a 2D convex domain.
    template <Shape S>
    [[nodiscard]] inline Vec2<Scalar> project(const S& s, Vec2<Scalar> p,
                                              Scalar h = Scalar(1e-3)) noexcept {
        if (s.sdf(p) <= Scalar(0)) return p; // already feasible
        return closest_point(s, p, h);
    }

    struct RayHit {
        bool hit{false};
        Scalar t{0}; // distance along dir to the surface
        Vec2<Scalar> point{}; // hit position
        Vec2<Scalar> normal{}; // outward SDF gradient at hit
    };

    // ── SDF sphere-trace raycast. dir need not be normalized; t is measured in |dir| units. ──
    template <Shape S>
    [[nodiscard]] inline RayHit raycast(const S& s, Vec2<Scalar> origin, Vec2<Scalar> dir,
                                        Scalar tmax = Scalar(1e4), Scalar eps = Scalar(1e-4)) noexcept {
        const Vec2<Scalar> nd = dir.normalized();
        if (nd.len2() < Scalar(1e-18)) return RayHit{};
        Scalar t = 0;
        for (int i = 0; i < 128 && t < tmax; ++i) {
            const Vec2<Scalar> p = origin + nd * t;
            const Scalar d = s.sdf(p);
            if (d < eps) {
                const Scalar h = Scalar(1e-3);
                Vec2<Scalar> g{
                    s.sdf({p.x + h, p.y}) - s.sdf({p.x - h, p.y}),
                    s.sdf({p.x, p.y + h}) - s.sdf({p.x, p.y - h})
                };
                return RayHit{true, t, p, g.normalized()};
            }
            t += d; // safe advance: distance to nearest surface
        }
        return RayHit{};
    }

    // ── Robust winding number of point p about a closed polygon (CCW verts). ───────────
    //    Nonzero ⇒ inside. Uses the crossing-count sign accumulation (integer-stable).
    template <class It>
    [[nodiscard]] inline int winding_number(Vec2<Scalar> p, It first, It last) noexcept {
        int wn = 0;
        if (first == last) return 0;
        for (It i = first; i != last; ++i) {
            It j = i;
            ++j;
            const Vec2<Scalar> a = *i;
            const Vec2<Scalar> b = (j == last) ? *first : *j;
            if (a.y <= p.y) {
                if (b.y > p.y && cross(b - a, p - a) > Scalar(0)) ++wn;
            }
            else {
                if (b.y <= p.y && cross(b - a, p - a) < Scalar(0)) --wn;
            }
        }
        return wn;
    }

    template <std::size_t N>
    [[nodiscard]] inline int winding_number(Vec2<Scalar> p, const ConvexPoly<N>& poly) noexcept {
        return winding_number(p, poly.verts.begin(), poly.verts.end());
    }
} // namespace akruti
