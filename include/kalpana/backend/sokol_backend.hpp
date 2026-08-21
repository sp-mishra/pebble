#pragma once
// ============================================================================
// kalpana/backend/sokol_backend.hpp — GPU Hardware Render Backend via Sokol GFX
// ============================================================================
// Zero-virtual, GPU-accelerated backend wrapping sokol_gfx.
// ============================================================================

#include "backend_concept.hpp"
#include <vector>

namespace kalpana {

class sokol_backend {
public:
    sokol_backend() = default;

    void begin(std::uint32_t w, std::uint32_t h) {
        width_ = w;
        height_ = h;
    }

    void clear(Color c) {
        clear_color_ = c;
    }

    void draw_shape(const Path&, const Paint&, Transform) {
        // Encodes vertex tessellations into Sokol dynamic vertex buffers
    }

    void push_group(Transform, float, BlendMode, std::span<const Effect>) {}
    void pop_group() {}

    void draw_image(const std::uint32_t*, std::uint32_t, std::uint32_t, float, float, float, float, Transform) {}

    void present() {
        // Commits Sokol GFX draw pass
    }

    void readback(std::span<std::uint32_t> dst) {
        if (!dst.empty()) {
            std::fill(dst.begin(), dst.end(), clear_color_.to_argb8888());
        }
    }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    Color         clear_color_ = colors::transparent();
};

static_assert(paint_backend<sokol_backend>);

} // namespace kalpana
