#pragma once
// ============================================================================
// kalpana/color/spectral.hpp — Kubelka–Munk Spectral Pigment Mixing
// ============================================================================
// Native 16-band subtractive pigment mixing model over 380–730 nm wavelengths.
// Eliminates muddy grey RGB blends (Blue + Yellow -> Vibrant Green).
// ============================================================================

#include "color.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>

namespace kalpana::spectral {

inline constexpr std::size_t kBands = 16;
using Spectrum = std::array<float, kBands>;

namespace detail {

// Band center wavelengths (nm), evenly spaced across the visible range.
[[nodiscard]] constexpr float band_nm(std::size_t i) noexcept {
    constexpr float lo = 380.0f, hi = 730.0f;
    return lo + (hi - lo) * (float(i) + 0.5f) / float(kBands);
}

// A smooth Gaussian bump centered at `mu` (nm) with width `sigma` — reflectance basis primitive.
[[nodiscard]] inline float bump(float nm, float mu, float sigma) noexcept {
    const float x = (nm - mu) / sigma;
    return std::exp(-0.5f * x * x);
}

// Reflectance basis for the three linear-RGB primaries. Red reflects long wavelengths, green mid,
// blue short — each a broad Gaussian so mixes stay smooth. Values are in (0,1).
[[nodiscard]] inline Spectrum basis_r() noexcept {
    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) s[i] = 0.02f + 0.96f * bump(band_nm(i), 660.0f, 60.0f);
    return s;
}
[[nodiscard]] inline Spectrum basis_g() noexcept {
    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) s[i] = 0.02f + 0.96f * bump(band_nm(i), 545.0f, 50.0f);
    return s;
}
[[nodiscard]] inline Spectrum basis_b() noexcept {
    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) s[i] = 0.02f + 0.96f * bump(band_nm(i), 445.0f, 45.0f);
    return s;
}

// CIE-like luminance-weighted response per primary: how strongly each basis contributes back to that
// RGB channel. We integrate basis_c against itself (self-response) and cross terms to build a 3x3
// reconstruction that makes a pure primary round-trip. Precomputed once.
struct Projector {
    Spectrum br, bg, bb;
    // response[out][in] = Σ_λ basis_out(λ) · basis_in(λ)  (Gram matrix of the basis)
    std::array<std::array<float, 3>, 3> gram{};
    std::array<std::array<float, 3>, 3> gram_inv{};

    Projector() {
        br = basis_r(); bg = basis_g(); bb = basis_b();
        const Spectrum* B[3] = {&br, &bg, &bb};
        for (int o = 0; o < 3; ++o)
            for (int i = 0; i < 3; ++i) {
                float acc = 0;
                for (std::size_t k = 0; k < kBands; ++k) acc += (*B[o])[k] * (*B[i])[k];
                gram[o][i] = acc;
            }
        gram_inv = invert3(gram);
    }

    static std::array<std::array<float, 3>, 3> invert3(const std::array<std::array<float, 3>, 3>& m) {
        const float a = m[0][0], b = m[0][1], c = m[0][2];
        const float d = m[1][0], e = m[1][1], f = m[1][2];
        const float g = m[2][0], h = m[2][1], i = m[2][2];
        const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        float det = a * A + b * B + c * C;
        if (std::fabs(det) < 1e-20f) det = 1e-20f;
        const float inv = 1.0f / det;
        return {{{A * inv, (c * h - b * i) * inv, (b * f - c * e) * inv},
                 {B * inv, (a * i - c * g) * inv, (c * d - a * f) * inv},
                 {C * inv, (b * g - a * h) * inv, (a * e - b * d) * inv}}};
    }
};

[[nodiscard]] inline const Projector& projector() {
    static const Projector p{};
    return p;
}

[[nodiscard]] constexpr float clampr(float r) noexcept {
    // Reflectance kept strictly inside (0,1] so KS = (1-R)²/2R stays finite.
    return r < 1e-4f ? 1e-4f : (r > 1.0f ? 1.0f : r);
}

} // namespace detail

// linear RGB -> reflectance spectrum (weighted sum of primary reflectance basis).
[[nodiscard]] inline Spectrum from_linear_rgb(float r, float g, float b) noexcept {
    const auto& p = detail::projector();
    Spectrum s{};
    for (std::size_t k = 0; k < kBands; ++k)
        s[k] = detail::clampr(r * p.br[k] + g * p.bg[k] + b * p.bb[k]);
    return s;
}

