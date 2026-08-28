#pragma once
// ============================================================================
// ecs/command_buffer.hpp — Deferred & Thread-Partitioned Mutation Buffer
// ============================================================================
// Supports thread-local command recording during parallel views with zero lock
// contention, merging queues during flush.
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

    // Merge another thread's command queue with zero lock on the worker thread
    void merge(std::vector<std::function<void(World&)>>&& other_ops) {
        std::lock_guard<std::mutex> lock(mutex_);
        ops_.insert(ops_.end(),
                   std::make_move_iterator(other_ops.begin()),
                   std::make_move_iterator(other_ops.end()));
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::function<void(World&)>> ops_;
};

// Thread-local recording context for parallel workers
class LocalCommandBuffer {
public:
    explicit LocalCommandBuffer(CommandBuffer& global)
        : global_(global) {}

    ~LocalCommandBuffer() {
        if (!ops_.empty()) {
            global_.merge(std::move(ops_));
        }
    }

    void despawn(Entity e);

    template <typename C>
    void add(Entity e, C c);

    template <typename C, typename... Args>
    void emplace(Entity e, Args&&... args);

    template <typename C>
    void remove(Entity e);

private:
    CommandBuffer& global_;
    std::vector<std::function<void(World&)>> ops_;
};

} // namespace pebble::ecs
