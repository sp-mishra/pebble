#pragma once
// ============================================================================
// kalpa.hpp — core + EDSL umbrella
// ============================================================================
// Pulls the concept vocabulary, Problem/Derivatives, the Solver driver, and the
// vakya-backed EDSL front end. Algorithm families (algo/*) and telemetry sinks
// (introspect/*) are separate opt-in includes so a translation unit pays only
// for what it uses. For "everything at once" (prototyping), see kalpa_all.hpp.
// ============================================================================

#ifndef PEBBLE_KALPA_HPP
#define PEBBLE_KALPA_HPP

#include <kalpa/core/concepts.hpp>
#include <kalpa/core/problem.hpp>
#include <kalpa/core/solver.hpp>
#include <kalpa/edsl/model.hpp>

#endif // PEBBLE_KALPA_HPP
