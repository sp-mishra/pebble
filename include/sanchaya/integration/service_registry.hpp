#pragma once

// ============================================================================
// sanchaya/integration/service_registry.hpp — Compile-Time Tagged Service Registries
// ============================================================================

#include "sanchaya/fwd.hpp"
#include <tuple>
#include <utility>

namespace sanchaya::integration {

    enum class service_role : std::uint8_t {
        logical_rewrite_provider,
        physical_candidate_provider,
        cost_provider,
        constrained_optimizer,
        learned_estimator,
        statistics_provider,
        transaction_coordinator,
        execution_provider
    };

    template <akshara::fixed_string Tag, class Service>
    struct service_instance {
        static constexpr auto tag = Tag;
        Service service;
    };

    template <class... ServiceInstances>
    class service_registry {
    public:
        constexpr explicit service_registry(ServiceInstances... instances)
            : instances_(std::move(instances)...) {}

        template <akshara::fixed_string Tag>
        [[nodiscard]] constexpr auto& get() noexcept {
            constexpr std::size_t idx = find_service_tag_index<Tag, ServiceInstances...>();
            static_assert(idx < sizeof...(ServiceInstances), "Requested service Tag was not found in service_registry");
            return std::get<idx>(instances_).service;
        }

        template <akshara::fixed_string Tag>
        [[nodiscard]] constexpr const auto& get() const noexcept {
            constexpr std::size_t idx = find_service_tag_index<Tag, ServiceInstances...>();
            static_assert(idx < sizeof...(ServiceInstances), "Requested service Tag was not found in service_registry");
            return std::get<idx>(instances_).service;
        }

    private:
        std::tuple<ServiceInstances...> instances_;

        template <akshara::fixed_string TargetTag, class... Instances>
        static consteval std::size_t find_service_tag_index() noexcept {
            std::size_t found = sizeof...(Instances);
            std::size_t idx = 0;
            ([&] {
                if constexpr (Instances::tag == TargetTag) {
                    found = idx;
                }
                ++idx;
            }(), ...);
            return found;
        }
    };

    template <class... ServiceInstances>
    [[nodiscard]] constexpr auto make_service_registry(ServiceInstances&&... instances) {
        return service_registry<std::decay_t<ServiceInstances>...>(std::forward<ServiceInstances>(instances)...);
    }

} // namespace sanchaya::integration
