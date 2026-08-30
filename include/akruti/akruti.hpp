#pragma once
// akruti/akruti.hpp — umbrella header for the akruti 2D shape & physics geometry system.
// Header-only, C++23/C++26, concept-based static dispatch, zero heap on hot paths.
//
// Subsystems:
//   math.hpp        POD Vec2/Mat2/AABB numeric math
//   shape.hpp       Shape concept (sdf / aabb / support)
//   primitives.hpp  Circle, Segment, Capsule, Box, OrientedBox, Triangle, RoundedBox, RoundedPoly, Sector, HalfPlane, ConvexPoly
//   query.hpp       raycast, point_inside, closest_point, winding_number
//   gjk.hpp         GJK boolean overlap + EPA penetration depth/normal
//   narrowphase.hpp SAT 2-point manifold generation + O(1) analytic fast paths + warm-started GJK
//   simd.hpp        Google Highway SIMD batch queries & packet raycasting
//   hull.hpp        Andrew monotone-chain convex hull
//   csg.hpp         Expression template EDSL + Flat Arena AST + dynamic CSG
//   ccd.hpp         Continuous collision detection (conservative advancement + speculative bound)
//   fracture.hpp    Voronoi shatter + Sutherland-Hodgman clipping
//   khanda.hpp      Advanced fracture pipeline (triangulation + convex decomp + polar moment + Poisson)
//   joint.hpp       Joint kinematic frames (Distance, Revolute, Prismatic, Weld, Motor)
//   scene/          Bulk storage, collision layers, AABBTree broadphase & parallel bulk ops
#include <cmath>
#include <algorithm>
#include <random>
#include <span>
#include <vector>
#include <cstdint>
#include <limits>

#include "math.hpp"
#include "shape.hpp"
#include "primitives.hpp"
#include "query.hpp"
#include "gjk.hpp"
#include "narrowphase.hpp"
#include "simd.hpp"
#include "hull.hpp"
#include "csg.hpp"
#include "ccd.hpp"
#include "fracture.hpp"
#include "poly_ops.hpp"
#include "khanda.hpp"
#include "joint.hpp"
#include "spline.hpp"
#include "morph.hpp"
#include "deform.hpp"
#include "body.hpp"
#include "layout.hpp"

// Scene layer
#include "scene/parallel.hpp"
#include "scene/batch.hpp"
#include "scene/scene.hpp"
#include "scene/bulk_ops.hpp"
