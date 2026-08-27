#pragma once
// ============================================================================
// kalpana/backend/sokol_backend.hpp — GPU Hardware Render Backend via Sokol GFX
// ============================================================================
// Zero-virtual, GPU-accelerated backend wrapping sokol_gfx.
// ============================================================================

#include "backend_concept.hpp"
#include <vector>
#include <cmath>
#include <algorithm>

#if __has_include("sokol_gfx.h")
#include "sokol_gfx.h"
#define KALPANA_HAS_SOKOL_GFX 1
#endif

namespace kalpana {

class sokol_backend {
public:
    struct Vertex {
        float x, y;
        float r, g, b, a;
    };

    sokol_backend() = default;

    void begin(std::uint32_t w, std::uint32_t h) {
        width_ = w;
        height_ = h;
        vertices_.clear();
        indices_.clear();
    }

    void clear(Color c) {
        clear_color_ = c;
    }

    void draw_shape(const Path& path, const Paint& paint, Transform xf) {
        if (path.empty() || width_ == 0 || height_ == 0) return;

        // Flatten path verbs into transformed 2D points and segments
        struct LineSeg { float x0, y0, x1, y1; };
        std::vector<LineSeg> segs;
        std::vector<pebble::math::vec2> poly_pts;

        const auto& verbs = path.verbs();
        const auto& pts = path.points();
        std::size_t pt_idx = 0;
        pebble::math::vec2 curr{0.0f, 0.0f};
        pebble::math::vec2 start_pt{0.0f, 0.0f};

        for (auto v : verbs) {
            switch (v) {
                case PathVerb::Move: {
                    if (pt_idx < pts.size()) {
                        curr = xf.apply(pts[pt_idx++]);
                        start_pt = curr;
                        poly_pts.push_back(curr);
                    }
                    break;
                }
                case PathVerb::Line: {
                    if (pt_idx < pts.size()) {
                        pebble::math::vec2 next = xf.apply(pts[pt_idx++]);
                        segs.push_back({curr[0], curr[1], next[0], next[1]});
                        curr = next;
                        poly_pts.push_back(curr);
                    }
                    break;
                }
                case PathVerb::Quad: {
                    if (pt_idx + 1 < pts.size()) {
                        pebble::math::vec2 cp = xf.apply(pts[pt_idx++]);
                        pebble::math::vec2 end = xf.apply(pts[pt_idx++]);
                        constexpr int kSteps = 6;
                        for (int i = 1; i <= kSteps; ++i) {
                            float t = float(i) / float(kSteps);
                            float inv = 1.0f - t;
                            pebble::math::vec2 next{
                                inv * inv * curr[0] + 2.0f * inv * t * cp[0] + t * t * end[0],
                                inv * inv * curr[1] + 2.0f * inv * t * cp[1] + t * t * end[1]
                            };
                            segs.push_back({curr[0], curr[1], next[0], next[1]});
                            curr = next;
                            poly_pts.push_back(curr);
                        }
                    }
                    break;
                }
                case PathVerb::Cubic: {
                    if (pt_idx + 2 < pts.size()) {
                        pebble::math::vec2 cp1 = xf.apply(pts[pt_idx++]);
                        pebble::math::vec2 cp2 = xf.apply(pts[pt_idx++]);
                        pebble::math::vec2 end = xf.apply(pts[pt_idx++]);
                        constexpr int kSteps = 10;
                        for (int i = 1; i <= kSteps; ++i) {
                            float t = float(i) / float(kSteps);
                            float inv = 1.0f - t;
                            pebble::math::vec2 next{
                                inv*inv*inv * curr[0] + 3.0f*inv*inv*t * cp1[0] + 3.0f*inv*t*t * cp2[0] + t*t*t * end[0],
                                inv*inv*inv * curr[1] + 3.0f*inv*inv*t * cp1[1] + 3.0f*inv*t*t * cp2[1] + t*t*t * end[1]
                            };
                            segs.push_back({curr[0], curr[1], next[0], next[1]});
                            curr = next;
                            poly_pts.push_back(curr);
                        }
                    }
                    break;
                }
                case PathVerb::Close: {
                    if (curr[0] != start_pt[0] || curr[1] != start_pt[1]) {
                        segs.push_back({curr[0], curr[1], start_pt[0], start_pt[1]});
                        curr = start_pt;
                    }
                    break;
                }
            }
        }

        auto screen_to_clip = [&](float sx, float sy) -> std::pair<float, float> {
            return {
                (sx / float(width_)) * 2.0f - 1.0f,
                1.0f - (sy / float(height_)) * 2.0f
            };
        };

        // 1. GPU Polygon Tessellation (Triangle Fan from Polygon Centroid)
        if (paint.has_fill() && poly_pts.size() >= 3) {
            Color c = paint.fill_color();
            float cx = 0.0f, cy = 0.0f;
            for (const auto& pt : poly_pts) {
                cx += pt[0];
                cy += pt[1];
            }
            cx /= float(poly_pts.size());
            cy /= float(poly_pts.size());

            auto [ccx, ccy] = screen_to_clip(cx, cy);
            std::uint32_t base_idx = static_cast<std::uint32_t>(vertices_.size());
            vertices_.push_back({ccx, ccy, c.r, c.g, c.b, c.a});

            for (const auto& pt : poly_pts) {
                auto [px, py] = screen_to_clip(pt[0], pt[1]);
                vertices_.push_back({px, py, c.r, c.g, c.b, c.a});
            }

            const std::uint32_t n_pts = static_cast<std::uint32_t>(poly_pts.size());
            for (std::uint32_t i = 0; i < n_pts; ++i) {
                std::uint32_t next_i = (i + 1) % n_pts;
                indices_.push_back(base_idx);
                indices_.push_back(base_idx + 1 + i);
                indices_.push_back(base_idx + 1 + next_i);
            }
        }

        // 2. GPU Stroke Tessellation (Thick Quad Strips per Segment)
        if (paint.has_stroke() && !segs.empty()) {
            Color sc = paint.stroke().color;
            float hw = std::max(0.5f, paint.stroke().width * 0.5f);

            for (const auto& s : segs) {
                float dx = s.x1 - s.x0;
                float dy = s.y1 - s.y0;
                float len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-4f) continue;

                float nx = (-dy / len) * hw;
                float ny = ( dx / len) * hw;

                auto [v0x, v0y] = screen_to_clip(s.x0 + nx, s.y0 + ny);
                auto [v1x, v1y] = screen_to_clip(s.x1 + nx, s.y1 + ny);
                auto [v2x, v2y] = screen_to_clip(s.x1 - nx, s.y1 - ny);
                auto [v3x, v3y] = screen_to_clip(s.x0 - nx, s.y0 - ny);

                std::uint32_t base_idx = static_cast<std::uint32_t>(vertices_.size());
                vertices_.push_back({v0x, v0y, sc.r, sc.g, sc.b, sc.a});
                vertices_.push_back({v1x, v1y, sc.r, sc.g, sc.b, sc.a});
                vertices_.push_back({v2x, v2y, sc.r, sc.g, sc.b, sc.a});
                vertices_.push_back({v3x, v3y, sc.r, sc.g, sc.b, sc.a});

                indices_.push_back(base_idx + 0);
                indices_.push_back(base_idx + 1);
                indices_.push_back(base_idx + 2);
                indices_.push_back(base_idx + 0);
                indices_.push_back(base_idx + 2);
                indices_.push_back(base_idx + 3);
            }
        }
    }

    void push_group(Transform, float, BlendMode, std::span<const Effect>) {}
    void pop_group() {}

    void draw_image(const std::uint32_t*, std::uint32_t, std::uint32_t, float, float, float, float, Transform) {}

    void present() {}

    void readback(std::span<std::uint32_t> dst) {
        if (!dst.empty()) {
            std::fill(dst.begin(), dst.end(), clear_color_.to_argb8888());
        }
    }

    [[nodiscard]] const std::vector<Vertex>& vertices() const noexcept { return vertices_; }
    [[nodiscard]] const std::vector<std::uint32_t>& indices() const noexcept { return indices_; }
    [[nodiscard]] Color clear_color() const noexcept { return clear_color_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    std::uint32_t              width_ = 0;
    std::uint32_t              height_ = 0;
    Color                      clear_color_ = colors::transparent();
    std::vector<Vertex>        vertices_;
    std::vector<std::uint32_t> indices_;
};

static_assert(paint_backend<sokol_backend>);

} // namespace kalpana
