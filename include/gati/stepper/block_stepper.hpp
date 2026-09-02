#pragma once
// ============================================================================
// gati/stepper/block_stepper.hpp — Hierarchical Power-of-Two Multi-Rate Stepper
// ============================================================================
// Modern C++23 header-only Aarseth/Symplectic Block-Step Integration.
// Assigns bodies to power-of-two sub-time-step rungs based on local acceleration.
// ============================================================================

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace gati::stepper {
    // Calculates optimal power-of-two step rung: k in [0..MaxRungs] -> dt_i = base_dt / 2^k
    template <std::uint8_t MaxRungs = 3>
    [[nodiscard]] constexpr std::uint8_t
    compute_acceleration_rung(float acc_magnitude,
                              float velocity_magnitude,
                              float eta = 0.02f) noexcept {
        if (acc_magnitude < 1e-4f) return 0;

        // Aarseth criterion: dt_i \approx eta * (v / a)
        const float dt_req = eta * (velocity_magnitude + 1.0f) / (acc_magnitude + 1e-3f);

        // Map required dt into power-of-two rung
        if (dt_req > 0.016f) return 0; // Rung 0: 1x dt (macro step)
        if (dt_req > 0.008f) return 1; // Rung 1: 0.5x dt
        if (dt_req > 0.004f) return 2; // Rung 2: 0.25x dt
        return std::min(MaxRungs, static_cast<std::uint8_t>(3)); // Rung 3: 0.125x dt (micro step for close binaries)
    }
} // namespace gati::stepper
