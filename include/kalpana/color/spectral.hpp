#pragma once
// ============================================================================
// kalpana/color/spectral.hpp — Kubelka–Munk Spectral Pigment Mixing & Science
// ============================================================================
// Native 16-band subtractive pigment mixing model over 380–730 nm wavelengths.
// Eliminates muddy grey RGB blends (Blue + Yellow -> Vibrant Green).
// Includes SpectralColor, SpectralGradient, and spectral bloom.
// ============================================================================

#include "color.hpp"
#include "color_space.hpp"
#include "akruti_spectral_km.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

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

// ── Spectral.js Inspired 7-Primary Subtractive Spectral Basis ────────────────
// Decomposes any RGB color into Cyan, Magenta, Yellow, Red, Green, Blue, White primaries.
// Basis spectra across 16 bands (380nm - 730nm) calibrated for pigment reflectance:

[[nodiscard]] inline Spectrum spectrum_c() noexcept { // Cyan (reflects blue+green, absorbs red)
    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) {
        float nm = band_nm(i);
        s[i] = nm <= 560.0f ? 0.92f : 0.04f;
    }
    return s;
}

[[nodiscard]] inline Spectrum spectrum_m() noexcept { // Magenta (reflects blue+red, absorbs green)
    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) {
        float nm = band_nm(i);
        s[i] = (nm <= 480.0f || nm >= 600.0f) ? 0.90f : 0.05f;
    }
    return s;
}

[[nodiscard]] inline Spectrum spectrum_y() noexcept { // Yellow (reflects green+red, absorbs blue)
    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) {
        float nm = band_nm(i);
        s[i] = nm >= 510.0f ? 0.96f : 0.03f;
    }
    return s;
}

[[nodiscard]] inline Spectrum spectrum_r() noexcept { // Red (reflects long wavelengths)
    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) {
        float nm = band_nm(i);
        s[i] = nm >= 595.0f ? 0.94f : 0.02f;
    }
    return s;
}

[[nodiscard]] inline Spectrum spectrum_g() noexcept { // Green (reflects mid wavelengths)
    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) {
        float nm = band_nm(i);
        s[i] = (nm >= 490.0f && nm <= 570.0f) ? 0.92f : 0.03f;
    }
    return s;
}

[[nodiscard]] inline Spectrum spectrum_b() noexcept { // Blue (reflects short wavelengths)
    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) {
        float nm = band_nm(i);
        s[i] = nm <= 480.0f ? 0.92f : 0.02f;
    }
    return s;
}

[[nodiscard]] inline Spectrum spectrum_w() noexcept { // White (full diffuse reflectance)
    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) s[i] = 0.98f;
    return s;
}

[[nodiscard]] constexpr float clampr(float r) noexcept {
    return r < 1e-4f ? 1e-4f : (r > 0.9999f ? 0.9999f : r);
}

} // namespace detail

// Converts RGB color to physical Spectral Reflectance Curve using km38 model
[[nodiscard]] inline Spectrum from_linear_rgb(float r, float g, float b) noexcept {
    akruti::spectral::km38::Config cfg;
    std::array<double, 3> srgb_255{
        double(kalpana::detail::clamp01(r) * 255.0f),
        double(kalpana::detail::clamp01(g) * 255.0f),
        double(kalpana::detail::clamp01(b) * 255.0f)
    };
    akruti::spectral::km38::Color<double> km_color(srgb_255, cfg);
    const auto& R_38 = km_color.R();

    Spectrum s{};
    for (std::size_t i = 0; i < kBands; ++i) {
        std::size_t idx = (i * (akruti::spectral::km38::kSize - 1)) / (kBands - 1);
        s[i] = static_cast<float>(R_38[idx]);
    }
    return s;
}

// Converts Reflectance Spectrum back to RGB via km38 calibrated CMF integration
inline void to_linear_rgb(const Spectrum& s, float& r, float& g, float& b) noexcept {
    akruti::spectral::km38::Config cfg;
    std::array<double, akruti::spectral::km38::kSize> R_38{};
    for (std::size_t i = 0; i < akruti::spectral::km38::kSize; ++i) {
        std::size_t idx = (i * (kBands - 1)) / (akruti::spectral::km38::kSize - 1);
        R_38[i] = static_cast<double>(s[i]);
    }
    akruti::spectral::km38::Color<double> km_color(R_38, cfg);
    const auto& srgb = km_color.sRGB();

    r = kalpana::detail::clamp01(static_cast<float>(srgb[0]) / 255.0f);
    g = kalpana::detail::clamp01(static_cast<float>(srgb[1]) / 255.0f);
    b = kalpana::detail::clamp01(static_cast<float>(srgb[2]) / 255.0f);
}

// Kubelka–Munk single-constant absorption/scattering ratio from reflectance.
[[nodiscard]] inline float ks_from_r(float R) noexcept {
    const float r = detail::clampr(R);
    return (1.0f - r) * (1.0f - r) / (2.0f * r);
}
// Inverse: reflectance from K/S.
[[nodiscard]] inline float r_from_ks(float ks) noexcept {
    return detail::clampr(1.0f + ks - std::sqrt(ks * ks + 2.0f * ks));
}

