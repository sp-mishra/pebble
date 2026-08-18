#pragma once
// ============================================================================
// RingBuffer<T, N> — Single-Producer / Single-Consumer lock-free ring buffer
// ============================================================================
//
// A wait-free (no retries on the happy path) bounded FIFO for exactly one
// producer thread and one consumer thread.  N must be a power of two; this is
// enforced at compile time.
//
// Memory layout
// -------------
//   - head_ and tail_ live on separate cache lines (false-sharing elimination).
//   - The slot array is aligned to a cache line and each slot contains the
//     payload plus a sequence counter so the consumer can detect that a push
//     has fully committed before trying to read.
//
// Guarantees
// ----------
//   - Linearisable: push() and pop() each appear to take effect atomically at
//     a single point in time.
//   - wait-free for a single producer/consumer pair.
//   - No dynamic allocation after construction.
//   - T must be move-constructible; no default-constructibility required.
//
// Usage
//   lockfree::RingBuffer<int, 1024> rb;
//   rb.try_push(42);           // returns bool
//   if (auto v = rb.try_pop()) { use(*v); }
// ============================================================================

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <new>       // placement new
#include <optional>
#include <type_traits>
#include <utility>

namespace lockfree { namespace detail {
        inline constexpr std::size_t cache_line_size = 64;

        template <class T>
        struct alignas(cache_line_size) CacheLinePad {
            T value{};
        };
    } // namespace detail

    template <typename T, std::size_t N>
        requires (N >= 2) && (std::has_single_bit(N))
        && std::move_constructible<T>
    class RingBuffer {
        static constexpr std::size_t mask = N - 1;

        // Each slot stores the payload and a sequence number.
        // sequence == index means "ready to write"; sequence == index+1 means "ready to read".
        struct alignas(detail::cache_line_size) Slot {
            std::atomic<std::size_t> sequence{};
            alignas(alignof(T)) unsigned char storage[sizeof(T)]{};

            T& ref() noexcept { return *std::launder(reinterpret_cast<T*>(storage)); }
            const T& ref() const noexcept { return *std::launder(reinterpret_cast<const T*>(storage)); }
        };

        // Slots array — separate from the counters to keep hot cache lines small.
        alignas(detail::cache_line_size) Slot slots_[N];

        // Producer-side counter — only mutated by the producer.
        alignas(detail::cache_line_size) std::atomic<std::size_t> head_{0};

        // Consumer-side counter — only mutated by the consumer.
        alignas(detail::cache_line_size) std::atomic<std::size_t> tail_{0};

    public:
        RingBuffer() noexcept {
            for (std::size_t i = 0; i < N; ++i)
                slots_[i].sequence.store(i, std::memory_order_relaxed);
        }

        ~RingBuffer() noexcept {
            // Drain any unconsumed items to invoke their destructors.
            while (try_pop()) {}
        }

        RingBuffer(const RingBuffer&) = delete;

        RingBuffer& operator=(const RingBuffer&) = delete;

        // Returns false if the buffer is full.
        template <typename U>
            requires std::constructible_from<T, U>
        [[nodiscard]] bool try_push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U>) {
            const std::size_t pos = head_.load(std::memory_order_relaxed);
            Slot& slot = slots_[pos & mask];
            const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff > 0) return false; // full — slot not yet recycled
            if (diff < 0) return false; // should not occur in correct SPSC usage
            head_.store(pos + 1, std::memory_order_relaxed);
            ::new(slot.storage) T(std::forward<U>(value));
            slot.sequence.store(pos + 1, std::memory_order_release);
            return true;
        }

        // Returns std::nullopt if the buffer is empty.
        [[nodiscard]] std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
            const std::size_t pos = tail_.load(std::memory_order_relaxed);
            Slot& slot = slots_[pos & mask];
            const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            const std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff != 0) return std::nullopt; // empty
            tail_.store(pos + 1, std::memory_order_relaxed);
            T result = std::move(slot.ref());
            slot.ref().~T();
            slot.sequence.store(pos + N, std::memory_order_release);
            return result;
        }

        // Conservative: loads both counters with acquire for correct cross-thread snapshots.
        // Exact only when called by the producer (head_ is authoritative for the producer).
        [[nodiscard]] bool empty() const noexcept {
            const std::size_t h = head_.load(std::memory_order_acquire);
            const std::size_t t = tail_.load(std::memory_order_acquire);
            return h == t;
        }

        [[nodiscard]] std::size_t size_approx() const noexcept {
            const std::size_t h = head_.load(std::memory_order_acquire);
            const std::size_t t = tail_.load(std::memory_order_acquire);
            return (h >= t) ? (h - t) : 0;
        }

        static constexpr std::size_t capacity() noexcept { return N; }
    };
} // namespace lockfree
