#pragma once
// ============================================================================
// dense.hppdynamic dense Matrix / Vector / MatrixView
// ============================================================================
// Matrix<T,SP,CP> IS a rank-2 DynamicTensor<T,SP,CP>.
// All compute delegates to tensor BLAS primitives — no duplicate kernels.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_DENSE_HPP
#define PEBBLE_CONTAINERS_MATRIX_DENSE_HPP

#include <containers/tensor/tensor.hpp>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace ga {

    // -----------------------------------------------------------------------
    // MatrixView<T> — non-owning rank-2 slice (zero-copy)
    // -----------------------------------------------------------------------
    template<typename T>
    class MatrixView {
    public:
        MatrixView(T* data, std::size_t rows, std::size_t cols,
                   std::size_t row_stride, std::size_t col_stride = 1) noexcept
            : data_(data), rows_(rows), cols_(cols),
              row_stride_(row_stride), col_stride_(col_stride) {}

        [[nodiscard]] T& operator()(std::size_t r, std::size_t c) noexcept {
            return data_[r * row_stride_ + c * col_stride_];
        }
        [[nodiscard]] const T& operator()(std::size_t r, std::size_t c) const noexcept {
            return data_[r * row_stride_ + c * col_stride_];
        }
        [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
        [[nodiscard]] std::size_t cols() const noexcept { return cols_; }
        [[nodiscard]] T* data() noexcept { return data_; }
        [[nodiscard]] const T* data() const noexcept { return data_; }

    private:
        T* data_;
        std::size_t rows_, cols_, row_stride_, col_stride_;
    };

    // -----------------------------------------------------------------------
    // Matrix<T, SP, CP> — rank-2 wrapper over DynamicTensor
    // -----------------------------------------------------------------------
    template<typename T,
             typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    class Matrix {
    public:
        using tensor_type = ts::DynamicTensor<T, SP, CP>;
        using value_type  = T;
        using storage_policy = SP;
        using computation_policy = CP;

        // ---- Construction ---------------------------------------------------
        Matrix() : tensor_({0, 0}) {}
        Matrix(std::size_t rows, std::size_t cols)
            : tensor_(ts::TensorShape{rows, cols}) {}
        Matrix(std::size_t rows, std::size_t cols, T fill_val)
            : tensor_(ts::TensorShape{rows, cols}) {
            std::fill(tensor_.data(), tensor_.data() + rows * cols, fill_val);
        }

        // construct from existing tensor (must be rank-2)
        explicit Matrix(tensor_type t) : tensor_(std::move(t)) {
            if (tensor_.shape().size() != 2)
                throw std::invalid_argument("Matrix: tensor must be rank-2");
        }

        // zero-copy view into raw data
        static Matrix from_data(T* data, std::size_t rows, std::size_t cols) {
            Matrix m(rows, cols);
            std::copy(data, data + rows * cols, m.tensor_.data());
            return m;
        }

        // ---- Identity / zeros -----------------------------------------------
        static Matrix identity(std::size_t n) {
            Matrix m(n, n, T{0});
            for (std::size_t i = 0; i < n; ++i) m(i, i) = T{1};
            return m;
        }

        // ---- Element access --------------------------------------------------
        [[nodiscard]] T& operator()(std::size_t r, std::size_t c) noexcept {
            return tensor_.data()[r * cols() + c];
        }
        [[nodiscard]] const T& operator()(std::size_t r, std::size_t c) const noexcept {
            return tensor_.data()[r * cols() + c];
        }

        [[nodiscard]] std::size_t rows() const noexcept { return tensor_.shape()[0]; }
        [[nodiscard]] std::size_t cols() const noexcept { return tensor_.shape()[1]; }
        [[nodiscard]] T* data() noexcept { return tensor_.data(); }
        [[nodiscard]] const T* data() const noexcept { return tensor_.data(); }
        [[nodiscard]] tensor_type& tensor() noexcept { return tensor_; }
        [[nodiscard]] const tensor_type& tensor() const noexcept { return tensor_; }

        // ---- Views (zero-copy) -----------------------------------------------
        [[nodiscard]] MatrixView<T> row(std::size_t r) noexcept {
            return {data() + r * cols(), 1, cols(), cols(), 1};
        }
        [[nodiscard]] MatrixView<const T> row(std::size_t r) const noexcept {
            return {data() + r * cols(), 1, cols(), cols(), 1};
        }
        [[nodiscard]] MatrixView<T> col(std::size_t c) noexcept {
            return {data() + c, rows(), 1, cols(), cols()};
        }
        [[nodiscard]] MatrixView<const T> col(std::size_t c) const noexcept {
            return {data() + c, rows(), 1, cols(), cols()};
        }
        [[nodiscard]] MatrixView<T> block(std::size_t r0, std::size_t c0,
                                          std::size_t h, std::size_t w) noexcept {
            return {data() + r0 * cols() + c0, h, w, cols(), 1};
        }

        // transpose returns a new matrix (allocates)
        [[nodiscard]] Matrix transpose() const {
            Matrix out(cols(), rows());
            for (std::size_t i = 0; i < rows(); ++i)
                for (std::size_t j = 0; j < cols(); ++j)
                    out(j, i) = (*this)(i, j);
            return out;
        }

        // diagonal as a row vector (new allocation)
        [[nodiscard]] Matrix diag() const {
            const std::size_t d = std::min(rows(), cols());
            Matrix out(1, d);
            for (std::size_t i = 0; i < d; ++i) out(0, i) = (*this)(i, i);
            return out;
        }

        // ---- Reductions ------------------------------------------------------
        [[nodiscard]] T trace() const noexcept {
            const std::size_t d = std::min(rows(), cols());
            T s = T{0};
            for (std::size_t i = 0; i < d; ++i) s += (*this)(i, i);
            return s;
        }

        [[nodiscard]] T norm() const {
            return ts::nrm2<T, SP, CP>(tensor_);
        }

        // det via LU (see factorize.hpp — declared here as forward)
        [[nodiscard]] T det() const;

        // ---- BLAS wrappers --------------------------------------------------
        // A*B → new matrix
        [[nodiscard]] friend Matrix operator*(const Matrix& A, const Matrix& B) {
            if (A.cols() != B.rows())
                throw std::invalid_argument("Matrix::operator*: inner dimensions mismatch");
            auto C_tensor = CP::template matmul<T, SP, CP>(A.tensor_, B.tensor_);
            return Matrix(std::move(C_tensor));
        }

        [[nodiscard]] friend Matrix operator+(const Matrix& A, const Matrix& B) {
            if (A.rows() != B.rows() || A.cols() != B.cols())
                throw std::invalid_argument("Matrix::operator+: shape mismatch");
            Matrix out(A.rows(), A.cols());
            const std::size_t n = A.rows() * A.cols();
            for (std::size_t i = 0; i < n; ++i) out.data()[i] = A.data()[i] + B.data()[i];
            return out;
        }

        [[nodiscard]] friend Matrix operator-(const Matrix& A, const Matrix& B) {
            if (A.rows() != B.rows() || A.cols() != B.cols())
                throw std::invalid_argument("Matrix::operator-: shape mismatch");
            Matrix out(A.rows(), A.cols());
            const std::size_t n = A.rows() * A.cols();
            for (std::size_t i = 0; i < n; ++i) out.data()[i] = A.data()[i] - B.data()[i];
            return out;
        }

        template<typename S> requires std::is_arithmetic_v<S>
        [[nodiscard]] friend Matrix operator*(const Matrix& A, S s) {
            Matrix out(A.rows(), A.cols());
            const std::size_t n = A.rows() * A.cols();
            for (std::size_t i = 0; i < n; ++i) out.data()[i] = static_cast<T>(A.data()[i] * s);
            return out;
        }

        // ---- Interop --------------------------------------------------------
        // as_tensor: zero-copy access to the underlying tensor
        [[nodiscard]] tensor_type& as_tensor() noexcept { return tensor_; }
        [[nodiscard]] const tensor_type& as_tensor() const noexcept { return tensor_; }

        // from_tensor: construct a Matrix from a tensor (rank-2 required)
        static Matrix from_tensor(tensor_type t) { return Matrix(std::move(t)); }

    private:
        tensor_type tensor_;
    };

    // -----------------------------------------------------------------------
    // Vector<T> — rank-1 wrapper (thin alias with convenience methods)
    // -----------------------------------------------------------------------
    template<typename T,
             typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    class Vector {
    public:
        using tensor_type = ts::DynamicTensor<T, SP, CP>;
        using value_type  = T;

        Vector() : tensor_(ts::TensorShape{0}) {}
        explicit Vector(std::size_t n) : tensor_(ts::TensorShape{n}) {}
        Vector(std::size_t n, T fill_val) : tensor_(ts::TensorShape{n}) {
            std::fill(tensor_.data(), tensor_.data() + n, fill_val);
        }
        explicit Vector(tensor_type t) : tensor_(std::move(t)) {}

        [[nodiscard]] T& operator[](std::size_t i) noexcept { return tensor_.data()[i]; }
        [[nodiscard]] const T& operator[](std::size_t i) const noexcept { return tensor_.data()[i]; }
        [[nodiscard]] T& operator()(std::size_t i) noexcept { return tensor_.data()[i]; }
        [[nodiscard]] const T& operator()(std::size_t i) const noexcept { return tensor_.data()[i]; }
        [[nodiscard]] std::size_t size() const noexcept { return tensor_.shape()[0]; }
        [[nodiscard]] T* data() noexcept { return tensor_.data(); }
        [[nodiscard]] const T* data() const noexcept { return tensor_.data(); }
        [[nodiscard]] tensor_type& tensor() noexcept { return tensor_; }
        [[nodiscard]] const tensor_type& tensor() const noexcept { return tensor_; }

        [[nodiscard]] T norm() const { return ts::nrm2<T, SP, CP>(tensor_); }
        [[nodiscard]] T dot(const Vector& b) const {
            if (size() != b.size()) throw std::invalid_argument("Vector::dot: size mismatch");
            T s = T{0};
            const T* a = data();
            const T* bp = b.data();
            for (std::size_t i = 0; i < size(); ++i) s += a[i] * bp[i];
            return s;
        }

        friend Vector operator+(const Vector& a, const Vector& b) {
            if (a.size() != b.size()) throw std::invalid_argument("Vector+: size mismatch");
            Vector out(a.size());
            for (std::size_t i = 0; i < a.size(); ++i) out[i] = a[i] + b[i];
            return out;
        }
        friend Vector operator-(const Vector& a, const Vector& b) {
            if (a.size() != b.size()) throw std::invalid_argument("Vector-: size mismatch");
            Vector out(a.size());
            for (std::size_t i = 0; i < a.size(); ++i) out[i] = a[i] - b[i];
            return out;
        }
        template<typename S> requires std::is_arithmetic_v<S>
        friend Vector operator*(const Vector& v, S s) {
            Vector out(v.size());
            for (std::size_t i = 0; i < v.size(); ++i) out[i] = static_cast<T>(v[i] * s);
            return out;
        }
        template<typename S> requires std::is_arithmetic_v<S>
        friend Vector operator*(S s, const Vector& v) { return v * s; }

        // axpy: this ← α·x + this
        void axpy(T alpha, const Vector& x) {
            ts::axpy<T,SP,CP>(alpha, x.tensor_, tensor_);
        }

    private:
        tensor_type tensor_;
    };

    // -----------------------------------------------------------------------
    // Free BLAS wrappers for ga::Matrix and ga::Vector
    // -----------------------------------------------------------------------
    template<typename T, typename SP, typename CP>
    void gemm(T alpha, const Matrix<T,SP,CP>& A, const Matrix<T,SP,CP>& B,
              T beta, Matrix<T,SP,CP>& C) {
        ts::gemm<T,SP,CP>(alpha, A.as_tensor(), B.as_tensor(), beta, C.as_tensor());
    }

    template<typename T, typename SP, typename CP>
    void gemv(T alpha, const Matrix<T,SP,CP>& A, const Vector<T,SP,CP>& x,
              T beta, Vector<T,SP,CP>& y) {
        ts::gemv<T,SP,CP>(alpha, A.as_tensor(), x.tensor(), beta, y.tensor());
    }

    template<typename T, typename SP, typename CP>
    void axpy(T alpha, const Vector<T,SP,CP>& x, Vector<T,SP,CP>& y) {
        ts::axpy<T,SP,CP>(alpha, x.tensor(), y.tensor());
    }

    template<typename T, typename SP, typename CP>
    T nrm2(const Vector<T,SP,CP>& x) {
        return ts::nrm2<T,SP,CP>(x.tensor());
    }

    template<typename T, typename SP, typename CP>
    T dot(const Vector<T,SP,CP>& a, const Vector<T,SP,CP>& b) {
        return a.dot(b);
    }

    template<typename T, typename SP, typename CP>
    void syrk(T alpha, const Matrix<T,SP,CP>& A, T beta, Matrix<T,SP,CP>& C, bool upper = true) {
        ts::syrk<T,SP,CP>(alpha, A.as_tensor(), beta, C.as_tensor(), upper);
    }

} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_DENSE_HPP
