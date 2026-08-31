// ============================================================================
// drishya_custom_widget.cpp — Drishya example: authoring a widget from scratch.
// ----------------------------------------------------------------------------
// Drishya widgets are plain value types that satisfy the Widget + PaintableWith
// concepts — no base class, virtual, macro, or registration is required. The
// widgets:: stock types just inherit WidgetBase for trivial defaults. Here we
// build a bespoke "VU meter" (a column of level segments) two ways:
//   1. inheriting WidgetBase and overriding only measure()/paint(),
//   2. as a fully standalone struct implementing every concept hook by hand.
// Both drop straight into the same App tree via AnyWidget's static vtable.
// ============================================================================

#include "test/example_registry.hpp"

#include "drishya/drishya.hpp"

#include <array>
#include <string_view>
#include <vector>

namespace {

using namespace pebble::drishya;
namespace w = pebble::drishya::widgets;

// --- (1) Reuse WidgetBase; override only what differs. ----------------------
struct VuMeter : w::WidgetBase {
    float level = 0.0f;             // 0..1 fill fraction
    std::uint32_t segments = 12;
    std::uint32_t on_color = 0xFF22C55Eu;
    std::uint32_t off_color = 0x40FFFFFFu;

    VuMeter() noexcept { style_.width = akruti::layout::SizeSpec::Px(24.0f); }
    explicit VuMeter(float lvl) noexcept : level(lvl) {
        style_.width = akruti::layout::SizeSpec::Px(24.0f);
    }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        return Size2D{24.0f, static_cast<float>(segments) * 6.0f};
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        const float clamped = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
        const auto lit = static_cast<std::uint32_t>(clamped * static_cast<float>(segments) + 0.5f);
        const float gap = 2.0f;
        const float seg_h = (box.h - gap * static_cast<float>(segments - 1)) / static_cast<float>(segments);
        for (std::uint32_t i = 0; i < segments; ++i) {
            // Segment 0 is at the bottom.
            const float y = box.y + box.h - (static_cast<float>(i) + 1.0f) * seg_h
                            - static_cast<float>(i) * gap;
            const bool on = i < lit;
            if constexpr (ColorPainter<P>) painter.set_color(on ? on_color : off_color);
            painter.fill_rect(Rect2D{box.x, y, box.w, seg_h});
        }
    }
};

// --- (2) A standalone widget: no base, every hook written out. --------------
struct Dot {
    LayoutStyle style_{};
    std::uint32_t color = 0xFFF59E0Bu;
    float diameter = 10.0f;

    [[nodiscard]] LayoutStyle style() const noexcept { return style_; }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        return Size2D{diameter, diameter};
    }

    [[nodiscard]] EventResult on_event(EventCtx&) noexcept { return EventResult::Ignored; }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(color);
        painter.round_rect(box, diameter * 0.5f);
    }
};

struct DrishyaCustomWidgetExample {
    static constexpr std::string_view name() { return "drishya_custom_widget"; }
    static constexpr std::string_view description() {
        return "Drishya extensibility: two bespoke widgets (a VU meter and a "
               "standalone dot) satisfying the Widget concept with no macros.";
    }
    static constexpr std::array<std::string_view, 2> tag_data{"drishya", "ui"};
    static constexpr std::span<const std::string_view> tags() { return tag_data; }

    static testfw::Result run() {
        using M = MonospaceMetrics;
        using P = DefaultPainter;

        // Compile-time proof the custom types are usable widgets.
        static_assert(Widget<VuMeter, M>, "VuMeter must satisfy Widget");
        static_assert(PaintableWith<VuMeter, P>, "VuMeter must be paintable");
        static_assert(Widget<Dot, M>, "Dot must satisfy Widget");
        static_assert(PaintableWith<Dot, P>, "Dot must be paintable");

        M metrics;
        App<M, P> app(metrics);

        auto row = w::hstack(6.0f);
        row.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
        row.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
        row.style_.align_items = akruti::layout::Align::End;
        const NodeId row_id = app.set_root(std::move(row));

        // A bank of VU meters at varying levels.
        const std::array<float, 6> levels{0.2f, 0.5f, 0.8f, 1.0f, 0.65f, 0.35f};
        for (const float lvl : levels) {
            VuMeter meter{lvl};
            meter.style_.height = akruti::layout::SizeSpec::Percent(90.0f);
            app.add_child(row_id, std::move(meter));
        }
        // A standalone indicator dot at the end.
        Dot dot;
        dot.color = 0xFFEF4444u;
        app.add_child(row_id, std::move(dot));

        kalpana::DefaultCanvas canvas(256, 128);
        app.set_viewport(Rect2D{0.0f, 0.0f, 256.0f, 128.0f});

        P painter(canvas, metrics);
        painter.begin_frame();
        painter.set_color(0xFF0B0E12u);
        painter.fill_rect(Rect2D{0.0f, 0.0f, 256.0f, 128.0f});
        app.paint(painter);
        painter.present();

        const std::vector<std::uint32_t> px = canvas.snapshot();
        if (px.size() != 256u * 128u) return testfw::fail("unexpected canvas size");
        if (app.tree().node_count() != 8) return testfw::fail("unexpected node count");
        return {};
    }
};

} // namespace
