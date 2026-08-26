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
    akruti::ConvexPoly<8>
>;

// Component: an Akruti primitive placed by the entity Transform
struct ShapeRef {
    ShapeVariant shape;
    AABB         cached_fat_aabb{};
    bool         tree_inserted = false;
};

namespace detail {
inline void translate(akruti::Circle& c, const Vec2& p)      { c.center = c.center + p; }
inline void translate(akruti::Box& b, const Vec2& p)         { b.center = b.center + p; }
inline void translate(akruti::Capsule& c, const Vec2& p)     { c.a = c.a + p; c.b = c.b + p; }
inline void translate(akruti::OrientedBox& o, const Vec2& p) { o.center = o.center + p; }
inline void translate(akruti::Triangle& t, const Vec2& p)    { t.a = t.a + p; t.b = t.b + p; t.c = t.c + p; }
inline void translate(akruti::RoundedBox& r, const Vec2& p)  { r.center = r.center + p; }
inline void translate(akruti::Sector& s, const Vec2& p)      { s.center = s.center + p; }
template <std::size_t N>
inline void translate(akruti::ConvexPoly<N>& poly, const Vec2& p) {
    for (auto& v : poly.verts) {
        v = v + p;
    }
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
    containers::AABBTree<AABB> tree{Scalar(0.1)}; // fat margin reduces churn
    Scalar fat_margin = Scalar(0.2);

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
                narrow(ctx, ea.index, other, sa.shape, ta.position, sb->shape, tb->position);
            });
        });
    }

private:
    static void narrow(StepContext& ctx, std::uint32_t a, std::uint32_t b,
                       const ShapeVariant& va, const Vec2& pa,
                       const ShapeVariant& vb, const Vec2& pb) {
        std::visit([&](auto sa) {
            std::visit([&](auto sb) {
                detail::translate(sa, pa);
                detail::translate(sb, pb);
                if (akruti::gjk_overlap(sa, sb)) {
                    const akruti::Contact c = akruti::epa(sa, sb);
                    if (c.hit) {
                        ctx.events.publish(ContactEvent{a, b, {c.normal.x, c.normal.y}, c.depth});
                    }
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
        {std::min(box_start.lo.x, box_end.lo.x), std::min(box_start.lo.y, box_end.lo.y)},
        {std::max(box_start.hi.x, box_end.hi.x), std::max(box_start.hi.y, box_end.hi.y)}
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

