#pragma once
// ============================================================================
// prakriti/compute/compute_backend.hpp — ComputeBackend concept.
// The uniform per-particle sweeps of the simulation loop (gravity, predict, velocity commit,
// damping, boundary clamp) are expressed once as a handful of stride-1 column primitives.
// A backend implements them; the engine is templated on the backend so the execution strategy
// (plain C++ / Highway SIMD / Pravaha parallel) is swappable with zero call-site change.
// ============================================================================
#include "../core/config.hpp"
#include <span>

namespace prakriti {

using CSpan = std::span<const Scalar>;
using MSpan = std::span<Scalar>;

template <class B>
concept ComputeBackend = requires(B b, MSpan out, CSpan a, CSpan mask, CSpan v, Scalar k,
                                  Scalar lo, Scalar hi) {
    // out[i] += mask[i] * k              (masked constant accumulate — gravity)
    b.axpy_const_masked(out, mask, k);
    // out[i] = base[i] + mask[i]*v[i]*k  (predict: pred = pos + active*vel*dt)
    b.predict(out, a, mask, v, k);
    // out[i] = (p[i] - q[i]) * k         (velocity from position delta)
    b.sub_scale(out, a, v, k);
    // out[i] *= s[i]                     (per-particle damping)
    b.mul_col(out, a);
    // out[i] = src[i]                    (commit)
    b.copy(out, a);
    // out[i] = clamp(out[i], lo, hi)     (boundary, per axis)
    b.clamp(out, lo, hi);
    // Sum 0.5 * m * (vx^2 + vy^2)        (SIMD kinetic energy reduction)
    { b.kinetic_energy(v, a, mask) } -> std::convertible_to<Scalar>;
};

} // namespace prakriti
