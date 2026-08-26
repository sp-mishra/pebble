#pragma once
// ============================================================================
// gati/collision.hpp — Akruti Geometry Bridge with Incremental Fat Margin Refit
// ============================================================================
// Uses containers::AABBTree broadphase with fat margin caching and Akruti GJK/EPA.
// ============================================================================

#include "math.hpp"
#include "ecs.hpp"
#include "transform.hpp"
#include "system.hpp"
#include "event.hpp"

#if defined(GATI_ENABLE_AKRUTI) && __has_include("akruti/akruti.hpp")
#define GATI_HAS_AKRUTI 1
#include "akruti/akruti.hpp"
#include "containers/tree/AABBTree.hpp"

#include <variant>
#include <vector>

namespace gati {

using ShapeVariant = std::variant<
    akruti::Circle,
    akruti::Box,
    akruti::Capsule,
    akruti::OrientedBox,
    akruti::Triangle,
    akruti::RoundedBox,
    akruti::Sector,
    akruti::ConvexPoly<8>,
    akruti::ChainShape<16>,
    akruti::GridSDF<16, 16>
>;

// Component: an Akruti primitive placed by the entity Transform
struct ShapeRef {
    ShapeVariant shape;
    AABB         cached_fat_aabb{};
    bool         tree_inserted = false;
};

namespace detail {
inline void translate(akruti::Circle& c, const Vec2& p)      { const akruti::Vec ap(p); c.center = c.center + ap; }
inline void translate(akruti::Box& b, const Vec2& p)         { const akruti::Vec ap(p); b.center = b.center + ap; }
inline void translate(akruti::Capsule& c, const Vec2& p)     { const akruti::Vec ap(p); c.a = c.a + ap; c.b = c.b + ap; }
inline void translate(akruti::OrientedBox& o, const Vec2& p) { const akruti::Vec ap(p); o.center = o.center + ap; }
inline void translate(akruti::Triangle& t, const Vec2& p)    { const akruti::Vec ap(p); t.a = t.a + ap; t.b = t.b + ap; t.c = t.c + ap; }
inline void translate(akruti::RoundedBox& r, const Vec2& p)  { const akruti::Vec ap(p); r.center = r.center + ap; }
inline void translate(akruti::Sector& s, const Vec2& p)      { const akruti::Vec ap(p); s.center = s.center + ap; }
template <std::size_t N>
inline void translate(akruti::ConvexPoly<N>& poly, const Vec2& p) {
    const akruti::Vec ap(p);
    for (auto& v : poly.verts) {
        v = v + ap;
    }
}
template <std::size_t N>
inline void translate(akruti::ChainShape<N>& chain, const Vec2& p) {
    const akruti::Vec ap(p);
    for (auto& v : chain.verts) {
        v = v + ap;
    }
    if (chain.has_prev_ghost) chain.prev_ghost = chain.prev_ghost + ap;
    if (chain.has_next_ghost) chain.next_ghost = chain.next_ghost + ap;
}
template <std::size_t W, std::size_t H>
inline void translate(akruti::GridSDF<W, H>& grid, const Vec2& p) {
    grid.bounds = AABB(grid.bounds.lo + p, grid.bounds.hi + p);
}
} // namespace detail

// World AABB of a shape placed at p
inline AABB shape_aabb(const ShapeVariant& v, const Vec2& p) {
    return std::visit([&](auto s) -> AABB {
        detail::translate(s, p);
        auto akruti_box = s.aabb();
        return AABB(akruti_box.lo, akruti_box.hi);
    }, v);
}

// Broadphase over transformed AABBs + GJK/EPA narrowphase; emits ContactEvents.
struct CollisionSystem {
    containers::AABBTree<AABB>                       tree{Scalar(0.1)}; // fat margin reduces churn
    Scalar                                           fat_margin = Scalar(0.2);
    std::unordered_map<std::uint64_t, akruti::SimplexCache> simplex_caches_{};

