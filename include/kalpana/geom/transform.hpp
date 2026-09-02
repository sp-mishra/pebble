#pragma once
// ============================================================================
// kalpana/geom/transform.hpp — 2D Affine Transform (Matrix 3x2)
// ============================================================================

#include "containers/numeric/math_vector.hpp"
#include <cmath>

namespace kalpana {
    struct Transform {
        float m00 = 1.0f, m01 = 0.0f, m02 = 0.0f; // Row 0
        float m10 = 0.0f, m11 = 1.0f, m12 = 0.0f; // Row 1

        [[nodiscard]] static constexpr Transform identity() noexcept {
            return Transform{};
        }

        [[nodiscard]] static constexpr Transform translate(float tx, float ty) noexcept {
            return Transform{1.0f, 0.0f, tx, 0.0f, 1.0f, ty};
        }

        [[nodiscard]] static constexpr Transform scale(float sx, float sy) noexcept {
            return Transform{sx, 0.0f, 0.0f, 0.0f, sy, 0.0f};
        }

        [[nodiscard]] static Transform rotate(float radians) noexcept {
            const float c = std::cos(radians);
            const float s = std::sin(radians);
            return Transform{c, -s, 0.0f, s, c, 0.0f};
        }

        [[nodiscard]] pebble::math::vec2 apply(const pebble::math::vec2& p) const noexcept {
            return pebble::math::vec2(
                m00 * p[0] + m01 * p[1] + m02,
                m10 * p[0] + m11 * p[1] + m12
            );
        }

        [[nodiscard]] Transform combine(const Transform& child) const noexcept {
            return Transform{
                m00 * child.m00 + m01 * child.m10,
                m00 * child.m01 + m01 * child.m11,
                m00 * child.m02 + m01 * child.m12 + m02,
                m10 * child.m00 + m11 * child.m10,
                m10 * child.m01 + m11 * child.m11,
                m10 * child.m02 + m11 * child.m12 + m12
            };
        }

        friend constexpr bool operator==(const Transform&, const Transform&) = default;
    };
} // namespace kalpana
