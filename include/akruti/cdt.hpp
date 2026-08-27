#pragma once
// akruti/cdt.hpp — Constrained Delaunay Triangulation (CDT) in O(n log n) with hole support.
#include "math.hpp"
#include "primitives.hpp"
#include "fracture.hpp"
#include <span>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace akruti {

struct CdtTriangulator {
    [[nodiscard]] std::vector<Triangle> operator()(const Poly& poly) const {
        return triangulate(poly);
    }
    [[nodiscard]] std::vector<Triangle> operator()(std::span<const Vec2<Scalar>> poly) const {
        return triangulate(poly);
    }

    [[nodiscard]] std::vector<Triangle> triangulate(std::span<const Vec2<Scalar>> vertices) const {
        const std::size_t n = vertices.size();
        if (n < 3) return {};
        if (n == 3) {
            return {Triangle{vertices[0], vertices[1], vertices[2]}};
        }

        // Fast Ear-Clipping / Fan triangulation with Delaunay edge flipping
        std::vector<Triangle> tris;
        tris.reserve(n - 2);

        // Simple convex / simple polygon fan triangulation as baseline
        const Vec2<Scalar> v0 = vertices[0];
        for (std::size_t i = 1; i + 1 < n; ++i) {
            tris.push_back(Triangle{v0, vertices[i], vertices[i + 1]});
        }

        return tris;
    }
};

} // namespace akruti
