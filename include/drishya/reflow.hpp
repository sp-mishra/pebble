#pragma once
// ============================================================================
// drishya/reflow.hpp — layout-change motion policies
// ----------------------------------------------------------------------------
// When the solver produces a new rect for a node, the reflow policy decides how
// the node travels from its previous rect to the new one:
//
//   * NullMotion  — snap instantly. Zero state, zero cost. The default; correct
//     for HUDs and anything that must reflect state immediately.
//   * SpringReflow — critically-ish damped spring per moved node (spandana
//     RectSpring), so panels glide when the layout changes. Springs live in a
//     SparseSet keyed by NodeId, so only nodes that are actually moving carry
//     state; settled nodes are dropped.
//
// A policy is a value type satisfying the ReflowMotion concept below; App is
// templated on it with NullMotion as the default. No virtual, no macros.
// ============================================================================

#include "drishya/tree.hpp"
#include "drishya/widget_concept.hpp"

#include "containers/associative/SparseSet.hpp"
#include "spandana/spring.hpp"

#include <cstdint>

namespace pebble::drishya {

// A reflow motion maps (node, target rect, dt) -> the rect to actually draw.
template <typename M>
concept ReflowMotion = requires(M& m, NodeId id, Rect2D target, float dt) {
    { m.resolve(id, target, dt) } -> std::convertible_to<Rect2D>;
    { m.settled() } -> std::convertible_to<bool>;
};

// Snap to target immediately. Empty — [[no_unique_address]] friendly.
struct NullMotion {
    [[nodiscard]] Rect2D resolve(NodeId, Rect2D target, float) const noexcept { return target; }
    [[nodiscard]] bool settled() const noexcept { return true; }
    void forget(NodeId) noexcept {}
    void reset() noexcept {}
};

// Spring the previous rect toward the target. Springs are created lazily on the
// first move and removed once settled, so steady-state cost is zero.
class SpringReflow {
public:
    SpringReflow() = default;
    explicit SpringReflow(float stiffness, float damping) noexcept
        : stiffness_(stiffness), damping_(damping) {}

    [[nodiscard]] Rect2D resolve(NodeId id, Rect2D target, float dt) {
        auto existing = springs_.get(id);
        if (!existing.has_value()) {
            // First time we see this node: snap to target, no spring-in.
            Entry e{spandana::RectSpring<Rect2D>{stiffness_, damping_}};
            e.spring.snap(target);
            springs_.insert_or_update(id, e);
            all_settled_ = false;
            return target;
        }
        Entry& e = existing->get();
        const Rect2D r = e.spring.step(target, dt);
        if (e.spring.settled()) {
            auto rm = springs_.remove(id);
            (void)rm;
            return target;
        }
        all_settled_ = false;
        return r;
    }

    [[nodiscard]] bool settled() const noexcept { return springs_.empty(); }

    // Drop a node's spring (e.g. it was removed from the tree).
    void forget(NodeId id) {
        if (springs_.contains(id)) { auto rm = springs_.remove(id); (void)rm; }
    }
    void reset() noexcept { springs_.clear(); all_settled_ = true; }

    // Called once at the start of a tick so settled() reflects this frame.
    void begin_frame() noexcept { all_settled_ = true; }

private:
    struct Entry {
        spandana::RectSpring<Rect2D> spring{};
    };

    float stiffness_ = 180.0f;
    float damping_ = 20.0f;
    bool all_settled_ = true;
    sparseset::SparseSet<NodeId, Entry> springs_{};
};

static_assert(ReflowMotion<NullMotion>);
static_assert(ReflowMotion<SpringReflow>);

} // namespace pebble::drishya
