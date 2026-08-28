#pragma once
// ============================================================================
// ecs/storage_policy.hpp — Concept-Constrained Policy Definitions for pebble::ecs
// ============================================================================
// Zero virtual functions, zero macros, modern C++23 concepts for:
// - StoragePolicy (SparseSetStoragePolicy, ArchetypeStoragePolicy)
// - AllocPolicy (ArenaAllocPolicy, SystemAllocPolicy)
// - SchedulerPolicy (AutoSchedulerPolicy, ManualSchedulerPolicy)
// - SparsePolicy (PagedSparsePolicy, FlatSparsePolicy)
// ============================================================================

#include "entity.hpp"
#include "paged_sparse.hpp"
#include "mem/arena.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace pebble::ecs {

// ── 1. Sparse Array Backing Policy ──────────────────────────────────────────

template <typename T, typename IndexT = std::uint32_t>
concept SparsePolicy = requires(T sp, std::size_t key) {
    { sp[key] } -> std::same_as<IndexT&>;
    { sp.get(key) } -> std::same_as<IndexT>;
    { sp.has(key) } -> std::same_as<bool>;
    sp.erase(key);
    sp.clear();
};

struct PagedSparsePolicy {
    template <std::unsigned_integral IndexT = std::uint32_t>
    using Type = PagedSparse<IndexT, 1024>;
};

struct FlatSparsePolicy {
    template <std::unsigned_integral IndexT = std::uint32_t>
    class Type {
    public:
        static constexpr IndexT kInvalid = std::numeric_limits<IndexT>::max();

        Type() = default;
        explicit Type(std::size_t cap) : vec_(cap, kInvalid) {}

        [[nodiscard]] IndexT& operator[](std::size_t key) {
            if (key >= vec_.size()) vec_.resize(key + 1, kInvalid);
            return vec_[key];
        }

        [[nodiscard]] IndexT get(std::size_t key) const noexcept {
            return key < vec_.size() ? vec_[key] : kInvalid;
        }

        [[nodiscard]] bool has(std::size_t key) const noexcept {
            return get(key) != kInvalid;
        }

        void erase(std::size_t key) noexcept {
            if (key < vec_.size()) vec_[key] = kInvalid;
        }

        void clear() noexcept {
            vec_.assign(vec_.size(), kInvalid);
        }

    private:
        std::vector<IndexT> vec_;
    };
};

// ── 2. Memory Allocation Policy ─────────────────────────────────────────────

struct ArenaAllocPolicy {
    explicit ArenaAllocPolicy(std::size_t initial_bytes = 64 * 1024)
        : arena_(initial_bytes) {}

    [[nodiscard]] void* allocate(std::size_t size, std::size_t align) noexcept {
        return arena_.allocate(size, align);
    }

    void reset() noexcept {
        arena_.reset();
    }

    [[nodiscard]] std::size_t used_bytes() const noexcept {
        return arena_.used_bytes();
    }

private:
    smriti::pools::LinearArena arena_;
};

struct SystemAllocPolicy {
    [[nodiscard]] void* allocate(std::size_t size, std::size_t /*align*/) noexcept {
        return std::malloc(size);
    }

    void reset() noexcept {}
};

template <typename T>
concept AllocPolicy = requires(T a, std::size_t size, std::size_t align) {
    { a.allocate(size, align) } -> std::same_as<void*>;
    a.reset();
};

// ── 3. System Scheduler Policy ──────────────────────────────────────────────

template <typename T, typename W>
concept SchedulerPolicy = requires(T s, W& w, float dt) {
    s.run(w, dt);
};

struct AutoSchedulerPolicy {};
struct ManualSchedulerPolicy {};

// ── 4. Storage Backend Policy ───────────────────────────────────────────────

struct SparseSetStoragePolicy {};
struct ArchetypeStoragePolicy {};

template <typename T>
concept StoragePolicy = std::is_same_v<T, SparseSetStoragePolicy> ||
                       std::is_same_v<T, ArchetypeStoragePolicy>;

} // namespace pebble::ecs
