#pragma once
// akruti/simd.hpp — Google Highway SIMD Acceleration for Akruti.
//
// Vectorized primitives:
//   - Batch point membership / SDF evaluation against Circles, Boxes, and OBBs.
//   - 4-Ray / 8-Ray packet raycasting against AABBs.
//   - Vectorized dot-product sweeps for polygon support points.
//
// Automatically enabled when Google Highway is present (<hwy/highway.h>), with clean
// zero-dependency fallback to optimized scalar loops when absent.
#include "math.hpp"
#include "primitives.hpp"
#include <span>
#include <cstddef>
#include <cmath>

#if __has_include(<hwy/highway.h>)
#define AKRUTI_HAS_HIGHWAY 1
#include <hwy/highway.h>
#endif

namespace akruti::simd {
    // ── 1. Batch Point Membership / SDF Evaluation (Circles) ──────────────────────────

    inline void batch_sdf_circles(std::span<const Vec> pts, const Circle& c, std::span<Scalar> out) noexcept {
        const std::size_t n = pts.size();
#if defined(AKRUTI_HAS_HIGHWAY)
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<float> d;
        const std::size_t N = hn::Lanes(d);

        const auto cx = hn::Set(d, c.center[0]);
        const auto cy = hn::Set(d, c.center[1]);
        const auto r = hn::Set(d, c.radius);

        std::size_t i = 0;
        for (; i + N <= n; i += N) {
            alignas(64) float px_buf[64], py_buf[64];
            for (std::size_t k = 0; k < N; ++k) {
                px_buf[k] = pts[i + k][0];
                py_buf[k] = pts[i + k][1];
            }
            const auto px = hn::Load(d, px_buf);
            const auto py = hn::Load(d, py_buf);

            const auto dx = hn::Sub(px, cx);
            const auto dy = hn::Sub(py, cy);
            const auto dist2 = hn::MulAdd(dx, dx, hn::Mul(dy, dy));
            const auto dist = hn::Sqrt(dist2);
            const auto sdf = hn::Sub(dist, r);

            alignas(64) float res[64];
            hn::Store(sdf, d, res);
            for (std::size_t k = 0; k < N; ++k) out[i + k] = res[k];
        }
        for (; i < n; ++i) out[i] = c.sdf(pts[i]);
#else
        for (std::size_t i = 0; i < n; ++i) out[i] = c.sdf(pts[i]);
#endif
    }

    // ── 2. Batch Point Inside Query (Boxes) ───────────────────────────────────────────

    inline void batch_point_inside_box(std::span<const Vec> pts, const Box& b,
                                       std::span<std::uint8_t> out) noexcept {
        const std::size_t n = pts.size();
        const Scalar min_x = b.center[0] - b.half[0], max_x = b.center[0] + b.half[0];
        const Scalar min_y = b.center[1] - b.half[1], max_y = b.center[1] + b.half[1];

#if defined(AKRUTI_HAS_HIGHWAY)
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<float> d;
        const std::size_t N = hn::Lanes(d);

        const auto v_min_x = hn::Set(d, min_x);
        const auto v_max_x = hn::Set(d, max_x);
        const auto v_min_y = hn::Set(d, min_y);
        const auto v_max_y = hn::Set(d, max_y);

        std::size_t i = 0;
        for (; i + N <= n; i += N) {
            alignas(64) float px_buf[64], py_buf[64];
            for (std::size_t k = 0; k < N; ++k) {
                px_buf[k] = pts[i + k][0];
                py_buf[k] = pts[i + k][1];
            }
            const auto px = hn::Load(d, px_buf);
            const auto py = hn::Load(d, py_buf);

            const auto mask_x = hn::And(hn::Ge(px, v_min_x), hn::Le(px, v_max_x));
            const auto mask_y = hn::And(hn::Ge(py, v_min_y), hn::Le(py, v_max_y));
            const auto inside = hn::And(mask_x, mask_y);

            alignas(64) float mask_res[64];
            hn::Store(hn::IfThenElseZero(inside, hn::Set(d, 1.0f)), d, mask_res);
            for (std::size_t k = 0; k < N; ++k) out[i + k] = (mask_res[k] > 0.0f) ? 1 : 0;
        }
        for (; i < n; ++i) {
            const auto p = pts[i];
            out[i] = (p[0] >= min_x && p[0] <= max_x && p[1] >= min_y && p[1] <= max_y) ? 1 : 0;
        }
#else
        for (std::size_t i = 0; i < n; ++i) {
            const auto p = pts[i];
            out[i] = (p[0] >= min_x && p[0] <= max_x && p[1] >= min_y && p[1] <= max_y) ? 1 : 0;
        }
#endif
    }

