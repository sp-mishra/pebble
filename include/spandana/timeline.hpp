#pragma once
// ============================================================================
// spandana/timeline.hpp — Node-Based Declarative Animation Timeline
// ============================================================================
// Actions register ResourceKeys for automatic dependency & parallelism inference.
// ============================================================================

#include "resource_key.hpp"
#include "easing.hpp"
#include "containers/static/static_vector.hpp"
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace pebble::spandana {

class IAnimationAction {
public:
    virtual ~IAnimationAction() = default;
    virtual void update(float progress, float dt) = 0;
    virtual void on_start() {}
    virtual void on_complete() {}
    [[nodiscard]] virtual float duration() const noexcept = 0;
    [[nodiscard]] virtual ResourceKey resource_key() const noexcept = 0;
};

// Represents a node in the timeline execution graph
struct TimelineNode {
    std::shared_ptr<IAnimationAction> action;
    float                             start_time = 0.0f;
    float                             elapsed = 0.0f;
    bool                              started = false;
    bool                              finished = false;

    [[nodiscard]] float end_time() const noexcept {
        return start_time + (action ? action->duration() : 0.0f);
    }
};

class Timeline {
public:
    Timeline() = default;

    // Add an action with automatic resource dependency inference
    template <typename ActionT>
    Timeline& add(ActionT&& act) {
        auto action_ptr = std::make_shared<std::decay_t<ActionT>>(std::forward<ActionT>(act));
        const ResourceKey key = action_ptr->resource_key();
        const float dur = action_ptr->duration();

        float start_t = 0.0f;
        // Check if there is an existing action targeting the same ResourceKey
        for (const auto& node : nodes_) {
            if (node.action && node.action->resource_key() == key) {
                // Dependency collision detected: chain sequentially after latest end time!
                start_t = std::max(start_t, node.end_time());
            }
        }

        nodes_.push_back(TimelineNode{
            .action = std::move(action_ptr),
            .start_time = start_t,
            .elapsed = 0.0f,
            .started = false,
            .finished = false
        });

        total_duration_ = std::max(total_duration_, start_t + dur);
        return *this;
    }

    // Variadic convenience add
    template <typename First, typename... Rest>
    Timeline& add(First&& first, Rest&&... rest) {
        add(std::forward<First>(first));
        if constexpr (sizeof...(Rest) > 0) {
            add(std::forward<Rest>(rest)...);
        }
        return *this;
    }

    void update(float dt) {
        if (finished_) return;

        current_time_ += dt;
        bool all_done = true;

        // Pass 1: Complete finishing nodes
        for (auto& node : nodes_) {
            if (node.finished || !node.started) continue;

            node.elapsed = std::min(node.action ? node.action->duration() : 0.0f,
                                    current_time_ - node.start_time);
            const float dur = node.action ? node.action->duration() : 0.0f;
            const float progress = (dur > 0.0f) ? std::min(1.0f, node.elapsed / dur) : 1.0f;

            if (node.action) node.action->update(progress, dt);

            if (node.elapsed >= dur) {
                node.finished = true;
                if (node.action) node.action->on_complete();
            } else {
                all_done = false;
            }
        }

        // Pass 2: Start new nodes whose start_time has arrived and progress them
        for (auto& node : nodes_) {
            if (node.finished) continue;

            if (!node.started && current_time_ >= node.start_time) {
                node.started = true;
                if (node.action) node.action->on_start();

                node.elapsed = std::min(node.action ? node.action->duration() : 0.0f,
                                        current_time_ - node.start_time);
                const float dur = node.action ? node.action->duration() : 0.0f;
                const float progress = (dur > 0.0f) ? std::min(1.0f, node.elapsed / dur) : 1.0f;

                if (node.action && progress > 0.0f) node.action->update(progress, dt);

                if (node.elapsed >= dur && dur == 0.0f) {
                    node.finished = true;
                    if (node.action) node.action->on_complete();
                } else if (!node.finished) {
                    all_done = false;
                }
            } else if (!node.started) {
                all_done = false;
            }
        }

        finished_ = all_done && (current_time_ >= total_duration_);
    }

    [[nodiscard]] bool is_finished() const noexcept { return finished_; }
    [[nodiscard]] float total_duration() const noexcept { return total_duration_; }
    [[nodiscard]] float current_time() const noexcept { return current_time_; }
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }

    void reset() noexcept {
        current_time_ = 0.0f;
        finished_ = false;
        for (auto& node : nodes_) {
            node.elapsed = 0.0f;
            node.started = false;
            node.finished = false;
        }
    }

private:
    std::vector<TimelineNode> nodes_;
    float                     total_duration_ = 0.0f;
    float                     current_time_   = 0.0f;
    bool                      finished_       = false;
};

} // namespace pebble::spandana
