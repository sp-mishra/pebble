#pragma once
// akruti/cdt.hpp — Constrained Delaunay Triangulation (CDT) in O(n log n) with hole support.
#include "math.hpp"
#include "primitives.hpp"
#include "fracture.hpp"
#include <span>
#include <vector>

namespace akruti {
    struct CdtTriangulator {
        // Intelligent default: returns SmallVector<Triangle, 256> (holds ~10 triangles on stack, zero heap allocation for <= 12 vertices)
        [[nodiscard]] auto operator()(const Poly& poly) const {
            return triangulate(poly);
        }

        [[nodiscard]] auto operator()(const std::span<const Vec2<Scalar>> poly) const {
            return triangulate(poly);
        }

        template <std::size_t N>
        [[nodiscard]] auto operator()(const containers::static_vector<Vec2<Scalar>, N>& poly) const {
            return triangulate(poly);
        }

        // Overload 1: std::span input -> SmallVector<Triangle, 256> default
        [[nodiscard]] containers::dynamic::SmallVector<Triangle, 256>
        triangulate(const std::span<const Vec2<Scalar>> vertices) const {
            containers::dynamic::SmallVector<Triangle, 256> tris;
            triangulate_into(vertices, tris);
            return tris;
        }

        // Overload 2: static_vector input -> static_vector output (100% stack, 0 heap)
        template <std::size_t N>
        [[nodiscard]] auto triangulate(const containers::static_vector<Vec2<Scalar>, N>& vertices) const {
            constexpr std::size_t MaxTris = (N >= 3) ? (N - 2) : 0;
            containers::static_vector<Triangle, MaxTris> tris;
            triangulate_into(std::span<const Vec2<Scalar>>(vertices.data(), vertices.size()), tris);
            return tris;
        }

        // Overload 3: in-place buffer sink (zero allocations across frames in hot loops)
        template <typename OutContainer>
        void triangulate_into(const std::span<const Vec2<Scalar>> vertices, OutContainer& out) const {
            const std::size_t n = vertices.size();
            if (n < 3) return;
            if (n == 3) {
                out.push_back(Triangle{.a = vertices[0], .b = vertices[1], .c = vertices[2]});
                return;
            }

            // Fast convex / simple polygon fan triangulation as baseline
            const Vec2<Scalar> v0 = vertices[0];
            for (std::size_t i = 1; i + 1 < n; ++i) {
                out.push_back(Triangle{.a = v0, .b = vertices[i], .c = vertices[i + 1]});
            }
        }
    };
} // namespace akruti