    void run(World& w, StepContext ctx) {
        // Rebuild or update tree with fat margin
        tree = containers::AABBTree<AABB>{fat_margin};

        w.view<ShapeRef, Transform>([&](Entity e, ShapeRef& s, Transform& tr) {
            auto current_box = shape_aabb(s.shape, tr.position);
            s.cached_fat_aabb = current_box.fattened(fat_margin);
            tree.insert(s.cached_fat_aabb, e.index);
        });

        w.view<ShapeRef, Transform>([&](Entity ea, ShapeRef& sa, Transform& ta) {
            const auto qbox = shape_aabb(sa.shape, ta.position);
            tree.query(qbox, [&](std::uint32_t other) {
                if (other <= ea.index) return; // a < b dedup + skip self
                ShapeRef*  sb = w.get_by_index<ShapeRef>(other);
                Transform* tb = w.get_by_index<Transform>(other);
                if (!sb || !tb) return;

                const std::uint64_t key = (static_cast<std::uint64_t>(ea.index) << 32) | static_cast<std::uint64_t>(other);
                auto& cache = simplex_caches_[key];
                narrow(ctx, ea.index, other, sa.shape, ta.position, sb->shape, tb->position, cache);
            });
        });
    }

private:
    static void narrow(StepContext& ctx, std::uint32_t a, std::uint32_t b,
                       const ShapeVariant& va, const Vec2& pa,
                       const ShapeVariant& vb, const Vec2& pb,
                       akruti::SimplexCache& cache) {
        std::visit([&](auto sa) {
            std::visit([&](auto sb) {
                detail::translate(sa, pa);
                detail::translate(sb, pb);
                akruti::Manifold m = akruti::collide_gjk_warm_started(sa, sb, &cache);
                if (m.hit) {
                    const Vec2 cp = m.points.empty() ? Vec2{0.0f, 0.0f} : Vec2{m.points[0].point.x, m.points[0].point.y};
                    ctx.events.publish(ContactEvent{a, b, {m.normal.x, m.normal.y}, m.depth, cp});
                }
            }, vb);
        }, va);
    }
};

// Broadphase-accelerated raycast: nearest entity hit + Akruti RayHit
struct RaycastResult {
    Entity         entity = null_entity;
    akruti::RayHit hit;
};

[[nodiscard]] inline RaycastResult raycast(World& w, CollisionSystem& cs,
                                           const Vec2& origin, const Vec2& dir,
                                           Scalar max_t = Scalar(1e4)) {
    RaycastResult best;
    best.hit.t = max_t;
    cs.tree.raycast(origin, dir, max_t, [&](std::uint32_t idx) {
        ShapeRef*  s = w.get_by_index<ShapeRef>(idx);
        Transform* t = w.get_by_index<Transform>(idx);
        if (!s || !t) return;
        std::visit([&](auto prim) {
            detail::translate(prim, t->position);
            const akruti::RayHit h = akruti::raycast(prim, origin, dir, max_t);
            if (h.hit && h.t < best.hit.t) {
                best.hit = h;
                best.entity = Entity{idx, w.generation_of(idx)};
            }
        }, s->shape);
    });
    return best;
}

// Continuous Collision Detection (CCD) Sweep Query between moving entity and static world
struct SweepResult {
    Entity            entity = null_entity;
    akruti::TOIResult toi{};
};

[[nodiscard]] inline SweepResult sweep_test(World& w, CollisionSystem& cs,
                                            const ShapeVariant& moving_shape,
                                            const Vec2& start_pos, const Vec2& delta_pos) {
    SweepResult best;
    best.toi.t = Scalar(1.0f);

    // Compute broadphase swept AABB
    AABB box_start = shape_aabb(moving_shape, start_pos);
    AABB box_end   = shape_aabb(moving_shape, start_pos + delta_pos);
    AABB swept_box{
        {std::min(box_start.lo[0], box_end.lo[0]), std::min(box_start.lo[1], box_end.lo[1])},
        {std::max(box_start.hi[0], box_end.hi[0]), std::max(box_start.hi[1], box_end.hi[1])}
    };

    cs.tree.query(swept_box, [&](std::uint32_t other_idx) {
        ShapeRef*  other_s = w.get_by_index<ShapeRef>(other_idx);
        Transform* other_t = w.get_by_index<Transform>(other_idx);
        if (!other_s || !other_t) return;

        std::visit([&](auto static_prim) {
            std::visit([&](auto mover_prim) {
                detail::translate(static_prim, other_t->position);
                detail::translate(mover_prim, start_pos);

                auto res = akruti::time_of_impact(static_prim, mover_prim, delta_pos);
                if (res.hit && res.t < best.toi.t) {
                    best.toi = res;
                    best.entity = Entity{other_idx, w.generation_of(other_idx)};
                }
            }, moving_shape);
        }, other_s->shape);
    });

    return best;
}

} // namespace gati
#endif // GATI_HAS_AKRUTI

