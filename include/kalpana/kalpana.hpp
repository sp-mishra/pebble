#pragma once
// ============================================================================
// kalpana/kalpana.hpp — Master Umbrella Header for Kalpana 2D Painting Engine
// ============================================================================
// Zero-virtual, concept-monomorphized 2D vector graphics, Kubelka-Munk spectral
// pigment mixing, realtime brush engine, scene graph, and pluggable backends.
// ============================================================================

#include "color/color.hpp"
#include "color/spectral.hpp"
#include "geom/transform.hpp"
#include "geom/path.hpp"
#include "paint/paint.hpp"
#include "effect/effect.hpp"
#include "brush/brush.hpp"
#include "scene/node.hpp"
#include "scene/scene.hpp"
#include "backend/backend_concept.hpp"
#include "backend/capture_backend.hpp"
#include "backend/sokol_backend.hpp"
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
