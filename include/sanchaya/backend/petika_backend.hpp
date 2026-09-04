#pragma once

// ============================================================================
// sanchaya/backend/petika_backend.hpp — In-Memory / WAL Storage Engine via Petika
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "petika/petika.hpp"
#include <string>
#include <sstream>
#include <expected>

namespace sanchaya::backend {

    template <class Key = std::string, class Value = std::string>
    class petika_storage_backend {
    public:
        explicit petika_storage_backend(std::string db_dir = "./petika_sanchaya_db")
            : options_{.db_dir = std::move(db_dir), .segment_size = 16 * 1024 * 1024, .sync_on_write = false},
              store_(options_) {}

        auto put(const Key& key, const Value& val) -> std::expected<bool, sanchaya_error> {
            store_.put(key, val);
            return true;
        }

        auto get(const Key& key) -> std::expected<std::optional<Value>, sanchaya_error> {
            auto res = store_.get(key);
            if (res.has_value()) {
                return std::optional<Value>{*res};
            }
            return std::optional<Value>{std::nullopt};
        }

        auto erase(const Key& key) -> std::expected<bool, sanchaya_error> {
            store_.erase(key);
            return true;
        }

        template <class Fn>
        void scan_prefix(std::string_view prefix, Fn&& fn) {
            store_.scan(prefix, std::forward<Fn>(fn));
        }

    private:
        petika::PetikaOptions options_;
        petika::SkipStore<Key, Value> store_;
    };

} // namespace sanchaya::backend
