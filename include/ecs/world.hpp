#pragma once
// ============================================================================
// ecs/world.hpp — High-Performance Entity-Component-System World Manager
// ============================================================================
// Policy-based, zero-virtual functions, zero macros, auto-lead-store selection,
// component bitmasks, reactive observers, entity relations, and parallel views.
// ============================================================================

#include "entity.hpp"
#include "storage_policy.hpp"
#include "component_store.hpp"
#include "command_buffer.hpp"
#include "query.hpp"
#include "observer.hpp"
#include "relation.hpp"
#include "change_detection.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace pebble::ecs {

template <
    StoragePolicy   Storage   = SparseSetStoragePolicy,
    AllocPolicy     Alloc     = ArenaAllocPolicy,
    typename        Scheduler = AutoSchedulerPolicy,
    typename        Sparse    = PagedSparsePolicy
>
class BasicWorld {
public:
    explicit BasicWorld(std::uint32_t universe_capacity = kDefaultUniverse)
        : universe_(universe_capacity) {
        slots_.reserve(std::min<std::size_t>(universe_capacity, 1024));
    }

    ~BasicWorld() {
        for (auto& store : stores_) {
            store.release();
        }
        stores_.clear();
    }

    BasicWorld(const BasicWorld&) = delete;
    BasicWorld& operator=(const BasicWorld&) = delete;

    BasicWorld(BasicWorld&& other) noexcept
        : universe_(other.universe_),
          alive_count_(other.alive_count_),
          current_tick_(other.current_tick_),
          slots_(std::move(other.slots_)),
          free_indices_(std::move(other.free_indices_)),
          stores_(std::move(other.stores_)),
          observers_(std::move(other.observers_)),
          relations_(std::move(other.relations_)),
          cmds_(std::move(other.cmds_)) {
        other.alive_count_ = 0;
    }