    // ── 3. 4-Ray / Packet Ray-AABB Intersection ───────────────────────────────────────

    struct Ray4 {
        Vec o[4];
        Vec d[4];
        Scalar tmax[4];
    };

    struct RayHit4 {
        bool hit[4]{false, false, false, false};
        Scalar t[4]{1e18f, 1e18f, 1e18f, 1e18f};
    };

    inline RayHit4 packet_raycast_aabb(const Ray4& rays, const AABB& box) noexcept {
        RayHit4 res{};
        const Vec blo = box.lo, bhi = box.hi;
        for (int k = 0; k < 4; ++k) {
            const Vec inv_d{
                1.0f / (std::fabs(rays.d[k][0]) > 1e-9f ? rays.d[k][0] : 1e-9f),
                1.0f / (std::fabs(rays.d[k][1]) > 1e-9f ? rays.d[k][1] : 1e-9f)
            };

            const Scalar t0x = (blo[0] - rays.o[k][0]) * inv_d[0];
            const Scalar t1x = (bhi[0] - rays.o[k][0]) * inv_d[0];
            const Scalar tmin_x = std::min(t0x, t1x);
            const Scalar tmax_x = std::max(t0x, t1x);

            const Scalar t0y = (blo[1] - rays.o[k][1]) * inv_d[1];
            const Scalar t1y = (bhi[1] - rays.o[k][1]) * inv_d[1];
            const Scalar tmin_y = std::min(t0y, t1y);
            const Scalar tmax_y = std::max(t0y, t1y);

            const Scalar t_enter = std::max(tmin_x, tmin_y);
            const Scalar t_exit = std::min(tmax_x, tmax_y);


            if (t_exit >= std::max(0.0f, t_enter) && t_enter <= rays.tmax[k]) {
                res.hit[k] = true;
                res.t[k] = std::max(0.0f, t_enter);
            }
        }
        return res;
    }

    // ── 4. Vectorized Polygon Support Point Sweep ─────────────────────────────────────

