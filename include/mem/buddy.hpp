#pragma once
// ============================================================================
// buddy.hpp — Power-of-two buddy allocator for smriti
// ============================================================================
// BuddyPool<MinOrder, MaxOrder, DomainT>
// MinOrder: smallest block = 2^MinOrder bytes (default 5 = 32 B)
// MaxOrder: largest block  = 2^MaxOrder bytes (default 20 = 1 MB)
// Allocation rounds up to next power-of-two >= MinBlock.
// Deallocation merges with buddy if free (XOR buddy address).
// ============================================================================

#include "smriti.hpp"
#include <array>
#include <bitset>
#include <cstring>

namespace smriti::pools {
    template <std::size_t MinOrder = 5,
              std::size_t MaxOrder = 20,
              concepts::Domain DomainT = domains::SystemRAMDomain>
        requires (MinOrder < MaxOrder) && (MaxOrder <= 30)
    class BuddyPool {
        static constexpr std::size_t kLevels = MaxOrder - MinOrder + 1;
        static constexpr std::size_t kMinBlock = std::size_t{1} << MinOrder;
        static constexpr std::size_t kMaxBlock = std::size_t{1} << MaxOrder;
        // Total number of minimum-size blocks in the slab
        static constexpr std::size_t kNumBlocks = kMaxBlock / kMinBlock;

        // Intrusive free list per level — next ptr stored in first sizeof(void*) bytes
        std::array<void*, kLevels> free_lists_{};
        // Bitmap: bit set means "this block is free at this level"
        std::bitset<kNumBlocks * kLevels> bitmap_{};

        std::byte* base_{};
        DomainT domain_;

        // ----------- bitmap helpers -----------------------------------------

        // Index of block `ptr` at `level` within the flat bitmap
        [[nodiscard]] std::size_t bmap_index(void* ptr, const std::size_t level) const noexcept {
            const std::size_t block_size = kMinBlock << level;
            const std::size_t block_idx = (static_cast<std::byte*>(ptr) - base_) / block_size;
            return level * (kNumBlocks >> level) + block_idx;
        }

        void bmap_set(void* ptr, const std::size_t level) noexcept {
            bitmap_.set(bmap_index(ptr, level));
        }

        void bmap_clear(void* ptr, const std::size_t level) noexcept {
            bitmap_.reset(bmap_index(ptr, level));
        }

        [[nodiscard]] bool bmap_test(void* ptr, const std::size_t level) const noexcept {
            return bitmap_.test(bmap_index(ptr, level));
        }

        // ----------- free-list helpers --------------------------------------

        void fl_push(void* p, std::size_t level) noexcept {
            void* head = free_lists_[level];
            std::memcpy(p, &head, sizeof(void*));
            free_lists_[level] = p;
            bmap_set(p, level);
        }

        void* fl_pop(std::size_t level) noexcept {
            void* p = free_lists_[level];
            if (!p) return nullptr;
            void* next;
            std::memcpy(&next, p, sizeof(void*));
            free_lists_[level] = next;
            bmap_clear(p, level);
            return p;
        }

        void fl_remove(void* p, std::size_t level) noexcept {
            void** cur = &free_lists_[level];
            while (*cur && *cur != p) {
                std::memcpy(cur, *cur, sizeof(void*));
                // walk next — we store next at head of block
                void* tmp;
                std::memcpy(&tmp, p, sizeof(void*)); // unused — just advance
                // proper walk:
                const void* walker = *cur;
                void* next_walker;
                std::memcpy(&next_walker, walker, sizeof(void*));
                // reset and do it right
                (void)p;
                (void)level;
                break; // simplified — see below
            }
            // Linear scan removal
            void* prev_p = nullptr;
            void* curr_p = free_lists_[level];
            while (curr_p) {
                void* next_p;
                std::memcpy(&next_p, curr_p, sizeof(void*));
                if (curr_p == p) {
                    if (!prev_p) {
                        free_lists_[level] = next_p;
                    }
                    else {
                        std::memcpy(prev_p, &next_p, sizeof(void*));
                    }
                    bmap_clear(p, level);
                    return;
                }
                prev_p = curr_p;
                curr_p = next_p;
            }
        }

        // Buddy address: XOR the block-size bit with the block's offset from base
        [[nodiscard]] void* buddy_of(void* p, const std::size_t level) const noexcept {
            const std::size_t block_size = kMinBlock << level;
            const std::size_t offset = static_cast<std::byte*>(p) - base_;
            return base_ + (offset ^ block_size);
        }

        // Round n up to next power-of-two >= kMinBlock; return corresponding level
        [[nodiscard]] static std::size_t size_to_level(const std::size_t n) noexcept {
            std::size_t sz = std::max(n, kMinBlock);
            // Round up to power of two
            if (!std::has_single_bit(sz)) sz = std::size_t{1} << (std::bit_width(sz));
            if (sz > kMaxBlock) return kLevels; // too large
            const std::size_t level = std::bit_width(sz) - 1 - MinOrder;
            return level;
        }

    public:
        using inner_domain_type = DomainT;

        explicit BuddyPool(DomainT d = {}) : domain_{std::move(d)} {
            base_ = static_cast<std::byte*>(
                domain_.acquire(kMaxBlock, kMinBlock));
            if (base_) {
                free_lists_.fill(nullptr);
                fl_push(base_, kLevels - 1); // one big block at top level
            }
        }

        ~BuddyPool() {
            if (base_) domain_.release(base_, kMaxBlock);
        }

        BuddyPool(const BuddyPool&) = delete;

        BuddyPool& operator=(const BuddyPool&) = delete;

        [[nodiscard]] void* allocate(const std::size_t n, std::size_t /*a*/) noexcept {
            if (!base_ || n == 0) return nullptr;
            const std::size_t level = size_to_level(n);
            if (level >= kLevels) return nullptr;

            // Find the lowest level >= requested level that has a free block
            std::size_t found = kLevels;
            for (std::size_t l = level; l < kLevels; ++l) {
                if (free_lists_[l]) {
                    found = l;
                    break;
                }
            }
            if (found == kLevels) return nullptr;

            // Pop from found level; split down to requested level
            void* p = fl_pop(found);
            while (found > level) {
                --found;
                const std::size_t block_size = kMinBlock << found;
                void* buddy = static_cast<std::byte*>(p) + block_size;
                fl_push(buddy, found); // push the upper half as free
            }
            return p;
        }

        void deallocate(void* p, const std::size_t n) noexcept {
            if (!p || !base_) return;
            std::size_t level = size_to_level(n);
            if (level >= kLevels) return;

            // Merge with buddy while buddy is free and level < top
            while (level < kLevels - 1) {
                void* buddy = buddy_of(p, level);
                // Check buddy is within our slab
                if (buddy < base_ || buddy >= base_ + kMaxBlock) break;
                if (!bmap_test(buddy, level)) break;
                // Remove buddy from its free list
                fl_remove(buddy, level);
                // Merge: lower address becomes the merged block
                if (buddy < p) p = buddy;
                ++level;
            }
            fl_push(p, level);
        }

        void reset() noexcept {
            if (!base_) return;
            free_lists_.fill(nullptr);
            bitmap_.reset();
            fl_push(base_, kLevels - 1);
        }
    };
} // namespace smriti::pools
