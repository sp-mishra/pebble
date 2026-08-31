#pragma once
// ============================================================================
// drishya/painter/draw_list.hpp — SoA command buffer for batched painting
// ----------------------------------------------------------------------------
// An optional intermediate between widget paint() calls and a backend. Instead
// of emitting a backend node per primitive, a widget can record into a DrawList —
// a struct-of-arrays command buffer — and the host flushes the whole list to a
// Painter in one pass at end of frame. This keeps per-primitive cost to a couple
// of vector pushes (no node construction on the hot path) and gives the host a
// single place to sort / cull / batch before touching the backend.
//
// DrawList records the drawing half of the Painter vocabulary (state + clip +
// primitives) but deliberately omits text metrics — it has no font knowledge, so
// it is not itself a Painter. A thin metrics-owning wrapper can forward the
// recording calls here and answer measure_text from its own metrics, letting a
// widget tree paint into the wrapper; replay(painter) then re-issues the recorded
// commands against a real Painter. Storage is plain std::vector (cold path, not
// per-frame-critical); clear() keeps capacity so a reused list never re-allocates
// after warm-up.
// ============================================================================

#include "drishya/widget_concept.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pebble::drishya {

// The kind of each recorded command; indexes the parallel SoA payload arrays.
enum class DrawOp : std::uint8_t {
    SetColor,
    PushClip,
    PopClip,
    FillRect,
    StrokeRect,
    RoundRect,
    Line,
    Text,
};

// SoA command buffer. Commands are appended in draw order; op_[] gives the kind
// and each command consumes the next slot of the payload array(s) it needs.
class DrawList {
public:
    // --- recording (Painter concept surface) ------------------------------
    void set_color(std::uint32_t argb) {
        op_.push_back(DrawOp::SetColor);
        u32_.push_back(argb);
    }
    void push_clip(Bounds2D b) {
        op_.push_back(DrawOp::PushClip);
        bounds_.push_back(b);
    }
    void pop_clip() { op_.push_back(DrawOp::PopClip); }

    void fill_rect(Rect2D r) { rect_cmd(DrawOp::FillRect, r, 0.0f); }
    void stroke_rect(Rect2D r, float width) { rect_cmd(DrawOp::StrokeRect, r, width); }
    void round_rect(Rect2D r, float radius) { rect_cmd(DrawOp::RoundRect, r, radius); }

    void line(Vec2 a, Vec2 b, float width) {
        op_.push_back(DrawOp::Line);
        line_.push_back(Line{a, b, width});
    }
    void text(std::string_view s, Vec2 pos, float font_size) {
        op_.push_back(DrawOp::Text);
        text_.push_back(Text{std::string{s}, pos, font_size});
    }

    // --- lifecycle ---------------------------------------------------------
    void clear() noexcept {
        op_.clear();
        u32_.clear();
        rect_.clear();
        line_.clear();
        text_.clear();
        bounds_.clear();
    }
    [[nodiscard]] std::size_t size() const noexcept { return op_.size(); }
    [[nodiscard]] bool empty() const noexcept { return op_.empty(); }

    // --- replay ------------------------------------------------------------
    // Re-issue every recorded command against a real Painter, in order.
    template <typename P>
        requires Painter<P>
    void replay(P& painter) const {
        std::size_t iu = 0, ir = 0, il = 0, it = 0, ib = 0;
        for (DrawOp op : op_) {
            switch (op) {
                case DrawOp::SetColor:
                    if constexpr (ColorPainter<P>) painter.set_color(u32_[iu]);
                    ++iu;
                    break;
                case DrawOp::PushClip: painter.push_clip(bounds_[ib++]); break;
                case DrawOp::PopClip:  painter.pop_clip(); break;
                case DrawOp::FillRect:   painter.fill_rect(rect_[ir].r); ++ir; break;
                case DrawOp::StrokeRect: painter.stroke_rect(rect_[ir].r, rect_[ir].aux); ++ir; break;
                case DrawOp::RoundRect:  painter.round_rect(rect_[ir].r, rect_[ir].aux); ++ir; break;
                case DrawOp::Line:       painter.line(line_[il].a, line_[il].b, line_[il].width); ++il; break;
                case DrawOp::Text:       painter.text(std::string_view{text_[it].s}, text_[it].pos, text_[it].font_size); ++it; break;
            }
        }
    }

private:
    struct RectCmd { Rect2D r; float aux; };  // aux = stroke width or corner radius
    struct Line { Vec2 a, b; float width; };
    struct Text { std::string s; Vec2 pos; float font_size; };

    void rect_cmd(DrawOp op, Rect2D r, float aux) {
        op_.push_back(op);
        rect_.push_back(RectCmd{r, aux});
    }

    std::vector<DrawOp> op_{};
    std::vector<std::uint32_t> u32_{};
    std::vector<RectCmd> rect_{};
    std::vector<Line> line_{};
    std::vector<Text> text_{};
    std::vector<Bounds2D> bounds_{};
};

} // namespace pebble::drishya
