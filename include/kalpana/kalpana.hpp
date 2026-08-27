#pragma once
// ============================================================================
// kalpana/kalpana.hpp — Master Umbrella Header for Kalpana 2.0 Graphics Language
// ============================================================================
// Zero-virtual, policy-configurable 2D vector graphics, Kubelka-Munk spectral
// pigment mixing & science, Rebelle-inspired physics brushes, composable effects
// EDSL, procedural fills, geometry modifiers, and declarative scene authoring.
// ============================================================================

#include "color/color.hpp"
#include "color/spectral.hpp"
#include "color/pigment_catalog.hpp"
#include "color/color_space.hpp"
#include "geom/transform.hpp"
#include "geom/path.hpp"
#include "geom/path_modifier.hpp"
#include "geom/shape_builders.hpp"
#include "paint/paint.hpp"
#include "fill/noise.hpp"
#include "fill/procedural.hpp"
#include "effect/effect.hpp"
#include "effect/effect_chain.hpp"
#include "brush/dynamics.hpp"
#include "brush/stamp_shape.hpp"
#include "brush/deposition.hpp"
#include "brush/brush_preset.hpp"
#include "brush/brush.hpp"
#include "layer/layer_combiner.hpp"
#include "layer/layer.hpp"
#include "layer/layer_stack.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "edsl/scene_builder.hpp"
#include "backend/backend_concept.hpp"
#include "backend/capture_backend.hpp"
#include "backend/sokol_backend.hpp"
#include "backend/instanced_pipeline.hpp"
#include "backend/notcurses_backend.hpp"
#include "canvas/canvas.hpp"

namespace kalpana {

// Default headless canvas for tests and software frame recording
using DefaultCanvas = Canvas<capture_backend>;

// GPU hardware canvas using Sokol GFX
using SokolCanvas = Canvas<sokol_backend>;

// Terminal text-mode canvas using Notcurses
using TerminalCanvas = Canvas<notcurses_backend>;

} // namespace kalpana

namespace pebble {
    namespace kalpana = ::kalpana;
}
