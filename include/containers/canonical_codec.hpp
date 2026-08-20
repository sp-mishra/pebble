#pragma once

// =============================================================================
// containers/canonical_codec.hpp — deterministic canonical byte encoder
//
// Provides:
//   canonical_writer   — append fixed-width LE scalars + content-sorted strings
//   content_digest<Alg> — hash a byte span with a pluggable digest policy
//   sha256_digest_policy — software SHA-256 (portable, no third-party dep)
//   null_digest_policy  — identity: returns the span unchanged as raw bytes (test)
//
// Guarantees:
//   • Fixed little-endian widths for all scalar types (no host-endian leakage).
//   • String table is content-sorted (lexicographic) before emission so
//     insertion-order differences vanish from the output.
//   • No unordered-container iteration ever reaches the output buffer.
//   • Round-trip stable: encoding the same logical data always produces
//     byte-identical output regardless of construction order.
//
// Usage:
//   canonical_writer w;
//   w.write_u32(42);
//   auto idx = w.intern_string("hello");
//   w.write_string_ref(idx);
//   w.finalize_string_table();   // must call before emit()
//   auto bytes = w.emit();
//
// content_digest:
//   auto digest = content_digest<sha256_digest_policy>(bytes);
//
// Generic: zero dependency on Lithe types.
// No virtual, no macros. Header-only C++23.
// =============================================================================

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace containers {
    // =============================================================================
    // canonical_writer — deterministic byte stream builder
    // =============================================================================

    class canonical_writer {
    public:
        // -------------------------------------------------------------------------
        // Scalar writes (always little-endian)
        // -------------------------------------------------------------------------

        void write_u8(const std::uint8_t v) {
            buf_.push_back(v);
        }

        void write_u16(const std::uint16_t v) {
            if constexpr (std::endian::native == std::endian::little) {
                const auto p = reinterpret_cast<const std::uint8_t*>(&v);
                buf_.push_back(p[0]);
                buf_.push_back(p[1]);
            }
            else {
                buf_.push_back(static_cast<std::uint8_t>(v & 0xFFu));
                buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
            }
        }

        void write_u32(const std::uint32_t v) {
            buf_.push_back(static_cast<std::uint8_t>(v & 0xFFu));
            buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
            buf_.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
            buf_.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
        }

        void write_u64(const std::uint64_t v) {
            for (int i = 0; i < 8; ++i)
                buf_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
        }

        void write_i32(const std::int32_t v) {
            write_u32(static_cast<std::uint32_t>(v));
        }

        void write_i64(const std::int64_t v) {
            write_u64(static_cast<std::uint64_t>(v));
        }

        void write_bool(const bool v) {
            write_u8(v ? 1u : 0u);
        }

        void write_bytes(std::span<const std::uint8_t> data) {
            buf_.insert(buf_.end(), data.begin(), data.end());
        }

        // -------------------------------------------------------------------------
        // String table (content-sorted dedup)
        // intern_string() collects strings; finalize_string_table() must be called
        // before emit() to ensure sort-stability of the output.
        // Returns the insertion-order index (remapped to sorted index after finalize).
        // -------------------------------------------------------------------------

        [[nodiscard]] std::uint32_t intern_string(std::string_view sv) {
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(strings_.size()); ++i)
                if (strings_[i] == sv) return i;
            strings_.emplace_back(sv);
            return static_cast<std::uint32_t>(strings_.size() - 1);
        }

        // write_string_ref: emit the sorted index for the string interned at
        // insertion index idx. Must be called AFTER finalize_string_table().
        void write_string_ref(const std::uint32_t insertion_idx) {
            write_u32(remap_[insertion_idx]);
        }

        // finalize_string_table: sorts string entries lexicographically, builds
        // the remap table (insertion_idx → sorted_idx), and appends the table
        // into the output buffer. Must call once before emit().
        void finalize_string_table() {
            const std::uint32_t n = static_cast<std::uint32_t>(strings_.size());
            // Build sorted order via index sort
            sort_order_.resize(n);
            for (std::uint32_t i = 0; i < n; ++i) sort_order_[i] = i;
            std::ranges::sort(sort_order_,
                              [this](const std::uint32_t a, const std::uint32_t b) {
                                  return strings_[a] < strings_[b];
                              });
            // remap_[insertion_idx] = sorted_idx
            remap_.resize(n);
            for (std::uint32_t sorted = 0; sorted < n; ++sorted)
                remap_[sort_order_[sorted]] = sorted;
            // Emit string table into output: count + (length + bytes) in sorted order
            write_u32(n);
            for (std::uint32_t sorted = 0; sorted < n; ++sorted) {
                const std::string& s = strings_[sort_order_[sorted]];
                write_u32(static_cast<std::uint32_t>(s.size()));
                for (const unsigned char c : s) write_u8(c);
            }
            finalized_ = true;
        }

        // -------------------------------------------------------------------------
        // Emit / reset
        // -------------------------------------------------------------------------

        [[nodiscard]] std::vector<std::uint8_t> emit() const {
            return buf_;
        }

        void reset() {
            buf_.clear();
            strings_.clear();
            sort_order_.clear();
            remap_.clear();
            finalized_ = false;
        }

        [[nodiscard]] bool is_finalized() const noexcept { return finalized_; }
        [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }

    private:
        std::vector<std::uint8_t> buf_;
        std::vector<std::string> strings_;
        std::vector<std::uint32_t> sort_order_;
        std::vector<std::uint32_t> remap_;
        bool finalized_ = false;
    };

    // =============================================================================
    // content_digest — pluggable hash over a byte span
    //
    // Policy interface:
    //   static constexpr std::size_t digest_bytes;
    //   static std::array<std::uint8_t, digest_bytes>
    //          compute(std::span<const std::uint8_t>);
    // =============================================================================

    // ---------------------------------------------------------------------------
    // null_digest_policy — identity, for testing: first digest_bytes bytes of input
    // ---------------------------------------------------------------------------

    struct null_digest_policy {
        static constexpr std::size_t digest_bytes = 32;

        [[nodiscard]] static std::array<std::uint8_t, digest_bytes>
        compute(const std::span<const std::uint8_t> data) noexcept {
            std::array<std::uint8_t, digest_bytes> out{};
            const std::size_t n = std::min(data.size(), digest_bytes);
            for (std::size_t i = 0; i < n; ++i) out[i] = data[i];
            return out;
        }
    };

    // ---------------------------------------------------------------------------
    // sha256_digest_policy — portable software SHA-256
    // No dependency on any crypto library.
    // ---------------------------------------------------------------------------

    struct sha256_digest_policy {
        static constexpr std::size_t digest_bytes = 32;

        [[nodiscard]] static std::array<std::uint8_t, digest_bytes>
        compute(std::span<const std::uint8_t> data) noexcept {
            // SHA-256 constants
            static constexpr std::array<std::uint32_t, 64> k = {
                0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
                0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
                0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
                0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
                0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
                0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
                0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
                0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
                0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
            };

            auto rotr = [](const std::uint32_t x, const int n) constexpr noexcept -> std::uint32_t {
                return (x >> n) | (x << (32 - n));
            };

            // Initial hash values
            std::uint32_t h0 = 0x6a09e667u, h1 = 0xbb67ae85u, h2 = 0x3c6ef372u,
                          h3 = 0xa54ff53au, h4 = 0x510e527fu, h5 = 0x9b05688cu,
                          h6 = 0x1f83d9abu, h7 = 0x5be0cd19u;

            // Pre-processing: add padding
            const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;
            std::vector<std::uint8_t> padded(data.begin(), data.end());
            padded.push_back(0x80u);
            while ((padded.size() % 64) != 56) padded.push_back(0x00u);
            // Append bit length as big-endian uint64
            for (int i = 7; i >= 0; --i)
                padded.push_back(static_cast<std::uint8_t>((bit_len >> (8 * i)) & 0xFFu));

            // Process each 512-bit (64-byte) chunk
            for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64) {
                std::array<std::uint32_t, 64> w{};
                for (int i = 0; i < 16; ++i) {
                    w[static_cast<std::size_t>(i)] =
                        (static_cast<std::uint32_t>(padded[chunk + static_cast<std::size_t>(i * 4)]) << 24) |
                        (static_cast<std::uint32_t>(padded[chunk + static_cast<std::size_t>(i * 4 + 1)]) << 16) |
                        (static_cast<std::uint32_t>(padded[chunk + static_cast<std::size_t>(i * 4 + 2)]) << 8) |
                        (static_cast<std::uint32_t>(padded[chunk + static_cast<std::size_t>(i * 4 + 3)]));
                }
                for (std::size_t i = 16; i < 64; ++i) {
                    const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, hv = h7;
                for (std::size_t i = 0; i < 64; ++i) {
                    const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                    const std::uint32_t ch = (e & f) ^ (~e & g);
                    const std::uint32_t temp1 = hv + S1 + ch + k[i] + w[i];
                    const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    const std::uint32_t temp2 = S0 + maj;
                    hv = g;
                    g = f;
                    f = e;
                    e = d + temp1;
                    d = c;
                    c = b;
                    b = a;
                    a = temp1 + temp2;
                }
                h0 += a;
                h1 += b;
                h2 += c;
                h3 += d;
                h4 += e;
                h5 += f;
                h6 += g;
                h7 += hv;
            }

            std::array<std::uint8_t, digest_bytes> out{};
            auto store_be = [](const std::uint32_t v, std::uint8_t* p) {
                p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
                p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
                p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
                p[3] = static_cast<std::uint8_t>(v & 0xFFu);
            };
            store_be(h0, out.data());
            store_be(h1, out.data() + 4);
            store_be(h2, out.data() + 8);
            store_be(h3, out.data() + 12);
            store_be(h4, out.data() + 16);
            store_be(h5, out.data() + 20);
            store_be(h6, out.data() + 24);
            store_be(h7, out.data() + 28);
            return out;
        }
    };

    // ---------------------------------------------------------------------------
    // content_digest<Policy> — compute digest over a byte span
    // ---------------------------------------------------------------------------

    template <class Policy = sha256_digest_policy>
    [[nodiscard]] std::array<std::uint8_t, Policy::digest_bytes>
    content_digest(std::span<const std::uint8_t> data) noexcept {
        return Policy::compute(data);
    }
} // namespace containers
