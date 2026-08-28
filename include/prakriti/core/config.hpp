#pragma once
// ============================================================================
// prakriti/core/config.hpp — engine scalar type + tunable constants.
// Directly reuses Pebble core math (pebble::math::vec2, pebble::math::aabb2).
// Every solver tunable lives here as a field. No magic numbers in solver bodies.
// ============================================================================
#include <containers/numeric/math_vector.hpp>
#include <cstdint>

namespace prakriti {

using Scalar     = float;    // Apple-Silicon f32 hot path
using MaterialId = std::uint16_t;
using Index      = std::uint32_t;

inline constexpr Index kInvalidIndex = ~Index{0};

// World-level simulation tunables. Defaults give a stable, plug-and-play setup.
struct WorldConfig {
    pebble::math::vec2 gravity{0.0f, -9.81f};
    Scalar dt            = Scalar(1.0 / 60.0); // frame step
    int    substeps      = 8;                  // XPBD small-steps for stiffness/stability
    int    solver_iters  = 4;                  // constraint iterations per substep
    Scalar cell_size     = Scalar(1);          // spatial-hash cell edge (~2x particle radius)
    Scalar global_damping = Scalar(0);         // velocity decay per substep [0,1)
    bool   clamp_negative_pressure = true;     // free-surface fluids
    pebble::math::aabb2 bounds{{-1e4f, -1e4f}, {1e4f, 1e4f}};
};

// SPH / fluid kernel tunables. Densities are in kernel-sum units (ρ = m·Σ W), NOT SI kg/m³ —
// PBF operates on relative compression, so rest_density is chosen consistent with mass + spacing.
struct FluidConfig {
    Scalar particle_mass  = Scalar(1);      // per-particle mass in the density sum
    Scalar rest_density   = Scalar(1.5);    // target ρ at rest spacing (kernel-sum units)
    Scalar smoothing_h    = Scalar(1);      // kernel support radius
    Scalar relaxation_eps = Scalar(0.01);   // PBF lambda denominator stabilizer
    Scalar scorr_k        = Scalar(1e-4);   // anti-clustering strength
    Scalar scorr_dq       = Scalar(0.2);    // scorr reference distance (fraction of h)
    int    scorr_n        = 4;              // scorr exponent
};

// Thermal diffusion tunables.
struct ThermalConfig {
    Scalar ambient_temp   = Scalar(20);
    Scalar diffusivity    = Scalar(0.1);    // global scale on conductivity/heat-capacity ratio
    bool   enabled        = true;
};

// Rigid-obstacle contact tunables (akruti ObstacleSolver). Contact is resolved as an XPBD-style
// positional projection out of the shape's negative-SDF region, plus Coulomb friction on the
// tangential component and restitution on the normal component of the post-solve velocity.
struct ObstacleConfig {
    Scalar friction         = Scalar(0.5);   // Coulomb coefficient (tangential)
    Scalar restitution      = Scalar(0);     // normal bounce [0,1]
    Scalar contact_offset   = Scalar(0);     // treat sdf < contact_offset as contact (skin)
    Scalar contact_stiffness = Scalar(1);    // fraction of penetration removed per iteration [0,1]
};

// Joint constraint tunables (akruti JointSolver). Per-joint compliance overrides this default.
struct JointConfig {
    Scalar default_compliance = Scalar(0);   // 0 => perfectly rigid
    int    iterations         = 4;           // projection sweeps per substep
};

} // namespace prakriti
