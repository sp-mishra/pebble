#pragma once
// ============================================================================
// drishya/ops.hpp — free-function algorithms over a WidgetTree
// ----------------------------------------------------------------------------
// Tree queries that don't belong to any single widget: structural visiting,
// predicate search, geometric hit-testing against solved rects, focus order,
// and subtree bounds. All are non-owning and heap-free on the hot path (callers
// pass an output span where a result set is needed).
//
// These operate on the retained WidgetTree plus (where geometry matters) a
// solved LayoutBridge. No virtual, no macros. Header-only C++23.
// ============================================================================

#include "drishya/layout_bridge.hpp"
#include "drishya/tree.hpp"

#include <cstddef>
#include <span>

namespace pebble::drishya::ops {

// Pre-order visit: fn(NodeId, depth) for every node from the root.
template <typename Tree, typename Fn>
void visit(const Tree& tree, Fn&& fn) {
    tree.walk(static_cast<Fn&&>(fn));
}

// First node (pre-order) satisfying pred(NodeId); kInvalidNode if none.
template <typename Tree, typename Pred>
[[nodiscard]] NodeId find(const Tree& tree, Pred pred) {
    NodeId found = kInvalidNode;
    tree.walk([&](NodeId id, std::size_t) {
        if (found == kInvalidNode && pred(id)) found = id;
    });
    return found;
}

// Write every node satisfying pred into `out`; return how many matched. Writing
// stops at out.size() but counting continues, so a caller can detect overflow.
template <typename Tree, typename Pred>
[[nodiscard]] std::size_t find_all(const Tree& tree, Pred pred, std::span<NodeId> out) {
    std::size_t n = 0;
    tree.walk([&](NodeId id, std::size_t) {
        if (pred(id)) {
            if (n < out.size()) out[n] = id;
            ++n;
        }
    });
    return n;
}

// Count of nodes in the subtree rooted at `id` (inclusive).
template <typename Tree>
[[nodiscard]] std::size_t subtree_size(const Tree& tree, NodeId id) {
    std::size_t n = 0;
    tree.walk(id, [&](NodeId, std::size_t) { ++n; });
    return n;
}

// Depth of `id` from the root (root == 0). kInvalidNode input → 0.
template <typename Tree>
[[nodiscard]] std::size_t depth_of(const Tree& tree, NodeId id) {
    std::size_t d = 0;
    for (NodeId p = tree.parent(id); p != kInvalidNode; p = tree.parent(p)) ++d;
    return d;
}

// Topmost widget NodeId whose solved rect contains (x, y); kInvalidNode if none.
template <typename Tree, typename Bridge>
[[nodiscard]] NodeId hit(const Tree&, const Bridge& bridge, float x, float y) {
    return bridge.hit(x, y);
}

// Union of the solved rects of every node in the subtree rooted at `id`.
template <typename Tree, typename Bridge>
[[nodiscard]] Rect2D subtree_bounds(const Tree& tree, const Bridge& bridge, NodeId id) {
    bool any = false;
    float minx = 0, miny = 0, maxx = 0, maxy = 0;
    tree.walk(id, [&](NodeId n, std::size_t) {
        const Rect2D r = bridge.rect(n);
        const float rx = r.x, ry = r.y, rr = r.x + r.w, rb = r.y + r.h;
        if (!any) { minx = rx; miny = ry; maxx = rr; maxy = rb; any = true; }
        else {
            if (rx < minx) minx = rx;
            if (ry < miny) miny = ry;
            if (rr > maxx) maxx = rr;
            if (rb > maxy) maxy = rb;
        }
    });
    return any ? Rect2D{minx, miny, maxx - minx, maxy - miny} : Rect2D{};
}

// Intrinsic measure of a single widget under the given metrics/scale.
template <typename Tree, typename Metrics>
[[nodiscard]] Size2D measure(const Tree& tree, NodeId id, const Metrics& metrics,
                             float scale = 1.0f) {
    MeasureCtxT<Metrics> mc{metrics, scale};
    return tree.widget(id).measure(mc);
}

} // namespace pebble::drishya::ops
