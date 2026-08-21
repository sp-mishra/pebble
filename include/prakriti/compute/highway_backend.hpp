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
};

static_assert(ComputeBackend<HighwayBackend>);

} // namespace prakriti

#endif // __has_include(<hwy/highway.h>)
