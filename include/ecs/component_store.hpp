#pragma once
// ============================================================================
// ecs/component_store.hpp — SparseSet Component Storage for pebble::ecs
// ============================================================================
// Stores components densely in memory with O(1) random access and zero-hashing
// sequential component type IDs.
// ============================================================================

#include "containers/associative/SparseSet.hpp"
#include <atomic>
#include <concepts>
#include <cstdint>
#include <cstddef>
#include <utility>

namespace pebble::ecs {

template <typename C>
concept Component = std::is_destructible_v<C> && !std::is_reference_v<C>;

// Sequential component type ID generator for zero-overhead array indexing
inline std::uint32_t next_component_type_id() noexcept {
    static std::atomic<std::uint32_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

template <Component C>
struct ComponentTypeId {
    static std::uint32_t id() noexcept {
        static const std::uint32_t value = next_component_type_id();
        return value;
    }
};

// Base interface for type-erased operations (e.g. bulk entity cleanup)
class IComponentStore {
public:
    virtual ~IComponentStore() = default;
    virtual void erase_by_index(std::uint32_t entity_idx) noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

template <Component C>
class ComponentStore final : public IComponentStore {
public:
    explicit ComponentStore(std::uint32_t universe_capacity)
        : set_(universe_capacity) {}

    void set(std::uint32_t idx, const C& c) {
        if (auto r = set_.get(idx)) {
            r->get() = c;
        } else {
            (void)set_.insert(idx, c);
        }
    }

    void set(std::uint32_t idx, C&& c) {
        if (auto r = set_.get(idx)) {
            r->get() = std::move(c);
        } else {
            (void)set_.insert(idx, std::move(c));
        }
    }

    template <typename... Args>
    C& emplace(std::uint32_t idx, Args&&... args) {
        if (auto r = set_.get(idx)) {
            r->get() = C(std::forward<Args>(args)...);
            return r->get();
        }
        (void)set_.insert(idx, C(std::forward<Args>(args)...));
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

    void erase_by_index(std::uint32_t idx) noexcept override {
        (void)set_.remove(idx);
    }

    [[nodiscard]] std::size_t size() const noexcept override {
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

private:
    sparseset::SparseSet<std::uint32_t, C> set_;
};

} // namespace pebble::ecs
