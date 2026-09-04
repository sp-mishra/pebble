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
            constexpr std::size_t idx = find_tag_index<Tag>();
            return std::get<idx>(instances_).service;
        }

        template <akshara::fixed_string Tag>
        [[nodiscard]] constexpr const auto& get() const noexcept {
            constexpr std::size_t idx = find_tag_index<Tag>();
            return std::get<idx>(instances_).service;
        }

    private:
        std::tuple<ServiceInstances...> instances_;

        template <akshara::fixed_string Tag>
        static constexpr std::size_t find_tag_index() noexcept {
            constexpr std::size_t n = sizeof...(ServiceInstances);
            std::size_t found_idx = n;
            std::size_t current = 0;
            auto check = [&]<class I>() {
                if (I::tag == Tag) {
                    found_idx = current;
                }
                ++current;
            };
            (check.template operator()<ServiceInstances>(), ...);
            return found_idx;
        }
    };

    template <class... ServiceInstances>
    [[nodiscard]] constexpr auto make_service_registry(ServiceInstances&&... instances) {
        return service_registry<std::decay_t<ServiceInstances>...>(std::forward<ServiceInstances>(instances)...);
    }

} // namespace sanchaya::integration
