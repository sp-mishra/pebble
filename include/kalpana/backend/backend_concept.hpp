#pragma once
// ============================================================================
// kalpana/backend/backend_concept.hpp — paint_backend Concept Definition
// ============================================================================

#include "../geom/path.hpp"
#include "../paint/paint.hpp"
#include "../effect/effect.hpp"
#include "../geom/transform.hpp"
#include "../color/color.hpp"
#include <concepts>
#include <cstdint>
#include <span>

namespace kalpana {

template <class B>
concept paint_backend = requires(B b, std::uint32_t dim, Color c, const Path& path,
                                 const Paint& paint, Transform xf, float opacity, BlendMode blend,
                                 std::span<const Effect> effects, const std::uint32_t* px,
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

} // namespace kalpana
