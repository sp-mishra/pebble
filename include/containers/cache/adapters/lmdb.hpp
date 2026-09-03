#pragma once

#if __has_include(<lmdb.h>)
// adapters/lmdb.hpp — LMDB-backed write-through / read-through adapter for kosha caches.
//
// Mode: write-through (LMDB written first, then in-memory cache) +
//       read-through (cache miss loads from LMDB).

#include "containers/cache/kosha.hpp"
#include <lmdb.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace kosha::adapter {
    // ============================================================================
    // Extended error codes for LMDB operations and serialization.
    // ============================================================================
    enum class LMDBError : std::uint8_t {
        NotFound,
        BackendError,
        SerializationError,
        DeserializationError,
        EnvironmentError,
        TransactionError,
    };

    // ============================================================================
    // Serializer concept for LMDB key/value.
    // ============================================================================
    template <typename S, typename K, typename V>
    concept LMDBSerializerFor = requires(const K& k, const V& v, std::string_view sv) {
        { S::serialize_key(k) } -> std::convertible_to<std::string>;
        { S::serialize_value(v) } -> std::convertible_to<std::string>;
        { S::deserialize_key(sv) } -> std::convertible_to<K>;
        { S::deserialize_value(sv) } -> std::convertible_to<V>;
    };

    // ============================================================================
    // Default StringSerializer for string key/values.
    // ============================================================================
    struct LMDBStringSerializer {
        static std::string serialize_key(const std::string& k) { return k; }
        static std::string serialize_value(const std::string& v) { return v; }
        static std::string deserialize_key(const std::string_view sv) { return std::string(sv); }
        static std::string deserialize_value(const std::string_view sv) { return std::string(sv); }
    };

    // ============================================================================
    // LMDB Options structure
    // ============================================================================
    struct LMDBOptions {
        std::size_t map_size{10 * 1024 * 1024}; // default 10MB
        unsigned int max_dbs{4};
        unsigned int max_readers{126};
        unsigned int env_flags{0}; // e.g. MDB_NOSYNC, MDB_NOTLS, MDB_NOSUBDIR, etc.
        mdb_mode_t mode{0664};
        std::string db_name{}; // empty for default unnamed database
    };

    // ============================================================================
    // LMDBAdapter<CacheType, Serializer>
    // ============================================================================
    template <typename CacheType, typename Serializer = LMDBStringSerializer>
        requires LMDBSerializerFor<Serializer,
                                   typename CacheType::key_type,
                                   typename CacheType::value_type>
    class LMDBAdapter {
    public:
        using key_type = CacheType::key_type;
        using value_type = CacheType::value_type;
        using Error = LMDBError;

        // Opens (or creates) an LMDB environment at env_path. Takes ownership of cache.
        explicit LMDBAdapter(std::string env_path, CacheType cache,
                             LMDBOptions opts = {})
            : cache_{std::move(cache)}
              , env_path_{std::move(env_path)}
              , opts_{std::move(opts)} {
            open_env();
        }

        ~LMDBAdapter() {
            close_env();
        }

        LMDBAdapter(LMDBAdapter&& o) noexcept
            : cache_{std::move(o.cache_)}
              , env_path_{std::move(o.env_path_)}
              , opts_{std::move(o.opts_)}
              , env_{o.env_}
              , dbi_{o.dbi_} {
            o.env_ = nullptr;
            o.dbi_ = 0;
        }

        LMDBAdapter& operator=(LMDBAdapter&& o) noexcept {
            if (this != &o) {
                close_env();
                cache_ = std::move(o.cache_);
                env_path_ = std::move(o.env_path_);
                opts_ = std::move(o.opts_);
                env_ = o.env_;
                dbi_ = o.dbi_;
                o.env_ = nullptr;
                o.dbi_ = 0;
            }
            return *this;
        }

        LMDBAdapter(const LMDBAdapter&) = delete;
        LMDBAdapter& operator=(const LMDBAdapter&) = delete;

        // Write-through: LMDB written and committed first; on failure cache is untouched.
        std::expected<void, Error> put(key_type key, value_type value) {
            std::string k_str = Serializer::serialize_key(key);
            std::string v_str = Serializer::serialize_value(value);

            MDB_txn* txn = nullptr;
            int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
            if (rc != MDB_SUCCESS) return std::unexpected(Error::TransactionError);

            MDB_val mkey{k_str.size(), const_cast<char*>(k_str.data())};
            MDB_val mdata{v_str.size(), const_cast<char*>(v_str.data())};

            rc = mdb_put(txn, dbi_, &mkey, &mdata, 0);
            if (rc != MDB_SUCCESS) {
                mdb_txn_abort(txn);
                return std::unexpected(Error::BackendError);
            }

            rc = mdb_txn_commit(txn);
            if (rc != MDB_SUCCESS) return std::unexpected(Error::BackendError);

            (void)cache_.put(std::move(key), std::move(value));
            return {};
        }

        // Read-through: cache miss transparently loads from LMDB and populates cache.
        std::expected<value_type, Error> get(const key_type& key) {
            auto r = cache_.get(key);
            if (r) return *r;

            std::string k_str = Serializer::serialize_key(key);
            MDB_txn* txn = nullptr;
            int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
            if (rc != MDB_SUCCESS) return std::unexpected(Error::TransactionError);

            MDB_val mkey{k_str.size(), const_cast<char*>(k_str.data())};
            MDB_val mdata{};

            rc = mdb_get(txn, dbi_, &mkey, &mdata);
            if (rc == MDB_NOTFOUND) {
                mdb_txn_abort(txn);
                return std::unexpected(Error::NotFound);
            }
            if (rc != MDB_SUCCESS) {
                mdb_txn_abort(txn);
                return std::unexpected(Error::BackendError);
            }

            std::string_view sv{static_cast<const char*>(mdata.mv_data), mdata.mv_size};
            value_type val = Serializer::deserialize_value(sv);
            mdb_txn_abort(txn); // read-only transaction done

            (void)cache_.put(key, val);
            return val;
        }

        // Erase removes from cache and deletes from LMDB in a transaction.
        std::expected<void, Error> erase(const key_type& key) {
            cache_.erase(key);

            std::string k_str = Serializer::serialize_key(key);
            MDB_txn* txn = nullptr;
            int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
            if (rc != MDB_SUCCESS) return std::unexpected(Error::TransactionError);

            MDB_val mkey{k_str.size(), const_cast<char*>(k_str.data())};
            rc = mdb_del(txn, dbi_, &mkey, nullptr);
            if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
                mdb_txn_abort(txn);
                return std::unexpected(Error::BackendError);
            }

            rc = mdb_txn_commit(txn);
            if (rc != MDB_SUCCESS) return std::unexpected(Error::BackendError);

            return {};
        }

        // Clears in-memory cache only; does not delete data in LMDB.
        void clear() { cache_.clear(); }

        // Clears both in-memory cache and drops all data in LMDB database.
        std::expected<void, Error> clear_all() {
            cache_.clear();
            MDB_txn* txn = nullptr;
            int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
            if (rc != MDB_SUCCESS) return std::unexpected(Error::TransactionError);

            rc = mdb_drop(txn, dbi_, 0); // 0 = empty db, don't delete handle
            if (rc != MDB_SUCCESS) {
                mdb_txn_abort(txn);
                return std::unexpected(Error::BackendError);
            }

            rc = mdb_txn_commit(txn);
            if (rc != MDB_SUCCESS) return std::unexpected(Error::BackendError);

            return {};
        }

        // Warms in-memory cache from LMDB up to max_keys.
        std::size_t load_all(std::size_t max_keys = std::numeric_limits<std::size_t>::max()) {
            MDB_txn* txn = nullptr;
            int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
            if (rc != MDB_SUCCESS) return 0;

            MDB_cursor* cursor = nullptr;
            rc = mdb_cursor_open(txn, dbi_, &cursor);
            if (rc != MDB_SUCCESS) {
                mdb_txn_abort(txn);
                return 0;
            }

            std::size_t loaded = 0;
            MDB_val mkey{};
            MDB_val mdata{};
            rc = mdb_cursor_get(cursor, &mkey, &mdata, MDB_FIRST);
            while (rc == MDB_SUCCESS && loaded < max_keys) {
                std::string_view k_sv{static_cast<const char*>(mkey.mv_data), mkey.mv_size};
                std::string_view v_sv{static_cast<const char*>(mdata.mv_data), mdata.mv_size};
                key_type k = Serializer::deserialize_key(k_sv);
                value_type v = Serializer::deserialize_value(v_sv);
                (void)cache_.put(std::move(k), std::move(v));
                ++loaded;
                rc = mdb_cursor_get(cursor, &mkey, &mdata, MDB_NEXT);
            }

            mdb_cursor_close(cursor);
            mdb_txn_abort(txn);
            return loaded;
        }

        // Explicit sync to disk.
        std::expected<void, Error> sync(bool force = true) {
            int rc = mdb_env_sync(env_, force ? 1 : 0);
            if (rc != MDB_SUCCESS) return std::unexpected(Error::BackendError);
            return {};
        }

        [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cache_.capacity(); }

        [[nodiscard]] MDB_env* env() noexcept { return env_; }
        [[nodiscard]] const MDB_env* env() const noexcept { return env_; }
        [[nodiscard]] MDB_dbi dbi() const noexcept { return dbi_; }

    private:
        CacheType cache_;
        std::string env_path_;
        LMDBOptions opts_;
        MDB_env* env_{nullptr};
        MDB_dbi dbi_{0};

        void open_env() {
            int rc = mdb_env_create(&env_);
            if (rc != MDB_SUCCESS)
                throw std::runtime_error("LMDBAdapter: failed to create env: " +
                    std::string(mdb_strerror(rc)));

            if (opts_.map_size > 0) {
                rc = mdb_env_set_mapsize(env_, opts_.map_size);
                if (rc != MDB_SUCCESS) {
                    close_env();
                    throw std::runtime_error("LMDBAdapter: failed to set mapsize: " +
                        std::string(mdb_strerror(rc)));
                }
            }

            if (opts_.max_dbs > 0) {
                rc = mdb_env_set_maxdbs(env_, opts_.max_dbs);
                if (rc != MDB_SUCCESS) {
                    close_env();
                    throw std::runtime_error("LMDBAdapter: failed to set maxdbs: " +
                        std::string(mdb_strerror(rc)));
                }
            }

            if (opts_.max_readers > 0) {
                rc = mdb_env_set_maxreaders(env_, opts_.max_readers);
                if (rc != MDB_SUCCESS) {
                    close_env();
                    throw std::runtime_error("LMDBAdapter: failed to set maxreaders: " +
                        std::string(mdb_strerror(rc)));
                }
            }

            // Ensure directory exists if not NOSUBDIR
            if (!(opts_.env_flags & MDB_NOSUBDIR)) {
                std::error_code ec;
                std::filesystem::create_directories(env_path_, ec);
            }

            rc = mdb_env_open(env_, env_path_.c_str(), opts_.env_flags, opts_.mode);
            if (rc != MDB_SUCCESS) {
                close_env();
                throw std::runtime_error("LMDBAdapter: failed to open env at " +
                    env_path_ + ": " + std::string(mdb_strerror(rc)));
            }

            // Open DBI
            MDB_txn* txn = nullptr;
            rc = mdb_txn_begin(env_, nullptr, 0, &txn);
            if (rc != MDB_SUCCESS) {
                close_env();
                throw std::runtime_error("LMDBAdapter: failed to start init txn: " +
                    std::string(mdb_strerror(rc)));
            }

            const char* name = opts_.db_name.empty() ? nullptr : opts_.db_name.c_str();
            rc = mdb_dbi_open(txn, name, MDB_CREATE, &dbi_);
            if (rc != MDB_SUCCESS) {
                mdb_txn_abort(txn);
                close_env();
                throw std::runtime_error("LMDBAdapter: failed to open dbi: " +
                    std::string(mdb_strerror(rc)));
            }

            rc = mdb_txn_commit(txn);
            if (rc != MDB_SUCCESS) {
                close_env();
                throw std::runtime_error("LMDBAdapter: failed to commit init txn: " +
                    std::string(mdb_strerror(rc)));
            }
        }

        void close_env() noexcept {
            if (env_) {
                if (dbi_ != 0) {
                    mdb_dbi_close(env_, dbi_);
                    dbi_ = 0;
                }
                mdb_env_close(env_);
                env_ = nullptr;
            }
        }
    };

    // Convenience alias
    template <typename K, typename V, typename Ser = LMDBStringSerializer>
    using LMDBLRUCache = LMDBAdapter<core::Cache<K, V, core::LRUPolicy<K>,
                                                 core::FlatHashStorage<K, V>>, Ser>;
} // namespace kosha::adapter

#endif // __has_include(<lmdb.h>)
