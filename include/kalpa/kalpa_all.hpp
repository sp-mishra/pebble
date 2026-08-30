#pragma once
// ============================================================================
// kalpa_all.hpp — everything, for prototyping
// ============================================================================
// Convenience header pulling core + EDSL + every algorithm family + telemetry.
// Heavier to compile than kalpa.hpp; prefer the granular includes in library
// code and reserve this for scratch / exploratory use.
// ============================================================================

#ifndef PEBBLE_KALPA_ALL_HPP
#define PEBBLE_KALPA_ALL_HPP

#include <kalpa/kalpa.hpp>
#include <kalpa/algo/unconstrained.hpp>
#include <kalpa/algo/constrained.hpp>
#include <kalpa/algo/global.hpp>
#include <kalpa/introspect/telemetry.hpp>

#endif // PEBBLE_KALPA_ALL_HPP
