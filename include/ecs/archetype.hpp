#pragma once
// ============================================================================
// ecs/archetype.hpp — Columnar Archetype Storage Backend for pebble::ecs
// ============================================================================
// Groups entities with identical component signatures into dense contiguous
// columns for cache-optimal bulk system sweeps.
//
// Zero virtual functions, zero macros, header-only C++23.
// ============================================================================

#include "entity.hpp"
#include "component_store.hpp"
#include "containers/cache/kosha.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace pebble::ecs {

struct Column {
    void* data = nullptr;
    std::size_t elem_size = 0;
    void (*destructor)(void*) noexcept = nullptr;

    void release() noexcept {
        if (data) {
            std::free(data);
            data = nullptr;
        }
    }
};

struct ArchetypeRecord {
    std::uint64_t signature = 0;
    std::vector<Entity> entities;
    std::vector<Column> columns; // indexed by component type id or offset
    std::size_t count = 0;
};

class ArchetypeStorage {
public:
    ArchetypeStorage() = default;
    ~ArchetypeStorage() {
        clear();
    }

    ArchetypeStorage(const ArchetypeStorage&) = delete;
    ArchetypeStorage& operator=(const ArchetypeStorage&) = delete;

    ArchetypeStorage(ArchetypeStorage&& other) noexcept
        : archetypes_(std::move(other.archetypes_)) {}

    ArchetypeStorage& operator=(ArchetypeStorage&& other) noexcept {
        if (this != &other) {
            clear();
            archetypes_ = std::move(other.archetypes_);
        }
        return *this;
    }

    void clear() noexcept {
        for (auto& arch : archetypes_) {
            for (auto& col : arch->columns) {
                col.release();
            }
        }
        archetypes_.clear();
    }

    [[nodiscard]] std::size_t archetype_count() const noexcept {
        return archetypes_.size();
    }

    ArchetypeRecord& get_or_create(std::uint64_t signature) {
        for (auto& arch : archetypes_) {
            if (arch && arch->signature == signature) return *arch;
        }
        archetypes_.push_back(std::make_unique<ArchetypeRecord>(ArchetypeRecord{.signature = signature}));
        return *archetypes_.back();
    }

private:
    std::vector<std::unique_ptr<ArchetypeRecord>> archetypes_;
};

static_assert(!std::is_polymorphic_v<ArchetypeStorage>, "ArchetypeStorage must have zero virtual functions");

} // namespace pebble::ecs
