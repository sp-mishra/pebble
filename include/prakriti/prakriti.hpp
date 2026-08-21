#pragma once
// ============================================================================
// prakriti/prakriti.hpp — umbrella. Unified material-state 2D physics engine.
// Header-only C++23, no virtual, no macro.
//
//   #include <prakriti/prakriti.hpp>
//   prakriti::World<> w;               // default material law + solver stack
//   auto steel = w.materials().add(prakriti::MaterialRegistry::steel());
//   w.particles().add({.position=pebble::math::vec2{0,10}, .material=steel});
//   w.step();
//
// See docs/prakriti/prakriti.md for architecture, algorithms, and usage.
// ============================================================================
#include "core/config.hpp"
#include "core/spatial_hash.hpp"
#include "state/material_registry.hpp"
#include "state/particle_store.hpp"
#include "state/edge_store.hpp"
#include "material/phase.hpp"
#include "material/eos.hpp"
#include "material/constitutive.hpp"
#include "compute/compute_backend.hpp"
#include "compute/scalar_backend.hpp"
#include "compute/highway_backend.hpp"
#include "compute/pravaha_backend.hpp"
#include "solvers/kernels.hpp"
#include "solvers/solver_base.hpp"
#include "solvers/thermal.hpp"
#include "solvers/xpbd.hpp"
#include "solvers/density.hpp"
#include "solvers/damage.hpp"
#include "engine.hpp"

// Optional akruti integration (rigid-obstacle contact + joints). Included only when akruti is on
// the include path — Prakriti never forces the dependency. When present, these expose
// prakriti::ObstacleSolver / prakriti::JointSolver as ordinary PhysicsSolvers you can drop into a
// custom mechanics SolverStack (see World's MechanicsStack template parameter).
#if __has_include("akruti/akruti.hpp")
#include "solvers/obstacle.hpp"
#endif
#if __has_include("akruti/joint.hpp")
#include "solvers/joint.hpp"
#endif
