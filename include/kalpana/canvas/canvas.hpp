#pragma once
// ============================================================================
// kalpana/canvas/canvas.hpp — Scene Traversal & Monomorphized Render Driver
// ============================================================================

#include "../backend/backend_concept.hpp"
#include "../scene/scene.hpp"
#include <vector>
#include <cstdint>
#include <span>

namespace kalpana {

template <paint_backend B>
class Canvas {
public:
    Canvas(std::uint32_t width, std::uint32_t height)
        : w_(width), h_(height) {}

    template <class... Args>
    explicit Canvas(std::uint32_t width, std::uint32_t height, Args&&... args)
        : w_(width), h_(height), backend_(std::forward<Args>(args)...) {}

    void render(const Scene& scene) {
        backend_.begin(w_, h_);
        backend_.clear(scene.clear_color());

        if (scene.has_layers()) {
            for (const auto& layer : scene.layers().layers()) {
                if (!layer.visible()) continue;
                backend_.push_group(Transform::identity(), layer.opacity(), layer.blend(), layer.effect_chain());
                for (const auto& node : layer.nodes()) {
                    walk(node, false);
                }
                backend_.pop_group();
            }
        } else {
            walk(scene.root(), /*is_root=*/true);
        }

        backend_.present();
    }

    [[nodiscard]] std::vector<std::uint32_t> snapshot() {
        std::vector<std::uint32_t> out(static_cast<std::size_t>(w_) * h_);
        backend_.readback(out);
        return out;
    }

    [[nodiscard]] B&       backend() noexcept { return backend_; }
    [[nodiscard]] const B& backend() const noexcept { return backend_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return w_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return h_; }

private:
    void walk(const Node& n, bool is_root) {
        if (const auto* g = std::get_if<GroupNode>(&n.content)) {
            if (!is_root) {
                backend_.push_group(n.xf, n.opacity, n.blend, n.effects);
            }
            for (const Node& child : g->children) {
                walk(child, false);
            }
            if (!is_root) {
                backend_.pop_group();
            }
        } else if (const auto* s = std::get_if<ShapeNode>(&n.content)) {
            backend_.draw_shape(s->path, s->paint, n.xf);
        } else if (const auto* im = std::get_if<ImageNode>(&n.content)) {
            backend_.draw_image(im->pixels, im->w, im->h, im->dx, im->dy, im->dw, im->dh, n.xf);
        } else if (const auto* t = std::get_if<TextNode>(&n.content)) {
            // Software text draw fallback if backend provides draw_text
            if constexpr (requires(B b) { b.draw_text(t->text, t->color, t->font_size, t->x, t->y); }) {
                backend_.draw_text(t->text, t->color, t->font_size, t->x, t->y);
            }
        }
    }

    std::uint32_t w_ = 0;
    std::uint32_t h_ = 0;
    B             backend_{};
};

} // namespace kalpana
