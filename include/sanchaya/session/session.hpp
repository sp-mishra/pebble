#pragma once

// ============================================================================
// sanchaya/session/session.hpp — Zero-Allocation Generational Handles & Sessions
// ============================================================================
//
// entity_handle<T> is backed by the workspace's slot_map<T, ...> store.
// It stores a (const store*, generational_handle<T>, epoch) triple — no raw
// new, no raw pointer to entity_slot.  All is_valid / get / operator-> calls
// resolve through the slot_map's generation-checked find(), which returns
// nullptr immediately for stale or erased handles.
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "containers/associative/slot_map.hpp"
#include <expected>
#include <functional>
#include <optional>

namespace sanchaya {

    // The concrete per-entity store type used by workspace.
    // Defined here so session handles and workspace agree on the same type.
    template <class Entity>
    using entity_store_t = containers::slot_map<
        Entity,
        containers::generational_handle<Entity>,  // phantom-typed, index-based
        std::allocator<Entity>,
        containers::small_vector_storage<4>        // 4 inline slots, spills to heap
    >;

    // =========================================================================
    // session_scoped_handle_policy
    //
    // Provides handle<Entity>, the rich session handle type:
    //   is_valid(epoch)   — checks epoch and live generation
    //   is_valid()        — checks live generation only (no epoch gate)
    //   get(epoch)        — returns std::expected<cref<Entity>, sanchaya_error>
    //   operator->()      — const Entity*  (nullptr when stale/erased)
    //   operator*()       — const Entity&  (UB if stale — caller must check)
    //   key()             — returns the underlying generational_handle<Entity>
    // =========================================================================
    struct session_scoped_handle_policy {
        template <class Entity>
        class handle {
            using store_type = entity_store_t<Entity>;
            using key_type   = containers::generational_handle<Entity>;

        public:
            constexpr handle() = default;

            // Constructed by workspace::put — store must outlive handle.
            constexpr handle(const store_type* store,
                             key_type          key,
                             std::uint64_t     epoch) noexcept
                : store_(store), key_(key), session_epoch_(epoch) {}

            // is_valid(epoch): live generation AND epoch matches insertion epoch
            [[nodiscard]] constexpr bool is_valid(std::uint64_t current_epoch) const noexcept {
                return store_ != nullptr &&
                       session_epoch_ == current_epoch &&
                       store_->find(key_) != nullptr;
            }

            // is_valid(): live generation only — no epoch gate (for callers
            // that don't track epochs, e.g. single-session scenarios)
            [[nodiscard]] constexpr bool is_valid() const noexcept {
                return store_ != nullptr && store_->find(key_) != nullptr;
            }

            [[nodiscard]] auto get(std::uint64_t current_epoch) const noexcept
                -> std::expected<std::reference_wrapper<const Entity>, sanchaya_error>
            {
                auto* ptr = (store_ && session_epoch_ == current_epoch) ? store_->find(key_) : nullptr;
                if (!ptr) {
                    return std::unexpected(sanchaya_error{
                        .domain  = error_domain::binding,
                        .code    = 404,
                        .message = "Stale or evicted session handle"
                    });
                }
                return std::cref(*ptr);
            }

            [[nodiscard]] const Entity* operator->() const noexcept {
                return store_ ? store_->find(key_) : nullptr;
            }

            [[nodiscard]] const Entity& operator*() const noexcept {
                return *store_->find(key_);
            }

            // Exposes the underlying slot_map key for workspace::erase / workspace::get
            [[nodiscard]] constexpr key_type key() const noexcept { return key_; }

        private:
            const store_type* store_{nullptr};
            key_type          key_{};
            std::uint64_t     session_epoch_{0};
        };
    };

    template <class Entity, class HandlePolicy = session_scoped_handle_policy>
    using entity_handle = typename HandlePolicy::template handle<Entity>;

} // namespace sanchaya
