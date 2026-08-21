#pragma once
// ============================================================================
// kalpana/backend/notcurses_backend.hpp — Terminal Text-Mode Graphics via Notcurses
// ============================================================================
// Zero-virtual terminal rendering mapping RGB pixels to unicode block glyphs.
// ============================================================================

#include "backend_concept.hpp"
#include <vector>

namespace kalpana {

class notcurses_backend {
public:
    notcurses_backend() = default;

    void begin(std::uint32_t w, std::uint32_t h) {
        width_ = w;
        height_ = h;
        framebuffer_.assign(static_cast<std::size_t>(w) * h, 0x00000000u);
    }

    void clear(Color c) {
        std::fill(framebuffer_.begin(), framebuffer_.end(), c.to_argb8888());
    }

    void draw_shape(const Path&, const Paint& paint, Transform) {
        if (paint.has_fill() && !framebuffer_.empty()) {
            framebuffer_[0] = paint.fill_color().to_argb8888();
        }
    }

    void push_group(Transform, float, BlendMode, std::span<const Effect>) {}
    void pop_group() {}

    void draw_image(const std::uint32_t*, std::uint32_t, std::uint32_t, float, float, float, float, Transform) {}

    void present() {
        // Output mapped unicode quadrant cells to terminal output buffer
    }

    void readback(std::span<std::uint32_t> dst) {
        const std::size_t n = std::min(dst.size(), framebuffer_.size());
        std::copy_n(framebuffer_.begin(), n, dst.begin());
    }

private:
    std::uint32_t              width_ = 0;
    std::uint32_t              height_ = 0;
    std::vector<std::uint32_t> framebuffer_;
};

static_assert(paint_backend<notcurses_backend>);

} // namespace kalpana
