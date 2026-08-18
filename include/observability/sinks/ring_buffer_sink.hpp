#pragma once

#include "observability/nadi.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>

namespace utils::nadi {
    // ---------------------------------------------------------------------------
    // CanonicalEvent
    //
    // A fixed-size, type-erased representation of any Pulse that allows a single
    // RingBufferSink to hold events from *all* Pulse types in one contiguous
    // buffer — making timeline reconstruction possible across heterogeneous pulse
    // signatures (e.g. Pulse<"KV",...> and Pulse<"Physics",...> in the same ring).
    //
    // Layout (all fields fixed-width, no heap):
    //   id           — EventId of the pulse
    //   trace_id     — lineage trace_id at emit time
    //   parent_id    — lineage parent_id at emit time
    //   timestamp_ns — nanosecond timestamp
    //   phase        — PulsePhase
    //   category     — null-terminated category string (up to 63 chars)
    //   args         — serialised field key=value pairs as a flat char buffer
    //                  Format: "key=value\0key=value\0..." (null-terminated pairs,
    //                  double-null at end). Up to ArgsCapacity bytes total.
    // ---------------------------------------------------------------------------

    template <std::size_t ArgsCapacity = 256>
    struct CanonicalEvent {
        EventId id{};
        std::uint64_t trace_id{};
        std::uint64_t parent_id{};
        std::uint64_t timestamp_ns{};
        PulsePhase phase{};
        char category[64]{};
        char args[ArgsCapacity]{}; // key=value\0 pairs, double-\0 at end

        [[nodiscard]] std::string_view category_view() const noexcept {
            return {category};
        }

        // Iterate args: calls f(key, value) for each stored pair.
        template <typename F>
        void for_each_arg(F&& f) const {
            const char* p = args;
            const char* end = args + ArgsCapacity;
            while (p < end && *p != '\0') {
                std::string_view kv{p};
                const auto eq = kv.find('=');
                if (eq == std::string_view::npos) break;
                f(kv.substr(0, eq), kv.substr(eq + 1));
                p += kv.size() + 1;
            }
        }
    };

    static_assert(std::is_trivially_copyable_v<CanonicalEvent<256>>,
                  "CanonicalEvent must remain trivially copyable for atomic byte-wise extraction");

    // ---------------------------------------------------------------------------
    // detail: serialise a Pulse into a CanonicalEvent
    // ---------------------------------------------------------------------------

    namespace detail {
        // Append a null-terminated "key=value\0" pair into a fixed char buffer.
        // Returns the new write position, or pos unchanged if it would overflow.
        inline std::size_t append_kv(char* buf, const std::size_t cap,
                                     std::size_t pos,
                                     const std::string_view key,
                                     const std::string_view val) noexcept {
            // Need: key + '=' + val + '\0' + trailing '\0'
            if (pos + key.size() + 1 + val.size() + 2 > cap) return pos;
            std::memcpy(buf + pos, key.data(), key.size());
            pos += key.size();
            buf[pos++] = '=';
            std::memcpy(buf + pos, val.data(), val.size());
            pos += val.size();
            buf[pos++] = '\0';
            buf[pos] = '\0'; // keep double-null invariant
            return pos;
        }

