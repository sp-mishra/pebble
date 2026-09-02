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
            struct LineSeg {
                float x0, y0, x1, y1;
            };
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
                                inv * inv * inv * curr[0] + 3.0f * inv * inv * t * cp1[0] + 3.0f * inv * t * t * cp2[0]
                                + t * t * t * end[0],
                                inv * inv * inv * curr[1] + 3.0f * inv * inv * t * cp1[1] + 3.0f * inv * t * t * cp2[1]
                                + t * t * t * end[1]
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

            auto to_clip_x = [&](float px) -> float {
                return (px / float(width_)) * 2.0f - 1.0f;
            };
            auto to_clip_y = [&](float py) -> float {
                return 1.0f - (py / float(height_)) * 2.0f;
            };

            // 1. Polygon Fill Tessellation with Subpixel Antialiased Outer Fringe
            if (paint.has_fill() && poly_pts.size() >= 3) {
                Color fc = paint.fill_color();
                const std::uint32_t base_idx = static_cast<std::uint32_t>(vertices_.size());

                // Compute center of mass
                float cx = 0.0f, cy = 0.0f;
                for (const auto& p : poly_pts) {
                    cx += p[0];
                    cy += p[1];
                }
                cx /= float(poly_pts.size());
                cy /= float(poly_pts.size());

                // Center vertex
                vertices_.push_back(Vertex{to_clip_x(cx), to_clip_y(cy), fc.r, fc.g, fc.b, fc.a});

                // Boundary vertices (interior)
                for (const auto& p : poly_pts) {
                    vertices_.push_back(Vertex{to_clip_x(p[0]), to_clip_y(p[1]), fc.r, fc.g, fc.b, fc.a});
                }

                const std::uint32_t n_pts = static_cast<std::uint32_t>(poly_pts.size());
                for (std::uint32_t i = 0; i < n_pts; ++i) {
                    const std::uint32_t v0 = base_idx;
                    const std::uint32_t v1 = base_idx + 1 + i;
                    const std::uint32_t v2 = base_idx + 1 + ((i + 1) % n_pts);
                    indices_.push_back(v0);
                    indices_.push_back(v1);
                    indices_.push_back(v2);
                }

                // Antialiased Outer Fringe Skirt (1.2px feather to alpha 0)
                const std::uint32_t fringe_start = static_cast<std::uint32_t>(vertices_.size());
                for (std::size_t i = 0; i < n_pts; ++i) {
                    const auto& p = poly_pts[i];
                    float dx = p[0] - cx;
                    float dy = p[1] - cy;
                    float len = std::sqrt(dx * dx + dy * dy) + 1e-4f;
                    float ox = p[0] + (dx / len) * 1.25f;
                    float oy = p[1] + (dy / len) * 1.25f;
                    vertices_.push_back(Vertex{to_clip_x(ox), to_clip_y(oy), fc.r, fc.g, fc.b, 0.0f});
                }

                for (std::uint32_t i = 0; i < n_pts; ++i) {
                    const std::uint32_t next = (i + 1) % n_pts;
                    const std::uint32_t in0 = base_idx + 1 + i;
                    const std::uint32_t in1 = base_idx + 1 + next;
                    const std::uint32_t out0 = fringe_start + i;
                    const std::uint32_t out1 = fringe_start + next;

                    indices_.push_back(in0);
                    indices_.push_back(out0);
                    indices_.push_back(out1);

                    indices_.push_back(in0);
                    indices_.push_back(out1);
                    indices_.push_back(in1);
                }
            }

            // 2. Stroke Tessellation with Antialiased Edge Falloff
            if (paint.has_stroke() && !segs.empty()) {
                Color sc = paint.stroke().color;
                const float half_w = std::max(0.5f, paint.stroke().width * 0.5f);

                for (const auto& s : segs) {
                    const float dx = s.x1 - s.x0;
                    const float dy = s.y1 - s.y0;
                    const float len = std::sqrt(dx * dx + dy * dy);
                    if (len < 1e-4f) continue;

                    const float nx = -dy / len * half_w;
                    const float ny = dx / len * half_w;
                    const float fnx = -dy / len * (half_w + 1.2f);
                    const float fny = dx / len * (half_w + 1.2f);

                    const std::uint32_t base_idx = static_cast<std::uint32_t>(vertices_.size());

                    // Inner core quads
                    vertices_.push_back(Vertex{to_clip_x(s.x0 + nx), to_clip_y(s.y0 + ny), sc.r, sc.g, sc.b, sc.a});
                    vertices_.push_back(Vertex{to_clip_x(s.x0 - nx), to_clip_y(s.y0 - ny), sc.r, sc.g, sc.b, sc.a});
                    vertices_.push_back(Vertex{to_clip_x(s.x1 - nx), to_clip_y(s.y1 - ny), sc.r, sc.g, sc.b, sc.a});
                    vertices_.push_back(Vertex{to_clip_x(s.x1 + nx), to_clip_y(s.y1 + ny), sc.r, sc.g, sc.b, sc.a});

                    indices_.push_back(base_idx + 0);
                    indices_.push_back(base_idx + 1);
                    indices_.push_back(base_idx + 2);

                    indices_.push_back(base_idx + 0);
                    indices_.push_back(base_idx + 2);
                    indices_.push_back(base_idx + 3);

                    // Feather fringes (left and right)
                    vertices_.push_back(Vertex{to_clip_x(s.x0 + fnx), to_clip_y(s.y0 + fny), sc.r, sc.g, sc.b, 0.0f});
                    vertices_.push_back(Vertex{to_clip_x(s.x1 + fnx), to_clip_y(s.y1 + fny), sc.r, sc.g, sc.b, 0.0f});

                    indices_.push_back(base_idx + 0);
                    indices_.push_back(base_idx + 4);
                    indices_.push_back(base_idx + 5);

                    indices_.push_back(base_idx + 0);
                    indices_.push_back(base_idx + 5);
                    indices_.push_back(base_idx + 3);

                    vertices_.push_back(Vertex{to_clip_x(s.x0 - fnx), to_clip_y(s.y0 - fny), sc.r, sc.g, sc.b, 0.0f});
                    vertices_.push_back(Vertex{to_clip_x(s.x1 - fnx), to_clip_y(s.y1 - fny), sc.r, sc.g, sc.b, 0.0f});

                    indices_.push_back(base_idx + 1);
                    indices_.push_back(base_idx + 6);
                    indices_.push_back(base_idx + 7);

                    indices_.push_back(base_idx + 1);
                    indices_.push_back(base_idx + 7);
                    indices_.push_back(base_idx + 2);
                }
            }
        }

        void push_group(Transform, float, BlendMode, const EffectChain&) {}
        void pop_group() {}

        void draw_image(const std::uint32_t*, std::uint32_t, std::uint32_t, float, float, float, float, Transform) {}

        void present() {}

        void readback(std::span<std::uint32_t>) {}

        [[nodiscard]] const std::vector<Vertex>& vertices() const noexcept { return vertices_; }
        [[nodiscard]] const std::vector<std::uint32_t>& indices() const noexcept { return indices_; }

    private:
        std::uint32_t width_ = 0;
        std::uint32_t height_ = 0;
        Color clear_color_ = colors::black();
        std::vector<Vertex> vertices_;
        std::vector<std::uint32_t> indices_;
    };

    static_assert(paint_backend<sokol_backend>);
} // namespace kalpana
