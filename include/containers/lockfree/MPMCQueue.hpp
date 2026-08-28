#pragma once
// ============================================================================
// MPMCQueue<T, N> — Multi-Producer / Multi-Consumer bounded lock-free queue
// ============================================================================
//
// Based on Dmitry Vyukov's well-known MPMC ring-buffer design.
// Each slot carries a sequence counter that encodes its lifecycle state:
//
//   sequence == index            → slot is empty (ready to be written)
//   sequence == index + 1        → slot is full  (ready to be read)
//   sequence == index + N        → slot is recycled (ready to be written again)
//
// Push increments head_, pops increment tail_.  The difference between the
// sequence stored in the slot and the expected value tells the thread whether
// the slot is ready for its operation, causing it to spin (yield) if not.
//
// Guarantees
// ----------
//   - Bounded FIFO: capacity is exactly N (must be a power of two, >= 2).
//   - Safe for any number of concurrent producers AND consumers.
//   - Wait-free in the absence of contention; spin-wait under contention.
//   - No dynamic allocation after construction.
//   - T must be move-constructible; no default-constructibility required.
//
// Performance notes
// -----------------
//   - head_ and tail_ are on separate cache lines (false-sharing elimination).
//   - Each Slot is padded to a cache line to prevent adjacent-slot false sharing
//     on the hot write/read path.
//
// Usage
//   lockfree::MPMCQueue<int, 1024> q;
//   q.try_push(42);            // returns bool (false if full)
//   if (auto v = q.try_pop()) { use(*v); }
// ============================================================================

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace lockfree {
    template <typename T, std::size_t N>
        requires (N >= 2) && (std::has_single_bit(N))
        && std::move_constructible<T>
    class MPMCQueue {
    public:
        static constexpr std::size_t capacity_v = N;

    private:
        static constexpr std::size_t mask = N - 1;
        static constexpr std::size_t kCacheLineSize = 64;

        // Each slot has a sequence counter and raw storage for T.
        // Pad to a full cache line so neighboring slots don't share a line.
        struct alignas(kCacheLineSize) Slot {
            std::atomic<std::size_t> sequence{};
            alignas(alignof(T)) unsigned char storage[sizeof(T)]{};

            T& ref() noexcept { return *std::launder(reinterpret_cast<T*>(storage)); }
            const T& ref() const noexcept { return *std::launder(reinterpret_cast<const T*>(storage)); }
        };

        static_assert(sizeof(Slot) <= kCacheLineSize * 2,
                      "MPMCQueue: Slot exceeds two cache lines; false sharing likely. "
                      "Use a larger T with a custom allocator or increase kCacheLineSize.");

        // Slots array.
        alignas(kCacheLineSize) Slot slots_[N];

        // Producer counter — only producers write this.
        alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};

        // Consumer counter — only consumers write this.
        alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};

    public:
        MPMCQueue() noexcept {
            for (std::size_t i = 0; i < N; ++i)
                slots_[i].sequence.store(i, std::memory_order_relaxed);
        }

        ~MPMCQueue() noexcept {
            // Drain any unconsumed items to run their destructors.
            while (try_pop()) {}
        }

        MPMCQueue(const MPMCQueue&) = delete;

        MPMCQueue& operator=(const MPMCQueue&) = delete;

        // ---- Push ----
        // Returns false if the queue is full.
        // Spins briefly on the CAS and slot-sequence check; does not block.
        template <typename U>
            requires std::constructible_from<T, U>
        [[nodiscard]] bool try_push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U>) {
            std::size_t head = head_.load(std::memory_order_relaxed);

            for (;;) {
                Slot& slot = slots_[head & mask];
                const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
                const std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq)
                    - static_cast<std::ptrdiff_t>(head);

                if (diff == 0) {
                    // Slot is free — try to claim it.
                    if (head_.compare_exchange_weak(head, head + 1,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                        // Restore sequence on exception so the slot is not permanently stalled.
                        try {
                            ::new(slot.storage) T(std::forward<U>(value));
                        }
                        catch (...) {
                            slot.sequence.store(head, std::memory_order_release);
                            throw;
                        }
                        slot.sequence.store(head + 1, std::memory_order_release);
                        return true;
                    }
                    // CAS lost — reload head and retry.
                }
                else if (diff < 0) {
                    // The queue is full.
                    return false;
                }
                else {
                    // Another producer already claimed this slot and hasn't
                    // published yet; re-read head and retry.
                    head = head_.load(std::memory_order_relaxed);
                    std::this_thread::yield();
                }
            }
        }

        // ---- Pop ----
        // Returns std::nullopt if the queue is empty.
        // Spins briefly on the CAS and slot-sequence check; does not block.
        [[nodiscard]] std::optional<T> try_pop()
            noexcept(std::is_nothrow_move_constructible_v<T>) {
            std::size_t tail = tail_.load(std::memory_order_relaxed);

            for (;;) {
                Slot& slot = slots_[tail & mask];
                const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
                const std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq)
                    - static_cast<std::ptrdiff_t>(tail + 1);

                if (diff == 0) {
                    // Slot is full — try to claim it.
                    if (tail_.compare_exchange_weak(tail, tail + 1,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                        T result = std::move(slot.ref());
                        slot.ref().~T();
                        // Recycle: advance sequence by N to mark slot as empty again.
                        slot.sequence.store(tail + N, std::memory_order_release);
                        return result;
                    }
                    // CAS lost — reload tail and retry.
                }
                else if (diff < 0) {
                    // The queue is empty.
                    return std::nullopt;
                }
                else {
                    // A producer has claimed the slot but hasn't published yet.
                    tail = tail_.load(std::memory_order_relaxed);
                }
            }
        }

        // Blocking helpers (spin-wait) — convenient for producer/consumer loops.

        // Spins until push succeeds (the queue is full).
        template <typename U>
            requires std::constructible_from<T, U>
        void push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U>) {
            while (!try_push(std::forward<U>(value)))
                std::this_thread::yield();
        }

        // Spins until an item is available.
        [[nodiscard]] T pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            for (;;) {
                if (auto v = try_pop()) return std::move(*v);
                std::this_thread::yield();
            }
        }

        // ---- Introspection ----

        // Approximate occupancy — not precise under concurrent access.
        // Returns 0 if a snapshot observes head < tail (possible transiently near SIZE_MAX wrap).
        [[nodiscard]] std::size_t size_approx() const noexcept {
            const std::size_t h = head_.load(std::memory_order_acquire);
            const std::size_t t = tail_.load(std::memory_order_acquire);
            return (h >= t) ? (h - t) : 0;
        }

        [[nodiscard]] bool empty_approx() const noexcept {
            return size_approx() == 0;
        }

        static constexpr std::size_t capacity() noexcept { return N; }
    };
} // namespace lockfree
