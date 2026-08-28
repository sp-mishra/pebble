#pragma once
// ============================================================================
// kalpana/backend/backend_concept.hpp — paint_backend & Extension Concepts
// ============================================================================

#include "../geom/path.hpp"
#include "../paint/paint.hpp"
#include "../effect/effect_chain.hpp"
#include "../geom/transform.hpp"
#include "../color/color.hpp"
#include <concepts>
#include <cstdint>
#include <span>

#include "../layer/layer.hpp"

namespace kalpana {

template <class B>
concept paint_backend = requires(B b, std::uint32_t dim, Color c, const Path& path,
                                 const Paint& paint, Transform xf, float opacity, BlendMode blend,
                                 const EffectChain& effects, const std::uint32_t* px,
                                 std::uint32_t w, std::uint32_t h, float f, std::span<std::uint32_t> dst) {
    { b.begin(dim, dim) }                              -> std::same_as<void>;
    { b.clear(c) }                                     -> std::same_as<void>;
    { b.draw_shape(path, paint, xf) }                  -> std::same_as<void>;
    { b.push_group(xf, opacity, blend, effects) }      -> std::same_as<void>;
    { b.pop_group() }                                  -> std::same_as<void>;
    { b.draw_image(px, w, h, f, f, f, f, xf) }         -> std::same_as<void>;
    { b.present() }                                    -> std::same_as<void>;
    { b.readback(dst) }                                -> std::same_as<void>;
};

// Extended optional concepts for backends supporting advanced features
template <class B>
concept effect_backend = paint_backend<B> && requires(B b, const EffectChain& fx) {
    { b.apply_effect_chain(fx) } -> std::same_as<void>;
};

template <class B>
concept layer_backend = paint_backend<B> && requires(B b, const Layer& layer) {
    { b.begin_layer(layer) }  -> std::same_as<void>;
    { b.end_layer() }         -> std::same_as<void>;
};

} // namespace kalpana
