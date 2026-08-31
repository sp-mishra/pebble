#pragma once
// ============================================================================
// drishya/widget_concept.hpp — the widget & painter contracts
// ----------------------------------------------------------------------------
// Drishya composes UI from *concept-satisfying value types* — no virtual, no
// RTTI, no macros. A widget is anything that can measure itself, describe its
// layout style, handle an event, and expose its children. A painter is any
// backend adapter a widget can draw into. Both are duck-typed via C++20
// concepts so widgets and backends stay fully decoupled.
//
// Geometry types are reused from akruti::layout (Rect2D / Size2D / Bounds2D) and
// color from kalpana — drishya introduces no parallel geometry vocabulary.
// ============================================================================

#include "akruti/layout.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace pebble::drishya {

// Reuse akruti's geometry vocabulary rather than inventing a parallel one.
using akruti::layout::Rect2D;
using akruti::layout::Size2D;
using akruti::layout::Bounds2D;
using akruti::layout::LayoutStyle;
using akruti::layout::ITextMetrics;

// Minimal 2D vector for input positions (kept local; akruti's Vec2 is an
// implementation detail of the solver and math::vec2 pulls in the math lib).
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
    friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
};

// ----------------------------------------------------------------------------
// Input — drishya owns its pointer/keyboard frame because gati models input as
// an abstract button bitset + analog axes only (no pointer or key events).
// ----------------------------------------------------------------------------
enum class Key : std::uint16_t {
    Unknown = 0, Enter, Escape, Tab, Backspace, Delete,
    Left, Right, Up, Down, Home, End, PageUp, PageDown,
    Space, // printable handled via text field of InputFrame
};

struct KeyEvent {
    Key key = Key::Unknown;
    bool pressed = true;  // true = down edge, false = up edge
    bool repeat = false;
    std::uint8_t mods = 0; // bit0 shift, bit1 ctrl, bit2 alt, bit3 super
};

// Pointer button bitmask values.
enum PointerButton : std::uint32_t {
    kPointerNone   = 0,
    kPointerLeft   = 1u << 0,
    kPointerRight  = 1u << 1,
    kPointerMiddle = 1u << 2,
};

// One frame's worth of input, produced by the host and fed to the router.
struct InputFrame {
    Vec2 pointer{};        // absolute pointer position in viewport space
    Vec2 pointer_delta{};  // movement since last frame
    std::uint32_t buttons = kPointerNone; // currently-held pointer buttons
    std::uint32_t prev_buttons = kPointerNone; // held last frame (edge detection)
    float wheel = 0.0f;    // scroll delta this frame
    std::span<const KeyEvent> keys{}; // key edges this frame
    std::string_view text{};          // committed text input this frame (UTF-8)

    [[nodiscard]] bool pressed(std::uint32_t b) const noexcept {
        return (buttons & b) && !(prev_buttons & b);
    }
    [[nodiscard]] bool released(std::uint32_t b) const noexcept {
        return !(buttons & b) && (prev_buttons & b);
    }
    [[nodiscard]] bool held(std::uint32_t b) const noexcept { return (buttons & b) != 0; }
};

// ----------------------------------------------------------------------------
// Event handling result — a small flag set returned from on_event.
// ----------------------------------------------------------------------------
enum class EventResult : std::uint8_t {
    Ignored = 0,        // not handled; keep bubbling
    Consumed = 1,       // handled; stop bubbling
    CapturePointer = 2, // handled; route subsequent pointer input here
    ReleasePointer = 3, // handled; release a prior pointer capture
};

[[nodiscard]] inline bool handled(EventResult r) noexcept {
    return r != EventResult::Ignored;
}

// ----------------------------------------------------------------------------
// Measure / event context passed to widgets.
// ----------------------------------------------------------------------------
// MeasureCtx carries a type-erased-free text metrics reference (the concrete
// type is a template parameter so no virtual dispatch is involved) plus the
// display scale. A widget measures against this.
template <ITextMetrics Metrics>
struct MeasureCtxT {
    const Metrics& text;
    float scale = 1.0f;
};

