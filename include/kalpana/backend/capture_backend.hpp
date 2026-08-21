#pragma once
// ============================================================================
// kalpana/backend/capture_backend.hpp — Headless Verification & Rasterizer Backend
// ============================================================================
// Dependency-free headless software rasterizer and structural verification log.
// ============================================================================

#include "backend_concept.hpp"
#include <vector>
#include <string>
#include <algorithm>

namespace kalpana {

class capture_backend {
public:
    capture_backend() = default;

    void begin(std::uint32_t w, std::uint32_t h) {
        width_ = w;
        height_ = h;
        pixels_.assign(static_cast<std::size_t>(w) * h, 0x00000000u);
        log_.push_back("begin(" + std::to_string(w) + ", " + std::to_string(h) + ")");
    }

    void clear(Color c) {
        const std::uint32_t argb = c.to_argb8888();
        std::fill(pixels_.begin(), pixels_.end(), argb);
        log_.push_back("clear");
    }

    void draw_shape(const Path& path, const Paint& paint, Transform xf) {
        log_.push_back("draw_shape(verbs=" + std::to_string(path.verbs().size()) + ")");

        if (paint.has_fill() && !path.points().empty()) {
            const std::uint32_t fill_argb = paint.fill_color().to_argb8888();
            // Fill bounding box approximation for headless testing
            float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
            for (const auto& pt : path.points()) {
                auto tf_pt = xf.apply(pt);
                min_x = std::min(min_x, tf_pt[0]);
                min_y = std::min(min_y, tf_pt[1]);
                max_x = std::max(max_x, tf_pt[0]);
                max_y = std::max(max_y, tf_pt[1]);
            }

            const int x0 = std::clamp(static_cast<int>(min_x), 0, static_cast<int>(width_));
            const int y0 = std::clamp(static_cast<int>(min_y), 0, static_cast<int>(height_));
            const int x1 = std::clamp(static_cast<int>(max_x), 0, static_cast<int>(width_));
            const int y1 = std::clamp(static_cast<int>(max_y), 0, static_cast<int>(height_));

            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    pixels_[static_cast<std::size_t>(y) * width_ + x] = fill_argb;
                }
            }
        }
    }

    void push_group(Transform, float, BlendMode, std::span<const Effect>) {
        log_.push_back("push_group");
    }

    void pop_group() {
        log_.push_back("pop_group");
    }

    void draw_image(const std::uint32_t*, std::uint32_t, std::uint32_t, float, float, float, float, Transform) {
        log_.push_back("draw_image");
    }

    void present() {
        log_.push_back("present");
    }

    void readback(std::span<std::uint32_t> dst) {
        const std::size_t copy_n = std::min(dst.size(), pixels_.size());
        std::copy_n(pixels_.begin(), copy_n, dst.begin());
    }

    [[nodiscard]] const std::vector<std::string>& log() const noexcept { return log_; }
    [[nodiscard]] const std::vector<std::uint32_t>& pixels() const noexcept { return pixels_; }

private:
    std::uint32_t              width_ = 0;
    std::uint32_t              height_ = 0;
    std::vector<std::uint32_t> pixels_;
    std::vector<std::string>   log_;
};

static_assert(paint_backend<capture_backend>);

} // namespace kalpana
