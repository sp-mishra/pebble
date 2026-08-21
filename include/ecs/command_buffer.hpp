#pragma once
// ============================================================================
// ecs/command_buffer.hpp — Deferred Structural Mutation Buffer for pebble::ecs
// ============================================================================
// Records spawn, despawn, component add, and component remove operations during
// system execution or parallel views, and flushes them at sync points.
// ============================================================================

#include "entity.hpp"
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace pebble::ecs {

class World; // Forward declaration

class CommandBuffer {
public:
    CommandBuffer() = default;

    void despawn(Entity e);

    template <typename C>
    void add(Entity e, C c);

    template <typename C, typename... Args>
    void emplace(Entity e, Args&&... args);

    template <typename C>
    void remove(Entity e);

    void flush(World& w) {
        std::vector<std::function<void(World&)>> local_ops;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            local_ops.swap(ops_);
        }
        for (auto& op : local_ops) {
            op(w);
        }
    }

    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return ops_.empty();
    }

    void clear() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        ops_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::function<void(World&)>> ops_;
};

} // namespace pebble::ecs
