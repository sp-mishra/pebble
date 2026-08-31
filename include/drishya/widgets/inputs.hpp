#pragma once
// ============================================================================
// drishya/widgets/inputs.hpp — interactive input widgets
// ----------------------------------------------------------------------------
// Widgets that respond to pointer/keyboard: button, toggle (switch), checkbox,
// slider, text_field, select. Each implements on_event to update its own state
// and fire a Callback / write a bound value. Pointer-drag widgets (slider) use
// the router's capture protocol by returning CapturePointer / ReleasePointer.
//
// State is held by value inside the widget; a widget may also expose a setter
// the app binds to a Signal. No virtual, no macros; nothrow-move so they fit
// AnyWidget's inline buffer.
// ============================================================================

#include "drishya/reactive.hpp"
#include "drishya/widgets/base.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pebble::drishya::widgets {

using akruti::layout::SizeSpec;
using akruti::layout::Edges;

// ----------------------------------------------------------------------------
// Button — a clickable rounded rect with a text label. Fires on_click on a
// left-button release that lands inside the box (press+release semantics).
// ----------------------------------------------------------------------------
struct Button {
    LayoutStyle style_{};
    std::string text{};
    Callback on_click{};
    std::uint32_t background = 0xFF3B82F6u;
    std::uint32_t background_hover = 0xFF60A5FAu;
    std::uint32_t color = 0xFFFFFFFFu;
    float font_size = 14.0f;
    float radius = 6.0f;
    bool hovered = false;
    bool pressed = false;

    Button() = default;
    explicit Button(std::string t) : text(std::move(t)) {
        style_.padding = Edges{12.0f, 8.0f, 12.0f, 8.0f};
    }

    Button& on(Callback cb) { on_click = std::move(cb); return *this; }

    [[nodiscard]] LayoutStyle style() const noexcept { return style_; }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>& mc) const {
        const Size2D t = mc.text.measure(text.c_str(), 0.0f);
        return Size2D{t.w + 24.0f, t.h + 16.0f};
    }

    [[nodiscard]] EventResult on_event(EventCtx& ec) {
        hovered = ec.pointer_inside();
        if (!hovered) { pressed = false; return EventResult::Ignored; }
        if (ec.input.pressed(kPointerLeft)) { pressed = true; return EventResult::Consumed; }
        if (ec.input.released(kPointerLeft) && pressed) {
            pressed = false;
            if (on_click) on_click();
            return EventResult::Consumed;
        }
        return EventResult::Consumed; // hovering swallows so it can show hover state
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(hovered ? background_hover : background);
        painter.round_rect(box, radius);
        if constexpr (ColorPainter<P>) painter.set_color(color);
        painter.text(std::string_view{text},
                     Vec2{box.x + 12.0f, box.y + box.h * 0.5f + font_size * 0.35f}, font_size);
    }
};
[[nodiscard]] inline Button button(std::string t) { return Button{std::move(t)}; }

// ----------------------------------------------------------------------------
// Toggle / Switch — a boolean pill that flips on click and fires on_change.
// ----------------------------------------------------------------------------
struct Toggle : WidgetBase {
    bool on_state = false;
    BasicCallback<64> on_change{}; // called after the state flips
    std::uint32_t track_off = 0x40FFFFFFu;
    std::uint32_t track_on = 0xFF22C55Eu;
    std::uint32_t knob = 0xFFFFFFFFu;

    Toggle() noexcept { style_.width = SizeSpec::Px(40.0f); style_.height = SizeSpec::Px(22.0f); }
    explicit Toggle(bool v) noexcept : on_state(v) {
        style_.width = SizeSpec::Px(40.0f);
        style_.height = SizeSpec::Px(22.0f);
    }

    [[nodiscard]] EventResult on_event(EventCtx& ec) {
        if (ec.pointer_inside() && ec.input.released(kPointerLeft)) {
            on_state = !on_state;
            if (on_change) on_change();
            return EventResult::Consumed;
        }
        return EventResult::Ignored;
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(on_state ? track_on : track_off);
        painter.round_rect(box, box.h * 0.5f);
        const float d = box.h - 4.0f;
        const float kx = on_state ? (box.x + box.w - d - 2.0f) : (box.x + 2.0f);
        if constexpr (ColorPainter<P>) painter.set_color(knob);
        painter.round_rect(Rect2D{kx, box.y + 2.0f, d, d}, d * 0.5f);
    }
};

