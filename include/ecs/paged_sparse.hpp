#pragma once
// ============================================================================
// ecs/paged_sparse.hpp — On-Demand Paged Sparse Array for pebble::ecs
// ============================================================================
// Allocates sparse lookup slots in 4KB/1024-element pages on demand, eliminating
// upfront memory waste for sparse universes while preserving O(1) direct indexing.
//
// Zero virtual functions, zero macros, header-only C++23.
// ============================================================================

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

namespace pebble::ecs {
    template <std::unsigned_integral IndexT = std::uint32_t, std::size_t PageSize = 1024>
    class PagedSparse {
    public:
        static constexpr std::size_t kPageSize = PageSize;
        static constexpr IndexT kInvalid = std::numeric_limits < IndexT > ::max();

        struct Page {
            std::array<IndexT, PageSize> slots;
            Page() noexcept { slots.fill(kInvalid); }
        };

        PagedSparse() = default;
        ~PagedSparse() = default;

        PagedSparse(const PagedSparse& other) {
            pages_.resize(other.pages_.size());
            for (std::size_t i = 0; i < other.pages_.size(); ++i) {
                if (other.pages_[i]) {
                    pages_[i] = std::make_unique<Page>(*other.pages_[i]);
                }
            }
        }

        PagedSparse& operator=(const PagedSparse& other) {
            if (this != &other) {
                pages_.clear();
                pages_.resize(other.pages_.size());
                for (std::size_t i = 0; i < other.pages_.size(); ++i) {
                    if (other.pages_[i]) {
                        pages_[i] = std::make_unique<Page>(*other.pages_[i]);
                    }
                }
            }
            return *this;
        }

        PagedSparse(PagedSparse&&) noexcept = default;
        PagedSparse& operator=(PagedSparse&&) noexcept = default;

        // Mutating index access — allocates page on-demand if missing
        [[nodiscard]] IndexT& operator[](std::size_t key) {
            const std::size_t page_idx = key / PageSize;
            const std::size_t slot_idx = key % PageSize;
            if (page_idx >= pages_.size()) {
                pages_.resize(page_idx + 1);
            }
            if (!pages_[page_idx]) {
                pages_[page_idx] = std::make_unique<Page>();
            }
            return pages_[page_idx]->slots[slot_idx];
        }

        // Read-only slot access
        [[nodiscard]] IndexT get(std::size_t key) const noexcept {
            const std::size_t page_idx = key / PageSize;
            if (page_idx >= pages_.size() || !pages_[page_idx]) {
                return kInvalid;
            }
            return pages_[page_idx]->slots[key % PageSize];
        }

        // Fast check if key is present without page allocation
        [[nodiscard]] bool has(std::size_t key) const noexcept {
            return get(key) != kInvalid;
        }

        // Reset a specific key slot
        void erase(std::size_t key) noexcept {
            const std::size_t page_idx = key / PageSize;
            if (page_idx < pages_.size() && pages_[page_idx]) {
                pages_[page_idx]->slots[key % PageSize] = kInvalid;
            }
        }

        // Reset all allocated pages
        void clear() noexcept {
            for (auto& p : pages_) {
                if (p) p->slots.fill(kInvalid);
            }
        }

        // Release page allocations
        void release() noexcept {
            pages_.clear();
        }

        [[nodiscard]] std::size_t allocated_pages() const noexcept {
            std::size_t count = 0;
            for (const auto& p : pages_) {
                if (p) ++count;
            }
            return count;
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return pages_.size() * PageSize;
        }

    private:
        std::vector<std::unique_ptr<Page>> pages_;
    };

    static_assert(!std::is_polymorphic_v<PagedSparse<std::uint32_t>>, "PagedSparse must have zero virtual functions");
} // namespace pebble::ecs