    BasicWorld& operator=(BasicWorld&& other) noexcept {
        if (this != &other) {
            for (auto& store : stores_) {
                store.release();
            }
            universe_ = other.universe_;
            alive_count_ = other.alive_count_;
            current_tick_ = other.current_tick_;
            slots_ = std::move(other.slots_);
            free_indices_ = std::move(other.free_indices_);
            stores_ = std::move(other.stores_);
            observers_ = std::move(other.observers_);
            relations_ = std::move(other.relations_);
            cmds_ = std::move(other.cmds_);
            other.alive_count_ = 0;
        }
        return *this;
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
        s.component_mask = 0;
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

        Slot& s = slot(e.index);

        // Notify observers and erase from active stores via bitmask
        const std::uint64_t mask = s.component_mask;
        for (std::size_t id = 0; id < stores_.size(); ++id) {
            if (id < 64 && ((mask & (1ULL << id)) == 0)) {
                continue; // Skip stores the entity does not have
            }
            if (stores_[id].data) {
                observers_.notify_remove(static_cast<std::uint32_t>(id), e);
                stores_[id].erase_by_index(e.index);
            }
        }

        // Clean up relation store entries
        relations_.erase_entity(e.index);

        s.alive = false;
        s.component_mask = 0;
        ++s.generation; // Invalidate existing handles
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

    // ── Component Management ─────────────────────────────────────────────────

    template <Component C>
    void add(Entity e, C c) {
        if (!alive(e)) return;
        const std::uint32_t type_id = ComponentTypeId<C>::id();
        auto& st = store<C>();
        st.set(e.index, std::move(c));

        if (type_id < 64) {
            slot(e.index).component_mask |= (1ULL << type_id);
        }
        observers_.notify_add(type_id, e, st.try_get(e.index));
    }

    void add_by_type_id(Entity e, std::uint32_t type_id, void* payload) {
        if (!alive(e)) return;
        if (type_id >= stores_.size()) {
            stores_.resize(type_id + 1);
        }
        if (!stores_[type_id].data) {
            auto& f = store_factories();
            if (type_id < f.size() && f[type_id]) {
                stores_[type_id] = f[type_id](universe_);
                stores_[type_id].update_world_tick(current_tick_);
            } else {
                return;
            }
        }
        stores_[type_id].insert_raw(e.index, payload);
        if (type_id < 64) {
            slot(e.index).component_mask |= (1ULL << type_id);
        }
    }

    void remove_by_type_id(Entity e, std::uint32_t type_id) {
        if (!alive(e) || type_id >= stores_.size() || !stores_[type_id].data) return;
        observers_.notify_remove(type_id, e);
        stores_[type_id].erase_by_index(e.index);
        if (type_id < 64) {
            slot(e.index).component_mask &= ~(1ULL << type_id);
        }
    }

    template <Component C, typename... Args>
    C* emplace(Entity e, Args&&... args) {
        if (!alive(e)) return nullptr;  // Callers must check; no silent thread_local corruption
        const std::uint32_t type_id = ComponentTypeId<C>::id();
        auto& st = store<C>();
        C& ref = st.emplace(e.index, std::forward<Args>(args)...);

        if (type_id < 64) {
            slot(e.index).component_mask |= (1ULL << type_id);
        }
        observers_.notify_add(type_id, e, &ref);
        return &ref;
    }

    template <Component C>
    void remove(Entity e) {
        if (!alive(e)) return;
        const std::uint32_t type_id = ComponentTypeId<C>::id();
        if (type_id < 64 && ((slot(e.index).component_mask & (1ULL << type_id)) == 0)) {
            return;
        }
        observers_.notify_remove(type_id, e);
        if (auto* s = try_store<C>()) {
            s->erase_by_index(e.index);
        }
        if (type_id < 64) {
            slot(e.index).component_mask &= ~(1ULL << type_id);
        }
    }

    template <Component C>
    [[nodiscard]] bool has(Entity e) const noexcept {
        if (!alive(e)) return false;
        const std::uint32_t type_id = ComponentTypeId<C>::id();
        if (type_id < 64) {
            return (slot(e.index).component_mask & (1ULL << type_id)) != 0;
        }
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
        if (!stores_[id].data) {
            auto* typed = new ComponentStore<C>(universe_);
            typed->set_current_world_tick(current_tick_);
            stores_[id] = ComponentStore<C>::make_erased(typed);
        }
        return *static_cast<ComponentStore<C>*>(stores_[id].data);
    }

    template <Component C>
    [[nodiscard]] const ComponentStore<C>* try_store() const noexcept {
        const std::uint32_t id = ComponentTypeId<C>::id();
        if (id >= stores_.size() || !stores_[id].data) return nullptr;
        return static_cast<const ComponentStore<C>*>(stores_[id].data);
    }

    template <Component C>
    [[nodiscard]] ComponentStore<C>* try_store() noexcept {
        const std::uint32_t id = ComponentTypeId<C>::id();
        if (id >= stores_.size() || !stores_[id].data) return nullptr;
        return static_cast<ComponentStore<C>*>(stores_[id].data);
    }

    // ── Auto-Lead-Store Dense Join View ──────────────────────────────────────

    template <Component Primary, Component... Rest, typename Fn>
    void view(Fn&& fn) {
        auto* p_store = try_store<Primary>();
        if (!p_store || p_store->empty()) return;

        if constexpr (sizeof...(Rest) > 0) {
            if ((!try_store<Rest>() || ...)) return;
        }

        if constexpr (sizeof...(Rest) == 0) {
            for (auto&& [entity_idx, comp] : p_store->pairs()) {
                Entity e{entity_idx, slots_[entity_idx - 1].generation};
                fn(e, comp);
            }
        } else {
            // Find smallest store among all query types
            std::size_t min_size = p_store->size();
            std::size_t min_idx = 0;
            std::size_t curr_idx = 1;
            ((try_store<Rest>()->size() < min_size ? (min_size = try_store<Rest>()->size(), min_idx = curr_idx, 0) : (++curr_idx, 0)), ...);

            // Execute iteration driven by the smallest store
            if (min_idx == 0) {
                for (auto&& [entity_idx, comp] : p_store->pairs()) {
                    if (!(has_idx<Rest>(entity_idx) && ...)) continue;
                    Entity e{entity_idx, slots_[entity_idx - 1].generation};
                    fn(e, comp, *try_store<Rest>()->try_get(entity_idx)...);
                }
            } else {
                // Secondary store is smallest - iterate lead and probe companion stores
                std::size_t branch_idx = 1;
                auto dispatch_lead = [&]<Component Lead>(ComponentStore<Lead>* lead_store) {
                    if (min_idx == branch_idx) {
                        for (auto&& [entity_idx, _] : lead_store->pairs()) {
                            if (!has_idx<Primary>(entity_idx) || !(has_idx<Rest>(entity_idx) && ...)) continue;
                            Entity e{entity_idx, slots_[entity_idx - 1].generation};
                            fn(e, *p_store->try_get(entity_idx), *try_store<Rest>()->try_get(entity_idx)...);
                        }
                    }
                    ++branch_idx;
                };
                (dispatch_lead(try_store<Rest>()), ...);
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

        if constexpr (sizeof...(Rest) == 0) {
            for (auto&& [entity_idx, comp] : p_store->pairs()) {
                Entity e{entity_idx, slots_[entity_idx - 1].generation};
                fn(e, comp);
            }
        } else {
            // Find smallest store among all query types
            std::size_t min_size = p_store->size();
            std::size_t min_idx = 0;
            std::size_t curr_idx = 1;
            ((try_store<Rest>()->size() < min_size ? (min_size = try_store<Rest>()->size(), min_idx = curr_idx, 0) : (++curr_idx, 0)), ...);

            if (min_idx == 0) {
                for (auto&& [entity_idx, comp] : p_store->pairs()) {
                    if (!(has_idx<Rest>(entity_idx) && ...)) continue;
                    Entity e{entity_idx, slots_[entity_idx - 1].generation};
                    fn(e, comp, *try_store<Rest>()->try_get(entity_idx)...);
                }
            } else {
                std::size_t branch_idx = 1;
                auto dispatch_lead = [&]<Component Lead>(const ComponentStore<Lead>* lead_store) {
                    if (min_idx == branch_idx) {
                        for (auto&& [entity_idx, _] : lead_store->pairs()) {
                            if (!has_idx<Primary>(entity_idx) || !(has_idx<Rest>(entity_idx) && ...)) continue;
                            Entity e{entity_idx, slots_[entity_idx - 1].generation};
                            fn(e, *p_store->try_get(entity_idx), *try_store<Rest>()->try_get(entity_idx)...);
                        }
                    }
                    ++branch_idx;
                };
                (dispatch_lead(try_store<Rest>()), ...);
            }
        }
    }

    // ── Rich Filtered Query ──────────────────────────────────────────────────

    template <typename WithClause, typename WithoutClause = Without<>, typename OptionalClause = Optional<void>, typename Fn>
    void query(Fn&& fn) {
        query_impl(WithClause{}, WithoutClause{}, OptionalClause{}, std::forward<Fn>(fn));
    }

    // ── view with Without<> exclusion filter ─────────────────────────────────

    template <Component Primary, Component... Rest, Component... Excludes, typename Fn>
    void view(Without<Excludes...>, Fn&& fn) {
        auto* p_store = try_store<Primary>();
        if (!p_store || p_store->empty()) return;

        if constexpr (sizeof...(Rest) > 0) {
            if ((!try_store<Rest>() || ...)) return;
        }

        for (auto&& [entity_idx, comp] : p_store->pairs()) {
            if constexpr (sizeof...(Rest) > 0) {
                if (!(has_idx<Rest>(entity_idx) && ...)) continue;
            }
            if constexpr (sizeof...(Excludes) > 0) {
                if ((has_idx<Excludes>(entity_idx) || ...)) continue;
            }
            Entity e{entity_idx, slots_[entity_idx - 1].generation};
            if constexpr (sizeof...(Rest) > 0) {
                fn(e, comp, *try_store<Rest>()->try_get(entity_idx)...);
            } else {
                fn(e, comp);
            }
        }
    }

    // ── Chunk view: processes components in contiguous spans (SoA-friendly) ──
    // For SparseSetStoragePolicy: delegates to per-entity view (single-entity chunk).
    // For ArchetypeStoragePolicy: would pass full column spans; here we provide the
    // SparseSet fallback that makes the API available without storage policy detection.

    template <Component Primary, Component... Rest, typename Fn>
    void chunk_view(Fn&& fn) {
        auto* p_store = try_store<Primary>();
        if (!p_store || p_store->empty()) return;

        if constexpr (sizeof...(Rest) > 0) {
            if ((!try_store<Rest>() || ...)) return;
        }

        // Scalar fallback: call fn with single-element spans
        for (auto&& [entity_idx, comp] : p_store->pairs()) {
            if constexpr (sizeof...(Rest) > 0) {
                if (!(has_idx<Rest>(entity_idx) && ...)) continue;
            }
            Entity e{entity_idx, slots_[entity_idx - 1].generation};
            if constexpr (sizeof...(Rest) > 0) {
                fn(std::span<Primary>(&comp, 1),
                   std::span<Rest>(try_store<Rest>()->try_get(entity_idx), 1)...);
            } else {
                fn(std::span<Primary>(&comp, 1));
            }
        }
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

    // ── Reactive Observers ───────────────────────────────────────────────────

    template <Component C, typename Fn>
    void on_add(Fn&& fn) {
        observers_.template register_on_add<C>(&fn);
    }

    template <Component C, typename Fn>
    void on_remove(Fn&& fn) {
        observers_.template register_on_remove<C>(&fn);
    }

    // ── Entity Relations ─────────────────────────────────────────────────────

    template <typename Rel = ChildOf>
    void relate(Entity source, Entity target) {
        if (!alive(source) || !alive(target)) return;
        relations_.relate(source, target);
    }

    template <typename Rel = ChildOf>
    void unrelate(Entity source, Entity target) {
        relations_.unrelate(source, target);
    }

    template <typename Rel = ChildOf>
    [[nodiscard]] std::span<const Entity> related_to(Entity source) const noexcept {
        return relations_.related_to(source);
    }

    template <typename Rel = ChildOf, typename Fn>
    void for_each_child(Entity parent, Fn&& fn) const {
        relations_.for_each_related(parent, std::forward<Fn>(fn));
    }

    template <typename Rel = ChildOf>
    void despawn_cascade(Entity root) {
        if (!alive(root)) return;
        auto children = related_to<Rel>(root);
        for (const auto& child : children) {
            despawn_cascade<Rel>(child);
        }
        despawn(root);
    }

    // ── Change Detection ─────────────────────────────────────────────────────

    void advance_tick() noexcept {
        ++current_tick_;
        for (auto& st : stores_) {
            st.update_world_tick(current_tick_);
        }
    }

    [[nodiscard]] std::uint32_t current_tick() const noexcept {
        return current_tick_;
    }

    // ── Command Buffer ───────────────────────────────────────────────────────
    [[nodiscard]] CommandBuffer& commands() noexcept { return cmds_; }
    void flush_commands() { cmds_.flush(*this); }

    [[nodiscard]] std::uint32_t universe() const noexcept { return universe_; }

private:
    struct Slot {
        std::uint32_t generation = 1;
        std::uint64_t component_mask = 0;
        bool alive = false;
    };

    Slot& slot(std::uint32_t idx) { return slots_[idx - 1]; }
    const Slot& slot(std::uint32_t idx) const { return slots_[idx - 1]; }

    template <Component C>
    [[nodiscard]] bool has_idx(std::uint32_t idx) const noexcept {
        const auto* s = try_store<C>();
        return s && s->has(idx);
    }

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
    std::uint32_t current_tick_ = 0;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_indices_;
    std::vector<ErasedStore> stores_;
    ObserverRegistry observers_;
    RelationStore<ChildOf> relations_;
    CommandBuffer cmds_;
};

// Default World alias matching plain `pebble::ecs::World`
using World = BasicWorld<SparseSetStoragePolicy, ArenaAllocPolicy, AutoSchedulerPolicy, PagedSparsePolicy>;

static_assert(!std::is_polymorphic_v<World>, "World must have zero virtual functions");

} // namespace pebble::ecs
