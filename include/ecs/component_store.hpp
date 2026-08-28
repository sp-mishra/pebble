#pragma once
// ============================================================================
// ecs/component_store.hpp — SparseSet Component Storage for pebble::ecs
// ============================================================================
// Stores components densely in memory with O(1) random access, zero-hashing
// sequential component type IDs, per-component mutation tick tracking, and
// type-erased dispatch via constexpr function-pointer tables.
//
// Strictly ZERO virtual functions, ZERO macros, header-only C++23.
// ============================================================================

#include "entity.hpp"
#include "containers/associative/SparseSet.hpp"

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace pebble::ecs {

template <typename C>
concept Component = std::is_destructible_v<C> && !std::is_reference_v<C>;

// Sequential component type ID generator for zero-overhead array indexing
inline std::uint32_t next_component_type_id() noexcept {
    static std::atomic<std::uint32_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

template <Component C>
struct ComponentTypeId;

// ── ErasedStore: Zero-Virtual Type-Erased Function Pointer Dispatch ─────────

struct ErasedStore {
    void* data = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    void (*erase)(void*, std::uint32_t) noexcept = nullptr;
    void (*set_raw)(void*, std::uint32_t, void*) = nullptr;
    void (*set_tick)(void*, std::uint32_t) noexcept = nullptr;
    std::size_t (*size)(const void*) noexcept = nullptr;
    bool (*has)(const void*, std::uint32_t) noexcept = nullptr;
    std::uint32_t type_id = 0;

    void release() noexcept {
        if (data && destroy) {
            destroy(data);
            data = nullptr;
        }
    }

    void erase_by_index(std::uint32_t idx) const noexcept {
        if (data && erase) erase(data, idx);
    }

    void insert_raw(std::uint32_t idx, void* payload) const {
        if (data && set_raw) set_raw(data, idx, payload);
    }

    void update_world_tick(std::uint32_t tick) const noexcept {
        if (data && set_tick) set_tick(data, tick);
    }

    [[nodiscard]] std::size_t get_size() const noexcept {
        return (data && size) ? size(data) : 0;
    }

    [[nodiscard]] bool contains(std::uint32_t idx) const noexcept {
        return (data && has) ? has(data, idx) : false;
    }
};

static_assert(!std::is_polymorphic_v<ErasedStore>, "ErasedStore must have zero virtual functions");

using StoreFactoryFn = ErasedStore (*)(std::uint32_t universe_capacity);

inline std::vector<StoreFactoryFn>& store_factories() {
    static std::vector<StoreFactoryFn> factories;
    return factories;
}

// ── ComponentStore<C>: Concrete Dense Component Store ───────────────────────

template <Component C>
class ComponentStore {
public:
    using value_type = C;

    explicit ComponentStore(std::uint32_t universe_capacity = kDefaultUniverse)
        : set_(universe_capacity) {}

    ~ComponentStore() = default;

    ComponentStore(const ComponentStore&) = delete;
    ComponentStore& operator=(const ComponentStore&) = delete;
    ComponentStore(ComponentStore&&) noexcept = default;
    ComponentStore& operator=(ComponentStore&&) noexcept = default;

    void set(std::uint32_t idx, const C& c) {
        set_.insert_or_update(idx, c);
        tick_ = current_world_tick_;
    }

    void set(std::uint32_t idx, C&& c) {
        set_.insert_or_update(idx, std::move(c));
        tick_ = current_world_tick_;
    }

    template <typename... Args>
    C& emplace(std::uint32_t idx, Args&&... args) {
        tick_ = current_world_tick_;
        if (auto r = set_.get(idx)) {
            r->get() = C(std::forward<Args>(args)...);
            return r->get();
        }
        (void)set_.insert_or_update(idx, C(std::forward<Args>(args)...));
        return set_.get(idx)->get();
    }

    [[nodiscard]] bool has(std::uint32_t idx) const noexcept {
        return set_.contains(idx);
    }

    [[nodiscard]] C* try_get(std::uint32_t idx) noexcept {
        auto r = set_.get(idx);
        return r ? &r->get() : nullptr;
    }

    [[nodiscard]] const C* try_get(std::uint32_t idx) const noexcept {
        auto r = set_.get(idx);
        return r ? &r->get() : nullptr;
    }

    void erase_by_index(std::uint32_t idx) noexcept {
        (void)set_.remove(idx);
        tick_ = current_world_tick_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return set_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return set_.empty();
    }

    // Dense iteration over {entity_index, C&} pairs with cache locality
    [[nodiscard]] auto pairs() noexcept {
        return set_.all_pairs();
    }

    [[nodiscard]] auto pairs() const noexcept {
        return set_.all_pairs();
    }

    // Dense iteration over entity keys
    [[nodiscard]] auto keys() const noexcept {
        return set_.all_keys();
    }

    // Dense iteration over component values
    [[nodiscard]] auto values() noexcept {
        return set_.all_values();
    }

    [[nodiscard]] auto values() const noexcept {
        return set_.all_values();
    }

    // Change detection tick
    [[nodiscard]] std::uint32_t last_mutation_tick() const noexcept {
        return tick_;
    }

    void set_current_world_tick(std::uint32_t tick) noexcept {
        current_world_tick_ = tick;
    }

    // Static thunk factory for ErasedStore
    static ErasedStore make_erased(ComponentStore<C>* store) noexcept {
        return ErasedStore{
            .data = store,
            .destroy = [](void* p) noexcept {
                delete static_cast<ComponentStore<C>*>(p);
            },
            .erase = [](void* p, std::uint32_t idx) noexcept {
                static_cast<ComponentStore<C>*>(p)->erase_by_index(idx);
            },
            .set_raw = [](void* p, std::uint32_t idx, void* payload) {
                static_cast<ComponentStore<C>*>(p)->set(idx, std::move(*static_cast<C*>(payload)));
            },
            .set_tick = [](void* p, std::uint32_t tick) noexcept {
                static_cast<ComponentStore<C>*>(p)->set_current_world_tick(tick);
            },
            .size = [](const void* p) noexcept -> std::size_t {
                return static_cast<const ComponentStore<C>*>(p)->size();
            },
            .has = [](const void* p, std::uint32_t idx) noexcept -> bool {
                return static_cast<const ComponentStore<C>*>(p)->has(idx);
            },
            .type_id = ComponentTypeId<C>::id(),
        };
    }

private:
    sparseset::SparseSet<std::uint32_t, C> set_;
    std::uint32_t tick_ = 0;
    std::uint32_t current_world_tick_ = 0;
};

static_assert(!std::is_polymorphic_v<ComponentStore<int>>, "ComponentStore must have zero virtual functions");

template <Component C>
struct ComponentTypeId {
    static std::uint32_t id() noexcept {
        static const std::uint32_t value = []() {
            std::uint32_t new_id = next_component_type_id();
            auto& f = store_factories();
            if (new_id >= f.size()) {
                f.resize(new_id + 1, nullptr);
            }
            f[new_id] = [](std::uint32_t universe_capacity) -> ErasedStore {
                auto* typed = new ComponentStore<C>(universe_capacity);
                return ComponentStore<C>::make_erased(typed);
            };
            return new_id;
        }();
        return value;
    }
};

} // namespace pebble::ecs
