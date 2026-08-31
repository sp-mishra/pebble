#pragma once
// ============================================================================
// drishya/widgets/stubs.hpp — placeholder widgets (concept-complete, TODO paint)
// ----------------------------------------------------------------------------
// These types satisfy the Widget + PaintableWith concepts today so they compose
// and lay out, but their paint is a labelled placeholder pending a full build.
// Grouping them here keeps the "real" widget headers focused; each stub is a
// value type (no virtual, no macros) and nothrow-move for AnyWidget storage.
//
// Shared placeholder behaviour lives in StubBase (faint outline + kind label);
// each stub inherits it and passes its kind through kind(). Replace a stub by
// giving it a real measure/paint in its own header when it graduates.
// ============================================================================

#include "drishya/widgets/base.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pebble::drishya::widgets {

// Base for placeholder widgets: paints a faint outline and a kind label so an
// in-progress UI stays legible. Derived stubs override kind() to name themselves
// and add their own data fields. Non-virtual — kind() is hidden by the derived
// member and picked up statically by paint() through the derived `this`.
struct StubBase : WidgetBase {
    std::string label{};

    [[nodiscard]] std::string_view kind() const noexcept { return "widget"; }

    template <typename P, typename Self>
        requires Painter<P>
    void paint_as(P& painter, Rect2D box, const Self& self) const {
        if constexpr (ColorPainter<P>) painter.set_color(0x402A333Du);
        painter.stroke_rect(box, 1.0f);
        if constexpr (ColorPainter<P>) painter.set_color(0xFF5B6673u);
        const std::string_view text = label.empty() ? self.kind() : std::string_view{label};
        painter.text(text, Vec2{box.x + 6.0f, box.y + 16.0f}, 12.0f);
    }
};

// --- Dashboard / document stubs --------------------------------------------
struct Markdown : StubBase {
    std::string source{};
    [[nodiscard]] std::string_view kind() const noexcept { return "markdown"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct CodeEditor : StubBase {
    std::string source{}; std::string lang{};
    [[nodiscard]] std::string_view kind() const noexcept { return "code_editor"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct TextArea : StubBase {
    std::string value{};
    [[nodiscard]] std::string_view kind() const noexcept { return "text_area"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct LogView : StubBase {
    std::size_t line_count = 0;
    [[nodiscard]] std::string_view kind() const noexcept { return "log_view"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct TreeView : StubBase {
    std::size_t node_count = 0;
    [[nodiscard]] std::string_view kind() const noexcept { return "tree_view"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Chart : StubBase {
    [[nodiscard]] std::string_view kind() const noexcept { return "chart"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Heatmap : StubBase {
    std::uint32_t rows = 0, cols = 0;
    [[nodiscard]] std::string_view kind() const noexcept { return "heatmap"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct ImageGrid : StubBase {
    std::uint32_t columns = 1;
    [[nodiscard]] std::string_view kind() const noexcept { return "image_grid"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};

// --- Input stubs ------------------------------------------------------------
struct ColorPicker : StubBase {
    std::uint32_t value = 0xFFFFFFFFu;
    [[nodiscard]] std::string_view kind() const noexcept { return "color_picker"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct DatePicker : StubBase {
    std::string value{};
    [[nodiscard]] std::string_view kind() const noexcept { return "date_picker"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct FileDrop : StubBase {
    [[nodiscard]] std::string_view kind() const noexcept { return "file_drop"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Combo : StubBase {
    std::string value{};
    [[nodiscard]] std::string_view kind() const noexcept { return "combo"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct RangeSlider : StubBase {
    float lo = 0.0f, hi = 1.0f;
    [[nodiscard]] std::string_view kind() const noexcept { return "range_slider"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct NumberField : StubBase {
    double value = 0.0;
    [[nodiscard]] std::string_view kind() const noexcept { return "number"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct RadioGroup : StubBase {
    std::uint32_t selected = 0;
    [[nodiscard]] std::string_view kind() const noexcept { return "radio_group"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};

// --- Container / overlay stubs ---------------------------------------------
struct Accordion : StubBase {
    bool expanded = false;
    [[nodiscard]] std::string_view kind() const noexcept { return "accordion"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Overlay : StubBase {
    bool open = false;
    [[nodiscard]] std::string_view kind() const noexcept { return "overlay"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Modal : StubBase {
    bool open = false;
    [[nodiscard]] std::string_view kind() const noexcept { return "modal"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Popover : StubBase {
    bool open = false;
    [[nodiscard]] std::string_view kind() const noexcept { return "popover"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Drawer : StubBase {
    bool open = false;
    [[nodiscard]] std::string_view kind() const noexcept { return "drawer"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Avatar : StubBase {
    [[nodiscard]] std::string_view kind() const noexcept { return "avatar"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Image : StubBase {
    std::string source{};
    [[nodiscard]] std::string_view kind() const noexcept { return "image"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};

// --- Game / HUD stubs -------------------------------------------------------
struct InventoryGrid : StubBase {
    std::uint32_t rows = 1, cols = 1;
    [[nodiscard]] std::string_view kind() const noexcept { return "inventory_grid"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Minimap : StubBase {
    [[nodiscard]] std::string_view kind() const noexcept { return "minimap"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct DialogueBox : StubBase {
    std::string speaker{}; std::string body{};
    [[nodiscard]] std::string_view kind() const noexcept { return "dialogue_box"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};
struct Hotbar : StubBase {
    std::uint32_t slots = 8; std::uint32_t active = 0;
    [[nodiscard]] std::string_view kind() const noexcept { return "hotbar"; }
    template <typename P> requires Painter<P>
    void paint(P& p, Rect2D b) const { paint_as(p, b, *this); }
};

} // namespace pebble::drishya::widgets
