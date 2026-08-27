#pragma once
// ============================================================================
// kalpana/fill/noise.hpp — Concept-Driven Procedural Noise Generators
// ============================================================================
// Zero-dependency Simplex, FBM, Worley Cellular, and Turbulence noise generators
// with plug-and-play user extension support.
// ============================================================================

#include <cmath>
#include <concepts>
#include <cstdint>
#include <algorithm>

namespace kalpana::noise {

// Concept satisfied by any custom procedural noise generator
template <typename G>
concept noise_generator = requires(const G& g, float x, float y) {
    { g.evaluate(x, y) } noexcept -> std::same_as<float>;
};

namespace detail {

// Fast floor integer cast
[[nodiscard]] constexpr int fast_floor(float x) noexcept {
    const int xi = static_cast<int>(x);
    return x < float(xi) ? xi - 1 : xi;
}

// 2D Hash function
[[nodiscard]] constexpr float hash2d(int x, int y) noexcept {
    int n = x + y * 57;
    n = (n << 13) ^ n;
    const int nn = (n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff;
    return 1.0f - float(nn) / 1073741824.0f;
}

// 8 Simplex 2D gradient vectors
inline constexpr float grad2[8][2] = {
    { 1.0f,  0.0f}, {-1.0f,  0.0f},
    { 0.0f,  1.0f}, { 0.0f, -1.0f},
    { 0.70710678f,  0.70710678f}, {-0.70710678f,  0.70710678f},
    { 0.70710678f, -0.70710678f}, {-0.70710678f, -0.70710678f}
};

} // namespace detail

// ── Built-in Noise Generators ───────────────────────────────────────────────

struct SimplexNoise {
    [[nodiscard]] float evaluate(float x, float y) const noexcept {
        constexpr float F2 = 0.366025403f; // 0.5 * (sqrt(3.0) - 1.0)
        constexpr float G2 = 0.211324865f; // (3.0 - sqrt(3.0)) / 6.0

        const float s = (x + y) * F2;
        const int i = detail::fast_floor(x + s);
        const int j = detail::fast_floor(y + s);

        const float t = float(i + j) * G2;
        const float X0 = float(i) - t;
        const float Y0 = float(j) - t;
        const float x0 = x - X0;
        const float y0 = y - Y0;

        int i1 = 0, j1 = 0;
        if (x0 > y0) { i1 = 1; j1 = 0; } else { i1 = 0; j1 = 1; }

        const float x1 = x0 - float(i1) + G2;
        const float y1 = y0 - float(j1) + G2;
        const float x2 = x0 - 1.0f + 2.0f * G2;
        const float y2 = y0 - 1.0f + 2.0f * G2;

        auto contrib = [](int gi, int gj, float px, float py) -> float {
            float t_val = 0.5f - px * px - py * py;
            if (t_val < 0.0f) return 0.0f;
            t_val *= t_val;
            const unsigned int h = static_cast<unsigned int>((gi * 374761393 + gj * 668265263) ^ 0x9e3779b9) & 7;
            const float gx = detail::grad2[h][0];
            const float gy = detail::grad2[h][1];
            return t_val * t_val * (gx * px + gy * py);
        };

        const float n0 = contrib(i, j, x0, y0);
        const float n1 = contrib(i + i1, j + j1, x1, y1);
        const float n2 = contrib(i + 1, j + 1, x2, y2);

        // Scaled to [-1, 1]
        return 70.0f * (n0 + n1 + n2);
    }
};

struct FbmNoise {
    int   octaves    = 6;
    float lacunarity = 2.0f;
    float gain       = 0.5f;

    [[nodiscard]] float evaluate(float x, float y) const noexcept {
        float sum = 0.0f;
        float freq = 1.0f;
        float amp = 1.0f;
        float max_amp = 0.0f;
        const SimplexNoise sn;

        for (int i = 0; i < octaves; ++i) {
            sum += sn.evaluate(x * freq, y * freq) * amp;
            max_amp += amp;
            freq *= lacunarity;
            amp *= gain;
        }
        return (max_amp > 0.0f) ? (sum / max_amp) : 0.0f;
    }
};

struct WorleyNoise {
    [[nodiscard]] float evaluate(float x, float y) const noexcept {
        const int xi = detail::fast_floor(x);
        const int yi = detail::fast_floor(y);
        float min_dist = 10.0f;

        for (int ox = -1; ox <= 1; ++ox) {
            for (int oy = -1; oy <= 1; ++oy) {
                const int cx = xi + ox;
                const int cy = yi + oy;
                const float hx = 0.5f + 0.5f * detail::hash2d(cx * 17 + 3, cy * 31 + 7);
                const float hy = 0.5f + 0.5f * detail::hash2d(cx * 53 + 11, cy * 79 + 13);
                const float px = float(cx) + hx;
                const float py = float(cy) + hy;
                const float dx = x - px;
                const float dy = y - py;
                const float dist = std::sqrt(dx * dx + dy * dy);
                min_dist = std::min(min_dist, dist);
            }
        }
        return std::clamp(min_dist, 0.0f, 1.0f);
    }
};

struct TurbulenceNoise {
    int octaves = 6;

    [[nodiscard]] float evaluate(float x, float y) const noexcept {
        float sum = 0.0f;
        float freq = 1.0f;
        float amp = 1.0f;
        float max_amp = 0.0f;
        const SimplexNoise sn;

        for (int i = 0; i < octaves; ++i) {
            sum += std::fabs(sn.evaluate(x * freq, y * freq)) * amp;
            max_amp += amp;
            freq *= 2.0f;
            amp *= 0.5f;
        }
        return (max_amp > 0.0f) ? (sum / max_amp) : 0.0f;
    }
};

struct PerlinNoise {
    [[nodiscard]] float evaluate(float x, float y) const noexcept {
        const SimplexNoise sn;
        return sn.evaluate(x, y);
    }
};

// ── Free Functions ──────────────────────────────────────────────────────────

[[nodiscard]] inline float simplex(float x, float y) noexcept {
    return SimplexNoise{}.evaluate(x, y);
}

[[nodiscard]] inline float fbm(float x, float y, int octaves = 6, float lacunarity = 2.0f, float gain = 0.5f) noexcept {
    return FbmNoise{.octaves = octaves, .lacunarity = lacunarity, .gain = gain}.evaluate(x, y);
}

[[nodiscard]] inline float worley(float x, float y) noexcept {
    return WorleyNoise{}.evaluate(x, y);
}

[[nodiscard]] inline float turbulence(float x, float y, int octaves = 6) noexcept {
    return TurbulenceNoise{.octaves = octaves}.evaluate(x, y);
}

} // namespace kalpana::noise
