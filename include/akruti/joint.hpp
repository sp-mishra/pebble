#pragma once
// akruti/joint.hpp — joint-frame GEOMETRY only. Defines the kinematic constraint data a solver
// needs; contains NO dynamics. Independent, header-only, no virtual, no macros.
//
// A joint binds two bodies (by opaque uint32 id; particle index or rigid-body handle — the
// solver decides) through local anchor points and, for some types, an axis and limits. Anchors
// are stored in each body's LOCAL frame; the solver transforms them to world before projecting.
#include "math.hpp"
#include <cstdint>

namespace akruti {
    enum class JointType : std::uint8_t {
        Distance, // rigid/rod: keep |worldA - worldB| == rest_length (compliance for springy rod)
        Revolute, // pin: worldA == worldB (2 positional constraints); free relative rotation
        Prismatic, // slider: relative motion constrained to `axis`; angle locked
        Weld, // fully rigid: coincident anchors + locked relative angle
        Motor, // drive relative angle toward target_angle (with max torque via compliance)
    };

    struct Joint {
        JointType type{JointType::Distance};
        std::uint32_t body_a{0};
        std::uint32_t body_b{0};

        Vec2<Scalar> anchor_a{}; // local-frame anchor on body A
        Vec2<Scalar> anchor_b{}; // local-frame anchor on body B

        Vec2<Scalar> axis{1, 0}; // prismatic slide axis / motor reference (body-A local)

        Scalar rest_length{0}; // Distance: target separation
        Scalar min_limit{-1e18f}; // Prismatic: slide range; Motor/Revolute: angle range
        Scalar max_limit{1e18f};
        Scalar target_angle{0}; // Motor: driven relative angle (radians)

        Scalar compliance{0}; // XPBD compliance = 1/stiffness (0 => perfectly rigid)
    };

    // ── Builders ───────────────────────────────────────────────────────────────────────
    [[nodiscard]] inline Joint make_distance(const std::uint32_t a, const std::uint32_t b, const Vec2<Scalar> la,
                                             const Vec2<Scalar> lb, const Scalar rest, const Scalar compliance = 0) noexcept {
        Joint j;
        j.type = JointType::Distance;
        j.body_a = a;
        j.body_b = b;
        j.anchor_a = la;
        j.anchor_b = lb;
        j.rest_length = rest;
        j.compliance = compliance;
        return j;
    }

    [[nodiscard]] inline Joint make_revolute(const std::uint32_t a, const std::uint32_t b, const Vec2<Scalar> la,
                                             const Vec2<Scalar> lb, const Scalar compliance = 0) noexcept {
        Joint j;
        j.type = JointType::Revolute;
        j.body_a = a;
        j.body_b = b;
        j.anchor_a = la;
        j.anchor_b = lb;
        j.compliance = compliance;
        return j;
    }

    [[nodiscard]] inline Joint make_prismatic(const std::uint32_t a, const std::uint32_t b, const Vec la,
                                              const Vec lb, const Vec axis,
                                              const Scalar lo, const Scalar hi, const Scalar compliance = 0) noexcept {
        Joint j;
        j.type = JointType::Prismatic;
        j.body_a = a;
        j.body_b = b;
        j.anchor_a = la;
        j.anchor_b = lb;
        j.axis = akruti::normalize(axis);
        j.min_limit = lo;
        j.max_limit = hi;
        j.compliance = compliance;
        return j;
    }

    [[nodiscard]] inline Joint make_weld(const std::uint32_t a, const std::uint32_t b, const Vec2<Scalar> la,
                                         const Vec2<Scalar> lb, const Scalar compliance = 0) noexcept {
        Joint j;
        j.type = JointType::Weld;
        j.body_a = a;
        j.body_b = b;
        j.anchor_a = la;
        j.anchor_b = lb;
        j.compliance = compliance;
        return j;
    }

    [[nodiscard]] inline Joint make_motor(const std::uint32_t a, const std::uint32_t b, const Scalar target,
                                          const Scalar lo, const Scalar hi, const Scalar compliance = 0) noexcept {
        Joint j;
        j.type = JointType::Motor;
        j.body_a = a;
        j.body_b = b;
        j.target_angle = target;
        j.min_limit = lo;
        j.max_limit = hi;
        j.compliance = compliance;
        return j;
    }

    // Transform a local anchor to world given a rigid frame (rotation + translation).
    [[nodiscard]] inline Vec2<Scalar> anchor_world(const Mat2<Scalar>& rot, const Vec2<Scalar> translate,
                                                   const Vec2<Scalar> local) noexcept {
        return rot * local + translate;
    }
} // namespace akruti
