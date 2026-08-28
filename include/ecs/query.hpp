#pragma once
// ============================================================================
// ecs/query.hpp — Rich Query Filter Tags (With, Without, Optional, Changed)
// ============================================================================
// Compile-time filter expressions for multi-store join queries.
// Zero virtual functions, zero macros, modern C++23.
// ============================================================================

#include "entity.hpp"
#include <type_traits>

namespace pebble::ecs {

// Filter tags for compile-time query matching
template <typename... Ts>
struct With {};

template <typename... Ts>
struct Without {};

template <typename T>
struct Optional {};

} // namespace pebble::ecs
