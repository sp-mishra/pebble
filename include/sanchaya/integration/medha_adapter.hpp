#pragma once

// ============================================================================
// sanchaya/integration/medha_adapter.hpp — Medha Transactional Adapter for Sanchaya
// ============================================================================

#include "sanchaya/backend/anukrama_backend.hpp"
#include "sanchaya/backend/petika_backend.hpp"
#include "medha/medha.hpp"
#include <string>
#include <expected>
#include <vector>
#include <algorithm>
#include <mutex>
#include <optional>

namespace sanchaya::integration {

    template <class Entity>
    struct entity_table_resource {
        using key_type = std::string;
        using value_type = Entity;

        backend::anukrama_storage_backend<key_type, value_type>* memory_store{nullptr};
        backend::petika_storage_backend<key_type, std::string>* durable_store{nullptr};

        struct staged_write {
            key_type key;
            value_type value;
        };

        struct attempt {
            medha::transaction_context* context{nullptr};
            std::vector<staged_write> writes{};
        };

        mutable std::mutex mutex_{};
        std::vector<attempt> attempts_{};

        attempt& ensure_attempt_locked(medha::transaction_context& context) {
            auto it = std::find_if(attempts_.begin(), attempts_.end(), [&](const attempt& a) {
                return a.context == std::addressof(context);
            });
            if (it == attempts_.end()) {
                attempts_.push_back({std::addressof(context), {}});
                it = std::prev(attempts_.end());
            }
            return *it;
        }

        auto find_attempt_locked(medha::transaction_context& context) {
            return std::find_if(attempts_.begin(), attempts_.end(), [&](const attempt& a) {
                return a.context == std::addressof(context);
            });
        }
    };

} // namespace sanchaya::integration

template <class Entity>
struct medha::resource_traits<sanchaya::integration::entity_table_resource<Entity>> {
    static constexpr bool transactional = true;
    static constexpr bool value_trivially_copyable = std::is_trivially_copyable_v<Entity>;
    static constexpr bool resource_stages_values = true;
    static constexpr bool supports_snapshot = true;
    static constexpr bool supports_rollback = true;
    static constexpr medha::commit_capability commit_protocol =
        medha::commit_capability::atomic_multi_key_within_resource;
    static constexpr bool aba_safe = true;
    using key_type = std::string;
    using value_type = Entity;
};

namespace sanchaya::integration {

    template <class Entity>
    auto tx_read(entity_table_resource<Entity>& r, medha::transaction_context& ctx, const std::string& key)
        -> std::expected<Entity, medha::tx_error>
    {
        std::lock_guard lock{r.mutex_};
        auto& attempt = r.ensure_attempt_locked(ctx);
        for (auto it = attempt.writes.rbegin(); it != attempt.writes.rend(); ++it) {
            if (it->key == key) {
                return it->value;
            }
        }
        if (r.memory_store) {
            if (auto val = r.memory_store->get_latest(key)) {
                return *val;
            }
        }
        if (r.durable_store) {
            auto val_str = r.durable_store->get(key);
            if (val_str && val_str->has_value()) {
                auto decoded = backend::object_codec<Entity>::decode(**val_str);
                if (decoded) {
                    return *decoded;
                }
            }
        }
        return std::unexpected(medha::tx_error{
            .status = medha::tx_status::aborted,
            .message = "Key not found"
        });
    }

    template <class Entity>
    auto tx_stage(entity_table_resource<Entity>& r, medha::transaction_context& ctx, const std::string& key, const Entity& val)
        -> std::expected<void, medha::tx_error>
    {
        std::lock_guard lock{r.mutex_};
        auto& attempt = r.ensure_attempt_locked(ctx);
        for (auto& staged : attempt.writes) {
            if (staged.key == key) {
                staged.value = val;
                return {};
            }
        }
        attempt.writes.push_back({key, val});
        return {};
    }

    template <class Entity>
    auto tx_validate(entity_table_resource<Entity>&, medha::transaction_context&)
        -> std::expected<void, medha::tx_error>
    {
        return {};
    }

    template <class Entity>
    auto tx_commit(entity_table_resource<Entity>& r, medha::transaction_context& ctx)
        -> std::expected<void, medha::tx_error>
    {
        std::lock_guard lock{r.mutex_};
        auto it = r.find_attempt_locked(ctx);
        if (it == r.attempts_.end()) return {};

        for (const auto& staged : it->writes) {
            if (r.memory_store) {
                auto res = r.memory_store->put(staged.key, staged.value);
                if (!res) {
                    return std::unexpected(medha::tx_error{
                        .status = medha::tx_status::conflict,
                        .message = "Memory store commit conflict"
                    });
                }
            }
            if (r.durable_store) {
                std::string encoded = backend::object_codec<Entity>::encode(staged.value);
                auto res = r.durable_store->put(staged.key, encoded);
                if (!res) {
                    return std::unexpected(medha::tx_error{
                        .status = medha::tx_status::conflict,
                        .message = "Durable store commit conflict"
                    });
                }
            }
        }
        r.attempts_.erase(it);
        return {};
    }

    template <class Entity>
    void tx_rollback(entity_table_resource<Entity>& r, medha::transaction_context& ctx) noexcept {
        std::lock_guard lock{r.mutex_};
        auto it = r.find_attempt_locked(ctx);
        if (it != r.attempts_.end()) {
            r.attempts_.erase(it);
        }
    }

} // namespace sanchaya::integration