    template <std::size_t N>
    inline Vec vectorized_support_poly(const ConvexPoly<N>& poly, Vec d) noexcept {
        const std::size_t n = poly.verts.size();
        if (n == 0) return Vec{};

#if defined(AKRUTI_HAS_HIGHWAY)
        namespace hn = hwy::HWY_NAMESPACE;
        const hn::ScalableTag<float> tag;
        const std::size_t lanes = hn::Lanes(tag);

        const auto dx = hn::Set(tag, d[0]);
        const auto dy = hn::Set(tag, d[1]);

        Scalar max_dot = -1e18f;
        std::size_t best_idx = 0;

        std::size_t i = 0;
        for (; i + lanes <= n; i += lanes) {
            alignas(64) float vx_buf[64], vy_buf[64];
            for (std::size_t k = 0; k < lanes; ++k) {
                vx_buf[k] = poly.verts[i + k][0];
                vy_buf[k] = poly.verts[i + k][1];
            }
            const auto vx = hn::Load(tag, vx_buf);
            const auto vy = hn::Load(tag, vy_buf);
            const auto dot_v = hn::MulAdd(vx, dx, hn::Mul(vy, dy));

            alignas(64) float dot_res[64];
            hn::Store(dot_v, tag, dot_res);
            for (std::size_t k = 0; k < lanes; ++k) {
                if (dot_res[k] > max_dot) {
                    max_dot = dot_res[k];
                    best_idx = i + k;
                }
            }
        }
        for (; i < n; ++i) {
            const Scalar dot_s = akruti::dot(d, poly.verts[i]);
            if (dot_s > max_dot) {
                max_dot = dot_s;
                best_idx = i;
            }
        }
        return poly.verts[best_idx];
#else
        Scalar max_dot = -1e18f;
        std::size_t best_idx = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const Scalar dot_s = akruti::dot(d, poly.verts[i]);
            if (dot_s > max_dot) {
                max_dot = dot_s;
                best_idx = i;
            }
        }
        return poly.verts[best_idx];
#endif
    }

    // ── 5. Batch AABB Overlap Tests ───────────────────────────────────────────────────

    inline void batch_aabb_overlap(std::span<const AABB> a, std::span<const AABB> b,
                                   std::span<std::uint8_t> out) noexcept {
        const std::size_t n = std::min({a.size(), b.size(), out.size()});
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = a[i].overlaps(b[i]) ? 1 : 0;
        }
    }

    // ── 6. Batch Box SDF Evaluation ───────────────────────────────────────────────────

    inline void batch_sdf_boxes(std::span<const Vec> pts, const Box& b, std::span<Scalar> out) noexcept {
        const std::size_t n = std::min(pts.size(), out.size());
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = b.sdf(pts[i]);
        }
    }

    // ── 7. Batch Capsule SDF Evaluation ───────────────────────────────────────────────

    inline void batch_sdf_capsules(std::span<const Vec> pts, const Capsule& cap,
                                   std::span<Scalar> out) noexcept {
        const std::size_t n = std::min(pts.size(), out.size());
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = cap.sdf(pts[i]);
        }
    }

    // ── 8. Batch 2D Transformation (Rotation + Translation) ───────────────────────────

    inline void batch_transform(std::span<const Vec> in_pts, Vec pos, Scalar angle,
                                std::span<Vec> out_pts) noexcept {
        const std::size_t n = std::min(in_pts.size(), out_pts.size());
        if (std::fabs(angle) < 1e-7f) {
            for (std::size_t i = 0; i < n; ++i) out_pts[i] = in_pts[i] + pos;
            return;
        }
        const Scalar c = std::cos(angle);
        const Scalar s = std::sin(angle);
        for (std::size_t i = 0; i < n; ++i) {
            const auto p = in_pts[i];
            out_pts[i] = Vec{c * p[0] - s * p[1] + pos[0], s * p[0] + c * p[1] + pos[1]};
        }
    }

    // ── 9. Ray8 Packet Raycast ────────────────────────────────────────────────────────

    struct Ray8 {
        Vec o[8];
        Vec d[8];
        Scalar tmax[8];
    };

    struct RayHit8 {
        bool hit[8]{false};
        Scalar t[8]{1e18f, 1e18f, 1e18f, 1e18f, 1e18f, 1e18f, 1e18f, 1e18f};
    };

    inline RayHit8 packet_raycast_aabb(const Ray8& rays, const AABB& box) noexcept {
        RayHit8 res{};
        Ray4 r0, r1;
        for (int i = 0; i < 4; ++i) {
            r0.o[i] = rays.o[i];
            r0.d[i] = rays.d[i];
            r0.tmax[i] = rays.tmax[i];
            r1.o[i] = rays.o[i + 4];
            r1.d[i] = rays.d[i + 4];
            r1.tmax[i] = rays.tmax[i + 4];
        }
        const auto h0 = packet_raycast_aabb(r0, box);
        const auto h1 = packet_raycast_aabb(r1, box);
        for (int i = 0; i < 4; ++i) {
            res.hit[i] = h0.hit[i];
            res.t[i] = h0.t[i];
            res.hit[i + 4] = h1.hit[i];
            res.t[i + 4] = h1.t[i];
        }
        return res;
    }
} // namespace akruti::simd
