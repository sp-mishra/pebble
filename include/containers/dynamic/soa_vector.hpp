#pragma once
// ============================================================================
// containers/dynamic/soa_vector.hpp — Policy-Driven Structure-of-Arrays (SoA)
// ============================================================================
// Modern C++23 header-only, policy-based contiguous SoA vector for SIMD physics.
// Supports Static, SmallVector (SBO), and Dynamic Heap storage with Smriti memory.
// ============================================================================

#include "containers/dynamic/SmallVector.hpp"
#include "containers/static/static_vector.hpp"
#include <vector>
#include <tuple>
#include <span>
#include <cstddef>
#include <concepts>
#include <utility>
#include <memory>

namespace containers::dynamic {

// ── 1. Storage Policy Archetypes ────────────────────────────────────────────

// Dynamic heap-allocated column (supports standard or Smriti arena allocators)
template <typename Alloc = std::allocator<std::byte>>
struct DynamicStoragePolicy {
    template <typename T>
    using ColumnType = std::vector<T, typename std::allocator_traits<Alloc>::template rebind_alloc<T>>;
};

// Small-Buffer Optimized inline column (falls back to heap if exceeding N)
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

// ── 2. Policy-Based SoAVector ───────────────────────────────────────────────

template <typename StoragePolicy, typename... Components>
class BasicSoAVector {
public:
    using TupleType = std::tuple<Components...>;
    static constexpr std::size_t kComponentCount = sizeof...(Components);

    BasicSoAVector() = default;

    explicit BasicSoAVector(std::size_t reserve_capacity) {
        reserve(reserve_capacity);
    }

    void reserve(std::size_t capacity) {
        std::apply([capacity](auto&... cols) {
            auto reserve_if_supported = [capacity](auto& col) {
                if constexpr (requires { col.reserve(capacity); }) {
                    col.reserve(capacity);
                }
            };
            (reserve_if_supported(cols), ...);
        }, columns_);
    }

    void clear() noexcept {
        std::apply([](auto&... cols) {
            (cols.clear(), ...);
        }, columns_);
    }

    void push_back(Components... values) {
        push_back_tuple(std::make_tuple(std::move(values)...), std::index_sequence_for<Components...>{});
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return std::get<0>(columns_).size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return std::get<0>(columns_).empty();
    }

    // Access single contiguous column span for zero-copy SIMD processing
    template <std::size_t Index>
    [[nodiscard]] auto get_column() noexcept {
        return std::span(std::get<Index>(columns_));
    }

    template <std::size_t Index>
    [[nodiscard]] auto get_column() const noexcept {
        return std::span(std::get<Index>(columns_));
    }

    // Direct pointer access for vectorized loops
    template <std::size_t Index>
    [[nodiscard]] auto* data() noexcept {
        return std::get<Index>(columns_).data();
    }

    template <std::size_t Index>
    [[nodiscard]] const auto* data() const noexcept {
        return std::get<Index>(columns_).data();
    }

    // Swap and pop for O(1) removal
    void swap_pop_back(std::size_t index) noexcept {
        const std::size_t last = size() - 1;
        if (index != last) {
            std::apply([index, last](auto&... cols) {
                ((cols[index] = std::move(cols[last])), ...);
            }, columns_);
        }
        std::apply([](auto&... cols) {
            (cols.pop_back(), ...);
        }, columns_);
    }

    // High-Throughput SIMD Vectorized Verlet Integration for Kinematic Columns:
    // Columns: [0]=x, [1]=y, [2]=vx, [3]=vy, [4]=ax, [5]=ay
    void integrate_verlet_simd(float dt) noexcept requires (sizeof...(Components) >= 6) {
        const std::size_t n = size();
        auto* px = data<0>();
        auto* py = data<1>();
        auto* pvx = data<2>();
        auto* pvy = data<3>();
        const auto* pax = data<4>();
        const auto* pay = data<5>();

        const float half_dt2 = 0.5f * dt * dt;

        // Auto-vectorized contiguous unrolled loop (8-wide SIMD)
        #pragma clang loop vectorize(enable) interleave(enable)
        for (std::size_t i = 0; i < n; ++i) {
            px[i] += pvx[i] * dt + pax[i] * half_dt2;
            py[i] += pvy[i] * dt + pay[i] * half_dt2;
            pvx[i] += pax[i] * dt;
            pvy[i] += pay[i] * dt;
        }
    }

private:
    template <typename Tuple, std::size_t... Is>
    void push_back_tuple(Tuple&& t, std::index_sequence<Is...>) {
        ((void)std::get<Is>(columns_).push_back(std::get<Is>(std::forward<Tuple>(t))), ...);
    }

    std::tuple<typename StoragePolicy::template ColumnType<Components>...> columns_;
};

// Default dynamic heap SoAVector convenience alias
template <typename... Components>
using SoAVector = BasicSoAVector<DynamicStoragePolicy<>, Components...>;

// Small-buffer optimized SoAVector convenience alias
template <std::size_t SBO, typename... Components>
using SmallSoAVector = BasicSoAVector<SmallVectorStoragePolicy<SBO>, Components...>;

// Static stack-allocated SoAVector convenience alias
template <std::size_t Capacity, typename... Components>
using StaticSoAVector = BasicSoAVector<StaticStoragePolicy<Capacity>, Components...>;

} // namespace containers::dynamic
