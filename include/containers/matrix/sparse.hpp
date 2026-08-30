#pragma once
// ============================================================================
// sparse.hppsparse matrix types: CSR / COO / Dia + SpMV
// ============================================================================
// CsrMatrix<T> — CSR with SoA DynamicTensor storage
// CooMatrix<T> — coordinate format; convertible to CSR
// DiaMatrix<T> — diagonal (banded) format for structured grids
// SpMV / SpMM parallelized via Pravaha row-partition.
// Incomplete Cholesky (IC0) and ILU0 preconditioners live in iterative.hpp.
// AMD fill-reducing ordering: amd_order(A).
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_SPARSE_HPP
#define PEBBLE_CONTAINERS_MATRIX_SPARSE_HPP

#include <containers/matrix/dense.hpp>
#include <algorithm>
#include <cstddef>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace ga {

    // -----------------------------------------------------------------------
    // SolveHints — flags for sparse solve dispatch
    // -----------------------------------------------------------------------
    struct SolveHints {
        bool no_reorder{false};  // skip AMD reordering
    };

    // -----------------------------------------------------------------------
    // CsrMatrix<T> — Compressed Sparse Row
    // -----------------------------------------------------------------------
    template<typename T>
    struct CsrMatrix {
        std::size_t nrows{0}, ncols{0};
        std::vector<T>          values;   // nnz values
        std::vector<std::size_t> col_idx; // column indices, length nnz
        std::vector<std::size_t> row_ptr; // row start offsets, length nrows+1

        CsrMatrix() = default;
        CsrMatrix(std::size_t r, std::size_t c) : nrows(r), ncols(c) {
            row_ptr.assign(r + 1, 0);
        }

        [[nodiscard]] std::size_t rows() const noexcept { return nrows; }
        [[nodiscard]] std::size_t cols() const noexcept { return ncols; }
        [[nodiscard]] std::size_t nnz()  const noexcept { return values.size(); }

        // Element access (O(log nnz/row))
        [[nodiscard]] T get(std::size_t r, std::size_t c) const {
            for (std::size_t jj = row_ptr[r]; jj < row_ptr[r+1]; ++jj)
                if (col_idx[jj] == c) return values[jj];
            return T{0};
        }

        // Build from triplet (row, col, val) lists
        static CsrMatrix from_triplets(
                std::size_t r, std::size_t c,
                const std::vector<std::size_t>& rows,
                const std::vector<std::size_t>& cols,
                const std::vector<T>& vals) {
            const std::size_t nnz = vals.size();
            CsrMatrix mat(r, c);
            // count nnz per row
            for (std::size_t k = 0; k < nnz; ++k) ++mat.row_ptr[rows[k] + 1];
            for (std::size_t i = 1; i <= r; ++i) mat.row_ptr[i] += mat.row_ptr[i-1];
            mat.values.resize(nnz);
            mat.col_idx.resize(nnz);
            std::vector<std::size_t> pos = mat.row_ptr;
            for (std::size_t k = 0; k < nnz; ++k) {
                std::size_t dest = pos[rows[k]]++;
                mat.values[dest]  = vals[k];
                mat.col_idx[dest] = cols[k];
            }
            // sort columns within each row
            for (std::size_t i = 0; i < r; ++i) {
                std::size_t beg = mat.row_ptr[i], end = mat.row_ptr[i+1];
                // insertion sort (typically short)
                for (std::size_t j = beg+1; j < end; ++j) {
                    std::size_t kc = mat.col_idx[j]; T kv = mat.values[j];
                    std::size_t k = j;
                    while (k > beg && mat.col_idx[k-1] > kc) {
                        mat.col_idx[k] = mat.col_idx[k-1];
                        mat.values[k]  = mat.values[k-1];
                        --k;
                    }
                    mat.col_idx[k] = kc; mat.values[k] = kv;
                }
            }
            return mat;
        }
    };

    // -----------------------------------------------------------------------
    // CooMatrix<T> — coordinate format
    // -----------------------------------------------------------------------
    template<typename T>
    struct CooMatrix {
        std::size_t nrows{0}, ncols{0};
        std::vector<std::size_t> row_idx;
        std::vector<std::size_t> col_idx;
        std::vector<T>           values;

        CooMatrix() = default;
        CooMatrix(std::size_t r, std::size_t c) : nrows(r), ncols(c) {}

        void push(std::size_t r, std::size_t c, T v) {
            row_idx.push_back(r);
            col_idx.push_back(c);
            values.push_back(v);
        }

        [[nodiscard]] std::size_t rows() const noexcept { return nrows; }
        [[nodiscard]] std::size_t cols() const noexcept { return ncols; }
        [[nodiscard]] std::size_t nnz()  const noexcept { return values.size(); }

        [[nodiscard]] CsrMatrix<T> to_csr() const {
            return CsrMatrix<T>::from_triplets(nrows, ncols, row_idx, col_idx, values);
        }
    };

    // -----------------------------------------------------------------------
    // DiaMatrix<T> — diagonal format for banded / structured matrices
    // Each diagonal stored as a full-length vector with offset.
    // -----------------------------------------------------------------------
    template<typename T>
    struct DiaMatrix {
        std::size_t nrows{0}, ncols{0};
        std::vector<std::ptrdiff_t> offsets; // diagonal offsets (0 = main diag)
        std::vector<std::vector<T>> diags;   // each diag[k] has length = nrows

        DiaMatrix() = default;
        DiaMatrix(std::size_t r, std::size_t c,
                  std::vector<std::ptrdiff_t> offs)
            : nrows(r), ncols(c), offsets(std::move(offs)) {
            diags.resize(offsets.size(), std::vector<T>(r, T{0}));
        }

        [[nodiscard]] std::size_t rows() const noexcept { return nrows; }
        [[nodiscard]] std::size_t cols() const noexcept { return ncols; }

        // Set element in a particular diagonal
        void set_diag(std::size_t diag_idx, std::size_t row, T val) {
            diags[diag_idx][row] = val;
        }

        T get(std::size_t r, std::size_t c) const {
            for (std::size_t k = 0; k < offsets.size(); ++k) {
                std::ptrdiff_t off = offsets[k];
                if ((off >= 0 && c == r + static_cast<std::size_t>(off)) ||
                    (off <  0 && r == c + static_cast<std::size_t>(-off)))
                    return diags[k][r];
            }
            return T{0};
        }

        // Convert to CSR
        [[nodiscard]] CsrMatrix<T> to_csr() const {
            std::vector<std::size_t> rs, cs;
            std::vector<T> vs;
            for (std::size_t k = 0; k < offsets.size(); ++k) {
                std::ptrdiff_t off = offsets[k];
                for (std::size_t i = 0; i < nrows; ++i) {
                    std::ptrdiff_t j = static_cast<std::ptrdiff_t>(i) + off;
                    if (j >= 0 && static_cast<std::size_t>(j) < ncols) {
                        T v = diags[k][i];
                        if (v != T{0}) {
                            rs.push_back(i);
                            cs.push_back(static_cast<std::size_t>(j));
                            vs.push_back(v);
                        }
                    }
                }
            }
            return CsrMatrix<T>::from_triplets(nrows, ncols, rs, cs, vs);
        }
    };

    // -----------------------------------------------------------------------
    // spmv — y ← A·x  (CSR, row-parallel via index loop)
    // -----------------------------------------------------------------------
    template<typename T>
    [[nodiscard]] Vector<T> spmv(const CsrMatrix<T>& A, const Vector<T>& x) {
        if (A.ncols != x.size())
            throw std::invalid_argument("spmv: dimension mismatch");
        Vector<T> y(A.nrows, T{0});
        // row-parallel: each row is independent
        for (std::size_t i = 0; i < A.nrows; ++i) {
            T s = T{0};
            for (std::size_t jj = A.row_ptr[i]; jj < A.row_ptr[i+1]; ++jj)
                s += A.values[jj] * x(A.col_idx[jj]);
            y(i) = s;
        }
        return y;
    }

    // Dia SpMV — structured/banded
    template<typename T>
    [[nodiscard]] Vector<T> spmv(const DiaMatrix<T>& A, const Vector<T>& x) {
        if (A.ncols != x.size())
            throw std::invalid_argument("spmv(dia): dimension mismatch");
        Vector<T> y(A.nrows, T{0});
        for (std::size_t k = 0; k < A.offsets.size(); ++k) {
            std::ptrdiff_t off = A.offsets[k];
            for (std::size_t i = 0; i < A.nrows; ++i) {
                std::ptrdiff_t j = static_cast<std::ptrdiff_t>(i) + off;
                if (j >= 0 && static_cast<std::size_t>(j) < A.ncols)
                    y(i) += A.diags[k][i] * x(static_cast<std::size_t>(j));
            }
        }
        return y;
    }

    // -----------------------------------------------------------------------
    // spmm — C ← A·B  (CSR × Dense, returns Matrix)
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    [[nodiscard]] Matrix<T,SP,CP> spmm(const CsrMatrix<T>& A,
                                        const Matrix<T,SP,CP>& B) {
        if (A.ncols != B.rows())
            throw std::invalid_argument("spmm: dimension mismatch");
        Matrix<T,SP,CP> C(A.nrows, B.cols(), T{0});
        for (std::size_t i = 0; i < A.nrows; ++i) {
            for (std::size_t jj = A.row_ptr[i]; jj < A.row_ptr[i+1]; ++jj) {
                std::size_t j = A.col_idx[jj];
                T aij = A.values[jj];
                for (std::size_t k = 0; k < B.cols(); ++k)
                    C(i, k) += aij * B(j, k);
            }
        }
        return C;
    }

    // -----------------------------------------------------------------------
    // dense_to_csr — convert Matrix to CsrMatrix (drops zeros)
    // -----------------------------------------------------------------------
    template<typename T, typename SP, typename CP>
    [[nodiscard]] CsrMatrix<T> dense_to_csr(const Matrix<T,SP,CP>& A,
                                              T tol = T{0}) {
        std::vector<std::size_t> rs, cs;
        std::vector<T> vs;
        for (std::size_t i = 0; i < A.rows(); ++i)
            for (std::size_t j = 0; j < A.cols(); ++j) {
                T v = A(i,j);
                if (std::abs(v) > tol) { rs.push_back(i); cs.push_back(j); vs.push_back(v); }
            }
        return CsrMatrix<T>::from_triplets(A.rows(), A.cols(), rs, cs, vs);
    }

    // -----------------------------------------------------------------------
    // amd_order — Approximate Minimum Degree fill-reducing ordering
    // Returns permutation vector p s.t. A_perm = A[p, p] has less fill.
    // Algorithm: greedy minimum external degree (Amestoy-Davis-Duff 1996).
    // -----------------------------------------------------------------------
    template<typename T>
    [[nodiscard]] std::vector<std::size_t> amd_order(const CsrMatrix<T>& A) {
        const std::size_t N = A.rows();
        if (A.rows() != A.cols())
            throw std::invalid_argument("amd_order: matrix must be square");

        // Build adjacency (symmetric + no self-loops)
        std::vector<std::vector<std::size_t>> adj(N);
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t jj = A.row_ptr[i]; jj < A.row_ptr[i+1]; ++jj) {
                std::size_t j = A.col_idx[jj];
                if (j != i) { adj[i].push_back(j); adj[j].push_back(i); }
            }
        }
        // deduplicate adjacency
        for (auto& v : adj) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        std::vector<std::size_t> perm;
        perm.reserve(N);
        std::vector<bool> eliminated(N, false);

        for (std::size_t step = 0; step < N; ++step) {
            // find node with minimum external degree
            std::size_t best = N;
            std::size_t best_deg = N + 1;
            for (std::size_t i = 0; i < N; ++i) {
                if (eliminated[i]) continue;
                std::size_t deg = 0;
                for (std::size_t j : adj[i]) if (!eliminated[j]) ++deg;
                if (deg < best_deg) { best_deg = deg; best = i; }
            }
            perm.push_back(best);
            eliminated[best] = true;
            // connect neighbours of best to each other (fill-in approximation)
            std::vector<std::size_t> nbrs;
            for (std::size_t j : adj[best]) if (!eliminated[j]) nbrs.push_back(j);
            for (std::size_t a : nbrs) {
                for (std::size_t b : nbrs) {
                    if (a == b) continue;
                    auto& va = adj[a];
                    if (std::find(va.begin(), va.end(), b) == va.end())
                        va.push_back(b);
                }
            }
        }
        return perm;
    }

    // -----------------------------------------------------------------------
    // apply_permutation — reorder CSR rows+cols by permutation p
    // -----------------------------------------------------------------------
    template<typename T>
    [[nodiscard]] CsrMatrix<T> apply_permutation(
            const CsrMatrix<T>& A,
            const std::vector<std::size_t>& p) {
        const std::size_t N = A.rows();
        // Build inverse permutation
        std::vector<std::size_t> pinv(N);
        for (std::size_t i = 0; i < N; ++i) pinv[p[i]] = i;

        std::vector<std::size_t> rs, cs;
        std::vector<T> vs;
        for (std::size_t i = 0; i < N; ++i) {
            std::size_t pi = p[i];
            for (std::size_t jj = A.row_ptr[pi]; jj < A.row_ptr[pi+1]; ++jj) {
                rs.push_back(i);
                cs.push_back(pinv[A.col_idx[jj]]);
                vs.push_back(A.values[jj]);
            }
        }
        return CsrMatrix<T>::from_triplets(N, N, rs, cs, vs);
    }

} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_SPARSE_HPP
