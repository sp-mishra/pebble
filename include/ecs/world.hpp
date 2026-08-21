#pragma once
// ============================================================================
// ecs/world.hpp — High-Performance Entity-Component-System World Manager
// ============================================================================
// Zero-virtual hot path, SparseSet-backed component stores, generation-safe
// handles, O(min(A, B)) multi-component query joins, and Pravaha multi-threaded
// parallel view dispatch.
// ============================================================================

#include "entity.hpp"
#include "component_store.hpp"
#include "command_buffer.hpp"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pebble::ecs {

class World {
public:
    explicit World(std::uint32_t universe_capacity = kDefaultUniverse)
        : universe_(universe_capacity) {
        slots_.reserve(std::min<std::size_t>(universe_capacity, 1024));
    }

    // ── Entity Lifecycle ─────────────────────────────────────────────────────

    [[nodiscard]] Entity spawn() {
        std::uint32_t idx;
        if (!free_indices_.empty()) {
            idx = free_indices_.back();
            free_indices_.pop_back();
        } else {
            idx = static_cast<std::uint32_t>(slots_.size()) + 1; // 1-indexed (0 reserved for null)
            slots_.push_back(Slot{});
        }
        Slot& s = slot(idx);
        s.alive = true;
        ++alive_count_;
        return Entity{idx, s.generation};
    }

    [[nodiscard]] bool alive(Entity e) const noexcept {
        if (e.is_null() || e.index == 0 || e.index > slots_.size()) {
            return false;
        }
        const Slot& s = slots_[e.index - 1];
        return s.alive && s.generation == e.generation;
    }

    void despawn(Entity e) {
        if (!alive(e)) return;
        // Erase entity from all component stores
        for (auto& [ti, store] : stores_) {
            store->erase_by_index(e.index);
        }
        Slot& s = slot(e.index);
        s.alive = false;
        ++s.generation; // Invalidate all existing handles
        free_indices_.push_back(e.index);
        --alive_count_;
    }

    [[nodiscard]] std::size_t entity_count() const noexcept {
        return alive_count_;
    }

    [[nodiscard]] std::uint32_t generation_of(std::uint32_t idx) const noexcept {
        return (idx >= 1 && idx <= slots_.size()) ? slots_[idx - 1].generation : 0;
    }

    // ── Component Management ─────────────────────────────────────────────────

    template <Component C>
    void add(Entity e, C c) {
        if (!alive(e)) return;
        store<C>().set(e.index, std::move(c));
    }

    template <Component C, typename... Args>
    C& emplace(Entity e, Args&&... args) {
        if (!alive(e)) {
            static C dummy{};
            return dummy;
        }
        return store<C>().emplace(e.index, std::forward<Args>(args)...);
    }

    template <Component C>
    void remove(Entity e) {
        if (!alive(e)) return;
        if (auto* s = try_store<C>()) {
            s->erase_by_index(e.index);
        }
    }

    template <Component C>
    [[nodiscard]] bool has(Entity e) const noexcept {
        if (!alive(e)) return false;
        auto* s = try_store<C>();
        return s && s->has(e.index);
    }

    template <Component C>
    [[nodiscard]] C* get(Entity e) noexcept {
        if (!alive(e)) return nullptr;
        auto* s = try_store<C>();
        return s ? s->try_get(e.index) : nullptr;
    }

    template <Component C>
    [[nodiscard]] const C* get(Entity e) const noexcept {
        if (!alive(e)) return nullptr;
        auto* s = try_store<C>();
        return s ? s->try_get(e.index) : nullptr;
    }

    // Unchecked raw index access for fast broadphase/tree queries
    template <Component C>
    [[nodiscard]] C* get_by_index(std::uint32_t idx) noexcept {
        auto* s = try_store<C>();
        return s ? s->try_get(idx) : nullptr;
    }

    template <Component C>
    [[nodiscard]] const C* get_by_index(std::uint32_t idx) const noexcept {
        auto* s = try_store<C>();
        return s ? s->try_get(idx) : nullptr;
    }

    template <Component C>
    [[nodiscard]] ComponentStore<C>& store() {
        const std::type_index ti{typeid(C)};
        auto it = stores_.find(ti);
        if (it == stores_.end()) {
            auto sp = std::make_shared<ComponentStore<C>>(universe_);
            it = stores_.emplace(ti, sp).first;
        }
        return *static_cast<ComponentStore<C>*>(it->second.get());
    }

    template <Component C>
    [[nodiscard]] const ComponentStore<C>* try_store() const noexcept {
        auto it = stores_.find(std::type_index{typeid(C)});
        return it == stores_.end() ? nullptr : static_cast<const ComponentStore<C>*>(it->second.get());
    }

    template <Component C>
    [[nodiscard]] ComponentStore<C>* try_store() noexcept {
        auto it = stores_.find(std::type_index{typeid(C)});
        return it == stores_.end() ? nullptr : static_cast<ComponentStore<C>*>(it->second.get());
    }

