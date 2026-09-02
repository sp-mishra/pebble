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

#include "containers/dynamic/SmallVector.hpp"
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

        [[nodiscard]] inline Spectrum spectrum_c() noexcept { // Cyan (green + blue)
            Spectrum s{};
            for (std::size_t i = 0; i < kBands; ++i) {
                float nm = band_nm(i);
                s[i] = nm <= 570.0f ? 1.0f : 0.0f;
            }
            return s;
        }

        [[nodiscard]] inline Spectrum spectrum_m() noexcept { // Magenta (blue + red)
            Spectrum s{};
            for (std::size_t i = 0; i < kBands; ++i) {
                float nm = band_nm(i);
                s[i] = (nm <= 485.0f || nm >= 590.0f) ? 1.0f : 0.0f;
            }
            return s;
        }

        [[nodiscard]] inline Spectrum spectrum_y() noexcept { // Yellow (red + green)
            Spectrum s{};
            for (std::size_t i = 0; i < kBands; ++i) {
                float nm = band_nm(i);
                s[i] = nm >= 500.0f ? 1.0f : 0.0f;
            }
            return s;
        }

        [[nodiscard]] inline Spectrum spectrum_r() noexcept { // Red
            Spectrum s{};
            for (std::size_t i = 0; i < kBands; ++i) {
                float nm = band_nm(i);
                s[i] = nm >= 590.0f ? 1.0f : 0.0f;
            }
            return s;
        }

        [[nodiscard]] inline Spectrum spectrum_g() noexcept { // Green
            Spectrum s{};
            for (std::size_t i = 0; i < kBands; ++i) {
                float nm = band_nm(i);
                s[i] = (nm >= 500.0f && nm <= 570.0f) ? 1.0f : 0.0f;
            }
            return s;
        }

        [[nodiscard]] inline Spectrum spectrum_b() noexcept { // Blue
            Spectrum s{};
            for (std::size_t i = 0; i < kBands; ++i) {
                float nm = band_nm(i);
                s[i] = nm <= 485.0f ? 1.0f : 0.0f;
            }
            return s;
        }

        [[nodiscard]] inline Spectrum spectrum_w() noexcept { // White
            Spectrum s{};
            for (std::size_t i = 0; i < kBands; ++i) s[i] = 1.0f;
            return s;
        }

        [[nodiscard]] constexpr float clampr(float r) noexcept {
            return r < 1e-4f ? 1e-4f : (r > 0.9999f ? 0.9999f : r);
        }
    } // namespace detail

    // Converts RGB color to physical Spectral Reflectance Curve using 7-primary decomposition
    [[nodiscard]] inline Spectrum from_linear_rgb(float r, float g, float b) noexcept {
        r = kalpana::detail::clamp01(r);
        g = kalpana::detail::clamp01(g);
        b = kalpana::detail::clamp01(b);

        const float w = std::min({r, g, b});
        const float r_rem = r - w;
        const float g_rem = g - w;
        const float b_rem = b - w;

        // Subtractive secondary filters
        const float c = std::min(g_rem, b_rem);
        const float m = std::min(r_rem, b_rem);
        const float y = std::min(r_rem, g_rem);

        // Primary residuals
        const float rr = r_rem - m - y + std::min(m, y);
        const float gg = g_rem - c - y + std::min(c, y);
        const float bb = b_rem - c - m + std::min(c, m);

        const auto spec_c = detail::spectrum_c();
        const auto spec_m = detail::spectrum_m();
        const auto spec_y = detail::spectrum_y();
        const auto spec_r = detail::spectrum_r();
        const auto spec_g = detail::spectrum_g();
        const auto spec_b = detail::spectrum_b();
        const auto spec_w = detail::spectrum_w();

        Spectrum s{};
        for (std::size_t i = 0; i < kBands; ++i) {
            float val = w * spec_w[i] +
                c * spec_c[i] +
                m * spec_m[i] +
                y * spec_y[i] +
                rr * spec_r[i] +
                gg * spec_g[i] +
                bb * spec_b[i];
            s[i] = kalpana::detail::clamp01(val);
        }
        return s;
    }

    // Converts Reflectance Spectrum back to RGB via 16-band tristimulus integration
    inline void to_linear_rgb(const Spectrum& s, float& r, float& g, float& b) noexcept {
        // Red: average over long wavelength bands (nm >= 590nm)
        // Green: average over mid wavelength bands (500nm <= nm <= 570nm)
        // Blue: average over short wavelength bands (nm <= 485nm)
        float r_acc = 0.0f, g_acc = 0.0f, b_acc = 0.0f;
        int r_cnt = 0, g_cnt = 0, b_cnt = 0;

        for (std::size_t i = 0; i < kBands; ++i) {
            float nm = detail::band_nm(i);
            if (nm >= 590.0f) {
                r_acc += s[i];
                r_cnt++;
            }
            if (nm >= 500.0f && nm <= 570.0f) {
                g_acc += s[i];
                g_cnt++;
            }
            if (nm <= 485.0f) {
                b_acc += s[i];
                b_cnt++;
            }
        }

        r = kalpana::detail::clamp01(r_cnt > 0 ? (r_acc / float(r_cnt)) : 0.0f);
        g = kalpana::detail::clamp01(g_cnt > 0 ? (g_acc / float(g_cnt)) : 0.0f);
        b = kalpana::detail::clamp01(b_cnt > 0 ? (b_acc / float(b_cnt)) : 0.0f);
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
            std::array < double, 3 > lrgb{
                double(kalpana::detail::clamp01(c.r)),
                double(kalpana::detail::clamp01(c.g)),
                double(kalpana::detail::clamp01(c.b))
            };
            auto R_38 = akruti::spectral::km38::lRGB_to_R(lrgb, cfg);
            colors_storage.emplace_back(R_38, cfg);
        }

        if (wsum <= 0.0f) return colors::transparent();

        for (std::size_t i = 0; i < pigments.size(); ++i) {
            items.push_back(akruti::spectral::km38::MixItem<double>{
                .color = &colors_storage[i],
                .factor = static_cast<double>(pigments[i].second)
            });
        }

        auto out = akruti::spectral::km38::mix<double>(
            std::span<const akruti::spectral::km38::MixItem<double>>(items.data(), items.size()), cfg);
        auto xyz = akruti::spectral::km38::R_to_XYZ(out.R());
        auto lrgb = akruti::spectral::km38::XYZ_to_lRGB(xyz);

        float out_a = (wsum > 0.0f) ? (asum / wsum) : 1.0f;
        return Color{
            kalpana::detail::clamp01(static_cast<float>(lrgb[0])),
            kalpana::detail::clamp01(static_cast<float>(lrgb[1])),
            kalpana::detail::clamp01(static_cast<float>(lrgb[2])),
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
        float alpha = 1.0f;

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
        float offset = 0.0f; // [0, 1]
        SpectralColor pigment{};
    };

    struct SpectralGradient {
        containers::dynamic::SmallVector<SpectralGradientStop, 8> stops;

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
        SpectralGradient g;
        for (const auto& s : stops) { (void)g.stops.push_back(s); }
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