        // Convert a scalar field value to a small decimal string.
        // Uses a local char[32] — no heap.
        template <typename T>
        void append_field(char* buf, const std::size_t cap, std::size_t& pos,
                          const std::string_view name, const T& v) noexcept {
            char tmp[32]{};
            std::size_t tlen = 0;

            if constexpr (std::is_same_v<T, bool>) {
                const char* s = v ? "true" : "false";
                tlen = std::strlen(s);
                std::memcpy(tmp, s, tlen);
            }
            else if constexpr (std::is_integral_v<T>) {
                // Manual itoa — avoids sprintf/snprintf heap path on some impls.
                using U = std::make_unsigned_t<T>;
                U uv = static_cast<U>(v);
                const bool neg = std::is_signed_v<T> && v < 0;
                if (neg) uv = static_cast<U>(-static_cast<std::make_signed_t<U>>(uv));
                char* t = tmp + 31;
                *t = '\0';
                do {
                    *--t = static_cast<char>('0' + uv % 10);
                    uv /= 10;
                }
                while (uv);
                if (neg) *--t = '-';
                tlen = static_cast<std::size_t>(tmp + 31 - t);
                std::memmove(tmp, t, tlen);
            }
            else if constexpr (std::is_floating_point_v<T>) {
                // Fallback to a fixed-precision representation via integer cast.
                // For high-fidelity float output a proper formatter is needed, but
                // this keeps us allocation-free.
                const auto whole = static_cast<std::int64_t>(v);
                const auto frac = static_cast<std::uint64_t>(
                    (v - static_cast<double>(whole)) * 1e6 + 0.5);
                char ibuf[32]{}, fbuf[16]{};
                // Reuse integral path via recursive logic inline.
                const std::int64_t w = whole;
                char* t = ibuf + 31;
                *t = '\0';
                const bool neg = w < 0;
                auto uw = neg
                              ? static_cast<std::uint64_t>(-w)
                              : static_cast<std::uint64_t>(w);
                do {
                    *--t = static_cast<char>('0' + uw % 10);
                    uw /= 10;
                }
                while (uw);
                if (neg) *--t = '-';
                const std::size_t ilen = static_cast<std::size_t>(ibuf + 31 - t);
                std::memmove(ibuf, t, ilen);
                ibuf[ilen] = '\0';
                // frac part
                char* ft = fbuf + 15;
                *ft = '\0';
                auto uf = frac;
                int digits = 6;
                while (digits--) {
                    *--ft = static_cast<char>('0' + uf % 10);
                    uf /= 10;
                }
                const std::size_t flen = 6;
                std::memcpy(tmp, ibuf, ilen);
                tmp[ilen] = '.';
                std::memcpy(tmp + ilen + 1, ft, flen);
                tlen = ilen + 1 + flen;
            }
            else if constexpr (std::is_same_v<T, const char*>) {
                if (v) {
                    tlen = std::strlen(v);
                    std::memcpy(tmp, v, tlen < 31 ? tlen : 31);
                }
                else {
                    std::memcpy(tmp, "null", 4);
                    tlen = 4;
                }
            }
            else if constexpr (std::is_same_v<T, std::string_view>) {
                tlen = v.size() < 31 ? v.size() : 31;
                std::memcpy(tmp, v.data(), tlen);
            }
            else if constexpr (std::is_same_v<T, SourceLocation>) {
                // Encode as "file:line"
                const char* file = v.file ? v.file : "";
                std::size_t flen = std::strlen(file);
                if (flen > 20) {
                    file += flen - 20;
                    flen = 20;
                }
                std::memcpy(tmp, file, flen);
                tmp[flen] = ':';
                // append line number
                char lbuf[12]{};
                char* lt = lbuf + 11;
                *lt = '\0';
                auto ul = v.line;
                do {
                    *--lt = static_cast<char>('0' + ul % 10);
                    ul /= 10;
                }
                while (ul);
                const std::size_t llen = static_cast<std::size_t>(lbuf + 11 - lt);
                std::memcpy(tmp + flen + 1, lt, llen);
                tlen = flen + 1 + llen;
            }

            pos = append_kv(buf, cap, pos, name, {tmp, tlen});
        }

        template <std::size_t ArgsCapacity, typename PulseType>
        CanonicalEvent<ArgsCapacity> to_canonical(const PulseType& pulse) noexcept {
            CanonicalEvent<ArgsCapacity> ev;
            ev.id = pulse.id;
            ev.timestamp_ns = pulse.timestamp_ns;
            ev.phase = pulse.phase;

            // category
            auto cat = PulseType::category.view();
            std::size_t clen = cat.size() < 63 ? cat.size() : 63;
            std::memcpy(ev.category, cat.data(), clen);
            ev.category[clen] = '\0';

            // lineage — read directly from the pulse (embedded at construction time,
            // not from thread-local state, so async/coroutine migration is safe)
            ev.trace_id = pulse.trace_id;
            ev.parent_id = pulse.parent_id;

            // serialize payload fields
            std::size_t pos = 0;
            ev.args[0] = '\0';
            std::apply([&](const auto&... fields) {
                (append_field(ev.args, ArgsCapacity, pos, fields.name.view(), fields.value), ...);
            }, pulse.payload);

            return ev;
        }
    } // namespace detail

    // ---------------------------------------------------------------------------
    // RingBufferSink<Capacity, ArgsCapacity>
    //
    // A lock-free, heap-free, overwrite-oldest ring buffer that accepts *any*
    // Pulse type and stores it as a CanonicalEvent. This provides a single unified
    // temporal buffer for timeline reconstruction regardless of Pulse variety.
    //
    // Concurrency model — SPSC (single producer, single consumer):
    //   Writer: ONE thread calls emit() at a time. The writer atomically claims
    //     a write ticket, writes into the slot under an odd seqlock stamp, then
    //     marks the slot complete with an even stamp.
    //   Reader: ONE thread calls try_pop(). The seqlock seq field detects an
    //     in-progress write; the reader retries on mismatch.
    //
    //   WARNING: Multi-producer use is unsafe. Two concurrent writers can claim
    //     tickets w1 and w2 where (w1 % Capacity) == (w2 % Capacity) when the
    //     ring wraps, causing both to write into the same physical slot. The
    //     seqlock protects readers from torn reads but does NOT protect slots
    //     from concurrent writers. For multi-producer use, add a per-slot mutex
    //     or use a proper MPMC queue.
    //
    // Slot storage: Event bytes are held as std::array<std::atomic<char>> so
    //   the reader can copy them byte-by-byte without formal data-race UB.
    //   (reinterpret_cast of a normal object to atomic<char>* is not legal C++.)
    //
    // read_pos_ advancement: monotonic-max CAS — a new value is only committed
    //   if it strictly exceeds the current value, preventing backward movement
    //   under any write-ticket ordering.
    //
    // Type-erasure layer: CanonicalEvent<ArgsCapacity> is the stable transport
    //   format. Pulse is the typed semantic IR; CanonicalEvent is the
    //   storage/transport representation.
    // ---------------------------------------------------------------------------

