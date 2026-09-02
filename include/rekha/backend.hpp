#pragma once

#include "types.hpp"
#include "kalpana/kalpana.hpp"
#include <concepts>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rekha {
    template <class B>
    concept PlotBackend = requires(B b, std::uint32_t w, std::uint32_t h, Color c, std::span<const Vec2> pts,
                                   Scalar x, Scalar y, Scalar r, Scalar x0, Scalar y0, Scalar x1, Scalar y1,
                                   std::string_view label, StrokeStyle s) {
        { b.begin_frame(w, h, c) } -> std::same_as<void>;
        { b.draw_line(x0, y0, x1, y1, s) } -> std::same_as<void>;
        { b.draw_polyline(pts, s) } -> std::same_as<void>;
        { b.draw_circle(x, y, r, c) } -> std::same_as<void>;
        { b.draw_rect(x, y, x1, y1, c) } -> std::same_as<void>;
        { b.draw_text(label, x, y, c) } -> std::same_as<void>;
        { b.end_frame() } -> std::same_as<void>;
    };

    // Kalpana-backed retained scene backend. Agnostic Rekha API renders to Kalpana nodes.
    class KalpanaBackend {
    public:
        // Standard begin_frame — origin at (0,0).
        void begin_frame(std::uint32_t w, std::uint32_t h, Color clear) {
            ox_ = 0.0f;
            oy_ = 0.0f;
            width_ = w;
            height_ = h;
            scene_.clear();
            scene_.clear_color(clear);
        }

        // Offset begin_frame — all subsequent draw calls are translated by (ox, oy).
        // Used by RekhaWidget to paint into a sub-rect of the parent canvas without
        // coordinate rebasing in the caller.
        void begin_frame(float ox, float oy, std::uint32_t w, std::uint32_t h, Color clear) {
            ox_ = ox;
            oy_ = oy;
            width_ = w;
            height_ = h;
            scene_.clear();
            scene_.clear_color(clear);
        }

        void draw_line(Scalar x0, Scalar y0, Scalar x1, Scalar y1, StrokeStyle stroke) {
            kalpana::Path path;
            path.move_to(ox_ + x0, oy_ + y0).line_to(ox_ + x1, oy_ + y1);
            scene_.add(kalpana::Node::shape(std::move(path), kalpana::Paint::stroke(stroke.color, stroke.width)));
        }

        void draw_polyline(std::span<const Vec2> pts, StrokeStyle stroke) {
            if (pts.size() < 2) return;
            kalpana::Path path;
            path.move_to(ox_ + pts[0].x(), oy_ + pts[0].y());
            for (std::size_t i = 1; i < pts.size(); ++i)
                path.line_to(ox_ + pts[i].x(), oy_ + pts[i].y());
            scene_.add(kalpana::Node::shape(std::move(path), kalpana::Paint::stroke(stroke.color, stroke.width)));
        }

        void draw_circle(Scalar x, Scalar y, Scalar r, Color color) {
            kalpana::Path path;
            path.circle(ox_ + x, oy_ + y, r);
            scene_.add(kalpana::Node::shape(std::move(path), kalpana::Paint::fill(color)));
        }

        void draw_rect(Scalar x, Scalar y, Scalar w, Scalar h, Color color) {
            kalpana::Path path;
            path.rect(ox_ + x, oy_ + y, w, h);
            scene_.add(kalpana::Node::shape(std::move(path), kalpana::Paint::fill(color)));
        }

        void draw_text(std::string_view text, Scalar x, Scalar y, Color color) {
            scene_.add(kalpana::Node::text(text, color, 13.0f, ox_ + x, oy_ + y));
        }

        void end_frame() {}

        [[nodiscard]] const kalpana::Scene& scene() const noexcept { return scene_; }
        [[nodiscard]] kalpana::Scene& scene() noexcept { return scene_; }

        [[nodiscard]] std::vector<std::uint32_t> rasterize() const {
            kalpana::DefaultCanvas canvas(width_, height_);
            canvas.render(scene_);
            return canvas.snapshot();
        }

    private:
        float ox_ = 0.0f, oy_ = 0.0f;
        std::uint32_t width_ = 0;
        std::uint32_t height_ = 0;
        kalpana::Scene scene_{};
    };
} // namespace rekha

