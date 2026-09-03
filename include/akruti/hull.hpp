#pragma once
// akruti/hull.hpp — Andrew's monotone-chain convex hull (2D). O(n log n): sort by (x,y),
// build lower + upper chains, keeping CCW turns. Plain C++, no heap beyond the caller's
// output buffer. Returns a ConvexPoly<N> in CCW winding. Irregular sort/scan — not kernelized.
#include "math.hpp"
#include "primitives.hpp"
#include "containers/static/static_vector.hpp"
#include <algorithm>
#include <cstddef>

namespace akruti {
    // Right turn test: >0 CCW (left), <0 CW (right), 0 collinear.
    namespace detail {
        [[nodiscard]] inline Scalar hull_turn(const Vec2<Scalar> o, const Vec2<Scalar> a, const Vec2<Scalar> b) noexcept {
            return cross(a - o, b - o);
        }
    } // namespace detail

    // Build the convex hull of up to N input points. Output has CCW winding, no collinear
    // interior points, first vertex not repeated. Degenerate (<3 unique) input passes through.
    template <std::size_t N = 8>
    [[nodiscard]] inline ConvexPoly<N> convex_hull(containers::static_vector<Vec2<Scalar>, N> pts) noexcept {
        const std::size_t n = pts.size();
        ConvexPoly < N > hull;
        if (n < 3) {
            for (std::size_t i = 0; i < n; ++i) (void)hull.verts.push_back(pts[i]);
            return hull;
        }
        std::sort(pts.begin(), pts.end(), [](Vec a, Vec b) {
            return x(a) < x(b) || (x(a) == x(b) && y(a) < y(b));
        });

        containers::static_vector<Vec2<Scalar>, 2 * N> ch;
        // Lower hull.
        for (std::size_t i = 0; i < n; ++i) {
            while (ch.size() >= 2 &&
                detail::hull_turn(ch[ch.size() - 2], ch[ch.size() - 1], pts[i]) <= static_cast<Scalar>(0))
                ch.pop_back();
            (void)ch.push_back(pts[i]);
        }
        // Upper hull.
        const std::size_t lower = ch.size() + 1;
        for (std::size_t i = n; i-- > 0;) {
            while (ch.size() >= lower &&
                detail::hull_turn(ch[ch.size() - 2], ch[ch.size() - 1], pts[i]) <= static_cast<Scalar>(0))
                ch.pop_back();
            (void)ch.push_back(pts[i]);
        }
        // Last point duplicates the first; drop it.
        for (std::size_t i = 0; i + 1 < ch.size(); ++i) (void)hull.verts.push_back(ch[i]);
        return hull;
    }
} // namespace akruti
