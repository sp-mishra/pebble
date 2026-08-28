#pragma once
// ============================================================================
// kalpana/backend/notcurses_backend.hpp — Terminal Text-Mode Graphics Backend
// ============================================================================
// High-resolution double-vertical half-block (▀) 24-bit TrueColor terminal backend.
// ============================================================================

#include "backend_concept.hpp"
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <span>

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

    void draw_shape(const Path& path, const Paint& paint, Transform) {
        // High-speed pixel rasterization of circles / bounding boxes to terminal buffer
        if (paint.has_fill() && !framebuffer_.empty() && !path.points().empty()) {
            float min_px = path.points()[0][0], max_px = path.points()[0][0];
            float min_py = path.points()[0][1], max_py = path.points()[0][1];
            for (const auto& pt : path.points()) {
                min_px = std::min(min_px, pt[0]);
                max_px = std::max(max_px, pt[0]);
                min_py = std::min(min_py, pt[1]);
                max_py = std::max(max_py, pt[1]);
            }

            const int min_x = std::clamp(static_cast<int>(min_px), 0, static_cast<int>(width_) - 1);
            const int max_x = std::clamp(static_cast<int>(max_px), 0, static_cast<int>(width_) - 1);
            const int min_y = std::clamp(static_cast<int>(min_py), 0, static_cast<int>(height_) - 1);
            const int max_y = std::clamp(static_cast<int>(max_py), 0, static_cast<int>(height_) - 1);
            const std::uint32_t argb = paint.fill_color().to_argb8888();

            for (int y = min_y; y <= max_y; ++y) {
                for (int x = min_x; x <= max_x; ++x) {
                    framebuffer_[static_cast<std::size_t>(y) * width_ + x] = argb;
                }
            }
        }
    }

    void push_group(Transform, float, BlendMode, const EffectChain&) {}
    void pop_group() {}

    void draw_image(const std::uint32_t*, std::uint32_t, std::uint32_t, float, float, float, float, Transform) {}

    // Present 24-bit TrueColor double-vertical half block terminal frame (zero flicker)
    void present() {
        if (width_ == 0 || height_ == 0 || framebuffer_.empty()) return;

        std::string out;
        out.reserve(width_ * (height_ / 2) * 32 + 256);
        out += "\033[?25l\033[H"; // Reposition cursor and hide

        for (std::uint32_t y = 0; y < height_ / 2; ++y) {
            const std::uint32_t top_y = y * 2;
            const std::uint32_t bot_y = top_y + 1;

            for (std::uint32_t x = 0; x < width_; ++x) {
                const std::uint32_t top_c = framebuffer_[static_cast<std::size_t>(top_y) * width_ + x];
                const std::uint32_t bot_c = framebuffer_[static_cast<std::size_t>(bot_y) * width_ + x];

                const std::uint8_t tr = (top_c >> 16) & 0xFF;
                const std::uint8_t tg = (top_c >> 8) & 0xFF;
                const std::uint8_t tb = top_c & 0xFF;

                const std::uint8_t br = (bot_c >> 16) & 0xFF;
                const std::uint8_t bg = (bot_c >> 8) & 0xFF;
                const std::uint8_t bb = bot_c & 0xFF;

                if (top_c == 0 && bot_c == 0) {
                    out += " ";
                } else if (top_c != 0 && bot_c == 0) {
                    out += "\033[38;2;" + std::to_string(tr) + ";" + std::to_string(tg) + ";" + std::to_string(tb) + "m▀\033[0m";
                } else if (top_c == 0 && bot_c != 0) {
                    out += "\033[38;2;" + std::to_string(br) + ";" + std::to_string(bg) + ";" + std::to_string(bb) + "m▄\033[0m";
                } else {
                    out += "\033[38;2;" + std::to_string(tr) + ";" + std::to_string(tg) + ";" + std::to_string(tb) +
                           ";48;2;" + std::to_string(br) + ";" + std::to_string(bg) + ";" + std::to_string(bb) + "m▀\033[0m";
                }
            }
            out += "\n";
        }
        std::cout << out << std::flush;
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
