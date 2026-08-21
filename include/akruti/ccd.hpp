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
#include <cmath>

namespace akruti {

struct TOIResult {
    bool   hit{false};   // true if the shapes touch within the step
    Scalar t{1};         // time of impact in [0,1] (1 = no impact this step)
    int    iters{0};
};

// A shape displaced by a constant translation (thin wrapper preserving the Shape concept).
template <Shape S>
struct Translated {
    const S&     base;
    Vec2<Scalar> offset;
    [[nodiscard]] Scalar sdf(Vec2<Scalar> p) const noexcept { return base.sdf(p - offset); }
    [[nodiscard]] AABB<Scalar> aabb() const noexcept {
        AABB<Scalar> b = base.aabb();
        const Vec2<Scalar> blo{b.lo}, bhi{b.hi};
        return {{blo.x + offset.x, blo.y + offset.y}, {bhi.x + offset.x, bhi.y + offset.y}};
    }
    [[nodiscard]] Vec2<Scalar> support(Vec2<Scalar> d) const noexcept { return base.support(d) + offset; }
};

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
    Scalar t = 0;
    for (int i = 0; i < max_iter; ++i) {
        Translated<B> bt{b, motion * t};
        const Separation sep = gjk_distance(a, bt);
        if (sep.distance <= target_gap) return TOIResult{true, t, i + 1};
        // Closing speed along the separation direction bounds how far we can safely advance.
        const Scalar closing = std::fabs(motion.dot(sep.dir));
        if (closing < Scalar(1e-9)) return TOIResult{false, 1, i + 1}; // motion parallel to surfaces
        const Scalar dt = (sep.distance - target_gap) / closing;
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