// EventCtx carries the current input frame and the widget's resolved box so a
// widget can hit-test itself and interpret pointer coordinates locally.
struct EventCtx {
    const InputFrame& input;
    Rect2D box{};      // this widget's resolved rectangle (viewport space)
    Bounds2D clip{};   // active clip bounds
    float scale = 1.0f;

    [[nodiscard]] bool pointer_inside() const noexcept {
        const Vec2 p = input.pointer;
        return p.x >= box.x && p.x <= box.x + box.w &&
               p.y >= box.y && p.y <= box.y + box.h;
    }
    // Pointer position relative to the widget's top-left.
    [[nodiscard]] Vec2 local_pointer() const noexcept {
        return Vec2{input.pointer.x - box.x, input.pointer.y - box.y};
    }
};

// ----------------------------------------------------------------------------
// Painter concept — the backend surface a widget draws into. KalpanaPainter is
// the reference implementation (builds a kalpana::Scene per frame). The concept
// is intentionally an immediate-mode drawing vocabulary; the adapter translates
// it into whatever the backend needs (retained scene nodes for kalpana).
// ----------------------------------------------------------------------------
template <typename P>
concept Painter = requires(P& p, const P& cp,
                           Rect2D rect, Bounds2D clip, Vec2 a, Vec2 b,
                           float r, float w, std::string_view s, float fs) {
    // Clip stack (adapter emulates when the backend lacks native clipping).
    { p.push_clip(clip) };
    { p.pop_clip() };
    // Filled / stroked primitives.
    { p.fill_rect(rect) };
    { p.stroke_rect(rect, w) };
    { p.round_rect(rect, r) };
    { p.line(a, b, w) };
    // Text: draw + measure (measure is delegated to the painter's metrics ref).
    { p.text(s, a, fs) };
    { cp.measure_text(s, fs) } -> std::convertible_to<Size2D>;
};

// A painter that also exposes a settable draw color (most do). Kept separate so
// the core Painter concept stays minimal.
template <typename P>
concept ColorPainter = Painter<P> && requires(P& p, std::uint32_t argb) {
    { p.set_color(argb) };
};

// ----------------------------------------------------------------------------
// Widget concept — the core contract every widget value type satisfies.
// ----------------------------------------------------------------------------
// A widget must:
//   * measure(mc)   -> its intrinsic Size2D given a MeasureCtx
//   * style()       -> its akruti LayoutStyle (const)
//   * on_event(ec)  -> an EventResult given an EventCtx
// Children are exposed generically via a `children()` range in the composite
// concept below; leaf widgets need not provide one.
template <typename W, typename Metrics>
concept MeasurableWith = ITextMetrics<Metrics> && requires(const W& w, MeasureCtxT<Metrics> mc) {
    { w.measure(mc) } -> std::convertible_to<Size2D>;
};

template <typename W>
concept Styled = requires(const W& w) {
    { w.style() } -> std::convertible_to<LayoutStyle>;
};

template <typename W>
concept EventHandler = requires(W& w, EventCtx ec) {
    { w.on_event(ec) } -> std::convertible_to<EventResult>;
};

// The full widget contract, parameterized on the metrics type used to measure.
template <typename W, typename Metrics>
concept Widget = MeasurableWith<W, Metrics> && Styled<W> && EventHandler<W>;

// A widget that can be painted by a specific painter P.
template <typename W, typename P>
concept PaintableWith = Painter<P> && requires(const W& w, P& painter, Rect2D box) {
    { w.paint(painter, box) };
};

// A composite widget exposes its children as a range of child widgets.
template <typename W>
concept HasChildren = requires(const W& w) {
    { w.children() };
};

} // namespace pebble::drishya
