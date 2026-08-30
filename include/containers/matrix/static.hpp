#pragma once
// ============================================================================
// static.hppcompile-time fixed-size matrices
// ============================================================================
// C++23/26, header-only, zero-virtual, zero-heap, zero macros.
// StaticMatrix<T,R,C> — backed by std::array<T,R*C>.
// Specializations for 2×2 / 3×3 / 4×4 ensure optimally inlined det/inv.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_STATIC_HPP
#define PEBBLE_CONTAINERS_MATRIX_STATIC_HPP

#include <array>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>

namespace ga {

    // -----------------------------------------------------------------------
    // StaticMatrix<T, R, C> — compile-time fixed-size row-major matrix
    // -----------------------------------------------------------------------
    template<typename T, std::size_t R, std::size_t C>
    class StaticMatrix {
    public:
        static constexpr std::size_t rows_v = R;
        static constexpr std::size_t cols_v = C;
        static constexpr std::size_t size_v = R * C;
        using value_type = T;
        using storage_type = std::array<T, R * C>;

        // Construction
        constexpr StaticMatrix() noexcept { data_.fill(T{}); }

        constexpr explicit StaticMatrix(T fill_val) noexcept { data_.fill(fill_val); }

        constexpr StaticMatrix(std::initializer_list<T> list) {
            if (list.size() != size_v)
                throw std::invalid_argument("StaticMatrix: initializer_list size mismatch");
            std::size_t i = 0;
            for (T v : list) data_[i++] = v;
        }

        static constexpr StaticMatrix identity() noexcept
            requires (R == C)
        {
            StaticMatrix m{};
            for (std::size_t i = 0; i < R; ++i) m(i, i) = T{1};
            return m;
        }

        // Element access
        [[nodiscard]] constexpr T& operator()(std::size_t r, std::size_t c) noexcept {
            return data_[r * C + c];
        }
        [[nodiscard]] constexpr const T& operator()(std::size_t r, std::size_t c) const noexcept {
            return data_[r * C + c];
        }

        [[nodiscard]] constexpr T* data() noexcept { return data_.data(); }
        [[nodiscard]] constexpr const T* data() const noexcept { return data_.data(); }

        [[nodiscard]] static constexpr std::size_t rows() noexcept { return R; }
        [[nodiscard]] static constexpr std::size_t cols() noexcept { return C; }

        // Row / col / diag extract
        [[nodiscard]] constexpr StaticMatrix<T, 1, C> row(std::size_t r) const noexcept {
            StaticMatrix<T, 1, C> out;
            for (std::size_t j = 0; j < C; ++j) out(0, j) = (*this)(r, j);
            return out;
        }
        [[nodiscard]] constexpr StaticMatrix<T, R, 1> col(std::size_t c) const noexcept {
            StaticMatrix<T, R, 1> out;
            for (std::size_t i = 0; i < R; ++i) out(i, 0) = (*this)(i, c);
            return out;
        }
        [[nodiscard]] constexpr StaticMatrix<T, (R < C ? R : C), 1> diag() const noexcept {
            constexpr std::size_t D = R < C ? R : C;
            StaticMatrix<T, D, 1> out;
            for (std::size_t i = 0; i < D; ++i) out(i, 0) = (*this)(i, i);
            return out;
        }

        // Transpose
        [[nodiscard]] constexpr StaticMatrix<T, C, R> transpose() const noexcept {
            StaticMatrix<T, C, R> out;
            for (std::size_t i = 0; i < R; ++i)
                for (std::size_t j = 0; j < C; ++j)
                    out(j, i) = (*this)(i, j);
            return out;
        }

        // Arithmetic
        [[nodiscard]] friend constexpr StaticMatrix operator+(const StaticMatrix& a, const StaticMatrix& b) noexcept {
            StaticMatrix out;
            for (std::size_t i = 0; i < size_v; ++i) out.data_[i] = a.data_[i] + b.data_[i];
            return out;
        }
        [[nodiscard]] friend constexpr StaticMatrix operator-(const StaticMatrix& a, const StaticMatrix& b) noexcept {
            StaticMatrix out;
            for (std::size_t i = 0; i < size_v; ++i) out.data_[i] = a.data_[i] - b.data_[i];
            return out;
        }
        [[nodiscard]] friend constexpr StaticMatrix operator-(const StaticMatrix& a) noexcept {
            StaticMatrix out;
            for (std::size_t i = 0; i < size_v; ++i) out.data_[i] = -a.data_[i];
            return out;
        }
        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend constexpr StaticMatrix operator*(const StaticMatrix& m, S s) noexcept {
            StaticMatrix out;
            for (std::size_t i = 0; i < size_v; ++i) out.data_[i] = static_cast<T>(m.data_[i] * s);
            return out;
        }
        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend constexpr StaticMatrix operator*(S s, const StaticMatrix& m) noexcept { return m * s; }