    template <std::size_t Capacity, std::size_t ArgsCapacity = 256>
        requires (Capacity > 0)
    struct RingBufferSink {
        static constexpr bool enabled = true;
        static constexpr OverwriteOldest flow_control = {};

        using Event = CanonicalEvent<ArgsCapacity>;

        static void emit(const auto& pulse) noexcept {
            instance().push(detail::to_canonical<ArgsCapacity>(pulse));
        }

        // -----------------------------------------------------------------------
        // Public read API (not part of SinkPolicy)
        // -----------------------------------------------------------------------

        [[nodiscard]] std::optional<Event> try_pop() noexcept {
            while (true) {
                const auto r = read_pos_.load(std::memory_order_acquire);
                const auto w = write_pos_.load(std::memory_order_acquire);
                if (r >= w) return std::nullopt;

                const std::size_t slot = r % Capacity;
                Slot& s = slots_[slot];

                const auto seq1 = s.seq.load(std::memory_order_acquire);
                if (seq1 & 1u) continue; // writer mid-write

                // Byte-wise copy from atomic storage — no reinterpret_cast needed;
                // the slot holds Event as std::array<std::atomic<char>>.
                Event copy;
                {
                    auto* dst = reinterpret_cast<char*>(&copy);
                    for (std::size_t i = 0; i < sizeof(Event); ++i)
                        dst[i] = s.bytes[i].load(std::memory_order_relaxed);
                }

                const auto seq2 = s.seq.load(std::memory_order_acquire);
                if (seq1 != seq2) continue; // overwritten during read

                std::uint64_t expected = r;
                if (read_pos_.compare_exchange_weak(expected, r + 1,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed))
                    return copy;
            }
        }

        [[nodiscard]] std::size_t size() const noexcept {
            const auto w = write_pos_.load(std::memory_order_relaxed);
            const auto r = read_pos_.load(std::memory_order_relaxed);
            return w > r ? std::min<std::size_t>(w - r, Capacity) : 0;
        }

        static RingBufferSink& instance() noexcept {
            static RingBufferSink inst;
            return inst;
        }

        // Pre-fault all pages by touching every cache line of the slot array.
        // Call once at program startup (before the hot path) to eliminate the
        // first-touch OS page-fault latency spike on large buffers.
        static void warmup() noexcept {
            auto& inst = instance();
            for (std::size_t i = 0; i < Capacity; ++i)
                inst.slots_[i].seq.load(std::memory_order_relaxed);
        }

    private:
        void push(const Event& ev) noexcept {
            const auto w = write_pos_.fetch_add(1, std::memory_order_relaxed);
            const std::size_t slot = w % Capacity;
            Slot& s = slots_[slot];

            s.seq.fetch_add(1, std::memory_order_release); // even → odd (write in progress)

            // Write event bytes into atomic storage.
            const auto* src = reinterpret_cast<const char*>(&ev);
            for (std::size_t i = 0; i < sizeof(Event); ++i)
                s.bytes[i].store(src[i], std::memory_order_relaxed);

            s.seq.fetch_add(1, std::memory_order_release); // odd → even (write complete)

            // Monotonically advance read_pos_ past the oldest entry we just clobbered.
            // Use a CAS loop that only commits if the new value exceeds the current one,
            // preventing read_pos_ from ever going backward regardless of write ordering.
            if (w + 1 > Capacity) {
                const std::uint64_t desired = w - Capacity + 1;
                std::uint64_t current = read_pos_.load(std::memory_order_relaxed);
                while (current < desired &&
                    !read_pos_.compare_exchange_weak(current, desired,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed)) {}
            }
        }

        struct Slot {
            std::array<std::atomic<char>, sizeof(Event)> bytes{};
            std::atomic<unsigned> seq{0};
        };

        alignas(64) std::array<Slot, Capacity> slots_{};
        alignas(64) std::atomic<std::uint64_t> write_pos_{0};
        alignas(64) std::atomic<std::uint64_t> read_pos_{0};
    };
} // namespace utils::nadi
