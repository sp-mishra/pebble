#pragma once
// ============================================================================
// prakriti/constraints/fatigue_policies.hpp — muscle fatigue policy set.
// Zero-overhead default policy and an opt-in 3-compartment model.
// ============================================================================

#include "../core/config.hpp"

#include <algorithm>
#include <span>
#include <vector>

namespace prakriti {
    struct NoFatigue {
        struct State {};

        struct StateColumn {
            void reserve(std::size_t) noexcept {}
            void push_back(State) noexcept {}
            void update(std::span<State>, std::span<const Scalar>, Scalar) noexcept {}
        };

        static Scalar effective_activation(const State&, Scalar raw_activation) noexcept {
            return std::clamp(raw_activation, Scalar(0), Scalar(1));
        }
    };

    struct ThreeCompartmentFatigue {
        struct State {
            Scalar active_motor_units = Scalar(1);
            Scalar resting_motor_units = Scalar(0);
            Scalar fatigued_motor_units = Scalar(0);
        };

        struct StateColumn {
            std::vector<State> data;

            void reserve(std::size_t n) {
                data.reserve(n);
            }

            void push_back(State s) {
                normalize(s);
                data.push_back(s);
            }

            void update(std::span<State> states,
                        std::span<const Scalar> activations,
                        Scalar dt) noexcept {
                const std::size_t n = std::min(states.size(), activations.size());
                for (std::size_t i = 0; i < n; ++i) {
                    auto& s = states[i];
                    const Scalar u = std::clamp(activations[i], Scalar(0), Scalar(1));

                    // Ma/Feldman/Doschak-style three-state transfer rates.
                    constexpr Scalar activation_rate = Scalar(2.0);
                    constexpr Scalar fatigue_rate = Scalar(0.6);
                    constexpr Scalar recovery_rate = Scalar(0.4);

                    const Scalar dMA = activation_rate * u * s.resting_motor_units - fatigue_rate * s.
                        active_motor_units;
                    const Scalar dMR = recovery_rate * s.fatigued_motor_units - activation_rate * u * s.
                        resting_motor_units;
                    const Scalar dMF = fatigue_rate * s.active_motor_units - recovery_rate * s.fatigued_motor_units;

                    s.active_motor_units += dMA * dt;
                    s.resting_motor_units += dMR * dt;
                    s.fatigued_motor_units += dMF * dt;
                    normalize(s);
                }
            }

        private:
            static void normalize(State& s) noexcept {
                s.active_motor_units = std::clamp(s.active_motor_units, Scalar(0), Scalar(1));
                s.resting_motor_units = std::clamp(s.resting_motor_units, Scalar(0), Scalar(1));
                s.fatigued_motor_units = std::clamp(s.fatigued_motor_units, Scalar(0), Scalar(1));
                Scalar sum = s.active_motor_units + s.resting_motor_units + s.fatigued_motor_units;
                if (sum <= Scalar(1e-6f)) {
                    s = State{};
                    return;
                }
                const Scalar inv = Scalar(1) / sum;
                s.active_motor_units *= inv;
                s.resting_motor_units *= inv;
                s.fatigued_motor_units *= inv;
            }
        };

        static Scalar effective_activation(const State& s, Scalar raw_activation) noexcept {
            const Scalar clamped = std::clamp(raw_activation, Scalar(0), Scalar(1));
            return clamped * std::clamp(s.active_motor_units, Scalar(0), Scalar(1));
        }
    };
} // namespace prakriti
