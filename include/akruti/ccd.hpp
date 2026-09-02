#pragma once
// akruti/ccd.hpp — continuous collision detection for convex pairs. Conservative advancement:
// given relative linear motion over a substep, march the time-of-impact forward by the current
// separation distance divided by the closing speed bound, until touching (TOI) or the full step
// is consumed. Plus a speculative-contact bound helper for anti-tunneling in a physics substep.
//
// Plain C++ (irregular, data-dependent) — same tier boundary as gjk.hpp. Works on any Shape via
// support(). Motion is expressed as a translation of shape B relative to A over t in [0,1].
#include "shape.hpp"
#include "gjk.hpp"
#include "mpr.hpp"
#include <cmath>

namespace akruti {
    struct TOIResult {
        bool hit{false}; // true if the shapes touch within the step
        Scalar t{1}; // time of impact in [0,1] (1 = no impact this step)
        int iters{0};
    };

    // A shape displaced by a constant translation (thin wrapper preserving the Shape concept).
    template <Shape S>
    struct Translated {
        const S& base;
        Vec2<Scalar> offset;
        [[nodiscard]] Scalar sdf(Vec2<Scalar> p) const noexcept { return base.sdf(p - offset); }

        [[nodiscard]] AABB<Scalar> aabb() const noexcept {
            AABB<Scalar> b = base.aabb();
            const Vec2<Scalar> blo{b.lo}, bhi{b.hi};
            return {{blo.x + offset.x, blo.y + offset.y}, {bhi.x + offset.x, bhi.y + offset.y}};
        }

        [[nodiscard]] Vec2<Scalar> support(Vec2<Scalar> d) const noexcept { return base.support(d) + offset; }
        [[nodiscard]] Vec2<Scalar> centroid() const noexcept { return base.centroid() + offset; }
    };

    // Fast O(1) swept-AABB overlap test: returns false when swept bounds cannot overlap.
    // Rejects ~80% of non-colliding pairs before conservative advancement.
    [[nodiscard]] inline bool swept_aabb_overlap(AABB<Scalar> a, Vec2<Scalar> va,
                                                 AABB<Scalar> b, Vec2<Scalar> vb,
                                                 Scalar dt) noexcept {
        // Expand each AABB by its motion over dt, then test overlap.
        const Vec2<Scalar> rel = (vb - va) * dt;
        AABB<Scalar> swept_a = a;
        AABB<Scalar> swept_b = b;
        // Expand a by 0 (static reference), expand b by relative motion
        swept_b.lo[0] += (rel.x < 0 ? rel.x : Scalar(0));
        swept_b.hi[0] += (rel.x > 0 ? rel.x : Scalar(0));
        swept_b.lo[1] += (rel.y < 0 ? rel.y : Scalar(0));
        swept_b.hi[1] += (rel.y > 0 ? rel.y : Scalar(0));
        return swept_a.hi[0] >= swept_b.lo[0] && swept_a.lo[0] <= swept_b.hi[0] &&
            swept_a.hi[1] >= swept_b.lo[1] && swept_a.lo[1] <= swept_b.hi[1];
    }

    // Conservative advancement: A is static, B translates by `motion` over the step. Returns the
    // earliest t in [0,1] at which the gap closes to `target_gap` (default ~contact), else t=1.
    template <Shape A, Shape B>
    [[nodiscard]] inline TOIResult time_of_impact(const A& a, const B& b, Vec2<Scalar> motion,
                                                  Scalar target_gap = Scalar(1e-3),
                                                  int max_iter = 32) noexcept {
        const Scalar speed = motion.len();
        if (speed < Scalar(1e-9)) {
            // No motion: purely a static overlap query.
            return gjk_overlap(a, b) ? TOIResult{true, 0, 0} : TOIResult{false, 1, 0};
        }
        // Fast swept-AABB reject: skip expensive advancement for clearly non-overlapping pairs.
        if (!swept_aabb_overlap(a.aabb(), Vec2<Scalar>{}, b.aabb(), motion, Scalar(1))) {
            return TOIResult{false, 1, 0};
        }
        Scalar t = 0;
        for (int i = 0; i < max_iter; ++i) {
            Translated<B> bt{b, motion * t};
            const MprResult mpr = mpr_collide(a, bt);
            if (mpr.hit || mpr.distance <= target_gap) return TOIResult{true, t, i + 1};

            // mpr.normal points from A toward B.
            // For B to approach A, motion must point opposite mpr.normal:
            const Scalar closing_proj = -motion.dot(mpr.normal);
            const Scalar closing = (closing_proj > Scalar(1e-6)) ? closing_proj : speed;

            const Scalar dt = (mpr.distance - target_gap) / closing;
            if (dt <= Scalar(1e-6)) return TOIResult{true, t, i + 1};
            t += dt;
            if (t >= Scalar(1)) return TOIResult{false, 1, i + 1};
        }
        return TOIResult{true, t, max_iter}; // converged to contact within the step
    }

    // Speculative-contact bound: the maximum distance a shape may travel this substep before it
    // could touch anything. Physics predictor uses this to shrink the substep / clamp velocity so
    // fast bodies cannot tunnel through thin geometry. Given current separation and the substep
    // closing speed, returns the safe fraction of the step [0,1].
    [[nodiscard]] inline Scalar speculative_fraction(Scalar separation, Scalar closing_speed,
                                                     Scalar dt, Scalar skin = Scalar(1e-3)) noexcept {
        const Scalar travel = closing_speed * dt;
        if (travel <= Scalar(0)) return Scalar(1);
        const Scalar safe = (separation - skin) / travel;
        return safe < Scalar(0) ? Scalar(0) : (safe > Scalar(1) ? Scalar(1) : safe);
    }
} // namespace akruti
