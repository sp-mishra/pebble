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

        // SIMD Vectorized SPH Poly6 Kernel evaluation across lanes
        // Computes W(r2, h) = coeff * (h^2 - r2)^3 for valid r2 < h^2
        static void simd_sph_poly6(const float* r2_lanes, float* out_w, float h, std::size_t count) noexcept {
            namespace hn = hwy::HWY_NAMESPACE;
            const hn::ScalableTag<float> d;
            const std::size_t N = hn::Lanes(d);

            const float h2 = h * h;
            const float h4 = h2 * h2;
            const float h8 = h4 * h4;
            const float coeff_val = 4.0f / (3.141592653589793f * h8);

            const auto vh2 = hn::Set(d, h2);
            const auto vcoeff = hn::Set(d, coeff_val);
            const auto vzero = hn::Zero(d);

            std::size_t i = 0;
            for (; i + N <= count; i += N) {
                const auto vr2 = hn::LoadU(d, &r2_lanes[i]);
                const auto mask = hn::Lt(vr2, vh2);
                const auto diff = hn::Max(hn::Sub(vh2, vr2), vzero);
                const auto diff2 = hn::Mul(diff, diff);
                const auto diff3 = hn::Mul(diff2, diff);
                const auto w = hn::Mul(vcoeff, diff3);
                const auto res = hn::IfThenElseZero(mask, w);
                hn::StoreU(res, d, &out_w[i]);
            }
            for (; i < count; ++i) {
                if (r2_lanes[i] < h2) {
                    float diff = h2 - r2_lanes[i];
                    out_w[i] = coeff_val * (diff * diff * diff);
                }
                else {
                    out_w[i] = 0.0f;
                }
            }
        }

        // SIMD Vectorized SPH Poly6 Kernel evaluation directly from dx and dy displacement vectors
        static void simd_sph_poly6_dxdy(const float* dx_lanes, const float* dy_lanes,
                                        float* out_w, float h, std::size_t count) noexcept {
            namespace hn = hwy::HWY_NAMESPACE;
            const hn::ScalableTag<float> d;
            const std::size_t N = hn::Lanes(d);

            const float h2 = h * h;
            const float h4 = h2 * h2;
            const float h8 = h4 * h4;
            const float coeff_val = 4.0f / (3.141592653589793f * h8);

            const auto vh2 = hn::Set(d, h2);
            const auto vcoeff = hn::Set(d, coeff_val);
            const auto vzero = hn::Zero(d);

            std::size_t i = 0;
            for (; i + N <= count; i += N) {
                const auto vdx = hn::LoadU(d, &dx_lanes[i]);
                const auto vdy = hn::LoadU(d, &dy_lanes[i]);
                const auto vr2 = hn::MulAdd(vdx, vdx, hn::Mul(vdy, vdy));
                const auto mask = hn::Lt(vr2, vh2);
                const auto diff = hn::Max(hn::Sub(vh2, vr2), vzero);
                const auto diff2 = hn::Mul(diff, diff);
                const auto diff3 = hn::Mul(diff2, diff);
                const auto w = hn::Mul(vcoeff, diff3);
                const auto res = hn::IfThenElseZero(mask, w);
                hn::StoreU(res, d, &out_w[i]);
            }
            for (; i < count; ++i) {
                const float r2 = dx_lanes[i] * dx_lanes[i] + dy_lanes[i] * dy_lanes[i];
                if (r2 < h2) {
                    float diff = h2 - r2;
                    out_w[i] = coeff_val * (diff * diff * diff);
                }
                else {
                    out_w[i] = 0.0f;
                }
            }
        }

        // SIMD Vectorized SPH Spiky Gradient Kernel evaluation
        // Computes ∇W(r, h) = -10 / (pi * h^5) * (h - r)^3 * (r_vec / r)
        static void simd_sph_spiky_grad(const float* dx_lanes, const float* dy_lanes,
                                        float* out_gx, float* out_gy,
                                        float h, std::size_t count) noexcept {
            namespace hn = hwy::HWY_NAMESPACE;
            const hn::ScalableTag<float> d;
            const std::size_t N = hn::Lanes(d);

            const float h2 = h * h;
            const float h5 = h2 * h2 * h;
            const float coeff_val = -10.0f / (3.141592653589793f * h5);

            const auto vh = hn::Set(d, h);
            const auto vh2 = hn::Set(d, h2);
            const auto vcoeff = hn::Set(d, coeff_val);
            const auto veps = hn::Set(d, 1e-6f);

            std::size_t i = 0;
            for (; i + N <= count; i += N) {
                const auto vdx = hn::LoadU(d, &dx_lanes[i]);
                const auto vdy = hn::LoadU(d, &dy_lanes[i]);
                const auto vr2 = hn::MulAdd(vdx, vdx, hn::Mul(vdy, vdy));
                const auto mask = hn::Lt(vr2, vh2);

                const auto vr = hn::Sqrt(vr2);
                const auto diff = hn::Max(hn::Sub(vh, vr), hn::Zero(d));
                const auto diff3 = hn::Mul(diff, hn::Mul(diff, diff));
                const auto inv_r = hn::Div(hn::Set(d, 1.0f), hn::Max(vr, veps));
                const auto scale = hn::Mul(vcoeff, hn::Mul(diff3, inv_r));

                const auto gx = hn::IfThenElseZero(mask, hn::Mul(vdx, scale));
                const auto gy = hn::IfThenElseZero(mask, hn::Mul(vdy, scale));

                hn::StoreU(gx, d, &out_gx[i]);
                hn::StoreU(gy, d, &out_gy[i]);
            }
            for (; i < count; ++i) {
                const float r2 = dx_lanes[i] * dx_lanes[i] + dy_lanes[i] * dy_lanes[i];
                if (r2 < h2 && r2 > 1e-12f) {
                    const float r = std::sqrt(r2);
                    const float diff = h - r;
                    const float factor = coeff_val * (diff * diff * diff) / r;
                    out_gx[i] = dx_lanes[i] * factor;
                    out_gy[i] = dy_lanes[i] * factor;
                }
                else {
                    out_gx[i] = 0.0f;
                    out_gy[i] = 0.0f;
                }
            }
        }
    };

    static_assert(ComputeBackend<HighwayBackend>);
} // namespace prakriti

#endif // __has_include(<hwy/highway.h>)
