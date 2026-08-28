#pragma once

#if __has_include(<rocksdb/db.h>)
// adapters/rocksdb.hpp — RocksDB-backed write-through / read-through adapter for kosha caches.
//
// Mode: write-through (DB written first, then cache) + read-through (cache miss loads from DB).

#include "containers/cache/kosha.hpp"

#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"

#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace kosha::adapter {
    // ============================================================================
    // Extended error codes for I/O and serialization failures.
    // ============================================================================
    enum class AdapterError : std::uint8_t {
        NotFound,
        BackendError, // RocksDB operation failed
        SerializationError, // serialize_key / serialize_value threw or returned invalid data
        DeserializationError, // deserialize_key / deserialize_value failed
    };

    // ============================================================================
    // Serializer concept.
    // ============================================================================
    template <typename S, typename K, typename V>
    concept SerializerFor = requires(const K& k, const V& v, std::string_view sv) {
        { S::serialize_key(k) } -> std::convertible_to<std::string>;
        { S::serialize_value(v) } -> std::convertible_to<std::string>;
        { S::deserialize_key(sv) } -> std::convertible_to<K>;
        { S::deserialize_value(sv) } -> std::convertible_to<V>;
    };

    // ============================================================================
    // Default serializer for Cache<std::string, std::string>.
    // ============================================================================
    struct StringSerializer {
        static std::string serialize_key(const std::string& k) { return k; }
        static std::string serialize_value(const std::string& v) { return v; }
        static std::string deserialize_key(std::string_view sv) { return std::string(sv); }
        static std::string deserialize_value(std::string_view sv) { return std::string(sv); }
    };

    // ============================================================================
    // RocksDBAdapter<CacheType, Serializer>
    //
    // Write-through: DB is written first; on DB failure the cache is not updated,
    // keeping them consistent.
    // Read-through:  cache miss transparently reads from RocksDB and warms cache.
    // ============================================================================
    template <typename CacheType, typename Serializer = StringSerializer>
        requires SerializerFor<Serializer,
                               typename CacheType::key_type,
                               typename CacheType::value_type>
    class RocksDBAdapter {
    public:
        using key_type = typename CacheType::key_type;
        using value_type = typename CacheType::value_type;
        using Error = AdapterError;

        // Opens (or creates) a RocksDB at db_path. Takes ownership of cache.
        explicit RocksDBAdapter(std::string db_path, CacheType cache,
                                rocksdb::Options opts = {})
            : cache_{std::move(cache)}
              , db_path_{std::move(db_path)} {
            opts.create_if_missing = true;
            std::unique_ptr<rocksdb::DB> db_ptr;
            rocksdb::Status s = rocksdb::DB::Open(opts, db_path_, &db_ptr);
            if (!s.ok())
                throw std::runtime_error("RocksDBAdapter: failed to open " +
                    db_path_ + ": " + s.ToString());
            db_ = std::move(db_ptr);
        }

        RocksDBAdapter(RocksDBAdapter&&) = default;
        RocksDBAdapter& operator=(RocksDBAdapter&&) = default;
        RocksDBAdapter(const RocksDBAdapter&) = delete;
        RocksDBAdapter& operator=(const RocksDBAdapter&) = delete;

        // Write-through: DB written first; cache updated only on DB success.
        std::expected<void, Error> put(key_type key, value_type value) {
            auto s = db_->Put(write_opts_,
                              Serializer::serialize_key(key),
                              Serializer::serialize_value(value));
            if (!s.ok()) return std::unexpected(Error::BackendError);
            (void)cache_.put(std::move(key), std::move(value));
            return {};
        }

        // Read-through: cache miss transparently loads from RocksDB.
        std::expected<value_type, Error> get(const key_type& key) {
            auto r = cache_.get(key);
            if (r) return *r;

            std::string raw;
            auto s = db_->Get(read_opts_, Serializer::serialize_key(key), &raw);
            if (s.IsNotFound()) return std::unexpected(Error::NotFound);
            if (!s.ok()) return std::unexpected(Error::BackendError);

            value_type val = Serializer::deserialize_value(raw);
            (void)cache_.put(key, val);
            return val;
        }

        // Removes from cache and issues a RocksDB Delete.
        // Returns success/failure of the DB operation (key need not have existed).
        std::expected<void, Error> erase(const key_type& key) {
            cache_.erase(key);
            auto s = db_->Delete(write_opts_, Serializer::serialize_key(key));
            if (!s.ok()) return std::unexpected(Error::BackendError);
            return {};
        }

        // Clears in-memory cache only; does NOT wipe RocksDB.
        void clear() { cache_.clear(); }

        // Warms the in-memory cache from RocksDB up to max_keys entries.
        std::size_t load_all(std::size_t max_keys = std::numeric_limits<std::size_t>::max()) {
            rocksdb::ReadOptions scan_opts;
            scan_opts.fill_cache = false;
            std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(scan_opts));
            std::size_t loaded = 0;
            for (it->SeekToFirst(); it->Valid() && loaded < max_keys; it->Next()) {
                key_type k = Serializer::deserialize_key(it->key().ToStringView());
                value_type v = Serializer::deserialize_value(it->value().ToStringView());
                (void)cache_.put(std::move(k), std::move(v));
                ++loaded;
            }
            return loaded;
        }

        [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cache_.capacity(); }

        rocksdb::DB& db() noexcept { return *db_; }
        const rocksdb::DB& db() const noexcept { return *db_; }

    private:
        CacheType cache_;
        std::unique_ptr<rocksdb::DB> db_;
        std::string db_path_;
        rocksdb::WriteOptions write_opts_;
        rocksdb::ReadOptions read_opts_;
    };

    // Convenience alias
    template <typename K, typename V, typename Ser = StringSerializer>
    using RocksDBLRUCache = RocksDBAdapter<core::Cache<K, V, core::LRUPolicy<K>,
                                                       core::FlatHashStorage<K, V>>, Ser>;
} // namespace kosha::adapter

#endif // __has_include(<rocksdb/db.h>)
