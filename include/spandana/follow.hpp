#pragma once
// ============================================================================
// spandana/follow.hpp — "follow a cell" continuous spring action
// ============================================================================
// A FollowCell action continuously springs an output cell toward the value held
// by a target cell, re-reading the target every tick. Unlike the fixed-duration
// tween actions, a follow is open-ended: it keeps chasing a moving target until
// removed. This is the animation primitive behind reactive UI reflow, camera
// follow, and value widgets that track a Signal.
//
// A "cell" is anything with get()/set() (e.g. containers::reactive::Signal<T>,
// or a thin lambda-backed adapter). The damper is any stateful spring exposing
// `step(target_value, dt) -> value` and `settled()` — e.g. RectSpring<Rect>, or
// the ScalarCellSpring adapter below wrapping AnalyticalSpringDamper.
//
// Design charter (Pebble): zero virtual, zero heap, static policy dispatch. The
// action models spandana::AnimationAction so it drops straight into a Timeline's
// inline SBO buffer.
// ============================================================================

#include "spring.hpp"
#include "resource_key.hpp"
#include "concepts.hpp"
#include <cmath>
#include <limits>
#include <utility>

namespace pebble::spandana {

// A cell is a value holder with get()/set().
template <typename C>
concept Cell = requires(C& c, const C& cc) {
    { cc.get() };
    { c.set(cc.get()) };
};

// Stateful scalar spring with the step(target, dt) interface FollowCell expects,
// wrapping the stateless AnalyticalSpringDamper and holding its own velocity.
class ScalarCellSpring {
public:
    constexpr explicit ScalarCellSpring(float stiffness = 180.0f, float damping = 12.0f) noexcept
        : spring_(stiffness, damping) {}

    void snap(float v) noexcept { current_ = v; velocity_ = 0.0f; has_state_ = true; }

    [[nodiscard]] float step(float target, float dt) noexcept {
        if (!has_state_) { current_ = target; velocity_ = 0.0f; has_state_ = true; return current_; }
        auto [p, v] = spring_.step(current_, velocity_, target, dt);
        current_ = p;
        velocity_ = v;
        return current_;
    }

    [[nodiscard]] bool settled(float vel_eps = 0.01f) const noexcept {
        return has_state_ && std::fabs(velocity_) <= vel_eps;
    }

    [[nodiscard]] float value() const noexcept { return current_; }

private:
    AnalyticalSpringDamper spring_;
    float current_ = 0.0f;
    float velocity_ = 0.0f;
    bool has_state_ = false;
};

// FollowCell — springs `out` toward `target`'s value each tick.
//
// Template params:
//   OutCell    : cell written each tick (get()/set()).
//   TargetCell : cell read each tick for the current target value.
//   Damper     : stateful spring exposing step(target_value, dt) and settled().
//
// The cells are referenced (not owned); they must outlive the action. A fresh
// ResourceKey is assigned per instance so the Timeline can infer independence.
template <typename OutCell, typename TargetCell, typename Damper>
    requires Cell<OutCell> && Cell<TargetCell>
class FollowCell {
public:
    FollowCell(OutCell& out, const TargetCell& target, Damper damper,
               ResourceKey key = kWorldResource) noexcept
        : out_(&out), target_(&target), damper_(std::move(damper)), key_(key) {}

    void update(float /*elapsed*/, float dt) noexcept {
        const auto tgt = target_->get();
        out_->set(damper_.step(tgt, dt));
    }

    // Open-ended: report a large finite duration so the Timeline never auto-
    // completes it on elapsed alone; callers remove it when settled().
    [[nodiscard]] float duration() const noexcept {
        return std::numeric_limits<float>::max();
    }

    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

    [[nodiscard]] bool settled() const noexcept { return damper_.settled(); }

private:
    OutCell* out_;
    const TargetCell* target_;
    Damper damper_;
    ResourceKey key_;
};

// Factory: follow(out, target, damper) — deduces cell/damper types.
template <typename OutCell, typename TargetCell, typename Damper>
    requires Cell<OutCell> && Cell<TargetCell>
[[nodiscard]] FollowCell<OutCell, TargetCell, Damper>
follow(OutCell& out, const TargetCell& target, Damper damper,
       ResourceKey key = kWorldResource) noexcept {
    return FollowCell<OutCell, TargetCell, Damper>(out, target, std::move(damper), key);
}

// Convenience: scalar follow using a ScalarCellSpring.
template <typename OutCell, typename TargetCell>
    requires Cell<OutCell> && Cell<TargetCell>
[[nodiscard]] auto
follow_scalar(OutCell& out, const TargetCell& target,
              float stiffness = 180.0f, float damping = 12.0f,
              ResourceKey key = kWorldResource) noexcept {
    return follow(out, target, ScalarCellSpring(stiffness, damping), key);
}

} // namespace pebble::spandana
