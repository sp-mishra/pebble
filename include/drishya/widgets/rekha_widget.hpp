#pragma once
// ============================================================================
// drishya/widgets/rekha_widget.hpp — RekhaWidget: rekha::Figure as a Drishya widget
// ----------------------------------------------------------------------------
// Opt-in via #include — requires rekha/rekha.hpp to be on the include path.
// Guards: compilation silently skipped if rekha is absent.
//
// RekhaWidget embeds a rekha::Figure and renders it into a sub-rect of the
// parent kalpana::Scene using KalpanaBackend's offset begin_frame overload.
// After rendering, it merges the backend's scene nodes into the painter's
// scene via a loop over the root GroupNode's children.
//
// Usage:
//   RekhaWidget rw;
//   rw.figure().add(rekha::LinePlot{series});
//   // embed in an App tree — paint() wired automatically
// ============================================================================

#if __has_include("rekha/rekha.hpp")

#include "drishya/widgets/base.hpp"
#include "rekha/rekha.hpp"

#include <cstdint>
#include <variant>

namespace pebble::drishya::widgets {

// RekhaWidget — concept-satisfying widget that renders a rekha::Figure into
// the widget's allocated box using KalpanaBackend with coordinate offsetting.
// Callers build the figure via figure(), add plots, and let paint() do the rest.
class RekhaWidget : public WidgetBase {
public:
    explicit RekhaWidget(float preferred_w = 400.0f, float preferred_h = 300.0f,
                         std::uint32_t bg_argb = 0xFF0A0A12u)
        : bg_(argb_to_kalpana_color(bg_argb))
    {
        using akruti::layout::SizeSpec;
        style_.width  = SizeSpec::Px(preferred_w);
        style_.height = SizeSpec::Px(preferred_h);
    }

    // ---- accessors ---------------------------------------------------------
    [[nodiscard]] rekha::Figure& figure() noexcept { return figure_; }
    [[nodiscard]] const rekha::Figure& figure() const noexcept { return figure_; }

    // Point to an externally-owned figure updated each frame. When set, this
    // takes priority over figure_ so the widget always paints fresh data.
    // The pointed-to figure must outlive this widget.
    void set_live(const rekha::Figure* fig) noexcept { live_fig_ = fig; }

    void set_bg(std::uint32_t argb) noexcept { bg_ = argb_to_kalpana_color(argb); }

    // ---- Painter: KalpanaPainter specialisation ----------------------------
    // paint() only wires into KalpanaPainter-backed paint calls; other painters
    // receive a no-op (default from WidgetBase).

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (requires { painter.scene(); }) {
            do_paint(painter.scene(), box);
        }
    }

private:
    // Render figure at local (0,0) then translate all geometry to box origin.
    // Note: Figure::render() always calls begin_frame(w,h,bg) internally which
    // resets any prior offset — so we render at origin then shift post-hoc.
    void do_paint(kalpana::Scene& parent_scene, Rect2D box) const {
        const auto w = static_cast<std::uint32_t>(box.w > 0 ? box.w : 1u);
        const auto h = static_cast<std::uint32_t>(box.h > 0 ? box.h : 1u);

        rekha::Figure render_fig = live_fig_ ? *live_fig_ : figure_;
        render_fig.viewport({w, h, {}});
        render_fig.render(backend_);  // internally calls begin_frame(w,h,bg)
        backend_.end_frame();

        const float ox = box.x, oy = box.y;
        kalpana::Scene& bscene = backend_.scene();  // backend_ is mutable; no cast needed
        kalpana::Node& root = bscene.root();
        if (auto* grp = std::get_if<kalpana::GroupNode>(&root.content)) {
            for (kalpana::Node& child : grp->children) {
                translate_node(child, ox, oy);
                parent_scene.add(child);
            }
        }
    }

    static void translate_node(kalpana::Node& n, float ox, float oy) {
        if (auto* g = std::get_if<kalpana::GroupNode>(&n.content)) {
            for (kalpana::Node& child : g->children)
                translate_node(child, ox, oy);
        } else if (std::get_if<kalpana::ShapeNode>(&n.content)) {
            n.xf = kalpana::Transform::translate(ox, oy).combine(n.xf);
        } else if (auto* t = std::get_if<kalpana::TextNode>(&n.content)) {
            t->x += ox;
            t->y += oy;
        }
    }

    [[nodiscard]] static kalpana::Color argb_to_kalpana_color(std::uint32_t argb) noexcept {
        const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
        const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
        const float g = static_cast<float>((argb >> 8)  & 0xFF) / 255.0f;
        const float b = static_cast<float>( argb        & 0xFF) / 255.0f;
        return kalpana::Color{r, g, b, a};
    }

    rekha::Figure              figure_{};
    const rekha::Figure*       live_fig_ = nullptr; // externally owned; if set, used instead of figure_
    kalpana::Color             bg_{0.04f, 0.04f, 0.07f, 1.0f};
    mutable rekha::KalpanaBackend backend_{};
};

} // namespace pebble::drishya::widgets

#endif // __has_include("rekha/rekha.hpp")