    // ── Single-Threaded Join View ────────────────────────────────────────────
    // Walks the smallest component store and performs O(1) membership checks on the rest.
    template <Component Primary, Component... Rest, typename Fn>
    void view(Fn&& fn) {
        auto* p_store = try_store<Primary>();
        if (!p_store || p_store->empty()) return;

        // If Rest components are present, verify their stores exist
        if constexpr (sizeof...(Rest) > 0) {
            if ((!try_store<Rest>() || ...)) return;
        }

        for (auto&& [idx, comp] : p_store->pairs()) {
            if constexpr (sizeof...(Rest) > 0) {
                if (!(has_idx<Rest>(idx) && ...)) continue;
                Entity e{idx, slots_[idx - 1].generation};
                fn(e, comp, *try_store<Rest>()->try_get(idx)...);
            } else {
                Entity e{idx, slots_[idx - 1].generation};
                fn(e, comp);
            }
        }
    }

    // Read-only const view
    template <Component Primary, Component... Rest, typename Fn>
    void view(Fn&& fn) const {
        const auto* p_store = try_store<Primary>();
        if (!p_store || p_store->empty()) return;

        if constexpr (sizeof...(Rest) > 0) {
            if ((!try_store<Rest>() || ...)) return;
        }

        for (auto&& [idx, comp] : p_store->pairs()) {
            if constexpr (sizeof...(Rest) > 0) {
                if (!(has_idx<Rest>(idx) && ...)) continue;
                Entity e{idx, slots_[idx - 1].generation};
                fn(e, comp, *try_store<Rest>()->try_get(idx)...);
            } else {
                Entity e{idx, slots_[idx - 1].generation};
                fn(e, comp);
            }
        }
    }

    // ── Parallel View via Executor ───────────────────────────────────────────
    // Splits the primary component store into chunks and invokes Fn concurrently
    template <Component Primary, Component... Rest, typename Executor, typename Fn>
    void par_view(Executor& executor, Fn&& fn, std::size_t chunk_size = 256) {
        auto* p_store = try_store<Primary>();
        if (!p_store || p_store->empty()) return;

        if constexpr (sizeof...(Rest) > 0) {
            if ((!try_store<Rest>() || ...)) return;
        }

        const auto keys_span = p_store->keys();
        const std::size_t total = keys_span.size();
        if (total == 0) return;

        executor.for_range(total, [&](std::size_t i) {
            const std::uint32_t idx = keys_span[i];
            if constexpr (sizeof...(Rest) > 0) {
                if (!(has_idx<Rest>(idx) && ...)) return;
                Entity e{idx, slots_[idx - 1].generation};
                fn(e, *p_store->try_get(idx), *try_store<Rest>()->try_get(idx)...);
            } else {
                Entity e{idx, slots_[idx - 1].generation};
                fn(e, *p_store->try_get(idx));
            }
        }, chunk_size);
    }

    // ── Command Buffer ───────────────────────────────────────────────────────
    [[nodiscard]] CommandBuffer& commands() noexcept { return cmds_; }
    void flush_commands() { cmds_.flush(*this); }

    [[nodiscard]] std::uint32_t universe() const noexcept { return universe_; }

private:
    struct Slot {
        std::uint32_t generation = 1;
        bool alive = false;
    };

    Slot& slot(std::uint32_t idx) { return slots_[idx - 1]; }
    const Slot& slot(std::uint32_t idx) const { return slots_[idx - 1]; }

    template <Component C>
    [[nodiscard]] bool has_idx(std::uint32_t idx) const noexcept {
        const auto* s = try_store<C>();
        return s && s->has(idx);
    }

    std::uint32_t universe_;
    std::size_t alive_count_ = 0;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_indices_;
    std::unordered_map<std::type_index, std::shared_ptr<IComponentStore>> stores_;
    CommandBuffer cmds_;
};

// ── CommandBuffer Method Implementations ─────────────────────────────────────

inline void CommandBuffer::despawn(Entity e) {
    std::lock_guard<std::mutex> lock(mutex_);
    ops_.emplace_back([e](World& w) {
        w.despawn(e);
    });
}

template <typename C>
void CommandBuffer::add(Entity e, C c) {
    std::lock_guard<std::mutex> lock(mutex_);
    ops_.emplace_back([e, comp = std::move(c)](World& w) mutable {
        w.add<C>(e, std::move(comp));
    });
}

template <typename C, typename... Args>
void CommandBuffer::emplace(Entity e, Args&&... args) {
    std::lock_guard<std::mutex> lock(mutex_);
    ops_.emplace_back([e, ...args = std::forward<Args>(args)](World& w) mutable {
        w.emplace<C>(e, std::move(args)...);
    });
}

template <typename C>
void CommandBuffer::remove(Entity e) {
    std::lock_guard<std::mutex> lock(mutex_);
    ops_.emplace_back([e](World& w) {
        w.remove<C>(e);
    });
}

} // namespace pebble::ecs

