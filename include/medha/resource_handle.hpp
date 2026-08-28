#pragma once
// =============================================================================
// medha/resource_handle.hpp — instance-level resource contract
//
// C++23, header-only, no virtual, no macros.
//
// resource_handle<R>: wraps an R& providing transaction-aware read/stage/
//   validate/commit/rollback operations.
//
// transactional_resource concept: gates all transaction operations.
//
// resource_id is minted from a process-level slot_map + generational_handle.
// Stale-safe phantom-typed identity for locking, conflict checks, telemetry.
// =============================================================================

#include "medha/fwd.hpp"
#include "medha/key.hpp"
#include "medha/resource_traits.hpp"
#include "containers/associative/slot_map.hpp"

#include <expected>

namespace medha {
    // ============================================================================
    // transactional_resource concept
    // ============================================================================

    template <class R>
    concept transactional_resource = resource_traits<R>::transactional;

    // ============================================================================
    // resource_handle<R> — instance-level runtime contract
    // ============================================================================

    template <transactional_resource R>
    class resource_handle {
    public:
        using traits = resource_traits<R>;
        using key_type = typename traits::key_type;
        using value_type = typename traits::value_type;

        explicit resource_handle(R& resource, resource_id id) noexcept
            : resource_(resource), id_(id) {}

        [[nodiscard]] resource_id id() const noexcept { return id_; }
        [[nodiscard]] R& resource() noexcept { return resource_; }
        [[nodiscard]] const R& resource() const noexcept { return resource_; }

        // These are thin dispatch points; actual implementation lives in R via ADL/CPO.
        // R must provide:
        //   tx_read(R&, transaction_context&, key_type)        → expected<value_type, tx_error>
        //   tx_stage(R&, transaction_context&, key_type, value_type) → expected<void, tx_error>
        //   tx_validate(R&, transaction_context&)              → expected<void, tx_error>
        //   tx_commit(R&, transaction_context&)                → expected<void, tx_error>
        //   tx_rollback(R&, transaction_context&) noexcept

        [[nodiscard]] std::expected<value_type, tx_error>
        read(transaction_context& ctx, key_type key) {
            return tx_read(resource_, ctx, key);
        }

        [[nodiscard]] std::expected<void, tx_error>
        stage(transaction_context& ctx, key_type key, value_type value)
            requires (traits::value_trivially_copyable || traits::resource_stages_values) {
            return tx_stage(resource_, ctx, key, std::move(value));
        }

        [[nodiscard]] std::expected<void, tx_error>
        validate(transaction_context& ctx) {
            return tx_validate(resource_, ctx);
        }

        [[nodiscard]] std::expected<void, tx_error>
        commit(transaction_context& ctx) {
            return tx_commit(resource_, ctx);
        }

        void rollback(transaction_context& ctx) noexcept {
            tx_rollback(resource_, ctx);
        }

    private:
        R& resource_;
        resource_id id_;
    };

    // ============================================================================
    // resource_registry — process-level slot_map for stable resource identity
    // ============================================================================

    struct resource_registry_tag {};

    using resource_registry_handle = containers::generational_handle<resource_registry_tag>;

    // Map from resource_registry_handle → resource_id for consistent lookup.
    // Callers create one per process/engine; not a singleton.
    class resource_registry {
    public:
        resource_registry() = default;

        [[nodiscard]] resource_id register_resource() {
            auto h = map_.insert(0);
            return resource_id{
                .index = h.index,
                .generation = h.generation,
            };
        }

        void unregister(resource_id id) {
            resource_registry_handle h{id.index, id.generation};
            map_.erase(h);
        }

        [[nodiscard]] bool valid(resource_id id) const noexcept {
            resource_registry_handle h{id.index, id.generation};
            return map_.find(h) != nullptr;
        }

    private:
        containers::slot_map<std::uint32_t, resource_registry_handle> map_;
    };
} // namespace medha
