#pragma once
// ============================================================================
// prakriti/constraints/fiber_models.hpp — muscle contractile element models.
// ============================================================================

#include "../core/config.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace prakriti {
    struct HillTypeFiber {
        static Scalar force_length(Scalar normalized_length) noexcept {
            constexpr Scalar width = Scalar(0.35);
            const Scalar nl = std::clamp(normalized_length, Scalar(0.2f), Scalar(1.8f));
            const Scalar x = (nl - Scalar(1)) / width;
            return std::exp(-x * x);
        }

        static Scalar force_velocity(Scalar normalized_velocity,
                                     Scalar max_shortening_vel) noexcept {
            const Scalar vmax = std::max(max_shortening_vel, Scalar(1e-4f));
            const Scalar v_norm = std::clamp(normalized_velocity / vmax, Scalar(-1.5f), Scalar(1.5f));
            if (v_norm <= Scalar(0)) {
                // Concentric contraction: force drops hyperbolically with shortening speed.
                return std::max(Scalar(0), (Scalar(1) + v_norm) / (Scalar(1) - Scalar(0.25) * v_norm));
            }
            // Eccentric contraction: bounded force boost.
            return Scalar(1) + Scalar(0.4) * std::min(v_norm, Scalar(1));
        }

        static Scalar passive_force(Scalar normalized_length) noexcept {
            const Scalar nl = std::clamp(normalized_length, Scalar(0), Scalar(1.6f));
            if (nl <= Scalar(1)) return Scalar(0);
            const Scalar x = nl - Scalar(1);
            return Scalar(0.05) * (std::exp(Scalar(5) * x) - Scalar(1));
        }

        static Scalar compute_force(Scalar activation, Scalar lce, Scalar vce,
                                    Scalar lopt, Scalar vmax, Scalar f_max,
                                    Scalar pennation) noexcept {
            const Scalar lopt_safe = std::max(lopt, Scalar(1e-4f));
            const Scalar nl = lce / lopt_safe;
            const Scalar nv = vce / lopt_safe;
            const Scalar active = f_max * std::clamp(activation, Scalar(0), Scalar(1))
                * force_length(nl) * force_velocity(nv, vmax);
            const Scalar passive = f_max * passive_force(nl);
            const Scalar force = (active + passive) * std::cos(pennation);
            return std::clamp(force, Scalar(0), f_max * Scalar(2.5f));
        }

        static void compute_force_batch(std::span<const Scalar> activation,
                                        std::span<const Scalar> lce,
                                        std::span<const Scalar> vce,
                                        std::span<const Scalar> lopt,
                                        Scalar vmax,
                                        std::span<const Scalar> f_max,
                                        std::span<const Scalar> pennation,
                                        std::span<Scalar> out_force) noexcept {
            const std::size_t n = std::min({
                activation.size(), lce.size(), vce.size(), lopt.size(), f_max.size(), pennation.size(), out_force.size()
            });
            for (std::size_t i = 0; i < n; ++i) {
                out_force[i] = compute_force(activation[i], lce[i], vce[i], lopt[i], vmax, f_max[i], pennation[i]);
            }
        }
    };

    struct LinearFiber {
        static Scalar compute_force(Scalar activation, Scalar lce, Scalar,
                                    Scalar lopt, Scalar, Scalar f_max,
                                    Scalar pennation) noexcept {
            const Scalar nl = lce / std::max(lopt, Scalar(1e-4f));
            const Scalar stretch = std::max(nl - Scalar(1), Scalar(0));
            const Scalar active = f_max * std::clamp(activation, Scalar(0), Scalar(1));
            const Scalar passive = f_max * Scalar(0.1) * stretch;
            return (active + passive) * std::cos(pennation);
        }

        static void compute_force_batch(std::span<const Scalar> activation,
                                        std::span<const Scalar> lce,
                                        std::span<const Scalar> vce,
                                        std::span<const Scalar> lopt,
                                        Scalar vmax,
                                        std::span<const Scalar> f_max,
                                        std::span<const Scalar> pennation,
                                        std::span<Scalar> out_force) noexcept {
            (void)vce;
            (void)vmax;
            const std::size_t n = std::min({
                activation.size(), lce.size(), lopt.size(), f_max.size(), pennation.size(), out_force.size()
            });
            for (std::size_t i = 0; i < n; ++i) {
                out_force[i] = compute_force(activation[i], lce[i], Scalar(0), lopt[i], Scalar(0), f_max[i],
                                             pennation[i]);
            }
        }
    };
} // namespace prakriti