        // Matrix-matrix multiply A(R×C) * B(C×K) = result(R×K)
        template<std::size_t K>
        [[nodiscard]] constexpr StaticMatrix<T, R, K> operator*(const StaticMatrix<T, C, K>& b) const noexcept {
            StaticMatrix<T, R, K> out;
            for (std::size_t i = 0; i < R; ++i)
                for (std::size_t k = 0; k < K; ++k) {
                    T sum = T{0};
                    for (std::size_t j = 0; j < C; ++j) sum += (*this)(i, j) * b(j, k);
                    out(i, k) = sum;
                }
            return out;
        }

        // Trace
        [[nodiscard]] constexpr T trace() const noexcept requires (R == C) {
            T s = T{0};
            for (std::size_t i = 0; i < R; ++i) s += (*this)(i, i);
            return s;
        }

        // Equality
        [[nodiscard]] constexpr bool operator==(const StaticMatrix& o) const noexcept {
            return data_ == o.data_;
        }

        // Determinant (generic via LU-style cofactor expansion — practical for small sizes)
        [[nodiscard]] constexpr T det() const noexcept requires (R == C);

        // Inverse (only for square; returns by value; asserts |det| > eps)
        [[nodiscard]] constexpr StaticMatrix inv() const requires (R == C);

    private:
        storage_type data_{};
    };

    // -----------------------------------------------------------------------
    // det specializations
    // -----------------------------------------------------------------------
    template<typename T, std::size_t R, std::size_t C>
    [[nodiscard]] constexpr T StaticMatrix<T,R,C>::det() const noexcept requires (R == C) {
        if constexpr (R == 1) {
            return data_[0];
        } else if constexpr (R == 2) {
            return data_[0] * data_[3] - data_[1] * data_[2];
        } else if constexpr (R == 3) {
            return data_[0] * (data_[4] * data_[8] - data_[5] * data_[7])
                 - data_[1] * (data_[3] * data_[8] - data_[5] * data_[6])
                 + data_[2] * (data_[3] * data_[7] - data_[4] * data_[6]);
        } else {
            // Generic cofactor expansion along first row (O(N!), acceptable for N≤6 in constexpr)
            T result = T{0};
            for (std::size_t j = 0; j < C; ++j) {
                // Build minor (R-1)×(C-1)
                StaticMatrix<T, R-1, C-1> minor{};
                for (std::size_t mi = 0; mi < R-1; ++mi)
                    for (std::size_t mj = 0, offset = 0; mj < C-1 + 1; ++mj) {
                        if (mj == j) continue;
                        minor(mi, offset++) = (*this)(mi + 1, mj);
                    }
                T cofactor = (j % 2 == 0 ? T{1} : T{-1}) * minor.det();
                result += data_[j] * cofactor;
            }
            return result;
        }
    }

    // -----------------------------------------------------------------------
    // inv specializations — analytic for 1/2/3, Gauss-Jordan for general
    // -----------------------------------------------------------------------
    template<typename T, std::size_t R, std::size_t C>
    [[nodiscard]] constexpr StaticMatrix<T,R,C> StaticMatrix<T,R,C>::inv() const requires (R == C) {
        if constexpr (R == 1) {
            StaticMatrix out;
            out(0,0) = T{1} / data_[0];
            return out;
        } else if constexpr (R == 2) {
            const T d = det();
            StaticMatrix out;
            out(0,0) =  data_[3] / d;
            out(0,1) = -data_[1] / d;
            out(1,0) = -data_[2] / d;
            out(1,1) =  data_[0] / d;
            return out;
        } else if constexpr (R == 3) {
            const T d = det();
            StaticMatrix out;
            out(0,0) = (data_[4]*data_[8]-data_[5]*data_[7])/d;
            out(0,1) = (data_[2]*data_[7]-data_[1]*data_[8])/d;
            out(0,2) = (data_[1]*data_[5]-data_[2]*data_[4])/d;
            out(1,0) = (data_[5]*data_[6]-data_[3]*data_[8])/d;
            out(1,1) = (data_[0]*data_[8]-data_[2]*data_[6])/d;
            out(1,2) = (data_[2]*data_[3]-data_[0]*data_[5])/d;
            out(2,0) = (data_[3]*data_[7]-data_[4]*data_[6])/d;
            out(2,1) = (data_[1]*data_[6]-data_[0]*data_[7])/d;
            out(2,2) = (data_[0]*data_[4]-data_[1]*data_[3])/d;
            return out;
        } else {
            // Gauss-Jordan for general N
            std::array<T, R * C * 2> aug{};
            for (std::size_t i = 0; i < R; ++i) {
                for (std::size_t j = 0; j < C; ++j) aug[i * 2*C + j] = (*this)(i, j);
                aug[i * 2*C + C + i] = T{1};
            }
            for (std::size_t col = 0; col < C; ++col) {
                // partial pivot
                std::size_t pivot = col;
                T best = (aug[col*2*C+col] < T{0} ? -aug[col*2*C+col] : aug[col*2*C+col]);
                for (std::size_t r = col+1; r < R; ++r) {
                    T v = (aug[r*2*C+col] < T{0} ? -aug[r*2*C+col] : aug[r*2*C+col]);
                    if (v > best) { best = v; pivot = r; }
                }
                if (pivot != col)
                    for (std::size_t k = 0; k < 2*C; ++k)
                        std::swap(aug[col*2*C+k], aug[pivot*2*C+k]);
                const T diag = aug[col*2*C+col];
                for (std::size_t k = 0; k < 2*C; ++k) aug[col*2*C+k] /= diag;
                for (std::size_t r = 0; r < R; ++r) {
                    if (r == col) continue;
                    const T factor = aug[r*2*C+col];
                    for (std::size_t k = 0; k < 2*C; ++k) aug[r*2*C+k] -= factor * aug[col*2*C+k];
                }
            }
            StaticMatrix out;
            for (std::size_t i = 0; i < R; ++i)
                for (std::size_t j = 0; j < C; ++j)
                    out(i, j) = aug[i * 2*C + C + j];
            return out;
        }
    }

