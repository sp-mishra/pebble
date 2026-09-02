#pragma once
// akruti/broadphase_concepts.hpp — Plug-and-Play Policy Concepts for Geometry & Spatial Acceleration.
#include "primitives.hpp"
#include "narrowphase.hpp"
#include "fracture.hpp"
#include <concepts>
#include <span>

namespace akruti {
    // ── Broadphase: spatial acceleration structure ─────────────────────────
    template <class T>
    concept Broadphase = requires(T bp, Box2 box, uint32_t id) {
        { bp.insert(box, id) } -> std::same_as<uint32_t>;
        { bp.remove(id) };
        { bp.update(id, box) } -> std::same_as<bool>;
        bp.query(box, [](uint32_t) {});
        bp.raycast(Vec{}, Vec{}, Scalar{}, [](uint32_t) {});
        { bp.size() } -> std::convertible_to<std::size_t>;
    };

    // ── Narrowphase: collision detection algorithm ─────────────────────────
    template <class T>
    concept NarrowphaseAlgo = requires(T np) {
        {
            np.collide(ShapeType{}, static_cast<const void*>(nullptr),
                       ShapeType{}, static_cast<const void*>(nullptr))
        }
        -> std::same_as<Manifold>;
    };

    // ── Triangulator: polygon → triangles ──────────────────────────────────
    // Supports zero-heap SmallVector<Triangle>, static_vector, and std::vector automatically
    template <class T, class PolyT = Poly>
    concept Triangulator = requires(T tri, const PolyT& polygon) {
        { tri(polygon) } -> std::ranges::forward_range;
        requires std::same_as<std::ranges::range_value_t<decltype(tri(polygon))>, Triangle>;
    };

    // ── VoronoiBuilder: seed points → cell polygons ────────────────────────
    template <class T, class PolyT = Poly>
    concept VoronoiBuilder = requires(T vb, const PolyT& boundary,
                                      std::span<const Vec2<Scalar>> seeds) {
        { vb(boundary, seeds) } -> std::ranges::forward_range;
        requires std::same_as<std::ranges::range_value_t<decltype(vb(boundary, seeds))>, PolyT>;
    };

    // ── ConvexDecomposer: polygon / triangles → convex parts ───────────────
    template <class T>
    concept ConvexDecomposer = requires(T cd, std::span<const Triangle> tris) {
        { cd(tris) } -> std::ranges::forward_range;
        requires std::same_as<std::ranges::range_value_t<decltype(cd(tris))>, Poly>;
    };

    // ── DistanceOracle: shape pair → separation distance ───────────────────
    struct DistanceResult {
        Scalar distance{0};
        Vec closest_a{};
        Vec closest_b{};
        Vec normal{1, 0}; // normal from A to B
        bool overlap{false};
        int iterations{0};
    };

    template <class T>
    concept DistanceOracle = requires(T d) {
        {
            d.distance(ShapeType{}, static_cast<const void*>(nullptr),
                       ShapeType{}, static_cast<const void*>(nullptr))
        }
        -> std::same_as<DistanceResult>;
    };
} // namespace akruti
