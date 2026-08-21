#pragma once
// akruti/scene/batch.hpp — SoA storage for one primitive type. ShapeBatch<Prim> holds N shapes as
// struct-of-arrays (cache-coherent, auto-vectorizable for AABB refit / SDF sweeps) and reconstructs a
// Prim on demand for irregular kernels (gjk/raycast). Fixed-field primitives get explicit column
// layouts; irregular/unbounded ones (HalfPlane, ConvexPoly) fall back to AoS. A per-element leaf id
// and cached box support later broadphase update/remove.
#include "../primitives.hpp"

#include <cstdint>
#include <vector>

namespace akruti::scene {

// ── Per-primitive column layout. Primary template = AoS fallback (HalfPlane, ConvexPoly<N>). ──
// Specializations below give true SoA for fixed-field primitives.
template <class Prim>
struct BatchColumns {
    std::vector<Prim> items;

    std::uint32_t push(const Prim& p) {
        items.push_back(p);
        return static_cast<std::uint32_t>(items.size() - 1);
    }
    [[nodiscard]] Prim get(std::uint32_t i) const noexcept { return items[i]; }
    [[nodiscard]] std::size_t size() const noexcept { return items.size(); }
};

template <>
struct BatchColumns<Circle> {
    std::vector<Scalar> cx, cy, r;
    std::uint32_t push(const Circle& c) {
        cx.push_back(c.center.x); cy.push_back(c.center.y); r.push_back(c.radius);
        return static_cast<std::uint32_t>(cx.size() - 1);
    }
    [[nodiscard]] Circle get(std::uint32_t i) const noexcept { return Circle{{cx[i], cy[i]}, r[i]}; }
    [[nodiscard]] std::size_t size() const noexcept { return cx.size(); }
};

template <>
struct BatchColumns<Box> {
    std::vector<Scalar> cx, cy, hx, hy;
    std::uint32_t push(const Box& b) {
        cx.push_back(b.center.x); cy.push_back(b.center.y);
        hx.push_back(b.half.x);   hy.push_back(b.half.y);
        return static_cast<std::uint32_t>(cx.size() - 1);
    }
    [[nodiscard]] Box get(std::uint32_t i) const noexcept { return Box{{cx[i], cy[i]}, {hx[i], hy[i]}}; }
    [[nodiscard]] std::size_t size() const noexcept { return cx.size(); }
};

template <>
struct BatchColumns<Segment> {
    std::vector<Scalar> ax, ay, bx, by;
    std::uint32_t push(const Segment& s) {
        ax.push_back(s.a.x); ay.push_back(s.a.y); bx.push_back(s.b.x); by.push_back(s.b.y);
        return static_cast<std::uint32_t>(ax.size() - 1);
    }
    [[nodiscard]] Segment get(std::uint32_t i) const noexcept { return Segment{{ax[i], ay[i]}, {bx[i], by[i]}}; }
    [[nodiscard]] std::size_t size() const noexcept { return ax.size(); }
};

template <>
struct BatchColumns<Capsule> {
    std::vector<Scalar> ax, ay, bx, by, r;
    std::uint32_t push(const Capsule& c) {
        ax.push_back(c.a.x); ay.push_back(c.a.y); bx.push_back(c.b.x); by.push_back(c.b.y);
        r.push_back(c.radius);
        return static_cast<std::uint32_t>(ax.size() - 1);
    }
    [[nodiscard]] Capsule get(std::uint32_t i) const noexcept {
        return Capsule{{ax[i], ay[i]}, {bx[i], by[i]}, r[i]};
    }
    [[nodiscard]] std::size_t size() const noexcept { return ax.size(); }
};

// ── ShapeBatch<Prim> ──────────────────────────────────────────────────────────────────────
// SoA columns + a cached tight box per element + the broadphase leaf id per element.
template <class Prim>
struct ShapeBatch {
    BatchColumns<Prim>           cols;
    std::vector<AABB<Scalar>>    boxes;    // cached tight box, refit() recomputes
    std::vector<std::uint32_t>   leaves;   // AABBTree leaf id per element

    std::uint32_t add(const Prim& p) {
        const std::uint32_t i = cols.push(p);
        boxes.push_back(p.aabb());
        leaves.push_back(0);
        return i;
    }
    [[nodiscard]] Prim         get(std::uint32_t i) const noexcept { return cols.get(i); }
    [[nodiscard]] AABB<Scalar> box(std::uint32_t i) const noexcept { return boxes[i]; }
    [[nodiscard]] Scalar       sdf(std::uint32_t i, Vec2<Scalar> p) const noexcept {
        return cols.get(i).sdf(p);
    }
    [[nodiscard]] std::size_t  size() const noexcept { return cols.size(); }

    // Recompute all cached boxes from current columns (uniform per-element sweep).
    void refit() {
        const std::size_t n = cols.size();
        boxes.resize(n);
        for (std::size_t i = 0; i < n; ++i) boxes[i] = cols.get(static_cast<std::uint32_t>(i)).aabb();
    }
};

} // namespace akruti::scene
