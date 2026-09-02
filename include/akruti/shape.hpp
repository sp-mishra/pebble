#pragma once
// akruti/shape.hpp — the Shape concept. Static polymorphism (concept + templates),
// no virtual, no macros. A Shape exposes a signed-distance function, a bounding box,
// and a GJK support function.
#include "math.hpp"
#include <concepts>

namespace akruti {
    // A shape provides: sdf(p) signed distance (negative inside), aabb() bound,
    // support(d) farthest point along direction d (for GJK/EPA on convex shapes), and centroid().
    template <class S>
    concept Shape = requires(const S s, Vec p, Vec d) {
        { s.sdf(p) } -> std::convertible_to<Scalar>;
        { s.aabb() } -> std::convertible_to<AABB>;
        { s.support(d) } -> std::convertible_to<Vec>;
        { s.centroid() } -> std::convertible_to<Vec>;
    };
} // namespace akruti