// ----------------------------------------------------------------------------
// Checkbox — a boolean box with a check mark; flips on click.
// ----------------------------------------------------------------------------
struct Checkbox : WidgetBase {
    bool checked = false;
    BasicCallback<64> on_change{};
    std::uint32_t box_color = 0xFF9BA7B4u;
    std::uint32_t check_color = 0xFF3B82F6u;

    Checkbox() noexcept { style_.width = SizeSpec::Px(18.0f); style_.height = SizeSpec::Px(18.0f); }
    explicit Checkbox(bool v) noexcept : checked(v) {
        style_.width = SizeSpec::Px(18.0f);
        style_.height = SizeSpec::Px(18.0f);
    }

    [[nodiscard]] EventResult on_event(EventCtx& ec) {
        if (ec.pointer_inside() && ec.input.released(kPointerLeft)) {
            checked = !checked;
            if (on_change) on_change();
            return EventResult::Consumed;
        }
        return EventResult::Ignored;
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(box_color);
        painter.stroke_rect(box, 2.0f);
        if (checked) {
            if constexpr (ColorPainter<P>) painter.set_color(check_color);
            painter.fill_rect(Rect2D{box.x + 4.0f, box.y + 4.0f, box.w - 8.0f, box.h - 8.0f});
        }
    }
};

// ----------------------------------------------------------------------------
// Slider — horizontal value control over [min,max]. Grabs the pointer on press
// (CapturePointer) and tracks drag until release (ReleasePointer), writing the
// value and firing on_change each move.
// ----------------------------------------------------------------------------
struct Slider {
    LayoutStyle style_{};
    float value = 0.0f;
    float min_value = 0.0f;
    float max_value = 1.0f;
    BasicCallback<64> on_change{};
    std::uint32_t track = 0x40FFFFFFu;
    std::uint32_t fill = 0xFF3B82F6u;
    std::uint32_t knob = 0xFFFFFFFFu;
    bool dragging = false;

    Slider() noexcept { style_.height = SizeSpec::Px(20.0f); }
    Slider(float v, float lo, float hi) noexcept : value(v), min_value(lo), max_value(hi) {
        style_.height = SizeSpec::Px(20.0f);
    }

    [[nodiscard]] LayoutStyle style() const noexcept { return style_; }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        return Size2D{120.0f, 20.0f};
    }

    [[nodiscard]] EventResult on_event(EventCtx& ec) {
        if (ec.input.pressed(kPointerLeft) && ec.pointer_inside()) {
            dragging = true;
            set_from_pointer(ec);
            return EventResult::CapturePointer;
        }
        if (dragging && ec.input.held(kPointerLeft)) {
            set_from_pointer(ec);
            return EventResult::Consumed;
        }
        if (dragging && ec.input.released(kPointerLeft)) {
            dragging = false;
            return EventResult::ReleasePointer;
        }
        return EventResult::Ignored;
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        const float cy = box.y + box.h * 0.5f;
        const float th = 4.0f;
        if constexpr (ColorPainter<P>) painter.set_color(track);
        painter.round_rect(Rect2D{box.x, cy - th * 0.5f, box.w, th}, th * 0.5f);
        const float t = norm();
        if constexpr (ColorPainter<P>) painter.set_color(fill);
        painter.round_rect(Rect2D{box.x, cy - th * 0.5f, box.w * t, th}, th * 0.5f);
        const float d = box.h - 4.0f;
        if constexpr (ColorPainter<P>) painter.set_color(knob);
        painter.round_rect(Rect2D{box.x + box.w * t - d * 0.5f, cy - d * 0.5f, d, d}, d * 0.5f);
    }

private:
    [[nodiscard]] float norm() const noexcept {
        const float span = max_value - min_value;
        return span > 0.0f ? (value - min_value) / span : 0.0f;
    }
    void set_from_pointer(EventCtx& ec) {
        const float t = ec.box.w > 0.0f
                            ? (ec.local_pointer().x / ec.box.w) : 0.0f;
        const float clamped = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        value = min_value + clamped * (max_value - min_value);
        if (on_change) on_change();
    }
};