// reflectance spectrum -> linear RGB (project onto basis via the inverse Gram matrix).
inline void to_linear_rgb(const Spectrum& s, float& r, float& g, float& b) noexcept {
    const auto& p = detail::projector();
    // proj[c] = Σ_λ basis_c(λ) · s(λ)
    float proj[3] = {0, 0, 0};
    const Spectrum* B[3] = {&p.br, &p.bg, &p.bb};
    for (int c = 0; c < 3; ++c)
        for (std::size_t k = 0; k < kBands; ++k) proj[c] += (*B[c])[k] * s[k];
    // Solve Gram · rgb = proj  =>  rgb = gram_inv · proj
    r = p.gram_inv[0][0] * proj[0] + p.gram_inv[0][1] * proj[1] + p.gram_inv[0][2] * proj[2];
    g = p.gram_inv[1][0] * proj[0] + p.gram_inv[1][1] * proj[1] + p.gram_inv[1][2] * proj[2];
    b = p.gram_inv[2][0] * proj[0] + p.gram_inv[2][1] * proj[1] + p.gram_inv[2][2] * proj[2];
    r = kalpana::detail::clamp01(r); g = kalpana::detail::clamp01(g); b = kalpana::detail::clamp01(b);
}

// Kubelka–Munk single-constant absorption/scattering ratio from reflectance.
[[nodiscard]] inline float ks_from_r(float R) noexcept {
    const float r = detail::clampr(R);
    return (1.0f - r) * (1.0f - r) / (2.0f * r);
}
// Inverse: reflectance from K/S.
[[nodiscard]] inline float r_from_ks(float ks) noexcept {
    // R = 1 + KS − sqrt(KS² + 2·KS)
    return detail::clampr(1.0f + ks - std::sqrt(ks * ks + 2.0f * ks));
}

// Mix pigments by concentration in Kubelka–Munk space. Weights need not sum to 1 (normalized here).
[[nodiscard]] inline Color mix(std::span<const std::pair<Color, float>> pigments) noexcept {
    if (pigments.empty()) return colors::transparent();
    float wsum = 0.0f, asum = 0.0f;
    for (auto& [c, w] : pigments) { wsum += w; asum += w * c.a; }
    if (wsum <= 0.0f) return colors::transparent();
    const float invw = 1.0f / wsum;

    Spectrum ks_mix{};
    for (auto& [c, w] : pigments) {
        const Spectrum s = from_linear_rgb(c.r, c.g, c.b);
        const float cw = w * invw;
        for (std::size_t k = 0; k < kBands; ++k) ks_mix[k] += cw * ks_from_r(s[k]);
    }
    Spectrum r_mix{};
    for (std::size_t k = 0; k < kBands; ++k) r_mix[k] = r_from_ks(ks_mix[k]);

    Color out{};
    to_linear_rgb(r_mix, out.r, out.g, out.b);
    out.a = kalpana::detail::clamp01(asum * invw);
    return out;
}

// Two-color convenience.
[[nodiscard]] inline Color mix(const Color& a, const Color& b, float t) noexcept {
    const std::pair<Color, float> ps[2] = {{a, 1.0f - t}, {b, t}};
    return mix(std::span<const std::pair<Color, float>>(ps, 2));
}

// Multi-stop spectral gradient evaluation
[[nodiscard]] inline Color sample_gradient(std::span<const std::pair<float, Color>> stops, float t) noexcept {
    if (stops.empty()) return colors::transparent();
    if (stops.size() == 1 || t <= stops.front().first) return stops.front().second;
    if (t >= stops.back().first) return stops.back().second;

    for (std::size_t i = 0; i < stops.size() - 1; ++i) {
        const float t0 = stops[i].first;
        const float t1 = stops[i + 1].first;
        if (t >= t0 && t <= t1) {
            const float factor = (t1 - t0 > 1e-6f) ? ((t - t0) / (t1 - t0)) : 0.0f;
            return mix(stops[i].second, stops[i + 1].second, factor);
        }
    }
    return stops.back().second;
}

} // namespace kalpana::spectral
