#pragma once
// dhvani/physical/material.hpp — PhysicalMaterial concept and preset catalog.

#include <concepts>
#include <string_view>

namespace pebble::dhvani::physical {

// Normalized acoustic descriptor for a physical material
struct MaterialParams {
    float density     = 1.0f;  // normalized [0..1]
    float stiffness   = 0.5f;  // [0..1]: rubber=0, steel=1
    float damping     = 0.5f;  // [0..1]: dead=0, resonant≈0 (low = long ring)
    float brittleness = 0.5f;  // [0..1]: plastic=0, glass=1
    float roughness   = 0.5f;  // surface texture influencing friction timbre
    float thickness   = 0.5f;  // [0..1]: thin sheet=0, solid block=1
};

template <typename T>
concept PhysicalMaterial = requires(const T& m) {
    { m.material_params() } -> std::convertible_to<MaterialParams>;
    { m.name() }            -> std::convertible_to<std::string_view>;
};

// Preset carrier — satisfies PhysicalMaterial concept
struct MaterialPreset {
    std::string_view name_sv;
    MaterialParams   params;

    [[nodiscard]] constexpr MaterialParams material_params() const noexcept { return params; }
    [[nodiscard]] constexpr std::string_view name() const noexcept { return name_sv; }
};

static_assert(PhysicalMaterial<MaterialPreset>);

namespace presets {

[[nodiscard]] constexpr MaterialPreset steel() noexcept {
    return {"steel", {.density=0.98f, .stiffness=0.95f, .damping=0.05f, .brittleness=0.30f, .roughness=0.20f, .thickness=0.70f}};
}
[[nodiscard]] constexpr MaterialPreset glass() noexcept {
    return {"glass", {.density=0.60f, .stiffness=0.85f, .damping=0.02f, .brittleness=0.95f, .roughness=0.10f, .thickness=0.20f}};
}
[[nodiscard]] constexpr MaterialPreset wood() noexcept {
    return {"wood",  {.density=0.40f, .stiffness=0.45f, .damping=0.35f, .brittleness=0.50f, .roughness=0.60f, .thickness=0.80f}};
}
[[nodiscard]] constexpr MaterialPreset rubber() noexcept {
    return {"rubber",{.density=0.50f, .stiffness=0.05f, .damping=0.90f, .brittleness=0.02f, .roughness=0.80f, .thickness=0.50f}};
}
[[nodiscard]] constexpr MaterialPreset cloth() noexcept {
    return {"cloth", {.density=0.10f, .stiffness=0.02f, .damping=0.95f, .brittleness=0.05f, .roughness=0.90f, .thickness=0.05f}};
}
[[nodiscard]] constexpr MaterialPreset concrete() noexcept {
    return {"concrete",{.density=0.95f,.stiffness=0.90f, .damping=0.20f, .brittleness=0.70f, .roughness=0.80f, .thickness=1.00f}};
}
[[nodiscard]] constexpr MaterialPreset ceramic() noexcept {
    return {"ceramic",{.density=0.65f, .stiffness=0.80f, .damping=0.08f, .brittleness=0.88f, .roughness=0.30f, .thickness=0.25f}};
}
[[nodiscard]] constexpr MaterialPreset wood_hollow() noexcept {
    return {"wood_hollow",{.density=0.25f,.stiffness=0.38f,.damping=0.18f,.brittleness=0.40f,.roughness=0.55f,.thickness=0.15f}};
}

} // namespace presets
} // namespace pebble::dhvani::physical
