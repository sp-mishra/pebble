#pragma once
// ============================================================================
// drishya/widgets/widgets.hpp — widget library umbrella
// ----------------------------------------------------------------------------
// Pulls in every stock widget: layout containers, interactive inputs, display
// primitives, dashboard/data widgets, game/HUD widgets, and the concept-complete
// stubs still awaiting a full build. Include this (or the umbrella drishya.hpp)
// to get the whole vocabulary; include a single header to keep a translation
// unit lean.
// ============================================================================

#include "drishya/widgets/base.hpp"
#include "drishya/widgets/containers.hpp"
#include "drishya/widgets/data.hpp"
#include "drishya/widgets/display.hpp"
#include "drishya/widgets/game.hpp"
#include "drishya/widgets/inputs.hpp"
#include "drishya/widgets/stubs.hpp"
// Opt-in extensions — guarded; silently skipped if dependencies absent.
#include "drishya/widgets/rekha_widget.hpp"
#include "drishya/widgets/spandana_widgets.hpp"