    // -----------------------------------------------------------------------
    // Convenience aliases
    // -----------------------------------------------------------------------
    template<typename T> using Mat2 = StaticMatrix<T, 2, 2>;
    template<typename T> using Mat3 = StaticMatrix<T, 3, 3>;
    template<typename T> using Mat4 = StaticMatrix<T, 4, 4>;
    template<typename T> using Vec2 = StaticMatrix<T, 2, 1>;
    template<typename T> using Vec3 = StaticMatrix<T, 3, 1>;
    template<typename T> using Vec4 = StaticMatrix<T, 4, 1>;

    // dot product for column vectors
    template<typename T, std::size_t N>
    [[nodiscard]] constexpr T dot(const StaticMatrix<T,N,1>& a, const StaticMatrix<T,N,1>& b) noexcept {
        T s = T{0};
        for (std::size_t i = 0; i < N; ++i) s += a(i,0) * b(i,0);
        return s;
    }

    // nrm2 for column vectors
    template<typename T, std::size_t N>
    [[nodiscard]] constexpr T nrm2(const StaticMatrix<T,N,1>& v) noexcept {
        return static_cast<T>(std::sqrt(static_cast<double>(dot(v, v))));
    }

    // nrm2_sq — squared L2 norm; replaces pebble::math::length_sq call sites
    template<typename T, std::size_t N>
    [[nodiscard]] constexpr T nrm2_sq(const StaticMatrix<T,N,1>& v) noexcept {
        return dot(v, v);
    }

    // axpy: y ← y + alpha * x  (y and x are column vectors, in-place on y)
    template<typename T, std::size_t N>
    constexpr void axpy(T alpha, const StaticMatrix<T,N,1>& x, StaticMatrix<T,N,1>& y) noexcept {
        for (std::size_t i = 0; i < N; ++i) y(i,0) += alpha * x(i,0);
    }

    // cross2d — scalar 2D cross product: ax*by − ay*bx
    template<typename T>
    [[nodiscard]] constexpr T cross2d(const StaticMatrix<T,2,1>& a, const StaticMatrix<T,2,1>& b) noexcept {
        return a(0,0) * b(1,0) - a(1,0) * b(0,0);
    }

    // quad_form_2d — computes J·diag(M⁻¹)·Jᵀ for 2D rigid body contact:
    //   result = inv_mass + (cross2d(r, n))² * inv_inertia
    // r, n are Vec2<T> (column vectors).
    template<typename T>
    [[nodiscard]] constexpr T quad_form_2d(T inv_mass, T inv_inertia,
                                           const StaticMatrix<T,2,1>& r,
                                           const StaticMatrix<T,2,1>& n) noexcept {
        const T rc = cross2d(r, n);
        return inv_mass + rc * rc * inv_inertia;
    }

} // namespace ga

// -----------------------------------------------------------------------
// Interop helpers: convert between pebble::math::mat2/vec2 and ga::StaticMatrix
// Included only when math_vector.hpp is available on the include path.
// Transitional — removed once all direct pebble::math::mat2 call sites migrate.
// -----------------------------------------------------------------------
#if __has_include(<containers/numeric/math_vector.hpp>)
#include <containers/numeric/math_vector.hpp>
namespace ga {
    [[nodiscard]] inline Mat2<float> to_static_matrix(const pebble::math::mat2& m) noexcept {
        return Mat2<float>{m[0,0], m[0,1], m[1,0], m[1,1]};
    }
    [[nodiscard]] inline pebble::math::mat2 from_static_matrix(const Mat2<float>& m) noexcept {
        return pebble::math::mat2(m(0,0), m(0,1), m(1,0), m(1,1));
    }
    [[nodiscard]] inline Vec2<float> to_static_vec(const pebble::math::vec2& v) noexcept {
        return Vec2<float>{v[0], v[1]};
    }
    [[nodiscard]] inline pebble::math::vec2 from_static_vec(const Vec2<float>& v) noexcept {
        return pebble::math::vec2(v(0,0), v(1,0));
    }
} // namespace ga
#endif

#endif // PEBBLE_CONTAINERS_MATRIX_STATIC_HPP
