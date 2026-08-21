#pragma once
// ============================================================================
// gati/collision.hpp — Akruti Geometry & Collision Bridge (guarded GATI_HAS_AKRUTI)
// ============================================================================
// Uses containers::AABBTree broadphase and Akruti GJK/EPA narrowphase.
// Operates on pebble::math::vec2 and pebble::math::aabb2.
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

using ShapeVariant = std::variant<akruti::Circle, akruti::Box, akruti::Capsule>;

// Component: an Akruti primitive placed by the entity Transform
struct ShapeRef {
    ShapeVariant shape;
};

namespace detail {
inline void translate(akruti::Circle& c, const Vec2& p)  { c.center = c.center + p; }
inline void translate(akruti::Box& b, const Vec2& p)     { b.center = b.center + p; }
inline void translate(akruti::Capsule& c, const Vec2& p) { c.a = c.a + p; c.b = c.b + p; }
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

    void run(World& w, StepContext ctx) {
        tree = containers::AABBTree<AABB>{Scalar(0.1)}; // rebuild each step

        w.view<ShapeRef, Transform>([&](Entity e, ShapeRef& s, Transform& tr) {
            tree.insert(shape_aabb(s.shape, tr.position), e.index);
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

} // namespace gati
#endif // GATI_HAS_AKRUTI
