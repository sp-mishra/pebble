#pragma once
// ============================================================================
// AtomicStack<T, HR> — Treiber lock-free LIFO stack
// ============================================================================
//
// A classic Treiber stack: push and pop use a single compare-exchange on the
// top-of-stack pointer.  Safe for any number of concurrent producers and
// consumers.
//
// Safe memory reclamation
// -----------------------
//   Pop retires the detached node through HazardRegistry so that a concurrent
//   thread that loaded the pointer but hasn't dereferenced it yet is protected.
//   Pass your HazardRegistry type as the second template parameter (defaults
//   to DefaultHazardRegistry).
//
// ABA mitigation
// --------------
//   A 64-bit stamp is packed alongside the pointer in a 128-bit pair (on
//   platforms where std::atomic<TaggedPtr> is lock-free; the static_assert
//   below fires at compile time if it is not).  Both push and pop increment
//   the stamp so a recycled pointer with the same address is always
//   distinguished.
//
// Usage
//   lockfree::AtomicStack<int> s;
//   s.push(1);
//   if (auto v = s.pop()) { use(*v); }
// ============================================================================

#include <atomic>
#include <concepts>
#include <cstdint>
#include <optional>
#include <utility>

#include "containers/lockfree/HazardRegistry.hpp"

namespace lockfree {
    template <typename T, typename HR = DefaultHazardRegistry>
        requires std::move_constructible<T>
    class AtomicStack {
        struct Node {
            T value;
            Node* next{nullptr};

            template <typename U>
            explicit Node(U&& v) : value(std::forward<U>(v)) {}
        };

        // Tagged pointer — pointer + ABA stamp packed as a pair.
        struct TaggedPtr {
            Node* ptr{nullptr};
            std::uint64_t stamp{0};

            bool operator==(const TaggedPtr&) const noexcept = default;
        };

        // Verify the platform supports a 128-bit lock-free atomic for TaggedPtr.
        static_assert(sizeof(TaggedPtr) == 16);
        static_assert(std::atomic<TaggedPtr>::is_always_lock_free,
                      "AtomicStack: TaggedPtr must be lock-free. "
                      "On Apple Silicon ensure deployment target >= macOS 11 and SDK >= 14. "
                      "On x86_64 pass -mcx16.");

        std::atomic<TaggedPtr> top_{TaggedPtr{}};

    public:
        AtomicStack() noexcept = default;

        ~AtomicStack() noexcept {
            while (pop()) {}
        }

        AtomicStack(const AtomicStack&) = delete;

        AtomicStack& operator=(const AtomicStack&) = delete;

        // Push — safe to call from any thread. Throws std::bad_alloc on allocation failure.
        template <typename U>
            requires std::constructible_from<T, U>
        void push(U&& value) {
            Node* node = new Node(std::forward<U>(value));
            TaggedPtr cur = top_.load(std::memory_order_acquire);
            TaggedPtr next;
            do {
                node->next = cur.ptr;
                next = TaggedPtr{node, cur.stamp + 1};
            }
            while (!top_.compare_exchange_weak(cur, next,
                                               std::memory_order_release,
                                               std::memory_order_acquire));
        }

        // Pop — safe to call from any thread.
        // Returns std::nullopt if the stack is empty.
        [[nodiscard]] std::optional<T> pop() noexcept {
            typename HR::HazardGuard guard;
            TaggedPtr cur = top_.load(std::memory_order_acquire);
            while (true) {
                if (cur.ptr == nullptr) return std::nullopt;

                // Publish hazard before dereferencing cur.ptr->next.
                // Use a local atomic to avoid strict-aliasing UB when passing to protect().
                std::atomic<Node*> tmp{cur.ptr};
                guard.protect(tmp, std::memory_order_acquire);

                // Re-read after publishing: top_ might have changed.
                cur = top_.load(std::memory_order_acquire);
                if (cur.ptr == nullptr) return std::nullopt;

                if (TaggedPtr next{cur.ptr->next, cur.stamp + 1}; top_.compare_exchange_weak(cur, next,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                    T result = std::move(cur.ptr->value);
                    guard.clear();
                    HR::retire(cur.ptr);
                    return result;
                }
            }
        }

        [[nodiscard]] bool empty() const noexcept {
            return top_.load(std::memory_order_relaxed).ptr == nullptr;
        }
    };
} // namespace lockfree
