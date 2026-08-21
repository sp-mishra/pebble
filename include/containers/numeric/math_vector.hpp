#pragma once
// ============================================================================
// math_vector.hpp — High-Performance Game Math, Graphics & Vector Primitives
// ============================================================================
// C++23 / C++26, header-only, zero heap allocation, constexpr-enabled.
// Built on top of Pebble's static_tensor engine.
// Provides GLSL/HLSL style vector & matrix math (vec2, vec3, vec4, quat, mat4).
// ============================================================================

#ifndef PEBBLE_CONTAINERS_NUMERIC_MATH_VECTOR_HPP
#define PEBBLE_CONTAINERS_NUMERIC_MATH_VECTOR_HPP

#include <containers/tensor/tensor.hpp>
#include <cmath>
#include <cstdint>
#include <concepts>
#include <algorithm>

namespace pebble::math {

    // ========================================================================
    // 1. Vector & Matrix Type Aliases (Zero Heap Allocation / Stack Array)
    // ========================================================================
    using vec2 = ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, 2>;
    using vec3 = ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, 3>;
    using vec4 = ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, 4>;
    using quat = ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, 4>;
    using mat2 = ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, 2, 2>;
    using mat3 = ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, 3, 3>;
    using mat4 = ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, 4, 4>;

    // Double-precision variants
    using vec2d = ts::static_tensor<double, ts::default_storage_policy, ts::default_computation_policy, 2>;
    using vec3d = ts::static_tensor<double, ts::default_storage_policy, ts::default_computation_policy, 3>;
    using vec4d = ts::static_tensor<double, ts::default_storage_policy, ts::default_computation_policy, 4>;
    using mat4d = ts::static_tensor<double, ts::default_storage_policy, ts::default_computation_policy, 4, 4>;

    // Integer variants
    using vec2i = ts::static_tensor<int32_t, ts::default_storage_policy, ts::default_computation_policy, 2>;
    using vec3i = ts::static_tensor<int32_t, ts::default_storage_policy, ts::default_computation_policy, 3>;
    using vec4i = ts::static_tensor<int32_t, ts::default_storage_policy, ts::default_computation_policy, 4>;

    // ========================================================================
    // 2. Vector Arithmetic & Geometric Functions
    // ========================================================================

    // 2D Vector Cross Product (Scalar z-component)
    template<typename T>
    constexpr T cross(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, 2> &a,
                      const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, 2> &b) noexcept {
        return a[0] * b[1] - a[1] * b[0];
    }

    // 2D 90-degree Perpendicular Vector (counter-clockwise)
    template<typename T>
    constexpr auto perp(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, 2> &v) noexcept {
        return ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, 2>(-v[1], v[0]);
    }

