#pragma once
// ============================================================================
// ecs/change_detection.hpp — Component Change Detection for pebble::ecs
// ============================================================================
// Zero virtual functions, zero macros, modern C++23.
// ============================================================================

#include "entity.hpp"
#include "component_store.hpp"

#include <cstdint>
#include <type_traits>

namespace pebble::ecs {

// Filter tag indicating component must have mutated after a given tick
template <Component C>
struct Changed {};

template <Component C>
class ChangedView {
public:
    ChangedView(ComponentStore<C>& store, std::uint32_t since_tick)
        : store_(store), since_tick_(since_tick) {}

    template <typename Fn>
    void each(Fn&& fn) {
        if (store_.last_mutation_tick() <= since_tick_) return;
        for (auto&& [idx, comp] : store_.pairs()) {
            fn(idx, comp);
        }
    }

private:
    ComponentStore<C>& store_;
    std::uint32_t since_tick_;
};

static_assert(!std::is_polymorphic_v<ChangedView<int>>, "ChangedView must have zero virtual functions");

} // namespace pebble::ecs
