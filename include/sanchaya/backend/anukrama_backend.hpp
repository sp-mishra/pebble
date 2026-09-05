#pragma once

// ============================================================================
// sanchaya/backend/anukrama_backend.hpp — In-Memory MVCC Storage Backend
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "containers/anukrama/anukrama.hpp"
#include <expected>
#include <optional>
#include <functional>
#include <cstdint>

namespace sanchaya::backend {

    template <
        class Key,
        class Value,
        class Compare = std::less<Key>,
        template <class> class NodeAllocator = anukrama::heap_node_pool
    >
    class anukrama_storage_backend {
    public:
        using store_type = anukrama::store<
            Key,
            Value,
            Compare,
            anukrama::skip_list_index,
            anukrama::atomic_clock,
            anukrama::snapshot_isolation,
            NodeAllocator,
            anukrama::global_shared_lock,
            anukrama::multiset_snapshot_registry
        >;

        auto put(const Key& key, const Value& val) -> std::expected<std::uint64_t, sanchaya_error> {
            auto txn = store_.begin();
            txn.put(key, val);
            auto res = txn.commit();
            if (!res) {
                return std::unexpected(sanchaya_error{
                    .domain = error_domain::concurrency,
                    .code = 409,
                    .message = "Anukrama MVCC conflict detected"
                });
            }
            return *res;
        }

        [[nodiscard]] auto get_snapshot() const noexcept {
            return store_.snapshot_at_current();
        }

        [[nodiscard]] auto get_latest(const Key& key) const -> std::optional<Value> {
            auto snap = store_.snapshot_at_current();
            auto res = snap.get(key);
            if (res.has_value()) {
                return *res;
            }
            return std::nullopt;
        }

        void prune_history() noexcept {
            store_.prune();
        }

        [[nodiscard]] auto& store() noexcept { return store_; }
        [[nodiscard]] const auto& store() const noexcept { return store_; }

    private:
        store_type store_{};
    };

} // namespace sanchaya::backend
