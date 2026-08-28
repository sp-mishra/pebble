#pragma once
// ============================================================================
// ecs/world.hpp — High-Performance Entity-Component-System World Manager
// ============================================================================
// Zero-hashing dense array store indexing, rich query filters (With/Without/Optional),
// generational handles, and Pravaha multi-threaded parallel view execution.
// ============================================================================

#include "entity.hpp"
#include "component_store.hpp"
#include "command_buffer.hpp"
#include "query.hpp"

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <tuple>
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
        // Erase entity from all component stores via direct indexed vector walk
        for (auto& store : stores_) {
            if (store) store->erase_by_index(e.index);
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

    [[nodiscard]] Entity entity_from_index(std::uint32_t idx) const noexcept {
        return Entity{idx, generation_of(idx)};
    }

    [[nodiscard]] bool alive_index(std::uint32_t idx) const noexcept {
        if (idx == 0 || idx > slots_.size()) return false;
        return slots_[idx - 1].alive;
    }

    // ── Component Management (Zero-Hashing Flat Vector Lookup) ───────────────

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
        const std::uint32_t id = ComponentTypeId<C>::id();
        if (id >= stores_.size()) {
            stores_.resize(id + 1);
        }
        if (!stores_[id]) {
            stores_[id] = std::make_shared<ComponentStore<C>>(universe_);
        }
        return *static_cast<ComponentStore<C>*>(stores_[id].get());
    }

    template <Component C>
    [[nodiscard]] const ComponentStore<C>* try_store() const noexcept {
        const std::uint32_t id = ComponentTypeId<C>::id();
        if (id >= stores_.size() || !stores_[id]) return nullptr;
        return static_cast<const ComponentStore<C>*>(stores_[id].get());
    }

    template <Component C>
    [[nodiscard]] ComponentStore<C>* try_store() noexcept {
        const std::uint32_t id = ComponentTypeId<C>::id();
        if (id >= stores_.size() || !stores_[id]) return nullptr;
        return static_cast<ComponentStore<C>*>(stores_[id].get());
    }

    // ── Simple Join View ─────────────────────────────────────────────────────

    template <Component Primary, Component... Rest, typename Fn>
    void view(Fn&& fn) {
        auto* p_store = try_store<Primary>();
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

    // ── Rich Filtered Query: w.query<With<...>, Without<...>, Optional<...>>() ─

    template <typename WithClause, typename WithoutClause = Without<>, typename OptionalClause = Optional<void>, typename Fn>
    void query(Fn&& fn) {
        query_impl(WithClause{}, WithoutClause{}, OptionalClause{}, std::forward<Fn>(fn));
    }

    // ── Parallel View via Executor ───────────────────────────────────────────

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

    // Query helper implementation
    template <Component Primary, Component... WithRest, Component... Excludes, typename Opt, typename Fn>
    void query_impl(With<Primary, WithRest...>, Without<Excludes...>, Optional<Opt>, Fn&& fn) {
        auto* p_store = try_store<Primary>();
        if (!p_store || p_store->empty()) return;

        if constexpr (sizeof...(WithRest) > 0) {
            if ((!try_store<WithRest>() || ...)) return;
        }

        for (auto&& [idx, primary_comp] : p_store->pairs()) {
            if constexpr (sizeof...(WithRest) > 0) {
                if (!(has_idx<WithRest>(idx) && ...)) continue;
            }
            if constexpr (sizeof...(Excludes) > 0) {
                if ((has_idx<Excludes>(idx) || ...)) continue;
            }
            Entity e{idx, slots_[idx - 1].generation};
            if constexpr (std::is_same_v<Opt, void>) {
                if constexpr (sizeof...(WithRest) > 0) {
                    fn(e, primary_comp, *try_store<WithRest>()->try_get(idx)...);
                } else {
                    fn(e, primary_comp);
                }
            } else {
                Opt* opt_ptr = try_store<Opt>() ? try_store<Opt>()->try_get(idx) : nullptr;
                if constexpr (sizeof...(WithRest) > 0) {
                    fn(e, primary_comp, *try_store<WithRest>()->try_get(idx)..., opt_ptr);
                } else {
                    fn(e, primary_comp, opt_ptr);
                }
            }
        }
    }

    std::uint32_t universe_;
    std::size_t alive_count_ = 0;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_indices_;
    std::vector<std::shared_ptr<IComponentStore>> stores_; // Flat index by ComponentTypeId
    CommandBuffer cmds_;
};

// ── CommandBuffer & LocalCommandBuffer Method Implementations ───────────────

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

inline void LocalCommandBuffer::despawn(Entity e) {
    ops_.emplace_back([e](World& w) {
        w.despawn(e);
    });
}

template <typename C>
void LocalCommandBuffer::add(Entity e, C c) {
    ops_.emplace_back([e, comp = std::move(c)](World& w) mutable {
        w.add<C>(e, std::move(comp));
    });
}

template <typename C, typename... Args>
void LocalCommandBuffer::emplace(Entity e, Args&&... args) {
    ops_.emplace_back([e, ...args = std::forward<Args>(args)](World& w) mutable {
        w.emplace<C>(e, std::move(args)...);
    });
}

template <typename C>
void LocalCommandBuffer::remove(Entity e) {
    ops_.emplace_back([e](World& w) {
        w.remove<C>(e);
    });
}

} // namespace pebble::ecs
