#pragma once
// ============================================================================
// kalpana/color/pigment_catalog.hpp — Named Spectral Pigments & Extensible Registry
// ============================================================================
// Real-world Kubelka-Munk pigment spectral profiles with user-extensible registration.
// ============================================================================

#include "spectral.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>

namespace kalpana::pigments {

// ── Built-in Pigment Presets (Real-world spectral profiles) ──────────────────

// Cadmium Yellow: pure vivid yellow (reflects > 500nm, absorbs blue)
[[nodiscard]] inline spectral::SpectralColor cadmium_yellow() noexcept {
    return spectral::SpectralColor(spectral::from_linear_rgb(1.0f, 0.88f, 0.05f), 1.0f);
}

// Ultramarine Blue: deep vibrant cobalt/ultramarine blue (reflects blue, absorbs red/yellow)
[[nodiscard]] inline spectral::SpectralColor ultramarine_blue() noexcept {
    return spectral::SpectralColor(spectral::from_linear_rgb(0.04f, 0.18f, 0.95f), 1.0f);
}

// Cadmium Red: brilliant scarlet red (reflects red, absorbs green/blue)
[[nodiscard]] inline spectral::SpectralColor cadmium_red() noexcept {
    return spectral::SpectralColor(spectral::from_linear_rgb(0.95f, 0.08f, 0.08f), 1.0f);
}

// Burnt Sienna: rich warm reddish-brown earth pigment
[[nodiscard]] inline spectral::SpectralColor burnt_sienna() noexcept {
    return spectral::SpectralColor(spectral::from_linear_rgb(0.72f, 0.28f, 0.14f), 1.0f);
}

// Titanium White: maximum diffuse scattering
[[nodiscard]] inline spectral::SpectralColor titanium_white() noexcept {
    return spectral::SpectralColor(spectral::from_linear_rgb(0.98f, 0.98f, 0.98f), 1.0f);
}

// Ivory Black: high uniform absorption
[[nodiscard]] inline spectral::SpectralColor ivory_black() noexcept {
    return spectral::SpectralColor(spectral::from_linear_rgb(0.04f, 0.04f, 0.04f), 1.0f);
}

// Phthalo Green: intense emerald green (absorbs red/violet)
[[nodiscard]] inline spectral::SpectralColor phthalo_green() noexcept {
    return spectral::SpectralColor(spectral::from_linear_rgb(0.02f, 0.82f, 0.35f), 1.0f);
}

// Alizarin Crimson: deep crimson ruby red
[[nodiscard]] inline spectral::SpectralColor alizarin_crimson() noexcept {
    return spectral::SpectralColor(spectral::from_linear_rgb(0.85f, 0.05f, 0.35f), 1.0f);
}

// Raw Umber: muted dark greenish-brown earth pigment
[[nodiscard]] inline spectral::SpectralColor raw_umber() noexcept {
    return spectral::SpectralColor(spectral::from_linear_rgb(0.45f, 0.35f, 0.20f), 1.0f);
}

// Cerulean Blue: bright sky-cyan blue (reflects cyan-blue)
[[nodiscard]] inline spectral::SpectralColor cerulean_blue() noexcept {
    return spectral::SpectralColor(spectral::from_linear_rgb(0.08f, 0.65f, 0.92f), 1.0f);
}

// ── User-Extensible Pigment Registry ─────────────────────────────────────────

class PigmentRegistry {
public:
    static PigmentRegistry& instance() noexcept {
        static PigmentRegistry reg;
        return reg;
    }

    PigmentRegistry() {
        register_pigment("cadmium_yellow", cadmium_yellow());
        register_pigment("ultramarine_blue", ultramarine_blue());
        register_pigment("cadmium_red", cadmium_red());
        register_pigment("burnt_sienna", burnt_sienna());
        register_pigment("titanium_white", titanium_white());
        register_pigment("ivory_black", ivory_black());
        register_pigment("phthalo_green", phthalo_green());
        register_pigment("alizarin_crimson", alizarin_crimson());
        register_pigment("raw_umber", raw_umber());
        register_pigment("cerulean_blue", cerulean_blue());
    }

    // Register a custom user-defined spectral pigment
    void register_pigment(std::string_view name, spectral::SpectralColor pigment) {
        custom_pigments_[std::string(name)] = pigment;
    }

    // Register from custom reflectance function: f(float wavelength_nm) -> float [0, 1]
    void register_curve(std::string_view name, const std::function<float(float)>& curve_fn, float alpha = 1.0f) {
        spectral::Spectrum s{};
        for (std::size_t i = 0; i < spectral::kBands; ++i) {
            s[i] = std::clamp(curve_fn(spectral::detail::band_nm(i)), 0.0f, 1.0f);
        }
        register_pigment(name, spectral::SpectralColor(s, alpha));
    }

    // Find pigment by name; returns default black if not found
    [[nodiscard]] spectral::SpectralColor get(std::string_view name) const noexcept {
        auto it = custom_pigments_.find(std::string(name));
        if (it != custom_pigments_.end()) {
            return it->second;
        }
        return ivory_black();
    }

    [[nodiscard]] bool contains(std::string_view name) const noexcept {
        return custom_pigments_.contains(std::string(name));
    }

private:
    std::unordered_map<std::string, spectral::SpectralColor> custom_pigments_;
};

// Convenience free-function registry access
inline void register_custom_pigment(std::string_view name, spectral::SpectralColor pigment) {
    PigmentRegistry::instance().register_pigment(name, pigment);
}

inline void register_custom_pigment_curve(std::string_view name, const std::function<float(float)>& curve_fn, float alpha = 1.0f) {
    PigmentRegistry::instance().register_curve(name, curve_fn, alpha);
}

[[nodiscard]] inline spectral::SpectralColor get_pigment(std::string_view name) noexcept {
    return PigmentRegistry::instance().get(name);
}

} // namespace kalpana::pigments
