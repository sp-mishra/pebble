#pragma once
// akruti/auto_policies.hpp — Runtime-Adaptive Algorithm Policies with Automatic Thresholds.
#include "broadphase_concepts.hpp"
#include "spatial_hash.hpp"
#include "voronoi.hpp"
#include "cdt.hpp"
#include "khanda.hpp"

namespace akruti {
    // ── AutoTriangulator: EarClip (< 12 verts) vs CDT (>= 12 verts) ───────────
    struct AutoTriangulator {
        khanda::EarClipTriangulator ear_clip{};
        CdtTriangulator cdt{};

        [[nodiscard]] std::vector<Triangle> operator()(const Poly& poly) const {
            if (poly.size() < 12) {
                std::vector<Vec> v(poly.begin(), poly.end());
                auto [vertices, indices] = ear_clip(v, {});
                std::vector<Triangle> res;
                for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                    res.push_back(Triangle{
                        .a = vertices[indices[i]],
                        .b = vertices[indices[i + 1]],
                        .c = vertices[indices[i + 2]]
                    });
                }
                return res;
            }
            return cdt(poly);
        }
    };

    static_assert(Triangulator<AutoTriangulator>, "AutoTriangulator must satisfy Triangulator concept");

    // ── AutoVoronoiBuilder: Naive (< 20 seeds) vs Fortune (>= 20 seeds) ───────
    struct AutoVoronoiBuilder {
        NaiveVoronoiBuilder naive{};
        FortuneVoronoiBuilder fortune{};

        [[nodiscard]] std::vector<Poly> operator()(const Poly& boundary, const std::span<const Vec2<Scalar>> seeds) const {
            return seeds.size() < 20 ? naive(boundary, seeds) : fortune(boundary, seeds);
        }
    };

    static_assert(VoronoiBuilder<AutoVoronoiBuilder>, "AutoVoronoiBuilder must satisfy VoronoiBuilder concept");

    // ── HybridBroadphase: AABBTree (< 200 entities) vs SpatialHash (>= 200) ────
    struct HybridBroadphase {
        SpatialHashBroadphase hash{};
        std::size_t entity_count{0};
        static constexpr std::size_t kThreshold = 200;

        [[nodiscard]] bool use_hash() const noexcept { return entity_count >= kThreshold; }

        uint32_t insert(const AABB<Scalar> box, const uint32_t id) {
            ++entity_count;
            return hash.insert(box, id);
        }

        void remove(const uint32_t id) {
            if (entity_count > 0) --entity_count;
            hash.remove(id);
        }

        bool update(const uint32_t id, const AABB<Scalar> box) {
            return hash.update(id, box);
        }

        template <class Fn>
        void query(AABB<Scalar> box, Fn&& fn) const {
            hash.query(box, std::forward<Fn>(fn));
        }

        template <class Fn>
        void raycast(Vec2<Scalar> origin, Vec2<Scalar> dir, Scalar max_t, Fn&& fn) const {
            hash.raycast(origin, dir, max_t, std::forward<Fn>(fn));
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return hash.size();
        }
    };

    static_assert(Broadphase<HybridBroadphase>, "HybridBroadphase must satisfy Broadphase concept");
} // namespace akruti
