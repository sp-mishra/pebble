#pragma once
// ============================================================================
// akruti/simd_spline.hpp — Google Highway SIMD Vectorized Spline Batch Evaluator
// ============================================================================

#include "spline.hpp"
#include <hwy/highway.h>

namespace akruti {
    // Batch evaluation of N Bézier points across SIMD lanes
    inline void simd_evaluate_bezier_batch(const CubicBezierCurve& curve,
                                           const float* HWY_RESTRICT t_in,
                                           float* HWY_RESTRICT x_out,
                                           float* HWY_RESTRICT y_out,
                                           std::size_t count) noexcept {
        namespace hn = hwy::HWY_NAMESPACE;
        using D = hn::ScalableTag<float>;
        const D d;
        const std::size_t N = hn::Lanes(d);

        const auto p0x = hn::Set(d, curve.p0.x);
        const auto p0y = hn::Set(d, curve.p0.y);
        const auto p1x = hn::Set(d, curve.p1.x);
        const auto p1y = hn::Set(d, curve.p1.y);
        const auto p2x = hn::Set(d, curve.p2.x);
        const auto p2y = hn::Set(d, curve.p2.y);
        const auto p3x = hn::Set(d, curve.p3.x);
        const auto p3y = hn::Set(d, curve.p3.y);

        const auto c1 = hn::Set(d, 1.0f);
        const auto c3 = hn::Set(d, 3.0f);

        std::size_t i = 0;
        for (; i + N <= count; i += N) {
            const auto t = hn::Load(d, t_in + i);
            const auto u = hn::Sub(c1, t);

            const auto tt = hn::Mul(t, t);
            const auto uu = hn::Mul(u, u);
            const auto uuu = hn::Mul(uu, u);
            const auto ttt = hn::Mul(tt, t);

            const auto c_p1 = hn::Mul(c3, hn::Mul(uu, t));
            const auto c_p2 = hn::Mul(c3, hn::Mul(u, tt));

            const auto rx = hn::Add(hn::Add(hn::Mul(p0x, uuu), hn::Mul(p1x, c_p1)),
                                    hn::Add(hn::Mul(p2x, c_p2), hn::Mul(p3x, ttt)));

            const auto ry = hn::Add(hn::Add(hn::Mul(p0y, uuu), hn::Mul(p1y, c_p1)),
                                    hn::Add(hn::Mul(p2y, c_p2), hn::Mul(p3y, ttt)));

            hn::Store(rx, d, x_out + i);
            hn::Store(ry, d, y_out + i);
        }

        // Scalar remainder
        for (; i < count; ++i) {
            auto pt = curve.evaluate(t_in[i]);
            x_out[i] = pt.x;
            y_out[i] = pt.y;
        }
    }
} // namespace akruti
