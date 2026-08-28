#pragma once
// gati/contact_constraint.hpp — Sequential-Impulse Contact Constraint representation.
#include "akruti/math.hpp"
#include <cstdint>

namespace gati {

struct ContactConstraint {
    std::uint32_t body_a{0};
    std::uint32_t body_b{0};

    akruti::Vec normal{0, 1};       // Unit normal pointing from body_a into body_b
    akruti::Vec contact_point{};    // World contact position
    akruti::Vec tangent{-1, 0};     // Friction tangent vector

    float penetration{0.0f};

    float normal_impulse_accum{0.0f};  // Warm-started accumulated normal impulse
    float tangent_impulse_accum{0.0f}; // Warm-started accumulated friction impulse

    float bias{0.0f};                  // Baumgarte stabilization bias
    float effective_mass_normal{0.0f};
    float effective_mass_tangent{0.0f};

    float friction{0.5f};
    float restitution{0.0f};

    akruti::Vec r_a{};                 // Vector from body_a center to contact_point
    akruti::Vec r_b{};                 // Vector from body_b center to contact_point
};

} // namespace gati