// ----------------------------------------------------------------------------
// TextField — single-line editable text. Appends committed text, handles
// Backspace, fires on_change. Focus is decided by the router; the field paints
// a caret when it currently holds focus (the app can pass has_focus in).
// ----------------------------------------------------------------------------
struct TextField {
    LayoutStyle style_{};
    std::string value{};
    std::string placeholder{};
    BasicCallback<64> on_change{};
    std::uint32_t background = 0xFF1F262Eu;
    std::uint32_t border = 0xFF2A333Du;
    std::uint32_t color = 0xFFE6EDF3u;
    float font_size = 14.0f;
    bool focused = false;

    TextField() noexcept { init(); }
    explicit TextField(std::string initial) : value(std::move(initial)) { init(); }

    [[nodiscard]] LayoutStyle style() const noexcept { return style_; }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        return Size2D{160.0f, font_size + 12.0f};
    }

    [[nodiscard]] EventResult on_event(EventCtx& ec) {
        focused = ec.pointer_inside() ? true : focused;
        if (!focused) return EventResult::Ignored;
        bool changed = false;
        for (const KeyEvent& k : ec.input.keys) {
            if (k.pressed && k.key == Key::Backspace && !value.empty()) {
                value.pop_back();
                changed = true;
            }
        }
        if (!ec.input.text.empty()) {
            value.append(ec.input.text);
            changed = true;
        }
        if (changed && on_change) on_change();
        return (changed || ec.pointer_inside()) ? EventResult::Consumed : EventResult::Ignored;
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(background);
        painter.round_rect(box, 4.0f);
        if constexpr (ColorPainter<P>) painter.set_color(border);
        painter.stroke_rect(box, focused ? 2.0f : 1.0f);
        const bool empty = value.empty();
        if constexpr (ColorPainter<P>) painter.set_color(empty ? 0xFF5B6673u : color);
        const std::string_view shown = empty ? std::string_view{placeholder}
                                              : std::string_view{value};
        painter.text(shown, Vec2{box.x + 8.0f, box.y + box.h * 0.5f + font_size * 0.35f}, font_size);
    }

private:
    void init() noexcept {
        style_.height = SizeSpec::Px(font_size + 12.0f);
        style_.padding = Edges{8.0f, 6.0f, 8.0f, 6.0f};
    }
};

// ----------------------------------------------------------------------------
// Select — a dropdown showing the current choice; click cycles to the next
// option (a full popup list is a composite the builder can assemble later).
// ----------------------------------------------------------------------------
struct Select {
    LayoutStyle style_{};
    std::vector<std::string> options{};
    std::uint32_t index = 0;
    BasicCallback<64> on_change{};
    std::uint32_t background = 0xFF1F262Eu;
    std::uint32_t color = 0xFFE6EDF3u;
    float font_size = 14.0f;

    Select() noexcept { init(); }
    explicit Select(std::vector<std::string> opts) : options(std::move(opts)) { init(); }

    [[nodiscard]] LayoutStyle style() const noexcept { return style_; }

    template <ITextMetrics Metrics>
    [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>&) const noexcept {
        return Size2D{160.0f, font_size + 12.0f};
    }

    [[nodiscard]] std::string_view current() const noexcept {
        return (index < options.size()) ? std::string_view{options[index]} : std::string_view{};
    }

    [[nodiscard]] EventResult on_event(EventCtx& ec) {
        if (ec.pointer_inside() && ec.input.released(kPointerLeft) && !options.empty()) {
            index = (index + 1) % static_cast<std::uint32_t>(options.size());
            if (on_change) on_change();
            return EventResult::Consumed;
        }
        return EventResult::Ignored;
    }

    template <typename P>
        requires Painter<P>
    void paint(P& painter, Rect2D box) const {
        if constexpr (ColorPainter<P>) painter.set_color(background);
        painter.round_rect(box, 4.0f);
        if constexpr (ColorPainter<P>) painter.set_color(color);
        painter.text(current(), Vec2{box.x + 8.0f, box.y + box.h * 0.5f + font_size * 0.35f}, font_size);
    }

private:
    void init() noexcept {
        style_.height = SizeSpec::Px(font_size + 12.0f);
        style_.padding = Edges{8.0f, 6.0f, 8.0f, 6.0f};
    }
};

} // namespace pebble::drishya::widgets
