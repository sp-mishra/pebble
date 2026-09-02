#pragma once
// ============================================================================
// mat.hppga::Mat<T,CP,SP,AP> unified adaptive matrix front-end
// ============================================================================
// ga::Mat<T, CP, SP, AP> — std::variant<StaticStorage, DenseStorage,
//                           SparseStorage, DiaStorage> with stable public API.
// Construction dispatches through MatSelectionPolicy (compile-time).
// MatAdaptorPolicy (runtime hook) — NoAdaptorPolicy by default.
// maybe_adapt() is explicit + [[nodiscard]] — never adapts silently.
// ga::Mat<T,CP> with NoAdaptorPolicy + Dense is zero-overhead vs Matrix<T,CP>.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_MAT_HPP
#define PEBBLE_CONTAINERS_MATRIX_MAT_HPP

#include <containers/matrix/dense.hpp>
#include <containers/matrix/static.hpp>
#include <containers/matrix/sparse.hpp>
#include <containers/matrix/mat_info.hpp>
#include <cassert>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace ga {
    // -----------------------------------------------------------------------
    // Tag types for forced construction
    // -----------------------------------------------------------------------
    struct dense_tag {};

    struct sparse_tag {};

    struct banded_tag {
        std::size_t bandwidth{1};
    };

    struct static_tag {};

    // -----------------------------------------------------------------------
    // MatSelectionPolicy concept
    // -----------------------------------------------------------------------
    template <typename P>
    concept MatSelectionPolicy = requires(std::size_t r, std::size_t c, float density_hint) {
        { P::select(r, c, density_hint) } -> std::same_as<ga::MatKind>;
    };

    // -----------------------------------------------------------------------
    // Thresholds — configurable per translation unit
    // -----------------------------------------------------------------------
    struct Thresholds {
        std::size_t static_max{16}; // R*C <= this → Static
        float dense_min_density{0.1f}; // density > this → Dense
    };

    // -----------------------------------------------------------------------
    // SelectionPolicy<Thresholds> — default policy with tunable thresholds
    // -----------------------------------------------------------------------
    template <Thresholds TH = Thresholds{}>
    struct SelectionPolicy {
        static constexpr MatKind select(std::size_t r, std::size_t c,
                                        float density_hint) noexcept {
            if (r * c <= TH.static_max) return MatKind::Static;
            if (density_hint > TH.dense_min_density) return MatKind::Dense;
            return MatKind::Sparse;
        }
    };

    using DefaultSelectionPolicy = SelectionPolicy<>;

    // -----------------------------------------------------------------------
    // MatAdaptorPolicy concept
    // -----------------------------------------------------------------------
    template <typename AP, typename T>
    concept MatAdaptorPolicy = requires(const MatUsageStats& stats) {
        { AP::should_convert(stats) } -> std::same_as<std::optional<MatKind>>;
    };

    // -----------------------------------------------------------------------
    // NoAdaptorPolicy — zero overhead, no stats collected
    // -----------------------------------------------------------------------
    struct NoAdaptorPolicy {
        static std::optional<MatKind> should_convert(const MatUsageStats&) noexcept {
            return std::nullopt;
        }
    };

    // -----------------------------------------------------------------------
    // DensityAdaptorPolicy — convert sparse→dense when fill > threshold
    // -----------------------------------------------------------------------
    struct DensityAdaptorPolicy {
        float dense_threshold{0.1f};
        float sparse_threshold{0.05f};

        static std::optional<MatKind> should_convert(const MatUsageStats& stats) noexcept {
            if (stats.observed_density > 0.1f) return MatKind::Dense;
            if (stats.observed_density < 0.05f) return MatKind::Sparse;
            return std::nullopt;
        }
    };

    // -----------------------------------------------------------------------
    // ga::Mat<T, CP, SP, AP> — unified matrix type
    // -----------------------------------------------------------------------
    template <typename T,
              typename CP = ts::DefaultComputationPolicy,
              typename SP = ts::DefaultStoragePolicy,
              typename SelectPolicy = DefaultSelectionPolicy,
              typename AdaptPolicy = NoAdaptorPolicy>
    class Mat {
    public:
        using value_type = T;
        using computation_policy = CP;
        using storage_policy = SP;

        // Underlying storage types
        using DenseT = Matrix<T, SP, CP>;
        using SparseT = CsrMatrix<T>;
        using DiaT = DiaMatrix<T>;
        using Storage = std::variant<DenseT, SparseT, DiaT>;

        // Proxy for element access: dense allows mutation, sparse allows only read
        struct ElemProxy {
            Mat& mat;
            std::size_t r, c;

            operator T() const {
                return static_cast<const Mat&>(mat).operator()(r, c);
            }

            ElemProxy& operator=(T val) {
                if (auto* p = std::get_if<DenseT>(&mat.storage_)) {
                    (*p)(r, c) = val;
                    return *this;
                }
                throw std::runtime_error("Mat: mutable element access requires Dense storage");
            }
        };

        // ---- Construction ---------------------------------------------------

        // Auto-select based on size + density hint
        Mat(std::size_t rows, std::size_t cols, float density_hint = 1.0f) {
            MatKind kind = SelectPolicy::select(rows, cols, density_hint);
            construct_kind(kind, rows, cols);
        }

        // Tag-forced
        Mat(std::size_t rows, std::size_t cols, dense_tag) {
            storage_ = DenseT(rows, cols);
        }

        Mat(std::size_t rows, std::size_t cols, sparse_tag) {
            storage_ = SparseT(rows, cols);
        }

        Mat(std::size_t rows, std::size_t cols, banded_tag tag) {
            std::vector<std::ptrdiff_t> offsets;
            for (std::ptrdiff_t k = -static_cast<std::ptrdiff_t>(tag.bandwidth);
                 k <= static_cast<std::ptrdiff_t>(tag.bandwidth); ++k)
                offsets.push_back(k);
            storage_ = DiaT(rows, cols, std::move(offsets));
        }

        // Construct from existing types
        explicit Mat(DenseT m) : storage_(std::move(m)) {}
        explicit Mat(SparseT m) : storage_(std::move(m)) {}
        explicit Mat(DiaT m) : storage_(std::move(m)) {}

        // ---- Dimension queries -----------------------------------------------

        [[nodiscard]] std::size_t rows() const noexcept {
            return std::visit([](const auto& m) -> std::size_t { return m.rows(); }, storage_);
        }

        [[nodiscard]] std::size_t cols() const noexcept {
            return std::visit([](const auto& m) -> std::size_t { return m.cols(); }, storage_);
        }

        [[nodiscard]] MatKind kind() const noexcept {
            return std::visit([](const auto& m) -> MatKind {
                using M = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<M, DenseT>) return MatKind::Dense;
                if constexpr (std::is_same_v<M, SparseT>) return MatKind::Sparse;
                if constexpr (std::is_same_v<M, DiaT>) return MatKind::Diagonal;
                return MatKind::Dense;
            }, storage_);
        }

        // ---- Element access --------------------------------------------------

        // Mutable element access — returns proxy (throws on assignment to non-dense)
        [[nodiscard]] ElemProxy operator()(std::size_t r, std::size_t c) {
            return ElemProxy{*this, r, c};
        }

        // Const element access (all storage types)
        [[nodiscard]] T operator()(std::size_t r, std::size_t c) const {
            return std::visit([r, c](const auto& m) -> T {
                using M = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<M, DenseT>)
                    return m(r, c);
                else
                    return m.get(r, c);
            }, storage_);
        }

        // ---- Dense / sparse access ------------------------------------------
        [[nodiscard]] DenseT* as_dense() { return std::get_if<DenseT>(&storage_); }
        [[nodiscard]] SparseT* as_sparse() { return std::get_if<SparseT>(&storage_); }
        [[nodiscard]] DiaT* as_dia() { return std::get_if<DiaT>(&storage_); }

        [[nodiscard]] const DenseT* as_dense() const { return std::get_if<DenseT>(&storage_); }
        [[nodiscard]] const SparseT* as_sparse() const { return std::get_if<SparseT>(&storage_); }
        [[nodiscard]] const DiaT* as_dia() const { return std::get_if<DiaT>(&storage_); }

        // ---- Adaptation (explicit, caller-driven) ----------------------------

        // Update observed density in usage stats
        void record_density(float d) noexcept { usage_.observed_density = d; }

        // Explicit adaptation checkpoint — caller decides when to pay conversion cost
        [[nodiscard]] std::optional<MatKind> maybe_adapt() {
            auto rec = AdaptPolicy::should_convert(usage_);
            if (!rec) return std::nullopt;
            MatKind target = *rec;
            MatKind current = kind();
            if (target == current) return std::nullopt;
            convert_to(target);
            return target;
        }

        // ---- Introspection ---------------------------------------------------
        [[nodiscard]] MatInfo info() const {
            return std::visit([](const auto& m) { return ga::inspect(m); }, storage_);
        }

        const MatUsageStats& usage_stats() const noexcept { return usage_; }

    private:
        Storage storage_;
        MatUsageStats usage_;

        void construct_kind(MatKind kind, std::size_t rows, std::size_t cols) {
            switch (kind) {
            case MatKind::Dense:
                storage_ = DenseT(rows, cols);
                break;
            case MatKind::Sparse:
                storage_ = SparseT(rows, cols);
                break;
            case MatKind::Diagonal: {
                std::vector<std::ptrdiff_t> offs = {-1, 0, 1}; // default tridiagonal
                storage_ = DiaT(rows, cols, std::move(offs));
                break;
            }
            default:
                storage_ = DenseT(rows, cols);
                break;
            }
        }

        void convert_to(MatKind target) {
            MatKind current = kind();
            if (target == current) return;
            if (target == MatKind::Dense) {
                if (auto* sp = std::get_if<SparseT>(&storage_)) {
                    // CSR → Dense
                    DenseT dense(sp->nrows, sp->ncols, T{0});
                    for (std::size_t i = 0; i < sp->nrows; ++i)
                        for (std::size_t jj = sp->row_ptr[i]; jj < sp->row_ptr[i + 1]; ++jj)
                            dense(i, sp->col_idx[jj]) = sp->values[jj];
                    storage_ = std::move(dense);
                }
                if (auto* dia = std::get_if<DiaT>(&storage_)) {
                    auto csr = dia->to_csr();
                    DenseT dense(csr.nrows, csr.ncols, T{0});
                    for (std::size_t i = 0; i < csr.nrows; ++i)
                        for (std::size_t jj = csr.row_ptr[i]; jj < csr.row_ptr[i + 1]; ++jj)
                            dense(i, csr.col_idx[jj]) = csr.values[jj];
                    storage_ = std::move(dense);
                }
            }
            else if (target == MatKind::Sparse) {
                if (auto* dp = std::get_if<DenseT>(&storage_)) {
                    storage_ = dense_to_csr(*dp);
                }
            }
        }
    };

    // -----------------------------------------------------------------------
    // ga::inspect overload for ga::Mat
    // -----------------------------------------------------------------------
    template <typename T, typename CP, typename SP, typename SelP, typename AdP>
    [[nodiscard]] MatInfo inspect(const Mat<T, CP, SP, SelP, AdP>& m) noexcept {
        return m.info();
    }
} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_MAT_HPP
