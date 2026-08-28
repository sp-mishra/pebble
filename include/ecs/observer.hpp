#pragma once
// ============================================================================
// ecs/observer.hpp — Reactive Component Lifecycle Observers for pebble::ecs
// ============================================================================
// Zero virtual functions, zero heap allocation for typical observer counts (via
// SmallVector), and zero macros.
//
// Triggers callbacks automatically on component addition / removal.
// ============================================================================

#include "entity.hpp"
#include "component_store.hpp"
#include "containers/dynamic/SmallVector.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace pebble::ecs {

template <Component C>
struct OnAdd {
    Entity entity;
    C& component;
};

template <Component C>
struct OnRemove {
    Entity entity;
};

struct ObserverCallback {
    void* context = nullptr;
    void (*invoke)(void* ctx, Entity e, void* comp) noexcept = nullptr;
};

class ObserverRegistry {
public:
    using CallbackList = containers::dynamic::SmallVector<ObserverCallback, 64>; // 64 bytes inline storage

    template <Component C, typename Fn>
    void register_on_add(Fn* fn_ptr) {
        const std::uint32_t type_id = ComponentTypeId<C>::id();
        if (type_id >= on_add_observers_.size()) {
            on_add_observers_.resize(type_id + 1);
        }
        on_add_observers_[type_id].push_back(ObserverCallback{
            .context = reinterpret_cast<void*>(fn_ptr),
            .invoke = [](void* ctx, Entity e, void* comp) noexcept {
                auto* typed_fn = reinterpret_cast<Fn*>(ctx);
                (*typed_fn)(OnAdd<C>{e, *static_cast<C*>(comp)});
            }
        });
    }

    template <Component C, typename Fn>
    void register_on_remove(Fn* fn_ptr) {
        const std::uint32_t type_id = ComponentTypeId<C>::id();
        if (type_id >= on_remove_observers_.size()) {
            on_remove_observers_.resize(type_id + 1);
        }
        on_remove_observers_[type_id].push_back(ObserverCallback{
            .context = reinterpret_cast<void*>(fn_ptr),
            .invoke = [](void* ctx, Entity e, void* /*comp*/) noexcept {
                auto* typed_fn = reinterpret_cast<Fn*>(ctx);
                (*typed_fn)(OnRemove<C>{e});
            }
        });
    }

    void notify_add(std::uint32_t type_id, Entity e, void* comp) const {
        if (type_id < on_add_observers_.size()) {
            for (const auto& cb : on_add_observers_[type_id]) {
                if (cb.invoke) cb.invoke(cb.context, e, comp);
            }
        }
    }

    void notify_remove(std::uint32_t type_id, Entity e) const {
        if (type_id < on_remove_observers_.size()) {
            for (const auto& cb : on_remove_observers_[type_id]) {
                if (cb.invoke) cb.invoke(cb.context, e, nullptr);
            }
        }
    }

    void clear() noexcept {
        on_add_observers_.clear();
        on_remove_observers_.clear();
    }

private:
    std::vector<CallbackList> on_add_observers_;
    std::vector<CallbackList> on_remove_observers_;
};

static_assert(!std::is_polymorphic_v<ObserverRegistry>, "ObserverRegistry must have zero virtual functions");

} // namespace pebble::ecs
