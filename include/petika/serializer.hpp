#pragma once
// ============================================================================
// petika/serializer.hpp — Serialization & Comparison Policies for Petika
// ============================================================================

#include "petika/engine.hpp"

#include <bit>
#include <compare>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace petika {
    // ============================================================================
    // § 0  Endianness Normalisation — LE-canonical wire format
    // ============================================================================
    //
    // The WAL wire format is a durability contract: a log written on one host must
    // be readable on any other. Multi-byte integer fields are therefore stored
    // little-endian-canonical. On little-endian hosts (the common case) every
    // conversion is a compile-time no-op, so the hot path stays zero-cost; on
    // big-endian hosts the bytes are swapped on encode and decode.
    namespace detail {
        template <typename T>
            requires std::is_integral_v<T>
        [[nodiscard]] constexpr T to_le(T value) noexcept {
            if constexpr (std::endian::native == std::endian::big) return std::byteswap(value);
            else return value;
        }

        template <typename T>
            requires std::is_integral_v<T>
        [[nodiscard]] constexpr T from_le(T value) noexcept {
            return to_le(value); // byteswap is its own inverse
        }
    } // namespace detail

    // ============================================================================
    // § 1  Serializer Concepts & Implementations
    // ============================================================================

    template <typename S, typename K, typename V>
    concept SerializerFor = requires(const K& k, const V& v, std::string_view sv) {
        { S::serialize_key(k) } -> std::convertible_to<std::string>;
        { S::serialize_value(v) } -> std::convertible_to<std::string>;
        { S::deserialize_key(sv) } -> std::convertible_to<K>;
        { S::deserialize_value(sv) } -> std::convertible_to<V>;
    };

    // Concept: serializer whose deserialize returns owning strings (safe to keep beyond buffer lifetime).
    template <typename S>
    concept SerializerOwns = std::is_same_v<decltype(S::deserialize_key(std::string_view{})), std::string>;

    struct StringSerializer {
        static std::string serialize_key(const std::string& k) { return k; }
        static std::string serialize_value(const std::string& v) { return v; }
        static std::string deserialize_key(std::string_view sv) { return std::string(sv); }
        static std::string deserialize_value(std::string_view sv) { return std::string(sv); }
    };

    struct BinarySerializer {
        // Integral keys/values are stored little-endian-canonical for cross-host
        // WAL portability (no-op on LE hosts). Non-integral trivially-copyable
        // types keep native byte order — the caller owns their portability.
        template <typename T>
            requires std::is_trivially_copyable_v<T>
        static std::string serialize_key(const T& k) { return encode_scalar(k); }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        static std::string serialize_value(const T& v) { return encode_scalar(v); }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        static T deserialize_key(std::string_view sv) { return decode_scalar<T>(sv); }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        static T deserialize_value(std::string_view sv) { return decode_scalar<T>(sv); }

        // Specializations for std::string
        static std::string serialize_key(const std::string& k) { return k; }
        static std::string serialize_value(const std::string& v) { return v; }
        static std::string deserialize_key(std::string_view sv) { return std::string(sv); }
        static std::string deserialize_value(std::string_view sv) { return std::string(sv); }

    private:
        template <typename T>
            requires std::is_trivially_copyable_v<T>
        static std::string encode_scalar(const T& v) {
            std::string s(sizeof(T), '\0');
            if constexpr (std::is_integral_v<T>) {
                const T le = detail::to_le(v);
                std::memcpy(s.data(), &le, sizeof(T));
            } else {
                std::memcpy(s.data(), &v, sizeof(T));
            }
            return s;
        }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        static T decode_scalar(std::string_view sv) {
            T val{};
            if (sv.size() >= sizeof(T)) {
                std::memcpy(&val, sv.data(), sizeof(T));
                if constexpr (std::is_integral_v<T>) val = detail::from_le(val);
            }
            return val;
        }
    };

    // ViewSerializer: zero-copy views into WAL buffer memory.
    // Only valid while the underlying WAL buffer is alive (recovery path / zero-copy reads).
    struct ViewSerializer {
        static std::string_view serialize_key(std::string_view k) { return k; }
        static std::string_view serialize_value(std::string_view v) { return v; }
        static std::string_view deserialize_key(std::string_view sv) { return sv; }
        static std::string_view deserialize_value(std::string_view sv) { return sv; }
    };

    // ============================================================================
    // § 2  WAL Payload Codec
    // ============================================================================

    // Wire format (LE-canonical): [uint8_t op][uint32_t key_len][key_bytes][uint32_t val_len][val_bytes]
    // Length prefixes are stored little-endian for cross-host recovery portability.
    struct WalPayloadCodec {
        // Returns byte count written. `buf` must be at least encode_size(k, v) bytes.
        static std::size_t encode_size(std::string_view k, std::string_view v) noexcept {
            return sizeof(std::uint8_t) + 2 * sizeof(std::uint32_t) + k.size() + v.size();
        }

        // Zero-allocation variant: writes into caller-supplied buffer, returns bytes written.
        static std::size_t encode_to(std::byte* buf, EntryOp op,
                                      std::string_view k, std::string_view v) noexcept {
            const auto k_len = detail::to_le(static_cast<std::uint32_t>(k.size()));
            const auto v_len = detail::to_le(static_cast<std::uint32_t>(v.size()));
            std::byte* p = buf;
            *reinterpret_cast<std::uint8_t*>(p) = static_cast<std::uint8_t>(op); p += 1;
            std::memcpy(p, &k_len, 4); p += 4;
            if (!k.empty()) { std::memcpy(p, k.data(), k.size()); p += k.size(); }
            std::memcpy(p, &v_len, 4); p += 4;
            if (!v.empty()) { std::memcpy(p, v.data(), v.size()); p += v.size(); }
            return static_cast<std::size_t>(p - buf);
        }

        // Heap-allocating variant preserved for compatibility.
        static std::vector<std::byte> encode(EntryOp op, std::string_view k, std::string_view v) {
            std::vector<std::byte> buf(encode_size(k, v));
            encode_to(buf.data(), op, k, v);
            return buf;
        }

        static std::optional<std::tuple<EntryOp, std::string_view, std::string_view>>
        decode(std::span<const std::byte> payload) {
            if (payload.size() < sizeof(std::uint8_t) + 2 * sizeof(std::uint32_t)) return std::nullopt;

            const std::byte* ptr = payload.data();
            EntryOp op = static_cast<EntryOp>(*reinterpret_cast<const std::uint8_t*>(ptr));
            ptr += sizeof(std::uint8_t);

            std::uint32_t k_len = 0;
            std::memcpy(&k_len, ptr, sizeof(std::uint32_t));
            k_len = detail::from_le(k_len);
            ptr += sizeof(std::uint32_t);

            if (payload.size() < sizeof(std::uint8_t) + 2 * sizeof(std::uint32_t) + k_len) return std::nullopt;
            std::string_view k_sv{reinterpret_cast<const char*>(ptr), k_len};
            ptr += k_len;

            std::uint32_t v_len = 0;
            std::memcpy(&v_len, ptr, sizeof(std::uint32_t));
            v_len = detail::from_le(v_len);
            ptr += sizeof(std::uint32_t);

            if (payload.size() < sizeof(std::uint8_t) + 2 * sizeof(std::uint32_t) + k_len + v_len) return std::nullopt;
            std::string_view v_sv{reinterpret_cast<const char*>(ptr), v_len};

            return std::make_tuple(op, k_sv, v_sv);
        }

        static std::vector<std::byte> encode_batch(const std::vector<std::vector<std::byte>>& records) {
            std::size_t bytes = 1 + sizeof(std::uint32_t);
            for (const auto& r : records) bytes += sizeof(std::uint32_t) + r.size();
            std::vector<std::byte> out(bytes);
            auto* p = out.data();
            *reinterpret_cast<std::uint8_t*>(p) = static_cast<std::uint8_t>(EntryOp::Batch); ++p;
            const auto count = detail::to_le(static_cast<std::uint32_t>(records.size()));
            std::memcpy(p, &count, sizeof(count)); p += sizeof(count);
            for (const auto& r : records) {
                const auto n = detail::to_le(static_cast<std::uint32_t>(r.size()));
                std::memcpy(p, &n, sizeof(n)); p += sizeof(n);
                std::memcpy(p, r.data(), r.size()); p += r.size();
            }
            return out;
        }

        static std::optional<std::vector<std::vector<std::byte>>> decode_batch(std::span<const std::byte> payload) {
            if (payload.size() < 1 + sizeof(std::uint32_t) ||
                static_cast<EntryOp>(*reinterpret_cast<const std::uint8_t*>(payload.data())) != EntryOp::Batch)
                return std::nullopt;
            auto* p = payload.data() + 1;
            auto remaining = payload.size() - 1;
            std::uint32_t count{};
            std::memcpy(&count, p, sizeof(count)); p += sizeof(count); remaining -= sizeof(count);
            count = detail::from_le(count);
            std::vector<std::vector<std::byte>> out;
            out.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                if (remaining < sizeof(std::uint32_t)) return std::nullopt;
                std::uint32_t n{};
                std::memcpy(&n, p, sizeof(n)); p += sizeof(n); remaining -= sizeof(n);
                n = detail::from_le(n);
                if (n > remaining) return std::nullopt;
                out.emplace_back(p, p + n); p += n; remaining -= n;
            }
            return remaining == 0 ? std::optional{std::move(out)} : std::nullopt;
        }
    };

    // ============================================================================
    // § 3  Comparators
    // ============================================================================

    struct LexicalComparator {
        template <typename T>
        constexpr bool operator()(const T& a, const T& b) const noexcept {
            return a < b;
        }

        // Three-way comparison for scan optimisation: one branch instead of two.
        template <typename T>
        constexpr auto three_way(const T& a, const T& b) const noexcept {
            return a <=> b;
        }
    };

    // Concept: comparator that provides three_way for optimised scan termination.
    template <typename C, typename T>
    concept ThreeWayComparator = requires(const C& c, const T& a, const T& b) {
        { c.three_way(a, b) } -> std::same_as<std::strong_ordering>;
    };

} // namespace petika
