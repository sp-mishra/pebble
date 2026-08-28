#pragma once
// ============================================================================
// prakriti/celestial/sector_multipole.hpp — Fast Multipole Method (FMM) Potentials
// ============================================================================
// Zero-virtual, analytic tensor gravity for distant out-of-core cosmic sectors.
// ============================================================================

#include "sector_types.hpp"
#include <cmath>

namespace prakriti::celestial {

// Evaluates the collective gravitational acceleration vector produced by a dormant sector macro node:
// a_coll(r) = G * M * r_hat / (d^2 + epsilon^2) + Quadrupole Tensor Correction
[[nodiscard]] inline pebble::math::vec2
compute_collective_macro_gravity(const pebble::math::vec2& target_pos,
                                 const SectorMacroNode& macro_node,
                                 float grav_g = 18000.0f) noexcept {
    if (macro_node.total_mass <= 0.0f) return pebble::math::vec2{0.0f, 0.0f};

    const float dx = macro_node.bx - target_pos[0];
    const float dy = macro_node.by - target_pos[1];
    constexpr float kSoftening2 = 2500.0f; // 50px softening
    const float dist2 = dx * dx + dy * dy + kSoftening2;
    const float dist = std::sqrt(dist2);

    const float inv_dist = 1.0f / dist;
    const float inv_dist3 = inv_dist * inv_dist * inv_dist;

    // 1. Monopole acceleration from collective mass at Center of Mass
    const float mono_mag = (grav_g * macro_node.total_mass) * inv_dist3;
    pebble::math::vec2 a_grav{dx * mono_mag, dy * mono_mag};

    // 2. Quadrupole Tensor Correction for non-spherical dormant clusters
    const float inv_dist5 = inv_dist3 * inv_dist * inv_dist;
    const float q_term_x = (macro_node.q.qxx * dx + macro_node.q.qxy * dy) * inv_dist5;
    const float q_term_y = (macro_node.q.qxy * dx + macro_node.q.qyy * dy) * inv_dist5;

    a_grav[0] += grav_g * 0.5f * q_term_x;
    a_grav[1] += grav_g * 0.5f * q_term_y;

    return a_grav;
}

} // namespace prakriti::celestial
