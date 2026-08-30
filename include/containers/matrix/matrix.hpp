#pragma once
// ============================================================================
// matrix.hpp — containers/matrix umbrella header  (namespace ga::)
// ============================================================================
// Opt-in: include only the sub-headers you need, or include this for everything.
//
// Subsystem           Header
// ─────────────────── ─────────────────────────────────────────
// Static matrices     containers/matrix/static.hpp
// Dense matrix/vector containers/matrix/dense.hpp
// EDSL expressions    containers/matrix/expr.hpp
// Factorizations      containers/matrix/factorize.hpp
// Direct solve        containers/matrix/solve.hpp
// Iterative solvers   containers/matrix/iterative.hpp
// Sparse CSR/COO/Dia  containers/matrix/sparse.hpp
// Spectral / SVD      containers/matrix/eigen.hpp
// Stencil calculus    containers/matrix/stencil.hpp
// Multi-channel field containers/matrix/field.hpp
// Dual-number autodiff containers/matrix/dual.hpp
// Unified Mat<T>      containers/matrix/mat.hpp
// Introspection       containers/matrix/mat_info.hpp
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_HPP
#define PEBBLE_CONTAINERS_MATRIX_HPP

#include <containers/matrix/static.hpp>
#include <containers/matrix/dense.hpp>
#include <containers/matrix/expr.hpp>
#include <containers/matrix/factorize.hpp>
#include <containers/matrix/solve.hpp>
#include <containers/matrix/sparse.hpp>
#include <containers/matrix/iterative.hpp>
#include <containers/matrix/eigen.hpp>
#include <containers/matrix/stencil.hpp>
#include <containers/matrix/field.hpp>
#include <containers/matrix/dual.hpp>
#include <containers/matrix/mat_info.hpp>
#include <containers/matrix/mat.hpp>

#endif // PEBBLE_CONTAINERS_MATRIX_HPP
