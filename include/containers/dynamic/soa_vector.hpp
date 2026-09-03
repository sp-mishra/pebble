#pragma once
// ============================================================================
// containers/dynamic/soa_vector.hpp — Policy-Driven Structure-of-Arrays (SoA)
// ============================================================================
// Modern C++23 header-only, policy-based contiguous SoA vector for SIMD physics.
// Supports Static, SmallVector (SBO), and Dynamic Heap storage with Smriti memory.
//
// New in this version:
//   - AlignedStoragePolicy<Align>   — 32/64-byte aligned columns for vmovaps
//   - transform_columns<Is...>(fn)  — generalised SIMD kernel dispatch
//   - erase_if(pred)                — bulk O(N) conditional removal via swap-pop
//   - append_range(other)           — O(M) batch merge of two SoAs
//   - resize(n, vals...)            — pre-size with fill values
//   - pop_back()                    — remove last element
//   - Portable SIMD pragma          — clang + GCC + MSVC
//   - noexcept-correct swap_pop_back
// ============================================================================

#include "containers/dynamic/SmallVector.hpp"
#include "containers/static/static_vector.hpp"
#include <vector>
#include <tuple>
#include <span>
#include <cstddef>
#include <utility>
#include <memory>
#include <new>
#include <type_traits>

// ── Portable SIMD vectorization hint ────────────────────────────────────────
#if defined(__clang__)
#  define SOA_VECTORIZE \
    _Pragma("clang loop vectorize(enable) interleave(enable) vectorize_width(8)")
#elif defined(__GNUC__)
#  define SOA_VECTORIZE _Pragma("GCC ivdep")
#elif defined(_MSC_VER)
#  define SOA_VECTORIZE __pragma(loop(ivdep))
#else
#  define SOA_VECTORIZE
#endif

namespace containers::dynamic {
    // ── 1. Aligned Allocator ────────────────────────────────────────────────────

    template <typename T, std::size_t Alignment = 64>
    struct AlignedAllocator {
        using value_type = T;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using propagate_on_container_move_assignment = std::true_type;
        using is_always_equal = std::true_type;

        AlignedAllocator() noexcept = default;

        template <typename U>
        AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

        [[nodiscard]] T* allocate(std::size_t n) {
            if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
                throw std::bad_array_new_length{};
            void* ptr = ::operator new(n * sizeof(T), std::align_val_t{Alignment});
            return static_cast<T*>(ptr);
        }

        void deallocate(T* ptr, std::size_t) noexcept {
            ::operator delete(ptr, std::align_val_t{Alignment});
        }

        template <typename U>
        struct rebind {
            using other = AlignedAllocator<U, Alignment>;
        };
    };

    template <typename T, std::size_t A1, typename U, std::size_t A2>
    bool operator==(const AlignedAllocator<T, A1>&, const AlignedAllocator<U, A2>&) noexcept {
        return A1 == A2;
    }

    // ── 2. Storage Policy Archetypes ────────────────────────────────────────────

    // Dynamic heap-allocated column (supports standard or Smriti arena allocators)
    template <typename Alloc = std::allocator<std::byte>>
    struct DynamicStoragePolicy {
        template <typename T>
        using ColumnType = std::vector<T, typename std::allocator_traits<Alloc>::template rebind_alloc<T>>;
    };

    // AVX2-aligned columns (32-byte) — enables vmovaps, avoids store split penalties
    struct Aligned32StoragePolicy {
        template <typename T>
        using ColumnType = std::vector<T, AlignedAllocator<T, 32>>;
    };

    // AVX-512 / cache-line aligned columns (64-byte) — optimal for AVX-512 gathers
    struct Aligned64StoragePolicy {
        template <typename T>
        using ColumnType = std::vector<T, AlignedAllocator<T, 64>>;
    };

    // Alias: default aligned policy (64-byte, works for AVX2 + AVX-512)
    using AlignedStoragePolicy = Aligned64StoragePolicy;

    // Small-Buffer Optimized inline column (falls back to heap if exceeding N bytes)
    template <std::size_t InlineCapacity = 16>
    struct SmallVectorStoragePolicy {
        template <typename T>
        using ColumnType = containers::dynamic::SmallVector<T, InlineCapacity>;
    };

    // Fixed-capacity inline column that never touches the heap (guaranteed zero allocation)
    template <std::size_t FixedCapacity>
    struct StaticStoragePolicy {
        template <typename T>
        using ColumnType = containers::static_vector<T, FixedCapacity>;
    };

    // ── 3. Policy-Based SoAVector ───────────────────────────────────────────────

    template <typename StoragePolicy, typename... Components>
    class BasicSoAVector {
    public:
        using TupleType = std::tuple<Components...>;
        static constexpr std::size_t kComponentCount = sizeof...(Components);

        BasicSoAVector() = default;

        explicit BasicSoAVector(const std::size_t reserve_capacity) {
            reserve(reserve_capacity);
        }

        // ── Capacity ─────────────────────────────────────────────────────────────

