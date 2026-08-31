#pragma once
// ============================================================================
// prakriti/constraints/tendon_models.hpp — tendon series element models.
// ============================================================================

#include "../core/config.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace prakriti {

struct NonlinearTendon {
    static Scalar force(Scalar tendon_length, Scalar slack_length,
                        Scalar k_linear, Scalar toe_strain) noexcept {
        const Scalar slack = std::max(slack_length, Scalar(1e-4f));
        const Scalar strain = (tendon_length - slack) / slack;
        if (strain <= Scalar(0)) return Scalar(0);

        const Scalar toe = std::max(toe_strain, Scalar(1e-4f));
        if (strain < toe) {
            constexpr Scalar c1 = Scalar(1.0);
            constexpr Scalar c2 = Scalar(8.0);
            return c1 * (std::exp(c2 * strain) - Scalar(1));
        }

        constexpr Scalar c1 = Scalar(1.0);
        constexpr Scalar c2 = Scalar(8.0);
        const Scalar f_toe = c1 * (std::exp(c2 * toe) - Scalar(1));
        return k_linear * (strain - toe) + f_toe;
    }

    static void force_batch(std::span<const Scalar> tendon_length,
                            std::span<const Scalar> slack_length,
                            Scalar k_linear,
                            Scalar toe_strain,
                            std::span<Scalar> out_force) noexcept {
        const std::size_t n = std::min({tendon_length.size(), slack_length.size(), out_force.size()});
        for (std::size_t i = 0; i < n; ++i) {
            out_force[i] = force(tendon_length[i], slack_length[i], k_linear, toe_strain);
        }
    }
};

struct LinearTendon {
    static Scalar force(Scalar tendon_length, Scalar slack_length,
                        Scalar k_linear, Scalar) noexcept {
        const Scalar slack = std::max(slack_length, Scalar(1e-4f));
        const Scalar strain = (tendon_length - slack) / slack;
        return std::max(Scalar(0), k_linear * strain);
    }

    static void force_batch(std::span<const Scalar> tendon_length,
                            std::span<const Scalar> slack_length,
                            Scalar k_linear,
                            Scalar toe_strain,
                            std::span<Scalar> out_force) noexcept {
        (void)toe_strain;
        const std::size_t n = std::min({tendon_length.size(), slack_length.size(), out_force.size()});
        for (std::size_t i = 0; i < n; ++i) {
            out_force[i] = force(tendon_length[i], slack_length[i], k_linear, Scalar(0));
        }
    }
};

struct RigidTendon {
    static Scalar force(Scalar tendon_length, Scalar slack_length,
                        Scalar, Scalar) noexcept {
        return tendon_length > slack_length ? Scalar(1e6f) : Scalar(0);
    }

    static void force_batch(std::span<const Scalar> tendon_length,
                            std::span<const Scalar> slack_length,
                            Scalar k_linear,
                            Scalar toe_strain,
                            std::span<Scalar> out_force) noexcept {
        (void)k_linear;
        (void)toe_strain;
        const std::size_t n = std::min({tendon_length.size(), slack_length.size(), out_force.size()});
        for (std::size_t i = 0; i < n; ++i) {
            out_force[i] = force(tendon_length[i], slack_length[i], Scalar(0), Scalar(0));
        }
    }
};

} // namespace prakriti
