#pragma once
// ============================================================================
// petika/adapters/kosha.hpp — Kosha Cache Adapter for Petika Storage Engine
// ============================================================================
//
// Allows using any Petika Store (e.g. Petika<JournaledSkipEngine>, SkipStore)
// as a durable read-through / write-through storage adapter for Kosha caches.
// ============================================================================

#include "petika/petika.hpp"
#include "containers/cache/kosha.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace kosha::adapter {

    enum class PetikaAdapterError : std::uint8_t {
        NotFound,
        BackendError,
        SerializationError,
        DeserializationError
    };

    template <typename CacheType, typename PetikaStore = petika::StringSkipStore>
    class PetikaAdapter {
    public:
        using key_type = typename CacheType::key_type;
        using value_type = typename CacheType::value_type;
        using Error = PetikaAdapterError;

        explicit PetikaAdapter(std::filesystem::path db_dir, CacheType cache, petika::PetikaOptions opts = {})
            : cache_{std::move(cache)} {
            opts.db_dir = std::move(db_dir);
            store_ = std::make_unique<PetikaStore>(opts);
        }

        explicit PetikaAdapter(CacheType cache, std::unique_ptr<PetikaStore> custom_store)
            : cache_{std::move(cache)}, store_{std::move(custom_store)} {}

        ~PetikaAdapter() = default;
        PetikaAdapter(PetikaAdapter&&) noexcept = default;
        PetikaAdapter& operator=(PetikaAdapter&&) noexcept = default;
        PetikaAdapter(const PetikaAdapter&) = delete;
        PetikaAdapter& operator=(const PetikaAdapter&) = delete;

        // Write-through: persists to Petika WAL/Engine first, then in-memory cache
        std::expected<void, Error> put(key_type key, value_type value) {
            auto res = store_->put(key, value);
            if (!res) return std::unexpected(Error::BackendError);

            (void)cache_.put(std::move(key), std::move(value));
            return {};
        }

        // Read-through: cache hit returns immediately; cache miss loads from Petika store
        std::expected<value_type, Error> get(const key_type& key) {
            auto r = cache_.get(key);
            if (r) return *r;

            auto store_res = store_->get(key);
            if (!store_res) {
                return std::unexpected(Error::NotFound);
            }

            (void)cache_.put(key, *store_res);
            return *store_res;
        }

        // Erase: deletes in Petika store and in-memory cache
        std::expected<void, Error> erase(const key_type& key) {
            cache_.erase(key);
            auto res = store_->erase(key);
            if (!res && res.error() != petika::StorageError::NotFound) {
                return std::unexpected(Error::BackendError);
            }
            return {};
        }

        void clear() { cache_.clear(); }

        std::expected<void, Error> clear_all() {
            cache_.clear();
            auto res = store_->clear();
            if (!res) return std::unexpected(Error::BackendError);
            return {};
        }

        // Warms in-memory cache from Petika store
        std::size_t load_all(std::size_t max_keys = std::numeric_limits<std::size_t>::max()) {
            std::size_t loaded = 0;
            store_->for_each([&](const auto& entry) {
                if (loaded < max_keys) {
                    (void)cache_.put(entry.key, entry.value);
                    ++loaded;
                }
            });
            return loaded;
        }

        [[nodiscard]] std::size_t size() const noexcept { return cache_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return cache_.capacity(); }
        PetikaStore& store() noexcept { return *store_; }
        const PetikaStore& store() const noexcept { return *store_; }

    private:
        CacheType cache_;
        std::unique_ptr<PetikaStore> store_;
    };

    template <typename K, typename V>
    using PetikaLRUCache = PetikaAdapter<core::Cache<K, V, core::LRUPolicy<K>, core::FlatHashStorage<K, V>>,
                                         petika::Petika<petika::JournaledSkipEngine<K, V, petika::LexicalComparator>,
                                                        petika::BinarySerializer,
                                                        petika::LexicalComparator>>;

} // namespace kosha::adapter