        void reserve(std::size_t capacity) {
            std::apply([capacity](auto&... cols) {
                ([capacity](auto& col) {
                    if constexpr (requires { col.reserve(capacity); })
                        col.reserve(capacity);
                }(cols), ...);
            }, columns_);
        }

        // Resize all columns to n, filling new slots with provided values.
        void resize(std::size_t n, Components... fill_values) {
            resize_impl(n, std::make_tuple(std::move(fill_values)...),
                        std::index_sequence_for<Components...>{});
        }

        // Resize to n, default-constructing new elements.
        void resize(std::size_t n) {
            resize_impl(n, std::make_tuple(Components{}...),
                        std::index_sequence_for<Components...>{});
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return std::get<0>(columns_).size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return std::get<0>(columns_).empty();
        }

        void clear() noexcept {
            std::apply([](auto&... cols) { (cols.clear(), ...); }, columns_);
        }

        // ── Mutation ─────────────────────────────────────────────────────────────

        void push_back(Components... values) {
            push_back_impl(std::make_tuple(std::move(values)...),
                           std::index_sequence_for<Components...>{});
        }

        void pop_back() noexcept {
            std::apply([](auto&... cols) { (cols.pop_back(), ...); }, columns_);
        }

        // O(1) removal: swap element at index with last, then pop.
        // noexcept iff all component moves are noexcept.
        void swap_pop_back(std::size_t index)
            noexcept((std::is_nothrow_move_assignable_v<Components> && ...)) {
            const std::size_t last = size() - 1;
            if (index != last) {
                std::apply([index, last](auto&... cols) {
                    ((cols[index] = std::move(cols[last])), ...);
                }, columns_);
            }
            std::apply([](auto&... cols) { (cols.pop_back(), ...); }, columns_);
        }

        // Append all elements from another SoAVector (same component types, any storage policy).
        template <typename OtherPolicy>
        void append_range(const BasicSoAVector<OtherPolicy, Components...>& other) {
            const std::size_t m = other.size();
            if (m == 0) return;
            reserve(size() + m);
            append_range_impl(other, std::index_sequence_for<Components...>{});
        }

        // Erase all elements for which pred(Components...) returns true.
        // Uses swap-pop for O(N) total work. Returns number of elements removed.
        template <typename Pred>
        std::size_t erase_if(Pred&& pred) {
            std::size_t removed = 0;
            std::size_t i = 0;
            while (i < size()) {
                if (invoke_pred_at(pred, i, std::index_sequence_for<Components...>{})) {
                    swap_pop_back(i);
                    ++removed;
                    // Don't advance i: the swapped-in element needs checking.
                }
                else {
                    ++i;
                }
            }
            return removed;
        }

        // ── Column Access ─────────────────────────────────────────────────────────

        // Contiguous column span — zero-copy SIMD processing.
        template <std::size_t Index>
        [[nodiscard]] auto get_column() noexcept {
            return std::span(std::get<Index>(columns_));
        }

        template <std::size_t Index>
        [[nodiscard]] auto get_column() const noexcept {
            return std::span(std::get<Index>(columns_));
        }

        // Direct pointer for raw vectorised loops (guaranteed contiguous).
        template <std::size_t Index>
        [[nodiscard]] auto* data() noexcept {
            return std::get<Index>(columns_).data();
        }

        template <std::size_t Index>
        [[nodiscard]] const auto* data() const noexcept {
            return std::get<Index>(columns_).data();
        }

        // Element-wise indexed access (single component).
        template <std::size_t Index>
        [[nodiscard]] decltype(auto) get(std::size_t i) noexcept {
            return std::get<Index>(columns_)[i];
        }

        template <std::size_t Index>
        [[nodiscard]] decltype(auto) get(std::size_t i) const noexcept {
            return std::get<Index>(columns_)[i];
        }

        // Row as tuple — for generic algorithms. O(kComponentCount), no heap.
        [[nodiscard]] std::tuple<Components...> row(std::size_t i) const {
            return row_impl(i, std::index_sequence_for<Components...>{});
        }

        // ── Generalised SIMD Kernel Dispatch ─────────────────────────────────────
        //
        // Passes aligned data pointers + element count to a user-supplied kernel.
        // The kernel owns the loop — SoAVector guarantees contiguous aligned storage.
        //
        // Example:
        //   soa.transform_columns<0,1,2,3>([](float* x, float* y, float* vx, float* vy, size_t n) {
        //       SOA_VECTORIZE
        //       for (size_t i = 0; i < n; ++i) { x[i] += vx[i]*dt; y[i] += vy[i]*dt; }
        //   });
        template <std::size_t... Is, typename Fn>
        void transform_columns(Fn&& fn) noexcept(noexcept(fn(data<Is>()..., size()))) {
            fn(data<Is>()..., size());
        }

        template <std::size_t... Is, typename Fn>
        void transform_columns(Fn&& fn) const noexcept(noexcept(fn(data<Is>()..., size()))) {
            fn(data<Is>()..., size());
        }

        // ── Built-in Kernels ──────────────────────────────────────────────────────