// Mix pigments by concentration using high-fidelity 38-band Kubelka-Munk model
[[nodiscard]] inline Color mix(std::span<const std::pair<Color, float>> pigments) noexcept {
    if (pigments.empty()) return colors::transparent();
    if (pigments.size() == 1) return pigments.front().first;

    akruti::spectral::km38::Config cfg;
    std::vector<akruti::spectral::km38::Color<double>> colors_storage;
    colors_storage.reserve(pigments.size());
    std::vector<akruti::spectral::km38::MixItem<double>> items;
    items.reserve(pigments.size());

    float asum = 0.0f;
    float wsum = 0.0f;

    for (const auto& [c, w] : pigments) {
        wsum += w;
        asum += w * c.a;
        std::array<double, 3> srgb_255{
            double(kalpana::detail::clamp01(c.r) * 255.0f),
            double(kalpana::detail::clamp01(c.g) * 255.0f),
            double(kalpana::detail::clamp01(c.b) * 255.0f)
        };
        colors_storage.emplace_back(srgb_255, cfg);
    }

    if (wsum <= 0.0f) return colors::transparent();

    for (std::size_t i = 0; i < pigments.size(); ++i) {
        items.push_back(akruti::spectral::km38::MixItem<double>{
            .color = &colors_storage[i],
            .factor = static_cast<double>(pigments[i].second)
        });
    }

    auto out = akruti::spectral::km38::mix<double>(std::span<const akruti::spectral::km38::MixItem<double>>(items.data(), items.size()), cfg);
    const auto& srgb = out.sRGB();

    float out_a = (wsum > 0.0f) ? (asum / wsum) : 1.0f;
    return Color{
        kalpana::detail::clamp01(static_cast<float>(srgb[0]) / 255.0f),
        kalpana::detail::clamp01(static_cast<float>(srgb[1]) / 255.0f),
        kalpana::detail::clamp01(static_cast<float>(srgb[2]) / 255.0f),
        kalpana::detail::clamp01(out_a)
    };
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

// ============================================================================
// Kalpana 2.0 First-Class Spectral Types & Science
// ============================================================================

struct SpectralColor {
    Spectrum reflectance{};
    float    alpha = 1.0f;

    constexpr SpectralColor() noexcept = default;
    constexpr SpectralColor(const Spectrum& r, float a = 1.0f) noexcept
        : reflectance(r), alpha(a) {}

    [[nodiscard]] static SpectralColor from_color(const Color& c) noexcept {
        return SpectralColor{from_linear_rgb(c.r, c.g, c.b), c.a};
    }

    [[nodiscard]] Color to_color() const noexcept {
        Color c{};
        to_linear_rgb(reflectance, c.r, c.g, c.b);
        c.a = alpha;
        return c;
    }

    [[nodiscard]] SpectralColor mix_km(const SpectralColor& other, float t) const noexcept {
        Color c1 = to_color();
        Color c2 = other.to_color();
        Color mixed = spectral::mix(c1, c2, t);
        return SpectralColor::from_color(mixed);
    }

    friend constexpr bool operator==(const SpectralColor&, const SpectralColor&) = default;
};

struct SpectralGradientStop {
    float          offset = 0.0f; // [0, 1]
    SpectralColor  pigment{};
};

struct SpectralGradient {
    std::vector<SpectralGradientStop> stops;

    [[nodiscard]] SpectralColor sample(float t) const noexcept {
        if (stops.empty()) return SpectralColor{};
        if (stops.size() == 1 || t <= stops.front().offset) return stops.front().pigment;
        if (t >= stops.back().offset) return stops.back().pigment;

        for (std::size_t i = 0; i < stops.size() - 1; ++i) {
            const float t0 = stops[i].offset;
            const float t1 = stops[i + 1].offset;
            if (t >= t0 && t <= t1) {
                const float factor = (t1 - t0 > 1e-6f) ? ((t - t0) / (t1 - t0)) : 0.0f;
                return stops[i].pigment.mix_km(stops[i + 1].pigment, factor);
            }
        }
        return stops.back().pigment;
    }
};

[[nodiscard]] inline SpectralColor sample_spectral_gradient(
    std::span<const SpectralGradientStop> stops, float t) noexcept {
    SpectralGradient g{.stops = std::vector<SpectralGradientStop>(stops.begin(), stops.end())};
    return g.sample(t);
}

// Spectral bloom filter in reflectance illumination space
[[nodiscard]] inline Color spectral_bloom(const Color& c, float threshold = 0.8f, float intensity = 1.0f) noexcept {
    const float luma = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    if (luma < threshold) {
        return c;
    }

    const float bloom_strength = (luma - threshold) / (1.0f - threshold + 1e-5f) * intensity;
    return Color{
        kalpana::detail::clamp01(c.r + bloom_strength * 0.2f),
        kalpana::detail::clamp01(c.g + bloom_strength * 0.2f),
        kalpana::detail::clamp01(c.b + bloom_strength * 0.2f),
        c.a
    };
}

} // namespace kalpana::spectral
