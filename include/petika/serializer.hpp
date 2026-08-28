#pragma once
// ============================================================================
// petika/serializer.hpp — Serialization & Comparison Policies for Petika
// ============================================================================

#include "petika/engine.hpp"

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
    // § 1  Serializer Concept & Implementations
    // ============================================================================

    template <typename S, typename K, typename V>
    concept SerializerFor = requires(const K& k, const V& v, std::string_view sv) {
        { S::serialize_key(k) } -> std::convertible_to<std::string>;
        { S::serialize_value(v) } -> std::convertible_to<std::string>;
        { S::deserialize_key(sv) } -> std::convertible_to<K>;
        { S::deserialize_value(sv) } -> std::convertible_to<V>;
    };

    struct StringSerializer {
        static std::string serialize_key(const std::string& k) { return k; }
        static std::string serialize_value(const std::string& v) { return v; }
        static std::string deserialize_key(std::string_view sv) { return std::string(sv); }
        static std::string deserialize_value(std::string_view sv) { return std::string(sv); }
    };

    struct BinarySerializer {
        template <typename T>
            requires std::is_trivially_copyable_v<T>
        static std::string serialize_key(const T& k) {
            std::string s(sizeof(T), '\0');
            std::memcpy(s.data(), &k, sizeof(T));
            return s;
        }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        static std::string serialize_value(const T& v) {
            std::string s(sizeof(T), '\0');
            std::memcpy(s.data(), &v, sizeof(T));
            return s;
        }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        static T deserialize_key(std::string_view sv) {
            T val{};
            if (sv.size() >= sizeof(T)) {
                std::memcpy(&val, sv.data(), sizeof(T));
            }
            return val;
        }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        static T deserialize_value(std::string_view sv) {
            T val{};
            if (sv.size() >= sizeof(T)) {
                std::memcpy(&val, sv.data(), sizeof(T));
            }
            return val;
        }

        // Specializations for std::string / std::string_view
        static std::string serialize_key(const std::string& k) { return k; }
        static std::string serialize_value(const std::string& v) { return v; }
        static std::string deserialize_key(std::string_view sv) { return std::string(sv); }
        static std::string deserialize_value(std::string_view sv) { return std::string(sv); }
    };

    // Helper to binary-pack WAL frame payloads
    // Format: [uint8_t op][uint32_t key_len][key_bytes][uint32_t val_len][val_bytes]
    struct WalPayloadCodec {
        static std::vector<std::byte> encode(EntryOp op, std::string_view k, std::string_view v) {
            const std::uint32_t k_len = static_cast<std::uint32_t>(k.size());
            const std::uint32_t v_len = static_cast<std::uint32_t>(v.size());
            const std::size_t total = sizeof(std::uint8_t) + sizeof(std::uint32_t) + k_len + sizeof(std::uint32_t) +
                v_len;

            std::vector<std::byte> buf(total);
            std::byte* ptr = buf.data();

            *reinterpret_cast<std::uint8_t*>(ptr) = static_cast<std::uint8_t>(op);
            ptr += sizeof(std::uint8_t);

            std::memcpy(ptr, &k_len, sizeof(std::uint32_t));
            ptr += sizeof(std::uint32_t);
            if (k_len > 0) {
                std::memcpy(ptr, k.data(), k_len);
                ptr += k_len;
            }

            std::memcpy(ptr, &v_len, sizeof(std::uint32_t));
            ptr += sizeof(std::uint32_t);
            if (v_len > 0) {
                std::memcpy(ptr, v.data(), v_len);
                ptr += v_len;
            }

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
            ptr += sizeof(std::uint32_t);

            if (payload.size() < sizeof(std::uint8_t) + 2 * sizeof(std::uint32_t) + k_len) return std::nullopt;
            std::string_view k_sv{reinterpret_cast<const char*>(ptr), k_len};
            ptr += k_len;

            std::uint32_t v_len = 0;
            std::memcpy(&v_len, ptr, sizeof(std::uint32_t));
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
            const auto count = static_cast<std::uint32_t>(records.size()); std::memcpy(p, &count, sizeof(count)); p += sizeof(count);
            for (const auto& r : records) { const auto n = static_cast<std::uint32_t>(r.size()); std::memcpy(p, &n, sizeof(n)); p += sizeof(n); std::memcpy(p, r.data(), n); p += n; }
            return out;
        }
        static std::optional<std::vector<std::vector<std::byte>>> decode_batch(std::span<const std::byte> payload) {
            if (payload.size() < 1 + sizeof(std::uint32_t) || static_cast<EntryOp>(*reinterpret_cast<const std::uint8_t*>(payload.data())) != EntryOp::Batch) return std::nullopt;
            auto* p = payload.data() + 1; auto remaining = payload.size() - 1; std::uint32_t count{}; std::memcpy(&count, p, sizeof(count)); p += sizeof(count); remaining -= sizeof(count);
            std::vector<std::vector<std::byte>> out; out.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) { if (remaining < sizeof(std::uint32_t)) return std::nullopt; std::uint32_t n{}; std::memcpy(&n, p, sizeof(n)); p += sizeof(n); remaining -= sizeof(n); if (n > remaining) return std::nullopt; out.emplace_back(p, p + n); p += n; remaining -= n; }
            return remaining == 0 ? std::optional{std::move(out)} : std::nullopt;
        }
    };

    // ============================================================================
    // § 2  Comparators
    // ============================================================================

    struct LexicalComparator {
        template <typename T>
        constexpr bool operator()(const T& a, const T& b) const noexcept {
            return a < b;
        }
    };
} // namespace petika
