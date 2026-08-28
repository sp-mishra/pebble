#pragma once
// ============================================================================
// tensor_edsl.hpp — Master Umbrella Header for Tensor EDSL
// ============================================================================
// C++23 / C++26, header-only, rich symbolic tensor algebra & neural EDSL.
// Inspired by Sūtra & Vākya expression designs.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_TENSOR_EDSL_HPP
#define PEBBLE_CONTAINERS_TENSOR_EDSL_HPP

#include <containers/tensor/tensor.hpp>
#include <containers/tensor/edsl/sym_leaf.hpp>
#include <containers/tensor/edsl/shape_inference.hpp>
#include <containers/tensor/edsl/operators.hpp>
#include <containers/tensor/edsl/eval.hpp>

namespace ts {
    namespace edsl_literals = edsl::literals;
}

#endif // PEBBLE_CONTAINERS_TENSOR_EDSL_HPP
