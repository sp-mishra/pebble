#pragma once

// ============================================================================
// sanchaya/session/session.hpp — Zero-Allocation Generational Handles & Sessions
// ============================================================================

#include "sanchaya/fwd.hpp"
#include <optional>
#include <functional>
#include <expected>

namespace sanchaya {

    template <class Entity>
    struct entity_slot {
        std::optional<Entity> value{std::nullopt};
        std::uint32_t generation{0};
    };

    struct session_scoped_handle_policy {
        template <class Entity>
        class handle {
        public:
            constexpr handle() = default;
            constexpr handle(const entity_slot<Entity>* slot, std::uint32_t gen, std::uint64_t epoch) noexcept
                : slot_(slot), slot_generation_(gen), session_epoch_(epoch) {}

            [[nodiscard]] constexpr bool is_valid(std::uint64_t current_epoch) const noexcept {
                return slot_ != nullptr &&
                       session_epoch_ == current_epoch &&
                       slot_generation_ == slot_->generation &&
                       slot_->value.has_value();
            }

            [[nodiscard]] auto get(std::uint64_t current_epoch) const noexcept
                -> std::expected<std::reference_wrapper<const Entity>, sanchaya_error>
            {
                if (!is_valid(current_epoch)) {
                    return std::unexpected(sanchaya_error{
                        .domain = error_domain::binding,
                        .code = 404,
                        .message = "Stale or evicted session handle"
                    });
                }
                return std::cref(*slot_->value);
            }

        private:
            const entity_slot<Entity>* slot_{nullptr};
            std::uint32_t slot_generation_{0};
            std::uint64_t session_epoch_{0};
        };
    };

    template <class Entity, class HandlePolicy = session_scoped_handle_policy>
    using entity_handle = typename HandlePolicy::template handle<Entity>;

} // namespace sanchaya