        // Velocity-Verlet integration: columns [0]=x [1]=y [2]=vx [3]=vy [4]=ax [5]=ay
        // Works for any scalar type (float or double).
        void integrate_verlet_simd(float dt) noexcept requires (sizeof...(Components) >= 6) {
            const std::size_t n = size();
            auto* px = data<0>();
            auto* py = data<1>();
            auto* pvx = data<2>();
            auto* pvy = data<3>();
            const auto* pax = data<4>();
            const auto* pay = data<5>();
            const float half_dt2 = 0.5f * dt * dt;
            SOA_VECTORIZE
            for (std::size_t i = 0; i < n; ++i) {
                px[i] += pvx[i] * dt + pax[i] * half_dt2;
                py[i] += pvy[i] * dt + pay[i] * half_dt2;
                pvx[i] += pax[i] * dt;
                pvy[i] += pay[i] * dt;
            }
        }

        // Double-precision Verlet for high-accuracy integrators.
        void integrate_verlet_simd(double dt) noexcept requires (sizeof...(Components) >= 6) {
            const std::size_t n = size();
            auto* px = data<0>();
            auto* py = data<1>();
            auto* pvx = data<2>();
            auto* pvy = data<3>();
            const auto* pax = data<4>();
            const auto* pay = data<5>();
            const double half_dt2 = 0.5 * dt * dt;
            SOA_VECTORIZE
            for (std::size_t i = 0; i < n; ++i) {
                px[i] += pvx[i] * dt + pax[i] * half_dt2;
                py[i] += pvy[i] * dt + pay[i] * half_dt2;
                pvx[i] += pax[i] * dt;
                pvy[i] += pay[i] * dt;
            }
        }

        // Axpy on a single column: col[i] = col[i] * scale + bias
        template <std::size_t Index, typename Scalar>
        void scale_column(Scalar scale, Scalar bias = Scalar{0}) noexcept {
            const std::size_t n = size();
            auto* p = data<Index>();
            SOA_VECTORIZE
            for (std::size_t i = 0; i < n; ++i)
                p[i] = p[i] * scale + bias;
        }

        // Element-wise clamp on a single column.
        template <std::size_t Index, typename Scalar>
        void clamp_column(Scalar lo, Scalar hi) noexcept {
            const std::size_t n = size();
            auto* p = data<Index>();
            SOA_VECTORIZE
            for (std::size_t i = 0; i < n; ++i)
                p[i] = p[i] < lo ? lo : (p[i] > hi ? hi : p[i]);
        }

    private:
        // ── Implementation helpers ────────────────────────────────────────────────

        template <typename Tuple, std::size_t... Is>
        void push_back_impl(Tuple&& t, std::index_sequence<Is...>) {
            ((void)std::get<Is>(columns_).push_back(std::get<Is>(std::forward<Tuple>(t))), ...);
        }

        template <typename Tuple, std::size_t... Is>
        void resize_impl(std::size_t n, Tuple fill, std::index_sequence<Is...>) {
            ([n, &fill]<typename T0>(T0& col) {
                using ElemT = std::decay_t<T0>::value_type;
                if constexpr (requires(T0& c, std::size_t sz, ElemT v) { c.resize(sz, v); }) {
                    col.resize(n, std::get<Is>(fill));
                }
            }(std::get<Is>(columns_)), ...);
        }

        template <typename OtherPolicy, std::size_t... Is>
        void append_range_impl(const BasicSoAVector<OtherPolicy, Components...>& other,
                               std::index_sequence<Is...>) {
            const std::size_t m = other.size();
            ([m, &other]<typename T0>(auto& col, T0 idx_const) {
                constexpr std::size_t I = T0::value;
                const auto src = other.template get_column<I>();
                col.insert(col.end(), src.begin(), src.end());
            }(std::get<Is>(columns_), std::integral_constant<std::size_t, Is>{}), ...);
        }

        template <typename Pred, std::size_t... Is>
        bool invoke_pred_at(Pred&& pred, std::size_t i, std::index_sequence<Is...>) {
            return pred(std::get<Is>(columns_)[i]...);
        }

        template <std::size_t... Is>
        std::tuple<Components...> row_impl(std::size_t i, std::index_sequence<Is...>) const {
            return {std::get<Is>(columns_)[i]...};
        }

        std::tuple<typename StoragePolicy::template ColumnType<Components>...> columns_;
    };

    // ── 4. Convenience Aliases ───────────────────────────────────────────────────

    // Default dynamic heap SoAVector
    template <typename... Components>
    using SoAVector = BasicSoAVector<DynamicStoragePolicy<>, Components...>;

    // AVX2-aligned heap SoAVector (32-byte per column)
    template <typename... Components>
    using AlignedSoAVector = BasicSoAVector<Aligned64StoragePolicy, Components...>;

    // Small-buffer optimized SoAVector
    template <std::size_t SBO, typename... Components>
    using SmallSoAVector = BasicSoAVector<SmallVectorStoragePolicy<SBO>, Components...>;

    // Static stack-allocated SoAVector
    template <std::size_t Capacity, typename... Components>
    using StaticSoAVector = BasicSoAVector<StaticStoragePolicy<Capacity>, Components...>;
} // namespace containers::dynamic
