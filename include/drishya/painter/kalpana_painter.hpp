#pragma once
// ============================================================================
// drishya/painter/kalpana_painter.hpp — Painter adapter over a kalpana Canvas
// ----------------------------------------------------------------------------
// kalpana is a *retained* scene-graph renderer: you build a kalpana::Scene of
// value-typed nodes and call canvas.render(scene). Drishya's Painter concept is
// an *immediate-mode* drawing vocabulary. KalpanaPainter bridges the two: each
// immediate call appends a Node to a Scene it owns for the current frame, and
// present() hands the finished Scene to the canvas.
//
// kalpana has no clip API, so clipping is emulated: push_clip/pop_clip maintain
// a scissor stack drishya-side; drawing is intersected against the top clip
// before emission. kalpana also has no text metrics, so the painter holds a
// reference to an ITextMetrics object supplied by the host — measure_text
// delegates to it. This is the "adapter owns a metrics ref" decision.
// ============================================================================

#include "drishya/widget_concept.hpp"

#include "kalpana/kalpana.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pebble::drishya {
    // Convert a packed 0xAARRGGBB value to a linear kalpana::Color.
    [[nodiscard]] inline kalpana::Color argb_to_color(std::uint32_t argb) noexcept {
        const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
        const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
        const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
        const float b = static_cast<float>(argb & 0xFF) / 255.0f;
        // kalpana::Color is linear; treat the byte channels as already-linear here
        // (host themes provide linear values). from_srgb8 is available if callers
        // want gamma decoding.
        return kalpana::Color(r, g, b, a);
    }

    // ----------------------------------------------------------------------------
    // KalpanaPainter<Canvas, Metrics>
    //   Canvas  : a kalpana::Canvas<Backend> (defaults to DefaultCanvas).
    //   Metrics : an ITextMetrics providing measure(text, max_width).
    // ----------------------------------------------------------------------------
    template <typename Canvas, ITextMetrics Metrics>
    class KalpanaPainter;

    // A trivial monospace metrics used when the host supplies none. Estimates a
    // fixed advance per code unit — good enough for headless tests and blocky HUDs.
    struct MonospaceMetrics {
        float advance = 8.0f; // px per character at font_size == line
        float line = 16.0f; // line height in px
        [[nodiscard]] Size2D measure(const char* text, float /*max_width*/) const noexcept {
            std::size_t n = 0;
            if (text) { while (text[n] != '\0') ++n; }
            return Size2D{static_cast<float>(n) * advance, line};
        }
    };

    template <typename Canvas, ITextMetrics Metrics>
    class KalpanaPainter {
    public:
        explicit KalpanaPainter(Canvas& canvas, const Metrics& metrics) noexcept
            : canvas_(&canvas), metrics_(&metrics) {}

        // --- frame lifecycle --------------------------------------------------
        void begin_frame() {
            scene_.clear();
            clips_.clear();
            color_ = 0xFF000000u; // opaque black default
        }

        // Hand the accumulated scene to the canvas.
        void present() { canvas_->render(scene_); }

        kalpana::Scene& scene() noexcept { return scene_; }
        void set_clear_color(std::uint32_t argb) { scene_.clear_color(argb_to_color(argb)); }

        // --- Painter concept: state ------------------------------------------
        void set_color(std::uint32_t argb) noexcept { color_ = argb; }

        // --- Painter concept: clip stack (emulated) --------------------------
        void push_clip(Bounds2D b) { clips_.push_back(current_clip_intersect(b)); }
        void pop_clip() { if (!clips_.empty()) clips_.pop_back(); }

        // --- Painter concept: primitives -------------------------------------
        void fill_rect(Rect2D r) {
            if (!visible(r)) return;
            kalpana::Path p;
            p.rect(r.x, r.y, r.w, r.h);
            scene_ << kalpana::Node::shape(std::move(p), kalpana::Paint::fill(argb_to_color(color_)));
        }

        void stroke_rect(Rect2D r, float width) {
            if (!visible(r)) return;
            kalpana::Path p;
            p.rect(r.x, r.y, r.w, r.h);
            scene_ << kalpana::Node::shape(std::move(p), kalpana::Paint::stroke(argb_to_color(color_), width));
        }

        void round_rect(Rect2D r, float radius) {
            if (!visible(r)) return;
            kalpana::Path p;
            p.round_rect(r.x, r.y, r.w, r.h, radius, radius);
            scene_ << kalpana::Node::shape(std::move(p), kalpana::Paint::fill(argb_to_color(color_)));
        }

        void line(Vec2 a, Vec2 b, float width) {
            kalpana::Path p;
            p.move_to(a.x, a.y);
            p.line_to(b.x, b.y);
            scene_ << kalpana::Node::shape(std::move(p), kalpana::Paint::stroke(argb_to_color(color_), width));
        }

        // Fill an arbitrary prebuilt path (used by icons / sparklines).
        void fill_path(kalpana::Path path) {
            scene_ << kalpana::Node::shape(std::move(path), kalpana::Paint::fill(argb_to_color(color_)));
        }

        void text(std::string_view s, Vec2 pos, float font_size) {
            scene_ << kalpana::Node::text(s, argb_to_color(color_), font_size, pos.x, pos.y);
        }

        // Draw a pixel image (ARGB8888) into a destination rect.
        void image(const std::uint32_t* pixels, std::uint32_t w, std::uint32_t h, Rect2D dst) {
            kalpana::ImageNode img{};
            img.pixels = pixels;
            img.w = w;
            img.h = h;
            img.dx = dst.x;
            img.dy = dst.y;
            img.dw = dst.w;
            img.dh = dst.h;
            kalpana::Node node{};
            node.content = img;
            scene_ << std::move(node);
        }

        // --- Painter concept: metrics ----------------------------------------
        [[nodiscard]] Size2D measure_text(std::string_view s, float /*font_size*/) const {
            // Metrics::measure takes a NUL-terminated C string; copy through a small
            // buffer to guarantee termination for arbitrary string_views.
            text_scratch_.assign(s.begin(), s.end());
            return metrics_->measure(text_scratch_.c_str(), 0.0f);
        }

        [[nodiscard]] const Metrics& metrics() const noexcept { return *metrics_; }

    private:
        [[nodiscard]] Bounds2D current_clip_intersect(Bounds2D b) const {
            if (clips_.empty()) return b;
            const Bounds2D& top = clips_.back();
            float lo_x = (b.lo[0] > top.lo[0]) ? b.lo[0] : top.lo[0];
            float lo_y = (b.lo[1] > top.lo[1]) ? b.lo[1] : top.lo[1];
            float hi_x = (b.hi[0] < top.hi[0]) ? b.hi[0] : top.hi[0];
            float hi_y = (b.hi[1] < top.hi[1]) ? b.hi[1] : top.hi[1];
            if (hi_x < lo_x) hi_x = lo_x;
            if (hi_y < lo_y) hi_y = lo_y;
            return Bounds2D(akruti::layout::Vec2{lo_x, lo_y}, akruti::layout::Vec2{hi_x, hi_y});
        }

        // Cheap visibility test against the active clip (skip fully-clipped rects).
        [[nodiscard]] bool visible(Rect2D r) const {
            if (clips_.empty()) return true;
            const Bounds2D& c = clips_.back();
            if (r.x + r.w < c.lo[0] || r.x > c.hi[0]) return false;
            if (r.y + r.h < c.lo[1] || r.y > c.hi[1]) return false;
            return true;
        }

        Canvas* canvas_;
        const Metrics* metrics_;
        kalpana::Scene scene_{};
        std::vector<Bounds2D> clips_{};
        std::uint32_t color_ = 0xFF000000u;
        mutable std::string text_scratch_{};
    };

    // Convenience alias: the project-default painter is a capture-backed canvas with
    // monospace metrics (headless-friendly, satisfies Painter).
    using DefaultPainter = KalpanaPainter<kalpana::DefaultCanvas, MonospaceMetrics>;
} // namespace pebble::drishya
