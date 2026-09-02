#pragma once
// Typed Medha resource adapter for Anukrama MVCC stores.

#include "containers/anukrama/anukrama.hpp"
#include "medha/context.hpp"

#include <algorithm>
#include <expected>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace medha::adapters {
    template <class Key, class Value, class Compare = std::less<>,
              template <class, class, class> class IndexPolicy = anukrama::skip_list_index,
              anukrama::externally_advanceable_clock Clock = anukrama::atomic_clock,
              anukrama::conflict_policy ConflictPolicy = anukrama::snapshot_isolation>
    class anukrama_resource {
    public:
        using key_type = Key;
        using value_type = Value;
        using store_type = anukrama::store<Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>;

        explicit anukrama_resource(store_type& store) noexcept : store_{store} {}

    public: // Adapter CPO implementation detail; not part of the recommended call surface.
        struct staged_write {
            Key key;
            Value value;
            anukrama::timestamp observed_version{};
        };

        struct attempt {
            transaction_context* context{};
            std::optional<typename store_type::snapshot> snapshot{};
            std::vector<staged_write> writes{};
        };

        [[nodiscard]] static tx_error conflict_error() noexcept {
            return {tx_status::conflict, "Anukrama version conflict"};
        }

        [[nodiscard]] static tx_error not_found_error() noexcept {
            return {tx_status::rejected, "Anukrama key not found"};
        }

        [[nodiscard]] bool equivalent(const Key& left, const Key& right) const {
            const Compare compare{};
            return !compare(left, right) && !compare(right, left);
        }

        attempt& ensure_attempt_locked(transaction_context& context) {
            auto it = std::find_if(attempts_.begin(), attempts_.end(), [&](const attempt& value) {
                return value.context == std::addressof(context);
            });
            if (it == attempts_.end()) {
                attempts_.push_back({std::addressof(context), std::nullopt, {}});
                it = std::prev(attempts_.end());
                it->snapshot.emplace(store_.snapshot_at_current());
            }
            return *it;
        }

        auto find_attempt_locked(transaction_context& context) {
            return std::find_if(attempts_.begin(), attempts_.end(), [&](const attempt& value) {
                return value.context == std::addressof(context);
            });
        }

        store_type& store_;
        mutable std::mutex mutex_;
        std::vector<attempt> attempts_;
    };

    template <class Key, class Value, class Compare, template <class, class, class> class IndexPolicy,
              class Clock, class ConflictPolicy>
    std::expected<Value, tx_error>
    tx_read(anukrama_resource<Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>& resource,
            transaction_context& context, Key key) {
        std::lock_guard lock{resource.mutex_};
        auto& attempt = resource.ensure_attempt_locked(context);
        for (auto it = attempt.writes.rbegin(); it != attempt.writes.rend(); ++it) {
            if (resource.equivalent(it->key, key)) return it->value;
        }
        auto value = attempt.snapshot->get(key);
        if (!value)
            return std::unexpected(value.error() == anukrama::error::not_found
                                       ? resource.not_found_error()
                                       : resource.conflict_error());
        return *std::move(value);
    }

    template <class Key, class Value, class Compare, template <class, class, class> class IndexPolicy,
              class Clock, class ConflictPolicy>
    std::expected<void, tx_error>
    tx_stage(anukrama_resource<Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>& resource,
             transaction_context& context, Key key, Value value) {
        std::lock_guard lock{resource.mutex_};
        auto& attempt = resource.ensure_attempt_locked(context);
        for (auto& staged : attempt.writes) {
            if (resource.equivalent(staged.key, key)) {
                staged.value = std::move(value);
                return {};
            }
        }
        attempt.writes.push_back({
            key, std::move(value),
            resource.store_.version_at(key, attempt.snapshot->timestamp_value())
        });
        return {};
    }

    template <class Key, class Value, class Compare, template <class, class, class> class IndexPolicy,
              class Clock, class ConflictPolicy>
    std::expected<void, tx_error>
    tx_validate(anukrama_resource<Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>& resource,
                transaction_context& context) {
        std::lock_guard lock{resource.mutex_};
        const auto it = resource.find_attempt_locked(context);
        if (it == resource.attempts_.end()) return {};
        for (const auto& staged : it->writes) {
            if (resource.store_.version_of(staged.key) != staged.observed_version)
                return std::unexpected(resource.conflict_error());
        }
        return {};
    }

    template <class Key, class Value, class Compare, template <class, class, class> class IndexPolicy,
              class Clock, class ConflictPolicy>
    std::expected<void, tx_error>
    tx_commit(anukrama_resource<Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>& resource,
              transaction_context& context) {
        std::lock_guard lock{resource.mutex_};
        const auto it = resource.find_attempt_locked(context);
        if (it == resource.attempts_.end()) return {};
        std::vector < typename anukrama_resource<
            Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>::store_type::observation > observed;
        std::vector < typename anukrama_resource<
            Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>::store_type::write > writes;
        observed.reserve(it->writes.size());
        writes.reserve(it->writes.size());
        for (const auto& staged : it->writes) {
            observed.push_back({staged.key, staged.observed_version});
            writes.push_back({staged.key, staged.value});
        }
        auto committed = resource.store_.commit_if_unchanged(observed, writes);
        if (!committed) return std::unexpected(resource.conflict_error());
        resource.attempts_.erase(it);
        return {};
    }

    template <class Key, class Value, class Compare, template <class, class, class> class IndexPolicy,
              class Clock, class ConflictPolicy>
    void tx_rollback(anukrama_resource<Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>& resource,
                     transaction_context& context) noexcept {
        std::lock_guard lock{resource.mutex_};
        const auto it = resource.find_attempt_locked(context);
        if (it != resource.attempts_.end()) resource.attempts_.erase(it);
    }

    template <class Key, class Value, class Compare, template <class, class, class> class IndexPolicy,
              class Clock, class ConflictPolicy>
    version_stamp tx_version(const anukrama_resource<Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>& resource,
                             const Key& key) noexcept {
        return {resource.store_.version_of(key), 0};
    }
} // namespace medha::adapters

namespace medha {
    template <class Key, class Value, class Compare, template <class, class, class> class IndexPolicy,
              class Clock, class ConflictPolicy>
    struct resource_traits<adapters::anukrama_resource<Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>> {
        static constexpr bool transactional = true;
        static constexpr bool value_trivially_copyable = std::is_trivially_copyable_v<Value>;
        static constexpr bool value_move_only = false;
        static constexpr bool resource_stages_values = true;
        static constexpr bool supports_snapshot = true;
        static constexpr bool supports_rollback = true;
        static constexpr commit_capability commit_protocol = commit_capability::atomic_multi_key_within_resource;
        static constexpr bool aba_safe = true;
        static constexpr bool supports_range_reads = true;
        static constexpr bool supports_predicate_validation = false;
        using key_type = Key;
        using value_type = Value;
    };
} // namespace medha
