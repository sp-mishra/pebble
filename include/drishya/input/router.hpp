#pragma once
// ============================================================================
// drishya/input/router.hpp — pointer + keyboard event routing
// ----------------------------------------------------------------------------
// The router turns one InputFrame into on_event() calls on the widgets that
// should see it, using the solved layout to decide who is under the pointer and
// who owns focus:
//
//   * Pointer capture — if a widget returned CapturePointer, every subsequent
//     pointer frame goes straight to it (drag/slider grab) until it returns
//     ReleasePointer or dies. This is checked before hit-testing.
//   * Hit + bubble — otherwise akruti's hit_test_chain gives the node under the
//     pointer and its ancestor chain (leaf -> root). We deliver leaf-first and
//     stop at the first Consumed/Capture (event bubbling).
//   * Focus + keys — key/text frames go to the focused widget; Tab advances
//     focus through leaves in layout order (akruti for_each_leaf = tab order).
//
// The router speaks widget NodeIds; it maps to/from layout indices through the
// LayoutBridge. No virtual, no macros. Header-only C++23.
// ============================================================================

#include "drishya/layout_bridge.hpp"
#include "drishya/tree.hpp"
#include "drishya/widget_concept.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace pebble::drishya {

// Result of routing a frame — what the app may want to react to.
struct RouteResult {
    NodeId hovered = kInvalidNode;   // node under the pointer this frame
    NodeId consumed_by = kInvalidNode; // node that consumed a pointer press
    bool pointer_handled = false;
    bool key_handled = false;
};

template <typename Metrics, typename Painter_, std::size_t InlineBytes = 512>
    requires ITextMetrics<Metrics> && Painter<Painter_>
class Router {
public:
    using tree_type = WidgetTree<Metrics, Painter_, InlineBytes>;
    using bridge_type = LayoutBridge<Metrics, Painter_, InlineBytes>;

    // Route one input frame. `scale` is the display scale carried into EventCtx.
    RouteResult route(tree_type& tree, const bridge_type& bridge,
                      const InputFrame& input, float scale = 1.0f) {
        RouteResult out{};

        // 1) Pointer capture takes priority over hit-testing.
        if (capture_ != kInvalidNode && tree.valid(capture_)) {
            const EventResult r = deliver(tree, bridge, capture_, input, scale);
            out.pointer_handled = handled(r);
            out.consumed_by = capture_;
            apply_capture_transition(r, capture_);
            // Still compute hover for the app's convenience.
            out.hovered = bridge.hit(input.pointer.x, input.pointer.y);
            route_keys(tree, bridge, input, scale, out);
            return out;
        }

        // 2) Hit-test + bubble leaf -> root.
        std::array<std::uint32_t, kMaxChain> chain{};
        const std::size_t n = bridge.engine().hit_test_chain(
            input.pointer.x, input.pointer.y,
            std::span<std::uint32_t>(chain.data(), chain.size()));

        if (n > 0) out.hovered = bridge.layout_to_node(chain[0]);

        for (std::size_t i = 0; i < n; ++i) {
            const NodeId id = bridge.layout_to_node(chain[i]);
            if (id == kInvalidNode || !tree.valid(id)) continue;
            const EventResult r = deliver(tree, bridge, id, input, scale);
            if (handled(r)) {
                out.pointer_handled = true;
                out.consumed_by = id;
                apply_capture_transition(r, id);
                // A pointer press that lands claims focus for keyboard input.
                if (input.pressed(kPointerLeft)) focus_ = id;
                break;
            }
        }

        // 3) Keyboard / text to the focused widget (+ Tab focus traversal).
        route_keys(tree, bridge, input, scale, out);
        return out;
    }

    // --- focus control -----------------------------------------------------
    [[nodiscard]] NodeId focus() const noexcept { return focus_; }
    void set_focus(NodeId id) noexcept { focus_ = id; }
    [[nodiscard]] NodeId capture() const noexcept { return capture_; }
    void clear() noexcept { focus_ = kInvalidNode; capture_ = kInvalidNode; }

    // Move focus to the next / previous leaf in layout (tab) order.
    void focus_next(const bridge_type& bridge) { focus_step(bridge, +1); }
    void focus_prev(const bridge_type& bridge) { focus_step(bridge, -1); }

private:
    static constexpr std::size_t kMaxChain = 64; // max ancestor depth for a hit

    EventResult deliver(tree_type& tree, const bridge_type& bridge, NodeId id,
                        const InputFrame& input, float scale) {
        EventCtx ec{input, bridge.rect(id), bridge.clip(id), scale};
        return tree.widget(id).on_event(ec);
    }

    void apply_capture_transition(EventResult r, NodeId id) noexcept {
        if (r == EventResult::CapturePointer) capture_ = id;
        else if (r == EventResult::ReleasePointer) capture_ = kInvalidNode;
    }

    void route_keys(tree_type& tree, const bridge_type& bridge,
                    const InputFrame& input, float scale, RouteResult& out) {
        // Tab traversal is handled by the router itself, not delivered as a key.
        for (const KeyEvent& k : input.keys) {
            if (k.pressed && k.key == Key::Tab) {
                const bool shift = (k.mods & 0x1) != 0;
                if (shift) focus_prev(bridge); else focus_next(bridge);
                out.key_handled = true;
            }
        }
        if (focus_ != kInvalidNode && tree.valid(focus_) &&
            (!input.keys.empty() || !input.text.empty())) {
            const EventResult r = deliver(tree, bridge, focus_, input, scale);
            if (handled(r)) out.key_handled = true;
        }
    }

    void focus_step(const bridge_type& bridge, int dir) {
        // Collect leaves in tab order, then pick the neighbour of the current
        // focus. Bounded scratch avoids heap on the hot path.
        std::array<NodeId, kMaxLeaves> leaves{};
        std::size_t count = 0;
        bridge.engine().for_each_leaf([&](std::uint32_t li) {
            if (count < leaves.size()) leaves[count++] = bridge.layout_to_node(li);
        });
        if (count == 0) { focus_ = kInvalidNode; return; }

        std::size_t cur = count; // sentinel = "not found"
        for (std::size_t i = 0; i < count; ++i)
            if (leaves[i] == focus_) { cur = i; break; }

        std::size_t next;
        if (cur == count) {
            next = (dir > 0) ? 0 : count - 1;
        } else {
            next = (cur + static_cast<std::size_t>(dir < 0 ? count - 1 : 1)) % count;
        }
        focus_ = leaves[next];
    }

    static constexpr std::size_t kMaxLeaves = 256; // tab-order scratch bound

    NodeId focus_ = kInvalidNode;
    NodeId capture_ = kInvalidNode;
};

} // namespace pebble::drishya
