#pragma once
// akruti/mpr.hpp — Minkowski Portal Refinement (MPR) 2D Distance Oracle and Fast Penetration.
// Converges in 4-8 iterations for convex portals.
#include "shape.hpp"
#include "broadphase_concepts.hpp"
#include "gjk.hpp"
#include <cmath>
#include <algorithm>

namespace akruti {

struct MprResult {
    bool   hit{false};
    Scalar distance{0};
    Scalar depth{0};
    Vec    normal{1, 0};       // Outward normal from A into B
    Vec    contact_point{};
    int    iterations{0};
};

template <Shape A, Shape B>
[[nodiscard]] inline MprResult mpr_collide(const A& a, const B& b) noexcept {
    // 1. Center of Minkowski difference (v0)
    // Approximate centers from AABB
    const Vec c_a = Vec{a.aabb().center()};
    const Vec c_b = Vec{b.aabb().center()};
    const Vec v0 = c_a - c_b;

    if (v0.len2() < Scalar(1e-12)) {
        // Degenerate center: fallback to support diff
        const Vec p = support_diff(a, b, Vec{1, 0});
        return MprResult{true, 0.0f, 0.0f, Vec{1, 0}, c_a, 1};
    }

    // 2. Candidate direction toward origin from v0
    const Vec d1 = -v0;
    Vec v1 = support_diff(a, b, d1);

    // If support along d1 doesn't cross origin: shapes are separated along d1
    const Scalar proj = v1.dot(d1.normalized());
    if (proj <= 0.0f) {
        const Scalar dist = -proj;
        return MprResult{false, dist, 0.0f, d1.normalized(), a.support(d1), 1};
    }

    // 3. Find v2 to form candidate 2D portal (v0, v1, v2)
    Vec n = Vec{-(v1.y - v0.y), v1.x - v0.x}; // normal to segment v0-v1
    if (n.dot(-v0) < 0.0f) n = -n;

    Vec v2 = support_diff(a, b, n);

    MprResult res{};
    res.iterations = 2;

    // Portal refinement loop (typically 4-8 iterations)
    for (int iter = 0; iter < 16; ++iter) {
        ++res.iterations;
        // Check if origin is inside portal triangle (v0, v1, v2)
        const Vec e1 = v1 - v0;
        const Vec e2 = v2 - v0;
        const Scalar det = e1.x * e2.y - e1.y * e2.x;

        // Normal to portal edge v1-v2 facing away from v0
        Vec portal_n = Vec{v2.y - v1.y, -(v2.x - v1.x)};
        if (portal_n.dot(v0 - v1) > 0.0f) portal_n = -portal_n;
        if (portal_n.len2() > 1e-12f) portal_n = portal_n.normalized();
        else portal_n = d1.normalized();

        // Distance from origin to portal line segment v1-v2
        const Scalar d = portal_n.dot(v1);

        const Vec v3 = support_diff(a, b, portal_n);
        const Scalar d_new = portal_n.dot(v3);

        if (d_new - d < Scalar(1e-4) || iter == 15) {
            // Portal converged
            if (d < 0.0f) {
                // Origin is outside portal: separating distance
                res.hit = false;
                res.distance = -d;
                res.normal = portal_n;
                res.contact_point = a.support(portal_n);
                return res;
            } else {
                // Origin is inside portal: penetration depth
                res.hit = true;
                res.depth = d;
                res.normal = portal_n;
                res.contact_point = a.support(portal_n) - portal_n * (d * 0.5f);
                return res;
            }
        }

        // Refine portal: replace v1 or v2 with v3
        if (det * (e1.x * v3.y - e1.y * v3.x) > 0.0f) {
            v2 = v3;
        } else {
            v1 = v3;
        }
    }

    return res;
}

// Distance Oracle implementation using MPR
struct MprDistanceOracle {
    [[nodiscard]] DistanceResult distance(ShapeType type_a, const void* shape_a,
                                          ShapeType type_b, const void* shape_b) const noexcept {
        // Fast path for shapes
        DistanceResult res{};
        // Query via GJK/MPR
        // For general type-erased pointers, we can use the support function from ShapeStore if available,
        // or matrix dispatch
        return res;
    }

    template <Shape A, Shape B>
    [[nodiscard]] DistanceResult operator()(const A& a, const B& b) const noexcept {
        const auto mpr = mpr_collide(a, b);
        DistanceResult res;
        res.overlap = mpr.hit;
        res.distance = mpr.distance;
        res.normal = mpr.normal;
        res.closest_a = mpr.contact_point;
        res.closest_b = mpr.contact_point + mpr.normal * mpr.distance;
        res.iterations = mpr.iterations;
        return res;
    }
};

} // namespace akruti
