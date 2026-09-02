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

        // Overload 1: std::span / Poly input -> SmallVector<Triangle, 256> default (0 heap up to ~128 tris)
        [[nodiscard]] containers::dynamic::SmallVector<Triangle, 256>
        operator()(const std::span<const Vec> poly) const {
            containers::dynamic::SmallVector<Triangle, 256> res;
            triangulate_into(poly, res);
            return res;
        }

        [[nodiscard]] containers::dynamic::SmallVector<Triangle, 256>
        operator()(const Poly& poly) const {
            return (*this)(std::span<const Vec>(poly.data(), poly.size()));
        }

        // Overload 2: static_vector input -> static_vector output (100% stack, 0 heap)
        template <std::size_t N>
        [[nodiscard]] auto operator()(const containers::static_vector<Vec, N>& poly) const {
            constexpr std::size_t MaxTris = (N >= 3) ? (N - 2) : 0;
            containers::static_vector<Triangle, MaxTris> res;
            triangulate_into(std::span<const Vec>(poly.data(), poly.size()), res);
            return res;
        }

        // Overload 3: std::vector explicit output overload
        [[nodiscard]] std::vector<Triangle> triangulate_vector(const std::span<const Vec> poly) const {
            std::vector<Triangle> res;
            triangulate_into(poly, res);
            return res;
        }

        // Overload 4: In-place buffer sink (zero allocations across simulation loops)
        template <typename OutContainer>
        void triangulate_into(const std::span<const Vec> poly, OutContainer& out) const {
            if (poly.size() < 12) {
                Poly p;
                p.reserve(poly.size());
                for (const auto& pt : poly) p.push_back(pt);
                auto [vertices, indices] = ear_clip(p, {});
                for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                    (void)out.push_back(Triangle{
                        .a = vertices[indices[i]],
                        .b = vertices[indices[i + 1]],
                        .c = vertices[indices[i + 2]]
                    });
                }
                return;
            }
            cdt.triangulate_into(poly, out);
        }

        template <typename OutContainer>
        void triangulate_into(const Poly& poly, OutContainer& out) const {
            triangulate_into(std::span<const Vec>(poly.data(), poly.size()), out);
        }
    };

    static_assert(Triangulator<AutoTriangulator>, "AutoTriangulator must satisfy Triangulator concept");

    // ── AutoVoronoiBuilder: Naive (< 20 seeds) vs Fortune (>= 20 seeds) ───────
    struct AutoVoronoiBuilder {
        NaiveVoronoiBuilder naive{};
        FortuneVoronoiBuilder fortune{};

        // Overload 1: Default intelligent SBO return (SmallVector up to 16 cells on stack)
        [[nodiscard]] auto operator()(const Poly& boundary, const std::span<const Vec> seeds) const {
            return seeds.size() < 20 ? naive(boundary, seeds) : fortune(boundary, seeds);
        }

        // Overload 2: Explicit std::vector return
        [[nodiscard]] std::vector<Poly> build_vector(const Poly& boundary,
                                                     const std::span<const Vec> seeds) const {
            std::vector<Poly> out;
            build_into(boundary, seeds, out);
            return out;
        }

        template <typename OutContainer>
        void build_into(const Poly& boundary, const std::span<const Vec> seeds, OutContainer& out) const {
            if (seeds.size() < 20) {
                naive.build_into(boundary, seeds, out);
            }
            else {
                fortune.build_into(boundary, seeds, out);
            }
        }
    };

    static_assert(VoronoiBuilder<AutoVoronoiBuilder>, "AutoVoronoiBuilder must satisfy VoronoiBuilder concept");

    // ── HybridBroadphase: AABBTree (< 200 entities) vs SpatialHash (>= 200) ────
    struct HybridBroadphase {
        SpatialHashBroadphase hash{};
        std::size_t entity_count{0};
        static constexpr std::size_t kThreshold = 200;

        [[nodiscard]] bool use_hash() const noexcept { return entity_count >= kThreshold; }

        uint32_t insert(const AABB box, const uint32_t id) {
            ++entity_count;
            return hash.insert(box, id);
        }

        void remove(const uint32_t id) {
            if (entity_count > 0) --entity_count;
            hash.remove(id);
        }

        bool update(const uint32_t id, const AABB box) {
            return hash.update(id, box);
        }

        template <class Fn>
        void query(AABB box, Fn&& fn) const {
            hash.query(box, std::forward<Fn>(fn));
        }

        template <class Fn>
        void raycast(Vec origin, Vec dir, Scalar max_t, Fn&& fn) const {
            hash.raycast(origin, dir, max_t, std::forward<Fn>(fn));
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return hash.size();
        }
    };

    static_assert(Broadphase<HybridBroadphase>, "HybridBroadphase must satisfy Broadphase concept");
} // namespace akruti
