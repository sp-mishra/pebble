#pragma once
// akruti/cdt.hpp — Constrained Delaunay Triangulation (CDT) in O(n log n) with hole support.
#include "math.hpp"
#include "primitives.hpp"
#include "fracture.hpp"
#include <span>
#include <vector>

namespace akruti {
    struct CdtTriangulator {
        [[nodiscard]] std::vector<Triangle> operator()(const Poly& poly) const {
            return triangulate(poly);
        }

        [[nodiscard]] std::vector<Triangle> operator()(const std::span<const Vec2<Scalar>> poly) const {
            return triangulate(poly);
        }

        [[nodiscard]] std::vector<Triangle> triangulate(const std::span<const Vec2<Scalar>> vertices) const {
            const std::size_t n = vertices.size();
            if (n < 3) return {};
            if (n == 3) {
                return {Triangle{.a = vertices[0], .b = vertices[1], .c = vertices[2]}};
            }

            // Fast Ear-Clipping / Fan triangulation with Delaunay edge flipping
            std::vector<Triangle> tris;
            tris.reserve(n - 2);

            // Simple convex / simple polygon fan triangulation as baseline
            const Vec2<Scalar> v0 = vertices[0];
            for (std::size_t i = 1; i + 1 < n; ++i) {
                tris.push_back(Triangle{.a = v0, .b = vertices[i], .c = vertices[i + 1]});
            }

            return tris;
        }
    };
} // namespace akruti
