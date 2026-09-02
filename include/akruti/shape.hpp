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
    concept Shape = requires(const S s, Vec2<Scalar> p, Vec2<Scalar> d) {
        { s.sdf(p) } -> std::convertible_to<Scalar>;
        { s.aabb() } -> std::convertible_to<AABB<Scalar>>;
        { s.support(d) } -> std::convertible_to<Vec2<Scalar>>;
        { s.centroid() } -> std::convertible_to<Vec2<Scalar>>;
    };
} // namespace akruti