    // 3D Vector Cross Product
    constexpr vec3 cross(const vec3 &a, const vec3 &b) noexcept {
        return vec3(
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]
        );
    }


    constexpr vec3d cross(const vec3d &a, const vec3d &b) noexcept {
        return vec3d(
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]
        );
    }

    // N-Dimensional Vector Dot Product
    template<typename T, size_t N>
    constexpr T dot(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &a,
                    const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &b) noexcept {
        T sum = T{0};
        for (size_t i = 0; i < N; ++i) {
            sum += a[i] * b[i];
        }
        return sum;
    }

    // Squared Euclidean Length / Norm
    template<typename T, size_t N>
    constexpr auto length_sq(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &v) noexcept {
        return dot(v, v);
    }

    // Euclidean Length / Norm
    template<typename T, size_t N>
    inline auto length(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &v) noexcept {
        return std::sqrt(length_sq(v));
    }

    // Vector Normalization
    template<typename T, size_t N>
    inline auto normalize(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &v) noexcept {
        auto l = length(v);
        auto res = v;
        if (l > static_cast<decltype(l)>(0)) {
            for (size_t i = 0; i < N; ++i) {
                res[i] = static_cast<T>(res[i] / l);
            }
        }
        return res;
    }

    // Distance between two points
    template<typename T, size_t N>
    inline auto distance(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &a,
                         const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &b) noexcept {
        ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> diff;
        for (size_t i = 0; i < N; ++i) {
            diff[i] = a[i] - b[i];
        }
        return length(diff);
    }

    // Linear Interpolation (lerp)
    template<typename T, size_t N, typename F>
    constexpr auto lerp(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &a,
                        const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &b,
                        F t) noexcept {
        ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> res;
        for (size_t i = 0; i < N; ++i) {
            res[i] = static_cast<T>(a[i] + t * (b[i] - a[i]));
        }
        return res;
    }

    // Projection of vector a onto vector b
    template<typename T, size_t N>
    constexpr auto project(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &a,
                           const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &b) noexcept {
        T b_sq = length_sq(b);
        ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> res{};
        if (b_sq > T{0}) {
            T scale = dot(a, b) / b_sq;
            for (size_t i = 0; i < N; ++i) {
                res[i] = b[i] * scale;
            }
        }
        return res;
    }

    // Rejection of vector a from vector b (perpendicular component)
    template<typename T, size_t N>
    constexpr auto reject(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &a,
                          const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &b) noexcept {
        auto proj = project(a, b);
        ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> res{};
        for (size_t i = 0; i < N; ++i) {
            res[i] = a[i] - proj[i];
        }
        return res;
    }

    // Angle between two vectors (in radians)
    template<typename T, size_t N>
    inline double angle(const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &a,
                        const ts::static_tensor<T, ts::default_storage_policy, ts::default_computation_policy, N> &b) noexcept {
        auto denom = std::sqrt(length_sq(a) * length_sq(b));
        if (denom <= static_cast<decltype(denom)>(0)) return 0.0;
        auto cos_theta = std::clamp(static_cast<double>(dot(a, b)) / denom, -1.0, 1.0);
        return std::acos(cos_theta);
    }

    // Reflection vector: I - 2.0 * dot(N, I) * N
    template<size_t N>
    inline auto reflect(const ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, N> &I,
                        const ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, N> &Nvec) noexcept {
        float d = dot(Nvec, I);
        ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, N> res{};
        for (size_t i = 0; i < N; ++i) {
            res[i] = I[i] - 2.0f * d * Nvec[i];
        }
        return res;
    }

    // Refraction vector (Snell's law)
    template<size_t N>
    inline auto refract(const ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, N> &I,
                        const ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, N> &Nvec,
                        float eta) noexcept {
        float d = dot(Nvec, I);
        float k = 1.0f - eta * eta * (1.0f - d * d);
        ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, N> res{};
        if (k >= 0.0f) {
            for (size_t i = 0; i < N; ++i) {
                res[i] = eta * I[i] - (eta * d + std::sqrt(k)) * Nvec[i];
            }
        }
        return res;
    }

    // 2D 2x2 Determinant
    constexpr float determinant(const mat2 &m) noexcept {
        return m[0, 0] * m[1, 1] - m[0, 1] * m[1, 0];
    }

    // 2D 2x2 Matrix-Vector Multiplication (y = M * x)
    constexpr vec2 mul(const mat2 &m, const vec2 &v) noexcept {
        return vec2(
            m[0, 0] * v[0] + m[0, 1] * v[1],
            m[1, 0] * v[0] + m[1, 1] * v[1]
        );
    }

    // 2D 2x2 Matrix Multiplication
    constexpr mat2 mul(const mat2 &a, const mat2 &b) noexcept {
        return mat2(
            a[0, 0] * b[0, 0] + a[0, 1] * b[1, 0], a[0, 0] * b[0, 1] + a[0, 1] * b[1, 1],
            a[1, 0] * b[0, 0] + a[1, 1] * b[1, 0], a[1, 0] * b[0, 1] + a[1, 1] * b[1, 1]
        );
    }

    // 2D 2x2 Matrix Transpose
    constexpr mat2 transpose(const mat2 &m) noexcept {
        return mat2(m[0, 0], m[1, 0], m[0, 1], m[1, 1]);
    }

    // 2D 2x2 Matrix Inverse
    constexpr mat2 inverse(const mat2 &m) noexcept {
        float d = determinant(m);
        if (d > -1e-12f && d < 1e-12f) return mat2(1.0f, 0.0f, 0.0f, 1.0f);
        float inv_d = 1.0f / d;
        return mat2(
             m[1, 1] * inv_d, -m[0, 1] * inv_d,
            -m[1, 0] * inv_d,  m[0, 0] * inv_d
        );
    }

    // 2D 2x2 Rotation Matrix (radians)
    inline mat2 rotation2d(float radians) noexcept {
        float c = std::cos(radians), s = std::sin(radians);
        return mat2(c, -s, s, c);
    }

    // 2D 2x2 Scaling Matrix
    constexpr mat2 scaling2d(float sx, float sy) noexcept {
        return mat2(sx, 0.0f, 0.0f, sy);
    }

    // ========================================================================
    // 2D & 3D Axis-Aligned Bounding Box (AABB)
    // ========================================================================
    struct aabb2 {
        vec2 lo{};
        vec2 hi{};

        constexpr aabb2() noexcept = default;
        constexpr aabb2(vec2 lo_, vec2 hi_) noexcept : lo(lo_), hi(hi_) {}

        [[nodiscard]] constexpr vec2 center() const noexcept {
            return vec2((lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f);
        }
        [[nodiscard]] constexpr vec2 extent() const noexcept {
            return vec2(hi[0] - lo[0], hi[1] - lo[1]);
        }

        [[nodiscard]] constexpr bool contains(const vec2 &p) const noexcept {
            return p[0] >= lo[0] && p[0] <= hi[0] && p[1] >= lo[1] && p[1] <= hi[1];
        }
        template <class P>
        [[nodiscard]] constexpr bool contains(const P &p) const noexcept {
            if constexpr (requires { p.x; p.y; }) {
                return p.x >= lo[0] && p.x <= hi[0] && p.y >= lo[1] && p.y <= hi[1];
            } else {
                return p[0] >= lo[0] && p[0] <= hi[0] && p[1] >= lo[1] && p[1] <= hi[1];
            }
        }

        [[nodiscard]] constexpr bool overlaps(const aabb2 &o) const noexcept {
            return lo[0] <= o.hi[0] && hi[0] >= o.lo[0] && lo[1] <= o.hi[1] && hi[1] >= o.lo[1];
        }
        constexpr void expand(const vec2 &p) noexcept {
            lo[0] = std::min(lo[0], p[0]); lo[1] = std::min(lo[1], p[1]);
            hi[0] = std::max(hi[0], p[0]); hi[1] = std::max(hi[1], p[1]);
        }
        constexpr void expand(const aabb2 &o) noexcept { expand(o.lo); expand(o.hi); }
        [[nodiscard]] constexpr aabb2 fattened(float m) const noexcept {
            return aabb2(vec2(lo[0] - m, lo[1] - m), vec2(hi[0] + m, hi[1] + m));
        }
        [[nodiscard]] constexpr float area() const noexcept {
            const float dx = hi[0] - lo[0], dy = hi[1] - lo[1];
            return dx * dy;
        }
        [[nodiscard]] constexpr vec2 clamp(const vec2 &p) const noexcept {
            return vec2(std::clamp(p[0], lo[0], hi[0]), std::clamp(p[1], lo[1], hi[1]));
        }
        [[nodiscard]] static constexpr aabb2 merge(const aabb2 &a, const aabb2 &b) noexcept {
            return aabb2(vec2(std::min(a.lo[0], b.lo[0]), std::min(a.lo[1], b.lo[1])),
                         vec2(std::max(a.hi[0], b.hi[0]), std::max(a.hi[1], b.hi[1])));
        }

        friend constexpr bool operator==(const aabb2&, const aabb2&) = default;
    };

    // 3D Axis-Aligned Bounding Box (AABB)
    struct aabb3 {
        vec3 lo{};
        vec3 hi{};

        constexpr aabb3() noexcept = default;
        constexpr aabb3(vec3 lo_, vec3 hi_) noexcept : lo(lo_), hi(hi_) {}

        [[nodiscard]] constexpr vec3 center() const noexcept {
            return vec3((lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f);
        }
        [[nodiscard]] constexpr vec3 extent() const noexcept {
            return vec3(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
        }

        [[nodiscard]] constexpr bool contains(const vec3 &p) const noexcept {
            return p[0] >= lo[0] && p[0] <= hi[0] &&
                   p[1] >= lo[1] && p[1] <= hi[1] &&
                   p[2] >= lo[2] && p[2] <= hi[2];
        }
        [[nodiscard]] constexpr bool overlaps(const aabb3 &o) const noexcept {
            return lo[0] <= o.hi[0] && hi[0] >= o.lo[0] &&
                   lo[1] <= o.hi[1] && hi[1] >= o.lo[1] &&
                   lo[2] <= o.hi[2] && hi[2] >= o.lo[2];
        }
        constexpr void expand(const vec3 &p) noexcept {
            lo[0] = std::min(lo[0], p[0]); lo[1] = std::min(lo[1], p[1]); lo[2] = std::min(lo[2], p[2]);
            hi[0] = std::max(hi[0], p[0]); hi[1] = std::max(hi[1], p[1]); hi[2] = std::max(hi[2], p[2]);
        }
        constexpr void expand(const aabb3 &o) noexcept { expand(o.lo); expand(o.hi); }
        [[nodiscard]] constexpr aabb3 fattened(float m) const noexcept {
            return aabb3(vec3(lo[0] - m, lo[1] - m, lo[2] - m),
                         vec3(hi[0] + m, hi[1] + m, hi[2] + m));
        }
        [[nodiscard]] constexpr float area() const noexcept {
            const float dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
            return 2.0f * (dx * dy + dy * dz + dz * dx);
        }
        [[nodiscard]] constexpr vec3 clamp(const vec3 &p) const noexcept {
            return vec3(std::clamp(p[0], lo[0], hi[0]),
                        std::clamp(p[1], lo[1], hi[1]),
                        std::clamp(p[2], lo[2], hi[2]));
        }
        [[nodiscard]] static constexpr aabb3 merge(const aabb3 &a, const aabb3 &b) noexcept {
            return aabb3(vec3(std::min(a.lo[0], b.lo[0]), std::min(a.lo[1], b.lo[1]), std::min(a.lo[2], b.lo[2])),
                         vec3(std::max(a.hi[0], b.hi[0]), std::max(a.hi[1], b.hi[1]), std::max(a.hi[2], b.hi[2])));
        }

        friend constexpr bool operator==(const aabb3&, const aabb3&) = default;
    };




    // 3D 3x3 Determinant
    constexpr float determinant(const mat3 &m) noexcept {
        return m[0, 0] * (m[1, 1] * m[2, 2] - m[1, 2] * m[2, 1]) -
               m[0, 1] * (m[1, 0] * m[2, 2] - m[1, 2] * m[2, 0]) +
               m[0, 2] * (m[1, 0] * m[2, 1] - m[1, 1] * m[2, 0]);
    }

    // Matrix-Vector multiplication (y = M * x)
    constexpr vec4 mul(const mat4 &m, const vec4 &v) noexcept {
        return vec4(
            m[0, 0] * v[0] + m[0, 1] * v[1] + m[0, 2] * v[2] + m[0, 3] * v[3],
            m[1, 0] * v[0] + m[1, 1] * v[1] + m[1, 2] * v[2] + m[1, 3] * v[3],
            m[2, 0] * v[0] + m[2, 1] * v[1] + m[2, 2] * v[2] + m[2, 3] * v[3],
            m[3, 0] * v[0] + m[3, 1] * v[1] + m[3, 2] * v[2] + m[3, 3] * v[3]
        );
    }

    constexpr vec3 mul_point(const mat4 &m, const vec3 &p) noexcept {
        vec4 res = mul(m, vec4(p[0], p[1], p[2], 1.0f));
        float inv_w = res[3] != 0.0f ? 1.0f / res[3] : 1.0f;
        return vec3(res[0] * inv_w, res[1] * inv_w, res[2] * inv_w);
    }

    constexpr vec3 mul_direction(const mat4 &m, const vec3 &d) noexcept {
        vec4 res = mul(m, vec4(d[0], d[1], d[2], 0.0f));
        return vec3(res[0], res[1], res[2]);
    }

    // 4x4 Matrix Multiplication
    constexpr mat4 mul(const mat4 &a, const mat4 &b) noexcept {
        mat4 res{};
        for (size_t i = 0; i < 4; ++i) {
            for (size_t j = 0; j < 4; ++j) {
                float sum = 0.0f;
                for (size_t k = 0; k < 4; ++k) {
                    sum += a[i, k] * b[k, j];
                }
                res[i, j] = sum;
            }
        }
        return res;
    }

    // Identity Matrix
    constexpr mat4 identity4x4() noexcept {
        return mat4(
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        );
    }

    // Translation Matrix
    constexpr mat4 translation(const vec3 &t) noexcept {
        return mat4(
            1.0f, 0.0f, 0.0f, t[0],
            0.0f, 1.0f, 0.0f, t[1],
            0.0f, 0.0f, 1.0f, t[2],
            0.0f, 0.0f, 0.0f, 1.0f
        );
    }

    // Scale Matrix
    constexpr mat4 scaling(const vec3 &s) noexcept {
        return mat4(
            s[0], 0.0f, 0.0f, 0.0f,
            0.0f, s[1], 0.0f, 0.0f,
            0.0f, 0.0f, s[2], 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        );
    }

    // Camera Look-At Matrix
    inline mat4 look_at(const vec3 &eye, const vec3 &target, const vec3 &up) noexcept {
        vec3 f = normalize(vec3(target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]));
        vec3 s = normalize(cross(f, up));
        vec3 u = cross(s, f);

        return mat4(
             s[0],  s[1],  s[2], -dot(s, eye),
             u[0],  u[1],  u[2], -dot(u, eye),
            -f[0], -f[1], -f[2],  dot(f, eye),
             0.0f,  0.0f,  0.0f,  1.0f
        );
    }

    // Perspective Projection Matrix
    inline mat4 perspective(float fov_rad, float aspect, float z_near, float z_far) noexcept {
        float tan_half_fov = std::tan(fov_rad / 2.0f);
        mat4 res{};
        res[0, 0] = 1.0f / (aspect * tan_half_fov);
        res[1, 1] = 1.0f / tan_half_fov;
        res[2, 2] = -(z_far + z_near) / (z_far - z_near);
        res[2, 3] = -(2.0f * z_far * z_near) / (z_far - z_near);
        res[3, 2] = -1.0f;
        res[3, 3] = 0.0f;
        return res;
    }

    // ========================================================================
    // 3. Quaternion Operations (Orientation / Rotations)
    // ========================================================================
    // Quaternion layout: [x, y, z, w]

    constexpr quat quat_identity() noexcept {
        return quat(0.0f, 0.0f, 0.0f, 1.0f);
    }

    constexpr quat quat_conjugate(const quat &q) noexcept {
        return quat(-q[0], -q[1], -q[2], q[3]);
    }

    // Quaternion Multiplication (Hamilton product)
    constexpr quat quat_mul(const quat &a, const quat &b) noexcept {
        return quat(
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]
        );
    }

    // Quaternion from Axis-Angle
    inline quat quat_axis_angle(const vec3 &axis, float angle_rad) noexcept {
        vec3 norm_axis = normalize(axis);
        float half_angle = angle_rad * 0.5f;
        float sin_half = std::sin(half_angle);
        float cos_half = std::cos(half_angle);
        return quat(norm_axis[0] * sin_half, norm_axis[1] * sin_half, norm_axis[2] * sin_half, cos_half);
    }

    // Rotate Vector by Quaternion
    constexpr vec3 quat_rotate(const quat &q, const vec3 &v) noexcept {
        // v' = q * (v, 0) * q^-1
        quat q_v(v[0], v[1], v[2], 0.0f);
        quat q_res = quat_mul(quat_mul(q, q_v), quat_conjugate(q));
        return vec3(q_res[0], q_res[1], q_res[2]);
    }

    // Spherical Linear Interpolation (Slerp)
    inline quat quat_slerp(const quat &a, const quat &b, float t) noexcept {
        float cos_theta = dot(a, b);
        quat target = b;

        // If negative dot product, invert target to take shorter arc
        if (cos_theta < 0.0f) {
            cos_theta = -cos_theta;
            target = quat(-b[0], -b[1], -b[2], -b[3]);
        }

        if (cos_theta > 0.9995f) {
            // Linear interpolation for very close orientations
            return normalize(quat(
                a[0] + t * (target[0] - a[0]),
                a[1] + t * (target[1] - a[1]),
                a[2] + t * (target[2] - a[2]),
                a[3] + t * (target[3] - a[3])
            ));
        }

        float theta = std::acos(cos_theta);
        float sin_theta = std::sin(theta);
        float scale_a = std::sin((1.0f - t) * theta) / sin_theta;
        float scale_b = std::sin(t * theta) / sin_theta;

        return quat(
            scale_a * a[0] + scale_b * target[0],
            scale_a * a[1] + scale_b * target[1],
            scale_a * a[2] + scale_b * target[2],
            scale_a * a[3] + scale_b * target[3]
        );
    }

} // namespace pebble::math

namespace ts::math {
    using namespace pebble::math;
}

#endif // PEBBLE_CONTAINERS_NUMERIC_MATH_VECTOR_HPP
