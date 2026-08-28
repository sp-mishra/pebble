#pragma once
// ============================================================================
// arena.hpp — Arena pools for smriti
// ============================================================================
// ScopedArena: stack-backed bump allocator (zero heap, self-contained)
// LinearArena: heap-backed bump with checkpoint/rollback
// TwoPhaseArena: primary + lazy overflow arena
// ============================================================================

#include "smriti.hpp"

namespace smriti::pools {
    // ScopedArena: inline stack buffer + bump cursor — zero heap, non-movable
    template <std::size_t N, std::size_t Align = alignof(std::max_align_t)>
    class ScopedArena {
        alignas(Align) std::byte buf_[N]{};
        std::size_t cursor_{};

    public:
        ScopedArena() = default;

        ScopedArena(const ScopedArena&) = delete;

        ScopedArena& operator=(const ScopedArena&) = delete;

        ScopedArena(ScopedArena&&) = delete;

        ScopedArena& operator=(ScopedArena&&) = delete;

        [[nodiscard]] void* allocate(const std::size_t n, const std::size_t a) noexcept {
            std::size_t aligned = detail::align_up(cursor_, a);
            if (aligned + n > N) return nullptr;
            cursor_ = aligned + n;
            return buf_ + aligned;
        }

        void deallocate(void*, std::size_t) noexcept {}

        void reset() noexcept { cursor_ = 0; }
        [[nodiscard]] std::size_t used_bytes() const noexcept { return cursor_; }
    };

    // LinearArena: heap-backed bump allocator with checkpoint/rollback
    class LinearArena : public BumpPool<domains::SystemRAMDomain> {
        using Base = BumpPool<domains::SystemRAMDomain>;

    public:
        explicit LinearArena(const std::size_t capacity) : Base{capacity} {}

        struct Checkpoint {
            std::size_t offset;
        };

        [[nodiscard]] Checkpoint checkpoint() const noexcept {
            return Checkpoint{used_bytes()};
        }

        // Only safe to call when no concurrent allocations are in flight.
        void rollback(const Checkpoint c) noexcept {
            atomic_offset().store(c.offset, std::memory_order_release);
        }
    };

    // TwoPhaseArena: allocates from primary; spills lazily to overflow on exhaustion
    class TwoPhaseArena {
        LinearArena primary_;
        LinearArena overflow_;

    public:
        explicit TwoPhaseArena(const std::size_t primary_cap,
                               const std::size_t overflow_cap) noexcept
            : primary_{primary_cap}, overflow_{overflow_cap} {}

        [[nodiscard]] void* allocate(const std::size_t n, const std::size_t a) noexcept {
            if (void* p = primary_.allocate(n, a)) return p;
            return overflow_.allocate(n, a);
        }

        void deallocate(void*, std::size_t) noexcept {}

        void reset() noexcept {
            primary_.reset();
            overflow_.reset();
        }

        [[nodiscard]] std::size_t used_bytes() const noexcept {
            return primary_.used_bytes() + overflow_.used_bytes();
        }
    };
} // namespace smriti::pools
