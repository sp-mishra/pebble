#pragma once
// ============================================================================
// spandana/easing.hpp — 30+ Robert Penner & Cubic-Bezier Easing Functions
// ============================================================================
// Header-only, constexpr-enabled, zero-overhead mathematical easings.
// ============================================================================

#include "concepts.hpp"
#include <cmath>
#include <numbers>

namespace pebble::spandana::ease {
    // ── Linear ──────────────────────────────────────────────────────────────────
    struct Linear {
        [[nodiscard]] constexpr float operator()(float t) const noexcept { return t; }
    };

    inline constexpr Linear linear{};

    // ── Quadratic ───────────────────────────────────────────────────────────────
    struct InQuad {
        [[nodiscard]] constexpr float operator()(float t) const noexcept { return t * t; }
    };

    struct OutQuad {
        [[nodiscard]] constexpr float operator()(float t) const noexcept { return t * (2.0f - t); }
    };

    struct InOutQuad {
        [[nodiscard]] constexpr float operator()(float t) const noexcept {
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
        }
    };

    inline constexpr InQuad in_quad{};
    inline constexpr OutQuad out_quad{};
    inline constexpr InOutQuad in_out_quad{};

    // ── Cubic ──────────────────────────────────────────────────────────────────
    struct InCubic {
        [[nodiscard]] constexpr float operator()(float t) const noexcept { return t * t * t; }
    };

    struct OutCubic {
        [[nodiscard]] constexpr float operator()(float t) const noexcept {
            float f = t - 1.0f;
            return f * f * f + 1.0f;
        }
    };

    struct InOutCubic {
        [[nodiscard]] constexpr float operator()(float t) const noexcept {
            return t < 0.5f ? 4.0f * t * t * t : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
        }
    };

    inline constexpr InCubic in_cubic{};
    inline constexpr OutCubic out_cubic{};
    inline constexpr InOutCubic in_out_cubic{};

    // ── Sine ───────────────────────────────────────────────────────────────────
    struct InSine {
        [[nodiscard]] float operator()(float t) const noexcept {
            return 1.0f - std::cos(t * (std::numbers::pi_v<float> * 0.5f));
        }
    };

    struct OutSine {
        [[nodiscard]] float operator()(float t) const noexcept {
            return std::sin(t * (std::numbers::pi_v<float> * 0.5f));
        }
    };

    struct InOutSine {
        [[nodiscard]] float operator()(float t) const noexcept {
            return -0.5f * (std::cos(std::numbers::pi_v<float> * t) - 1.0f);
        }
    };

    inline constexpr InSine in_sine{};
    inline constexpr OutSine out_sine{};
    inline constexpr InOutSine in_out_sine{};

    // ── Exponential ────────────────────────────────────────────────────────────
    struct InExpo {
        [[nodiscard]] float operator()(float t) const noexcept {
            return (t <= 0.0f) ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f));
        }
    };

    struct OutExpo {
        [[nodiscard]] float operator()(float t) const noexcept {
            return (t >= 1.0f) ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
        }
    };

    struct InOutExpo {
        [[nodiscard]] float operator()(float t) const noexcept {
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;
            if (t < 0.5f) return 0.5f * std::pow(2.0f, 20.0f * t - 10.0f);
            return 1.0f - 0.5f * std::pow(2.0f, -20.0f * t + 10.0f);
        }
    };

    inline constexpr InExpo in_expo{};
    inline constexpr OutExpo out_expo{};
    inline constexpr InOutExpo in_out_expo{};

    // ── Overshoot / Back ───────────────────────────────────────────────────────
    struct InBack {
        float s = 1.70158f;

        [[nodiscard]] constexpr float operator()(float t) const noexcept {
            return t * t * ((s + 1.0f) * t - s);
        }
    };

    struct OutBack {
        float s = 1.70158f;

        [[nodiscard]] constexpr float operator()(float t) const noexcept {
            float f = t - 1.0f;
            return f * f * ((s + 1.0f) * f + s) + 1.0f;
        }
    };

    struct InOutBack {
        float s = 1.70158f * 1.525f;

        [[nodiscard]] constexpr float operator()(float t) const noexcept {
            if (t < 0.5f) {
                return 0.5f * (4.0f * t * t * ((s + 1.0f) * 2.0f * t - s));
            }
            float f = 2.0f * t - 2.0f;
            return 0.5f * (f * f * ((s + 1.0f) * f + s) + 2.0f);
        }
    };

    inline constexpr InBack in_back{};
    inline constexpr OutBack out_back{};
    inline constexpr InOutBack in_out_back{};

    // ── Bounce ─────────────────────────────────────────────────────────────────
    struct OutBounce {
        [[nodiscard]] constexpr float operator()(float t) const noexcept {
            if (t < (1.0f / 2.75f)) {
                return 7.5625f * t * t;
            }
            else if (t < (2.0f / 2.75f)) {
                float f = t - (1.5f / 2.75f);
                return 7.5625f * f * f + 0.75f;
            }
            else if (t < (2.5f / 2.75f)) {
                float f = t - (2.25f / 2.75f);
                return 7.5625f * f * f + 0.9375f;
            }
            else {
                float f = t - (2.625f / 2.75f);
                return 7.5625f * f * f + 0.984375f;
            }
        }
    };

    struct InBounce {
        [[nodiscard]] constexpr float operator()(float t) const noexcept {
            return 1.0f - OutBounce{}(1.0f - t);
        }
    };

    struct InOutBounce {
        [[nodiscard]] constexpr float operator()(float t) const noexcept {
            if (t < 0.5f) return InBounce{}(t * 2.0f) * 0.5f;
            return OutBounce{}(t * 2.0f - 1.0f) * 0.5f + 0.5f;
        }
    };

    inline constexpr InBounce in_bounce{};
    inline constexpr OutBounce out_bounce{};
    inline constexpr InOutBounce in_out_bounce{};

    // ── Elastic ────────────────────────────────────────────────────────────────
    struct OutElastic {
        float p = 0.3f;

        [[nodiscard]] float operator()(float t) const noexcept {
            if (t <= 0.0f) return 0.0f;
            if (t >= 1.0f) return 1.0f;
            return std::pow(2.0f, -10.0f * t) * std::sin((t - p / 4.0f) * (2.0f * std::numbers::pi_v<float>) / p) +
                1.0f;
        }
    };

    inline constexpr OutElastic out_elastic{};
} // namespace pebble::spandana::ease
