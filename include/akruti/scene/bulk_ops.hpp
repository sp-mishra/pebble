#pragma once
// akruti/scene/bulk_ops.hpp — the four bulk geometric operations over a Scene. Each parallel op
// writes each result to a disjoint pre-indexed out[i] from read-only Scene state (no reduction, no
// shared mutable state), so the serial fallback is bit-identical to the Pravaha run regardless of
// chunk size. Reuses the existing serial kernels (gjk/epa, query::raycast, primitive sdf) and the
// AABBTree broadphase.
#include "../gjk.hpp"
#include "../query.hpp"
#include "scene.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace akruti::scene {
    struct PairId {
        std::uint32_t a{}, b{};
    };

    struct NearestHit {
        std::uint32_t payload{};
        Scalar dist{};
        Vec2<Scalar> point{};
    };

    struct Ray {
        Vec2<Scalar> o{}, d{};
        Scalar tmax{Scalar(1e4)};
    };

    struct GridSpec {
        Vec2<Scalar> origin{};
        Vec2<Scalar> cell{Scalar(1), Scalar(1)};
        std::uint32_t nx{}, ny{};
    };

    // ── (a) Broadphase pairs ─────────────────────────────────────────────────────────────────
    // For each leaf, query the tree; emit ordered pairs (a<b on packed payload) — dedup + self-exclude.
    // Serial: tree traversal is data-dependent and cheap vs narrowphase; emit order is deterministic.
    [[nodiscard]] inline std::vector<PairId> broadphase_pairs(const Scene& scene) {
        std::vector<PairId> pairs;
        scene.for_each_leaf([&](std::uint32_t a) {
            AABB<Scalar> box{};
            scene.dispatch(a, [&](const auto& batch, std::uint32_t idx) { box = batch.box(idx); });
            scene.tree().query(box, [&](std::uint32_t b) {
                if (a < b) pairs.push_back(PairId{a, b});
            });
        });
        return pairs;
    }

    // ── (b) Bulk narrowphase ─────────────────────────────────────────────────────────────────
    // Parallel over independent pairs; each pair reconstructs both prims and runs serial GJK/EPA.
    [[nodiscard]] inline std::vector<Contact> bulk_narrowphase(Scene& scene,
                                                               const std::vector<PairId>& pairs) {
        std::vector<Contact> out(pairs.size());
        scene.executor().for_range(pairs.size(), [&](std::size_t i) {
            const PairId pr = pairs[i];
            scene.dispatch(pr.a, [&](const auto& ba, std::uint32_t ia) {
                const auto sa = ba.get(ia);
                scene.dispatch(pr.b, [&](const auto& bb, std::uint32_t ib) {
                    const auto sb = bb.get(ib);
                    out[i] = gjk_overlap(sa, sb) ? epa(sa, sb) : Contact{false, 0, {}};
                });
            });
        });
        return out;
    }

    // ── (c) Bulk point membership ────────────────────────────────────────────────────────────
    // Parallel over query points; a point is inside iff some shape whose box contains it has sdf<0.
    // (Box-containment cull is exact for membership: sdf<0 => point in tight box.)
    inline void bulk_point_inside(Scene& scene, std::span<const Vec2<Scalar>> pts,
                                  std::span<std::uint8_t> out) {
        scene.executor().for_range(pts.size(), [&](std::size_t i) {
            const Vec2<Scalar> p = pts[i];
            bool inside = false;
            scene.tree().query(AABB<Scalar>{p, p}, [&](std::uint32_t payload) {
                if (inside) return;
                scene.dispatch(payload, [&](const auto& batch, std::uint32_t idx) {
                    if (batch.sdf(idx, p) < Scalar(0)) inside = true;
                });
            });
            out[i] = inside ? std::uint8_t(1) : std::uint8_t(0);
        });
    }

    // ── (c) Bulk raycast ─────────────────────────────────────────────────────────────────────
    // Parallel over rays; per ray the tree reports box-hit candidates, exact primitive raycast keeps
    // the smallest-t hit. (Ray/box slab cull is exact for "does the ray reach this shape".)
    inline void bulk_raycast(Scene& scene, std::span<const Ray> rays, std::span<RayHit> out) {
        scene.executor().for_range(rays.size(), [&](std::size_t i) {
            const Ray ray = rays[i];
            RayHit best{};
            best.t = std::numeric_limits<Scalar>::max();
            scene.tree().raycast(ray.o, ray.d, ray.tmax, [&](std::uint32_t payload) {
                scene.dispatch(payload, [&](const auto& batch, std::uint32_t idx) {
                    const RayHit h = ::akruti::raycast(batch.get(idx), ray.o, ray.d, ray.tmax);
                    if (h.hit && h.t < best.t) best = h;
                });
            });
            out[i] = best.hit ? best : RayHit{};
        });
    }

    // ── (c/d) Exact nearest shape ────────────────────────────────────────────────────────────
    // Expanding-radius query: grow a box around p until the current best distance is provably <= the query half-extent.
    [[nodiscard]] inline NearestHit nearest_at(const Scene& scene, Vec2<Scalar> p) {
        NearestHit best{0, std::numeric_limits<Scalar>::max(), {}};
        if (scene.count() == 0) return best;
        // Seed radius from the scene's overall extent so the first pass usually suffices.
        Scalar r = Scalar(1);
        for (int pass = 0; pass < 40; ++pass) {
            best.dist = std::numeric_limits<Scalar>::max();
            best.payload = 0;
            const AABB<Scalar> q{{p.x - r, p.y - r}, {p.x + r, p.y + r}};
            scene.tree().query(q, [&](std::uint32_t payload) {
                scene.dispatch(payload, [&](const auto& batch, std::uint32_t idx) {
                    const Scalar d = std::fabs(batch.sdf(idx, p));
                    if (d < best.dist) {
                        best.dist = d;
                        best.payload = payload;
                        best.point = p;
                    }
                });
            });
            // If the best surface distance fits inside the queried half-extent, no farther shape can be closer.
            if (best.dist <= r) return best;
            r *= Scalar(2);
        }
        return best;
    }

    inline void bulk_nearest_shape(Scene& scene, std::span<const Vec2<Scalar>> pts,
                                   std::span<NearestHit> out) {
        scene.executor().for_range(pts.size(), [&](std::size_t i) { out[i] = nearest_at(scene, pts[i]); });
    }

    // ── (d) Bulk SDF field ───────────────────────────────────────────────────────────────────
    // Min signed distance of the whole scene at each sample. Signed: negative inside any shape.
    [[nodiscard]] inline Scalar scene_sdf(const Scene& scene, Vec2<Scalar> p) {
        Scalar best = std::numeric_limits<Scalar>::max();
        Scalar r = Scalar(1);
        for (int pass = 0; pass < 40; ++pass) {
            Scalar pass_best = std::numeric_limits<Scalar>::max();
            const AABB<Scalar> q{{p.x - r, p.y - r}, {p.x + r, p.y + r}};
            scene.tree().query(q, [&](std::uint32_t payload) {
                scene.dispatch(payload, [&](const auto& batch, std::uint32_t idx) {
                    const Scalar d = batch.sdf(idx, p);
                    if (d < pass_best) pass_best = d;
                });
            });
            best = pass_best;
            if (best != std::numeric_limits<Scalar>::max() && std::fabs(best) <= r) break;
            r *= Scalar(2);
        }
        return best;
    }

    inline void bulk_sdf_field(Scene& scene, std::span<const Vec2<Scalar>> pts, std::span<Scalar> out) {
        scene.executor().for_range(pts.size(), [&](std::size_t i) { out[i] = scene_sdf(scene, pts[i]); });
    }

    inline void bulk_sdf_field(Scene& scene, const GridSpec& grid, std::span<Scalar> out) {
        const std::size_t total = std::size_t(grid.nx) * std::size_t(grid.ny);
        scene.executor().for_range(total, [&](std::size_t i) {
            const std::uint32_t ix = static_cast<std::uint32_t>(i % grid.nx);
            const std::uint32_t iy = static_cast<std::uint32_t>(i / grid.nx);
            const Vec2<Scalar> p{
                grid.origin.x + Scalar(ix) * grid.cell.x,
                grid.origin.y + Scalar(iy) * grid.cell.y
            };
            out[i] = scene_sdf(scene, p);
        });
    }
} // namespace akruti::scene
