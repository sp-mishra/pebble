#pragma once
// ============================================================================
// mat_info.hpp: () / MatRegistry (opt-in)
// ============================================================================
// ga::inspect(m) → ga::MatInfo (O(1), constexpr where possible)
// Works on Matrix, StaticMatrix, CsrMatrix, DiaMatrix, CooMatrix.
// std::formatter<ga::MatInfo> for C++23 std::format.
// MatRegistry: opt-in singleton (GA_ENABLE_REGISTRY=1) — kosha-backed map.
// No virtual. No RTTI.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_MAT_INFO_HPP
#define PEBBLE_CONTAINERS_MATRIX_MAT_INFO_HPP

#include <containers/matrix/dense.hpp>
#include <containers/matrix/static.hpp>
#include <containers/matrix/sparse.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

namespace ga {

    // -----------------------------------------------------------------------
    // MatKind — what representation does the matrix use
    // -----------------------------------------------------------------------
    enum class MatKind : std::uint8_t {
        Static,    // StaticMatrix<T,R,C>
        Dense,     // Matrix<T,SP,CP>
        Sparse,    // CsrMatrix<T> or CooMatrix<T>
        Diagonal,  // DiaMatrix<T>
    };

    // -----------------------------------------------------------------------
    // MatUsageStats — populated only when MatAdaptorPolicy != NoAdaptorPolicy
    // -----------------------------------------------------------------------
    struct MatUsageStats {
        std::uint64_t spmv_count{0};
        std::uint64_t gemm_count{0};
        float         observed_density{0.f};
        float         avg_spmv_ns{0.f};
        float         avg_gemm_ns{0.f};
    };

    // -----------------------------------------------------------------------
    // MatInfo — plain struct returned by ga::inspect()
    // -----------------------------------------------------------------------
    struct MatInfo {
        MatKind          kind{MatKind::Dense};
        std::size_t      rows{0}, cols{0};
        std::size_t      nnz{0};
        float            density{0.f};
        std::size_t      storage_bytes{0};
        std::string_view compute_policy{"DefaultComputationPolicy"};
        bool             is_symmetric{false};
        bool             is_spd{false};
        MatUsageStats    usage{};
    };

    // -----------------------------------------------------------------------
    // ga::inspect overloads
    // -----------------------------------------------------------------------

    // StaticMatrix<T,R,C>
    template<typename T, std::size_t R, std::size_t C>
    [[nodiscard]] constexpr MatInfo inspect(const StaticMatrix<T,R,C>&) noexcept {
        MatInfo info;
        info.kind          = MatKind::Static;
        info.rows          = R;
        info.cols          = C;
        info.nnz           = R * C;
        info.density       = 1.f;
        info.storage_bytes = R * C * sizeof(T);
        info.compute_policy = "StaticArray";
        return info;
    }

    // Matrix<T,SP,CP>
    template<typename T, typename SP, typename CP>
    [[nodiscard]] MatInfo inspect(const Matrix<T,SP,CP>& m) noexcept {
        MatInfo info;
        info.kind          = MatKind::Dense;
        info.rows          = m.rows();
        info.cols          = m.cols();
        info.nnz           = m.rows() * m.cols();
        info.density       = 1.f;
        info.storage_bytes = m.rows() * m.cols() * sizeof(T);
        if constexpr (std::is_same_v<CP, ts::DefaultComputationPolicy>)
            info.compute_policy = "DefaultComputationPolicy";
        else
            info.compute_policy = "CustomComputationPolicy";
        return info;
    }

    // CsrMatrix<T>
    template<typename T>
    [[nodiscard]] MatInfo inspect(const CsrMatrix<T>& m) noexcept {
        MatInfo info;
        info.kind          = MatKind::Sparse;
        info.rows          = m.nrows;
        info.cols          = m.ncols;
        info.nnz           = m.nnz();
        info.density       = (m.nrows * m.ncols > 0)
                             ? static_cast<float>(m.nnz()) / static_cast<float>(m.nrows * m.ncols)
                             : 0.f;
        info.storage_bytes = m.nnz() * (sizeof(T) + sizeof(std::size_t))
                           + (m.nrows + 1) * sizeof(std::size_t);
        info.compute_policy = "DefaultComputationPolicy";
        return info;
    }

