#pragma once
// ============================================================================
// ecs/relation.hpp — Typed Entity-to-Entity Relations for pebble::ecs
// ============================================================================
// Supports hierarchical relationships (ChildOf, Targets, custom tags),
// fast child queries, tree traversal, and cascade despawns.
//
// Zero virtual functions, zero macros, header-only C++23.
// ============================================================================

#include "entity.hpp"
#include "containers/associative/SparseSet.hpp"
#include "containers/dynamic/SmallVector.hpp"

#include <cstdint>
#include <concepts>
#include <span>
#include <type_traits>
#include <vector>

namespace pebble::ecs {

// Built-in Relation Tags
struct ChildOf {};
struct Targets {};
struct MemberOf {};

template <typename Rel>
class RelationStore {
public:
    using TargetList = containers::dynamic::SmallVector<Entity, 32>; // up to 4 entities inline (32 bytes)

    explicit RelationStore(std::uint32_t universe_capacity = kDefaultUniverse)
        : set_(universe_capacity) {}

    void relate(Entity source, Entity target) {
        if (auto r = set_.get(source.index)) {
            for (const auto& e : r->get()) {
                if (e == target) return;
            }
            r->get().push_back(target);
        } else {
            TargetList targets;
            targets.push_back(target);
            (void)set_.insert(source.index, std::move(targets));
        }
    }

    void unrelate(Entity source, Entity target) {
        if (auto r = set_.get(source.index)) {
            auto& list = r->get();
            auto it = std::remove(list.begin(), list.end(), target);
            list.erase(it, list.end());
            if (list.empty()) {
                (void)set_.remove(source.index);
            }
        }
    }

    [[nodiscard]] std::span<const Entity> related_to(Entity source) const noexcept {
        auto r = set_.get(source.index);
        if (!r || r->get().empty()) return {};
        return std::span<const Entity>(r->get().data(), r->get().size());
    }

    void erase_entity(std::uint32_t entity_idx) noexcept {
        (void)set_.remove(entity_idx);
        // Also remove entity as target across all active relations
        for (auto&& [src_idx, targets] : set_.all_pairs()) {
            auto it = std::remove_if(targets.begin(), targets.end(), [entity_idx](Entity e) {
                return e.index == entity_idx;
            });
            targets.erase(it, targets.end());
        }
    }

    template <typename Fn>
    void for_each_related(Entity source, Fn&& fn) const {
        if (auto r = set_.get(source.index)) {
            for (const auto& target : r->get()) {
                fn(target);
            }
        }
    }

    [[nodiscard]] bool empty() const noexcept {
        return set_.empty();
    }

    void clear() noexcept {
        set_.clear();
    }

private:
    sparseset::SparseSet<std::uint32_t, TargetList> set_;
};

static_assert(!std::is_polymorphic_v<RelationStore<ChildOf>>, "RelationStore must have zero virtual functions");

} // namespace pebble::ecs
