#pragma once
// ============================================================================
// prakriti/compute/highway_backend.hpp — tier-2 ComputeBackend: Google Highway SIMD.
// Vectorized stride-1 column ops (NEON on Apple Silicon, AVX-512/AVX2 on x86),
// with scalar tail for non-lane-multiple lengths.
//
// Guarded on Highway availability (<hwy/highway.h>). When absent, defines nothing.
// ============================================================================
#if __has_include(<hwy/highway.h>)
#define PRAKRITI_HAS_HIGHWAY_BACKEND 1

#include "compute_backend.hpp"
#include <hwy/highway.h>
#include <algorithm>
#include <cstddef>

namespace prakriti {

struct HighwayBackend {
    void axpy_const_masked(MSpan out, CSpan mask, Scalar k) const noexcept {
        const std::size_t n = out.size();
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<float> d;
        const std::size_t N = hn::Lanes(d);

        const auto vk = hn::Set(d, k);
        std::size_t i = 0;
        for (; i + N <= n; i += N) {
            const auto m = hn::LoadU(d, &mask[i]);
            const auto o = hn::LoadU(d, &out[i]);
            const auto res = hn::MulAdd(m, vk, o);
            hn::StoreU(res, d, &out[i]);
        }
        for (; i < n; ++i) {
            out[i] += mask[i] * k;
        }
    }

    void predict(MSpan out, CSpan base, CSpan mask, CSpan v, Scalar k) const noexcept {
        const std::size_t n = out.size();
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<float> d;
        const std::size_t N = hn::Lanes(d);

        const auto vk = hn::Set(d, k);
        std::size_t i = 0;
        for (; i + N <= n; i += N) {
            const auto b = hn::LoadU(d, &base[i]);
            const auto m = hn::LoadU(d, &mask[i]);
            const auto vel = hn::LoadU(d, &v[i]);
            const auto mv = hn::Mul(m, vel);
            const auto res = hn::MulAdd(mv, vk, b);
            hn::StoreU(res, d, &out[i]);
        }
        for (; i < n; ++i) {
            out[i] = base[i] + mask[i] * v[i] * k;
        }
    }

    void sub_scale(MSpan out, CSpan p, CSpan q, Scalar k) const noexcept {
        const std::size_t n = out.size();
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<float> d;
        const std::size_t N = hn::Lanes(d);

        const auto vk = hn::Set(d, k);
        std::size_t i = 0;
        for (; i + N <= n; i += N) {
            const auto vp = hn::LoadU(d, &p[i]);
            const auto vq = hn::LoadU(d, &q[i]);
            const auto diff = hn::Sub(vp, vq);
            const auto res = hn::Mul(diff, vk);
            hn::StoreU(res, d, &out[i]);
        }
        for (; i < n; ++i) {
            out[i] = (p[i] - q[i]) * k;
        }
    }

    void mul_col(MSpan out, CSpan s) const noexcept {
        const std::size_t n = out.size();
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<float> d;
        const std::size_t N = hn::Lanes(d);

        std::size_t i = 0;
        for (; i + N <= n; i += N) {
            const auto vo = hn::LoadU(d, &out[i]);
            const auto vs = hn::LoadU(d, &s[i]);
            const auto res = hn::Mul(vo, vs);
            hn::StoreU(res, d, &out[i]);
        }
        for (; i < n; ++i) {
            out[i] *= s[i];
        }
    }

    void copy(MSpan out, CSpan src) const noexcept {
        std::copy(src.begin(), src.end(), out.begin());
    }

    void clamp(MSpan out, Scalar lo, Scalar hi) const noexcept {
        const std::size_t n = out.size();
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<float> d;
        const std::size_t N = hn::Lanes(d);

        const auto vlo = hn::Set(d, lo);
        const auto vhi = hn::Set(d, hi);
        std::size_t i = 0;
        for (; i + N <= n; i += N) {
            const auto vo = hn::LoadU(d, &out[i]);
            const auto clamped = hn::Min(hn::Max(vo, vlo), vhi);
            hn::StoreU(clamped, d, &out[i]);
        }
        for (; i < n; ++i) {
            out[i] = std::min(std::max(out[i], lo), hi);
        }
    }

    [[nodiscard]] Scalar kinetic_energy(CSpan vx, CSpan vy, CSpan inv_mass) const noexcept {
        const std::size_t n = vx.size();
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<float> d;
        const std::size_t N = hn::Lanes(d);

        auto sum_vec = hn::Zero(d);
        const auto half = hn::Set(d, 0.5f);
        const auto one = hn::Set(d, 1.0f);
        const auto zero = hn::Zero(d);

        std::size_t i = 0;
        for (; i + N <= n; i += N) {
            const auto v_vx = hn::LoadU(d, &vx[i]);
            const auto v_vy = hn::LoadU(d, &vy[i]);
            const auto v_im = hn::LoadU(d, &inv_mass[i]);

            // mask where inv_mass > 0
            const auto is_dyn = hn::Gt(v_im, zero);
            const auto v_m = hn::Div(one, hn::Max(v_im, hn::Set(d, 1e-12f)));
            const auto v2 = hn::MulAdd(v_vx, v_vx, hn::Mul(v_vy, v_vy));
            const auto ke_lanes = hn::Mul(hn::Mul(half, v_m), v2);
            sum_vec = hn::Add(sum_vec, hn::IfThenElseZero(is_dyn, ke_lanes));
        }

        Scalar total = hn::ReduceSum(d, sum_vec);
        for (; i < n; ++i) {
            if (inv_mass[i] > Scalar(0)) {
                const Scalar m = Scalar(1) / inv_mass[i];
                total += Scalar(0.5) * m * (vx[i] * vx[i] + vy[i] * vy[i]);
            }
        }
        return total;
    }
};

static_assert(ComputeBackend<HighwayBackend>);

} // namespace prakriti

#endif // __has_include(<hwy/highway.h>)
