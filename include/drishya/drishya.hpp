#pragma once
// ============================================================================
// drishya.hpp — Drishya (दृश्य) retained-mode widget engine: umbrella + App
// ----------------------------------------------------------------------------
// Drishya composes UI from concept-satisfying value types (no virtual, no RTTI,
// no macros), lays them out with akruti::layout, routes input, animates reflow
// with spandana, and paints through any Painter adapter (KalpanaPainter is the
// reference). One widget vocabulary serves both AI/ML dashboards and game HUDs.
//
// This header is the umbrella: it pulls the core pieces and defines App<>, the
// object that owns a frame's worth of state and drives the loop:
//
//     App<Metrics, Painter> app{metrics};
//     app.set_root( ui::vstack(...) );      // build the retained tree
//     app.set_viewport({0,0,1280,720});
//     // each frame:
//     app.pump(input_frame);                // route pointer/keys
//     app.tick(dt);                         // advance reflow motion
//     app.solve();                          // (re)layout if dirty
//     app.paint(painter);                   // emit draw commands
//
// The (Metrics, Painter) pair is chosen once; DefaultApp wires the headless
// KalpanaPainter + MonospaceMetrics for tests and blocky HUDs.
// ============================================================================

#include "drishya/any_widget.hpp"
#include "drishya/input/router.hpp"
#include "drishya/layout_bridge.hpp"
#include "drishya/ops.hpp"
#include "drishya/reactive.hpp"
#include "drishya/reflow.hpp"
#include "drishya/theme.hpp"
#include "drishya/tree.hpp"
#include "drishya/widget_concept.hpp"

#include "drishya/painter/draw_list.hpp"
#include "drishya/painter/kalpana_painter.hpp"

#include "drishya/widgets/widgets.hpp"

#include <cstddef>
#include <utility>

namespace pebble::drishya {
    // ----------------------------------------------------------------------------
    // App<Metrics, Painter, Motion, InlineBytes>
    //   Owns the retained tree, the layout bridge, the input router, and the reflow
    //   motion policy. Metrics/Painter fix the erased widget type; Motion defaults
    //   to NullMotion (snap). Motion is stored [[no_unique_address]] so NullMotion
    //   costs nothing.
    // ----------------------------------------------------------------------------
    template <typename Metrics, typename Painter_, typename Motion = NullMotion,
              std::size_t InlineBytes = 512>
        requires ITextMetrics<Metrics> && Painter<Painter_> && ReflowMotion<Motion>
    class App {
    public:
        using tree_type = WidgetTree<Metrics, Painter_, InlineBytes>;
        using bridge_type = LayoutBridge<Metrics, Painter_, InlineBytes>;
        using router_type = Router<Metrics, Painter_, InlineBytes>;
        using widget_type = typename tree_type::widget_type;

        explicit App(Metrics& metrics) noexcept : metrics_(&metrics), bridge_(metrics) {}

        // --- tree construction -------------------------------------------------
        NodeId set_root(widget_type w) {
            const NodeId id = tree_.set_root(std::move(w));
            structural_dirty_ = true;
            return id;
        }

        NodeId add_child(NodeId parent, widget_type w) {
            const NodeId id = tree_.add_child(parent, std::move(w));
            structural_dirty_ = true;
            return id;
        }

        void remove(NodeId id) {
            tree_.remove(id);
            structural_dirty_ = true;
        }

        [[nodiscard]] tree_type& tree() noexcept { return tree_; }
        [[nodiscard]] const tree_type& tree() const noexcept { return tree_; }
        [[nodiscard]] bridge_type& layout() noexcept { return bridge_; }
        [[nodiscard]] router_type& router() noexcept { return router_; }
        [[nodiscard]] Motion& motion() noexcept { return motion_; }

        // --- viewport ----------------------------------------------------------
        void set_viewport(Rect2D vp) noexcept {
            if (!(vp == viewport_)) {
                viewport_ = vp;
                geometry_dirty_ = true;
            }
        }

        [[nodiscard]] Rect2D viewport() const noexcept { return viewport_; }

        // --- layout ------------------------------------------------------------
        // Solve layout if anything is dirty. Structural change → rebuild + full
        // solve; geometry/style change → incremental solve. No-op when clean.
        void solve() {
            const Bounds2D vp = akruti::layout::rect_to_bounds(viewport_);
            if (structural_dirty_) {
                bridge_.rebuild(tree_);
                bridge_.solve(vp);
                structural_dirty_ = false;
                geometry_dirty_ = false;
            }
            else if (geometry_dirty_ || style_dirty_) {
                flush_style_dirty();
                bridge_.solve_incremental(vp);
                geometry_dirty_ = false;
                style_dirty_ = false;
            }
        }

        // Mark a node's style stale (call after mutating a widget's style inputs).
        void invalidate_style(NodeId id) {
            tree_.mark_dirty(id, kDirtyLayout);
            style_dirty_ = true;
        }

        void invalidate_paint(NodeId id) { tree_.mark_dirty(id, kDirtyPaint); }

        // --- input -------------------------------------------------------------
        RouteResult pump(const InputFrame& input, float scale = 1.0f) {
            solve(); // routing needs an up-to-date layout
            return router_.route(tree_, bridge_, input, scale);
        }

        // --- motion ------------------------------------------------------------
        // Advance reflow springs. Returns true while motion is still animating (so
        // the host keeps requesting frames). NullMotion always returns false.
        bool tick(float dt) {
            if constexpr (requires(Motion& m) { m.begin_frame(); }) motion_.begin_frame();
            last_dt_ = dt;
            return !motion_.settled();
        }

        // --- paint -------------------------------------------------------------
        // Walk the tree pre-order, painting each widget at its resolved (and motion-
        // adjusted) rect. Widgets that clip their children call push_clip/pop_clip
        // themselves; App does not impose clipping.
        void paint(Painter_& painter) {
            solve();
            const NodeId root = tree_.root();
            if (root == kInvalidNode) return;
            tree_.walk(root, [&](NodeId id, std::size_t) {
                Rect2D r = bridge_.rect(id);
                r = motion_.resolve(id, r, last_dt_);
                tree_.widget(id).paint(painter, r);
            });
        }

        // Clear everything (tree, layout, focus, motion).
        void clear() {
            tree_.clear();
            router_.clear();
            motion_.reset();
            structural_dirty_ = true;
        }

    private:
        void flush_style_dirty() {
            tree_.drain_dirty([&](NodeId id, std::uint8_t bits) {
                if (bits & kDirtyLayout) bridge_.update_style(tree_, id);
            });
        }

        Metrics* metrics_;
        tree_type tree_{};
        bridge_type bridge_;
        router_type router_{};
        [[no_unique_address]] Motion motion_{};
        Rect2D viewport_{};
        float last_dt_ = 0.0f;
        bool structural_dirty_ = false;
        bool geometry_dirty_ = false;
        bool style_dirty_ = false;
    };

    // Project-default app: headless kalpana capture canvas + monospace metrics,
    // snap reflow. Good for tests and terminal/blocky HUDs.
    using DefaultApp = App<MonospaceMetrics, DefaultPainter>;
} // namespace pebble::drishya
