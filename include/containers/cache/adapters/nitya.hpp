#pragma once

#if __has_include("nitya/nitya.hpp")
// adapters/nitya.hpp — Nitya WAL-backed write-through / read-through adapter for kosha caches.
//
// Mode: write-through (Record appended and synced to Nitya WAL first, then in-memory cache) +
//       read-through (Recovery scan or point lookup from WAL log stream).

#include "containers/cache/kosha.hpp"
#include "nitya/nitya.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kosha::adapter {
    // ============================================================================
    // Extended error codes for Nitya WAL adapter operations.
    // ============================================================================
    enum class NityaAdapterError : std::uint8_t {
        NotFound,
        BackendError,
        SerializationError,
        DeserializationError,
        CorruptedRecord,
    };

    // ============================================================================
    // Binary serialization helper for Key-Value pairs into continuous byte buffer
    // Layout: [uint32_t key_len][key_bytes][uint32_t val_len][val_bytes]
    // ============================================================================
    template <typename S, typename K, typename V>
    concept NityaSerializerFor = requires(const K& k, const V& v, std::string_view sv) {
        { S::serialize_key(k) } -> std::convertible_to<std::string>;
        { S::serialize_value(v) } -> std::convertible_to<std::string>;
        { S::deserialize_key(sv) } -> std::convertible_to<K>;
        { S::deserialize_value(sv) } -> std::convertible_to<V>;
    };

    struct NityaStringSerializer {
        static std::string serialize_key(const std::string& k) { return k; }
        static std::string serialize_value(const std::string& v) { return v; }
        static std::string deserialize_key(const std::string_view sv) { return std::string(sv); }
        static std::string deserialize_value(const std::string_view sv) { return std::string(sv); }
    };

    // ============================================================================
    // NityaAdapter<CacheType, Serializer, WalEngine>
    // ============================================================================
    template <
        typename CacheType,
        typename Serializer = NityaStringSerializer,
        typename WalEngine = nitya::wal<>>
        requires NityaSerializerFor<Serializer,
                                    typename CacheType::key_type,
                                    typename CacheType::value_type>
    class NityaAdapter {
    public:
        using key_type = CacheType::key_type;
        using value_type = CacheType::value_type;
        using Error = NityaAdapterError;

        enum class OpType : std::uint8_t {
            Put = 1,
            Erase = 2,
            Clear = 3
        };

        explicit NityaAdapter(std::filesystem::path wal_dir, CacheType cache, nitya::wal_options opts = {})
            : cache_{std::move(cache)} {
            opts.wal_dir = std::move(wal_dir);
            wal_ = std::make_unique<WalEngine>(opts);
        }

        explicit NityaAdapter(CacheType cache, std::unique_ptr<WalEngine> custom_wal)
            : cache_{std::move(cache)}, wal_{std::move(custom_wal)} {}

        ~NityaAdapter() = default;
        NityaAdapter(NityaAdapter&&) noexcept = default;
        NityaAdapter& operator=(NityaAdapter&&) noexcept = default;
        NityaAdapter(const NityaAdapter&) = delete;
        NityaAdapter& operator=(const NityaAdapter&) = delete;

        // Write-through: record encoded as binary Op and appended to Nitya WAL first.
        std::expected<void, Error> put(key_type key, value_type value) {
            const std::string k_str = Serializer::serialize_key(key);
            const std::string v_str = Serializer::serialize_value(value);

            auto encoded = encode_entry(OpType::Put, k_str, v_str);
            auto append_res = wal_->append(std::span{encoded.data(), encoded.size()});
            if (!append_res) return std::unexpected(Error::BackendError);

            if (auto sync_res = wal_->sync(); !sync_res) {
                return std::unexpected(Error::BackendError);
            }

            (void)cache_.put(std::move(key), std::move(value));
            return {};
        }

        // Read-through: cache miss reads from in-memory cache, falls back to full replay / scan if needed.
        std::expected<value_type, Error> get(const key_type& key) {
            auto r = cache_.get(key);
            if (r) return *r;

            // Cache miss: scan WAL from beginning to latest to find current valid value
            std::string target_k = Serializer::serialize_key(key);
            std::optional<value_type> found_val;

            for (auto stream = wal_->recover(0); const auto& rec : stream) {
                auto parsed = decode_entry(rec.payload);
                if (!parsed) continue;

                auto [op, k_sv, v_sv] = *parsed;
                if (k_sv == target_k) {
                    if (op == OpType::Put) {
                        found_val = Serializer::deserialize_value(v_sv);
                    }
                    else if (op == OpType::Erase || op == OpType::Clear) {
                        found_val = std::nullopt;
                    }
                }
                else if (op == OpType::Clear) {
                    found_val = std::nullopt;
                }
            }

            if (found_val) {
                (void)cache_.put(key, *found_val);
                return *found_val;
            }

            return std::unexpected(Error::NotFound);
        }

        // Erase: appends an Erase marker to Nitya WAL and removes from cache.
        std::expected<void, Error> erase(const key_type& key) {
            cache_.erase(key);

            const std::string k_str = Serializer::serialize_key(key);
            auto encoded = encode_entry(OpType::Erase, k_str, "");
            auto append_res = wal_->append(std::span{encoded.data(), encoded.size()});
            if (!append_res) return std::unexpected(Error::BackendError);

            if (auto sync_res = wal_->sync(); !sync_res) {
                return std::unexpected(Error::BackendError);
            }

            return {};
        }

        // Clears in-memory cache only.
        void clear() { cache_.clear(); }

        // Appends Clear marker to WAL and clears cache.
        std::expected<void, Error> clear_all() {
            cache_.clear();
            auto encoded = encode_entry(OpType::Clear, "", "");
            auto append_res = wal_->append(std::span{encoded.data(), encoded.size()});
            if (!append_res) return std::unexpected(Error::BackendError);

            if (auto sync_res = wal_->sync(); !sync_res) {
                return std::unexpected(Error::BackendError);
            }

            return {};
        }

        // Warms in-memory cache by scanning all historical records in Nitya WAL.
        std::size_t load_all(const std::size_t max_keys = std::numeric_limits<std::size_t>::max()) {
            std::size_t loaded = 0;
            for (auto stream = wal_->recover(0); const auto& rec : stream) {
                auto parsed = decode_entry(rec.payload);
                if (!parsed) continue;

                auto [op, k_sv, v_sv] = *parsed;
                if (op == OpType::Put) {
                    key_type k = Serializer::deserialize_key(k_sv);
                    value_type v = Serializer::deserialize_value(v_sv);
                    (void)cache_.put(std::move(k), std::move(v));
                    ++loaded;
                }
                else if (op == OpType::Erase) {
                    key_type k = Serializer::deserialize_key(k_sv);
                    cache_.erase(k);
                }
                else if (op == OpType::Clear) {
                    cache_.clear();
                    loaded = 0;
                }

                if (loaded >= max_keys) break;
            }
            return loaded;
        }

        [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cache_.capacity(); }
        WalEngine& wal() noexcept { return *wal_; }
        const WalEngine& wal() const noexcept { return *wal_; }

    private:
        CacheType cache_;
        std::unique_ptr<WalEngine> wal_;

        static std::vector<std::byte> encode_entry(OpType op, const std::string_view k, const std::string_view v) {
            const auto k_len = static_cast<std::uint32_t>(k.size());
            const auto v_len = static_cast<std::uint32_t>(v.size());
            const std::size_t total = sizeof(std::uint8_t) + sizeof(std::uint32_t) + k_len + sizeof(std::uint32_t) + v_len;

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

        static std::optional<std::tuple<OpType, std::string_view, std::string_view>>
        decode_entry(const std::span<const std::byte> payload) {
            if (payload.size() < sizeof(std::uint8_t) + 2 * sizeof(std::uint32_t)) return std::nullopt;

            const std::byte* ptr = payload.data();
            auto op = static_cast<OpType>(*reinterpret_cast<const std::uint8_t*>(ptr));
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
    };

    template <typename K, typename V, typename Ser = NityaStringSerializer>
    using NityaLRUCache = NityaAdapter<core::Cache<K, V, core::LRUPolicy<K>,
                                                   core::FlatHashStorage<K, V>>, Ser>;
} // namespace kosha::adapter

#endif // __has_include("nitya/nitya.hpp")
