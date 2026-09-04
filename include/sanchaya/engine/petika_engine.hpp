#pragma once

// ============================================================================
// sanchaya/engine/petika_engine.hpp — Direct Native Petika Storage Operations
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "sanchaya/backend/petika_backend.hpp"
#include <string_view>
#include <expected>
#include <optional>

namespace sanchaya::engine {

    template <class Key, class Value>
    class petika_execution_driver {
    public:
        explicit petika_execution_driver(backend::petika_kv_backend<Key, Value>& backend)
            : backend_(backend) {}

        auto point_get(const Key& key) -> std::expected<std::optional<Value>, sanchaya_error> {
            return backend_.get(key);
        }

        auto point_put(const Key& key, const Value& value) -> std::expected<bool, sanchaya_error> {
            return backend_.put(key, value);
        }

        auto point_erase(const Key& key) -> std::expected<bool, sanchaya_error> {
            return backend_.erase(key);
        }

        template <class Visitor>
        void prefix_scan(std::string_view prefix, Visitor&& visitor) {
            backend_.scan(prefix, std::forward<Visitor>(visitor));
        }

    private:
        backend::petika_kv_backend<Key, Value>& backend_;
    };

} // namespace sanchaya::engine
