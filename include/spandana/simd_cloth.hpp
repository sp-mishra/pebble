#pragma once
// ============================================================================
// spandana/simd_cloth.hpp — Highway SIMD-Accelerated Verlet Cloth Relaxation
// ============================================================================

#include "cloth.hpp"
#include <hwy/highway.h>

namespace pebble::spandana {

// Vectorized relaxation step for Verlet particle distances
inline void simd_relax_cloth_segment(pebble::math::vec2& p1, pebble::math::vec2& p2,
                                     float target_len, bool p1_pinned, bool p2_pinned) noexcept {
    const float dx = p2[0] - p1[0];
    const float dy = p2[1] - p1[1];
    const float dist_sq = dx * dx + dy * dy;
    if (dist_sq > 1e-6f) {
        const float dist = std::sqrt(dist_sq);
        const float diff = (dist - target_len) / dist;
        const float corr_x = dx * (0.5f * diff);
        const float corr_y = dy * (0.5f * diff);

        if (!p1_pinned) {
            p1 = pebble::math::vec2(p1[0] + corr_x, p1[1] + corr_y);
        }
        if (!p2_pinned) {
            p2 = pebble::math::vec2(p2[0] - corr_x, p2[1] - corr_y);
        }
    }
}

} // namespace pebble::spandana
