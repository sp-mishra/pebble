#pragma once
// =============================================================================
// medha/adapters/smriti.hpp — Smriti arena adapter for transaction-local storage
//
// C++23, header-only, no virtual, no macros.
//
// Provides LinearArena checkpoint/rollback for per-attempt scratch.
// Rules (§21):
//   - transaction-local arena owned by exactly one attempt
//   - alignment must satisfy staged-value alignment
//   - arena exhaustion → tx_error::out_of_memory
//   - abort-time dtors must not throw
//   - rollback is single-writer: LinearArena::rollback(checkpoint)
//
// Usage:
//   #include "medha/adapters/smriti.hpp"
//   medha::adapters::arena_scope scope{arena, ctx};
//   // on scope destruction: rollback if not committed
// =============================================================================

#include "medha/fwd.hpp"

#if __has_include("mem/arena.hpp")
#  include "mem/arena.hpp"
#  define MEDHA_HAS_SMRITI 1
#endif

#include <cstddef>
#include <expected>

namespace medha::adapters {
#ifdef MEDHA_HAS_SMRITI

    // ============================================================================
    // arena_allocation — single allocation from the arena, auto-freed on rollback
    // ============================================================================

    struct arena_allocation {
        void* ptr = nullptr;
        std::size_t size = 0;
        std::size_t align = alignof(std::max_align_t);
    };

    // ============================================================================
    // arena_scope — RAII checkpoint/rollback for one transaction attempt
    // ============================================================================

    class arena_scope {
    public:
        explicit arena_scope(smriti::pools::LinearArena& arena) noexcept
            : arena_(arena)
              , checkpoint_(arena.checkpoint()) {}

        // Non-copyable, non-movable
        arena_scope(const arena_scope&) = delete;
        arena_scope& operator=(const arena_scope&) = delete;

        // Auto-rollback unless committed
        ~arena_scope() noexcept {
            if (!committed_) {
                arena_.rollback(checkpoint_);
            }
        }

        // Allocate from the arena; returns nullptr + oom error on exhaustion.
        [[nodiscard]] std::expected<void*, tx_error>
        allocate(std::size_t n, std::size_t align = alignof(std::max_align_t)) noexcept {
            void* p = arena_.allocate(n, align);
            if (!p)
                return std::unexpected(tx_error{
                    tx_status::out_of_memory,
                    "arena_scope: out of memory"
                });
            return p;
        }

        void commit() noexcept { committed_ = true; }

        [[nodiscard]] smriti::pools::LinearArena::Checkpoint checkpoint() const noexcept {
            return checkpoint_;
        }

    private:
        smriti::pools::LinearArena& arena_;
        smriti::pools::LinearArena::Checkpoint checkpoint_;
        bool committed_ = false;
    };

#endif  // MEDHA_HAS_SMRITI
} // namespace medha::adapters