    // CooMatrix<T>
    template<typename T>
    [[nodiscard]] MatInfo inspect(const CooMatrix<T>& m) noexcept {
        MatInfo info;
        info.kind          = MatKind::Sparse;
        info.rows          = m.nrows;
        info.cols          = m.ncols;
        info.nnz           = m.nnz();
        info.density       = (m.nrows * m.ncols > 0)
                             ? static_cast<float>(m.nnz()) / static_cast<float>(m.nrows * m.ncols)
                             : 0.f;
        info.storage_bytes = m.nnz() * (sizeof(T) + 2 * sizeof(std::size_t));
        info.compute_policy = "DefaultComputationPolicy";
        return info;
    }

    // DiaMatrix<T>
    template<typename T>
    [[nodiscard]] MatInfo inspect(const DiaMatrix<T>& m) noexcept {
        MatInfo info;
        info.kind          = MatKind::Diagonal;
        info.rows          = m.nrows;
        info.cols          = m.ncols;
        // count actual nnz (non-zero diag entries)
        std::size_t nz = 0;
        for (const auto& d : m.diags)
            for (T v : d) if (v != T{0}) ++nz;
        info.nnz           = nz;
        info.density       = (m.nrows * m.ncols > 0)
                             ? static_cast<float>(nz) / static_cast<float>(m.nrows * m.ncols)
                             : 0.f;
        info.storage_bytes = m.offsets.size() * m.nrows * sizeof(T);
        info.compute_policy = "DefaultComputationPolicy";
        return info;
    }

    // -----------------------------------------------------------------------
    // MatId — lightweight handle for MatRegistry
    // -----------------------------------------------------------------------
    using MatId = std::uint32_t;

#ifdef GA_ENABLE_REGISTRY
    // MatRegistry — opt-in singleton; thread-safe via kosha's atomic map
    // Only compiled when GA_ENABLE_REGISTRY is defined.
    class MatRegistry {
    public:
        static MatRegistry& instance() {
            static MatRegistry reg;
            return reg;
        }

        template<typename MatT>
        MatId register_mat(MatT* ptr) {
            MatId id = next_id_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mu_);
            entries_.emplace(id, Entry{ptr,
                [](void* p) -> MatInfo {
                    return ga::inspect(*static_cast<MatT*>(p));
                }});
            return id;
        }

        void unregister(MatId id) {
            std::lock_guard<std::mutex> lock(mu_);
            entries_.erase(id);
        }

        MatInfo info(MatId id) const {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = entries_.find(id);
            if (it == entries_.end()) return {};
            return it->second.inspect_fn(it->second.ptr);
        }

        template<typename F>
        void for_each(F&& fn) {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [id, e] : entries_) {
                MatInfo mi = e.inspect_fn(e.ptr);
                fn(id, mi);
            }
        }

        static void hint(MatId, MatKind) { /* hook for future adaptive policy */ }

    private:
        struct Entry {
            void* ptr;
            MatInfo (*inspect_fn)(void*);
        };
        std::unordered_map<MatId, Entry> entries_;
        std::atomic<MatId> next_id_{0};
        mutable std::mutex mu_;
    };
#endif // GA_ENABLE_REGISTRY

} // namespace ga

// -----------------------------------------------------------------------
// std::formatter<ga::MatInfo> — C++23 format support
// -----------------------------------------------------------------------
template<>
struct std::formatter<ga::MatInfo> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template<typename FormatCtx>
    auto format(const ga::MatInfo& info, FormatCtx& ctx) const {
        const char* kind_str = "Dense";
        switch (info.kind) {
        case ga::MatKind::Static:   kind_str = "Static";   break;
        case ga::MatKind::Sparse:   kind_str = "Sparse";   break;
        case ga::MatKind::Diagonal: kind_str = "Diagonal"; break;
        default: break;
        }
        return std::format_to(ctx.out(),
            "Mat[{} {}×{} | nnz={} | {}B | {} | density={:.4f} | sym={} | SPD={}]",
            kind_str, info.rows, info.cols, info.nnz,
            info.storage_bytes, info.compute_policy,
            info.density, info.is_symmetric, info.is_spd);
    }
};

#endif // PEBBLE_CONTAINERS_MATRIX_MAT_INFO_HPP
