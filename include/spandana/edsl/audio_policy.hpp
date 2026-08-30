#pragma once
// ============================================================================
// spandana/edsl/audio_policy.hpp — Auto-Sonification Policy for Spandana ⇄ Dhvani
// ============================================================================
// Bridges Spandana simulation directives to Dhvani procedural audio WITHOUT a
// hard dependency: sonification is a compile-time policy. The default policy
// (`NullSonifier`) is an empty type — via [[no_unique_address]] it costs zero
// bytes and every sonify call compiles to nothing, so audio is truly opt-in and
// zero-overhead when unused. Opting in means substituting `DhvaniSonifier`,
// which turns a simulation profile + normalized material state into the correct
// procedural cue, reusing Dhvani's prakriti/gati bridges as the single source
// of acoustic truth.
//
// "Based on the type of simulation, automatically create the sounds from
//  prakriti and gati" — SimProfile selects the palette; DhvaniSonifier maps
//  prakriti material state through from_prakriti_material() and emits the cue.
// ============================================================================

#include "../resource_key.hpp"
#include "dhvani/dhvani.hpp"
#include "dhvani/gati_bridge.hpp"
#include "dhvani/prakriti_bridge.hpp"
#include <cstdint>
#include <string_view>

namespace pebble::spandana::edsl {

// The kind of simulation event being sonified. Selects a cue palette.
enum class SimProfile : std::uint8_t {
    Impact,      // rigid contact / collision
    Fracture,    // brittle break / shatter
    Friction,    // sustained sliding contact
    Fluid,       // splash / flow
    Thermal,     // phase change (melt / freeze / boil)
    Explosion,   // radial impulse / blast
};

// A single acoustic cue: which procedural voice + how loud/how high.
struct SonifyCue {
    std::string_view name{};
    float            volume = 1.0f;
    float            pitch  = 1.0f;
};

// Normalized simulation state handed to the sonifier. All fields are optional
// context; a profile uses whichever it needs. Populated from prakriti/gati.
struct SonifyContext {
    float density     = 0.5f;  // normalized [0..1] (prakriti)
    float temperature = 0.0f;  // normalized [0..1] (prakriti)
    float pressure    = 0.0f;  // normalized [0..1] (prakriti)
    float intensity   = 1.0f;  // normalized impulse / velocity / heat [0..1] (gati)
};

// Map a simulation profile + context to the procedural cue that voices it.
// This is the "palette": one place that decides which Dhvani cue a profile
// speaks, so the choice is data, not scattered magic strings.
[[nodiscard]] inline SonifyCue sound_palette(SimProfile profile, const SonifyContext& ctx) noexcept {
    using dhvani::DhvaniCue;
    // Derive an acoustic material from prakriti state to modulate the cue.
    const auto mat = dhvani::from_prakriti_material(ctx.density, ctx.temperature, ctx.pressure);
    // Denser/stiffer media pitch up; the intensity drives volume.
    const float pitch = 0.75f + mat.stiffness * 0.5f;
    switch (profile) {
        case SimProfile::Fracture:
            return {DhvaniCue::fracture, ctx.intensity, pitch};
        case SimProfile::Friction:
            return {DhvaniCue::friction, ctx.intensity, pitch};
        case SimProfile::Explosion:
            return {DhvaniCue::fracture, ctx.intensity, pitch * 0.6f}; // low, big
        case SimProfile::Fluid:
            return {DhvaniCue::impact, ctx.intensity * 0.6f, pitch * 1.2f};
        case SimProfile::Thermal:
            return {DhvaniCue::friction, ctx.intensity * 0.4f, pitch * 1.4f};
        case SimProfile::Impact:
        default:
            return {DhvaniCue::impact, ctx.intensity, pitch};
    }
}

// ----------------------------------------------------------------------------
// Sonifier policy contract.
// ----------------------------------------------------------------------------
template <typename S>
concept Sonifier = requires(S& s, SimProfile p, const SonifyContext& ctx) {
    { s.sonify(p, ctx) } noexcept;
};

// Default policy: no audio. Empty type — [[no_unique_address]] makes it free.
struct NullSonifier {
    void sonify(SimProfile, const SonifyContext&) const noexcept {}
};
static_assert(Sonifier<NullSonifier>);

// Active policy: routes profile+context through the palette to a Dhvani SoundBus.
struct DhvaniSonifier {
    dhvani::SoundBus* bus = nullptr;
    float             master_volume = 1.0f;

    void sonify(SimProfile profile, const SonifyContext& ctx) const noexcept {
        if (!bus) return;
        const SonifyCue cue = sound_palette(profile, ctx);
        bus->play(cue.name, cue.volume * master_volume, cue.pitch);
    }
};
static_assert(Sonifier<DhvaniSonifier>);

// ----------------------------------------------------------------------------
// AutoSonifyAction — a timeline directive that emits a profile's cue on start.
// Templated on the Sonifier policy: with NullSonifier the action carries no
// sonifier bytes and on_start is a no-op the optimizer deletes.
// ----------------------------------------------------------------------------
template <Sonifier S = NullSonifier>
class AutoSonifyAction {
public:
    AutoSonifyAction(S sonifier, SimProfile profile, SonifyContext ctx, ResourceKey key)
        : sonifier_(sonifier), profile_(profile), ctx_(ctx), key_(key) {}

    void on_start() noexcept { sonifier_.sonify(profile_, ctx_); }
    void update(float, float) noexcept {}
    [[nodiscard]] float duration() const noexcept { return 0.0f; }
    [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

private:
    [[no_unique_address]] S sonifier_;
    SimProfile              profile_;
    SonifyContext           ctx_;
    ResourceKey             key_;
};

// Builder / entry point. `auto_sonify(profile).from(density,temp,...).via(sonifier)`.
template <Sonifier S = NullSonifier>
class AutoSonifyBuilder {
public:
    AutoSonifyBuilder(SimProfile profile, ResourceKey key, S sonifier = {})
        : profile_(profile), key_(key), sonifier_(sonifier) {}

    AutoSonifyBuilder& from(float density, float temperature = 0.f, float pressure = 0.f) {
        ctx_.density = density; ctx_.temperature = temperature; ctx_.pressure = pressure;
        return *this;
    }
    AutoSonifyBuilder& intensity(float i) { ctx_.intensity = i; return *this; }

    // Rebind to an active sonifier policy (e.g. a DhvaniSonifier over a SoundBus).
    template <Sonifier S2>
    [[nodiscard]] AutoSonifyBuilder<S2> via(S2 sonifier) const {
        AutoSonifyBuilder<S2> b(profile_, key_, sonifier);
        b.set_context(ctx_);
        return b;
    }

    void set_context(const SonifyContext& c) { ctx_ = c; }

    [[nodiscard]] AutoSonifyAction<S> build() const {
        return AutoSonifyAction<S>(sonifier_, profile_, ctx_, key_);
    }

    operator AutoSonifyAction<S>() const { return build(); }

private:
    SimProfile              profile_;
    ResourceKey             key_;
    [[no_unique_address]] S sonifier_;
    SonifyContext           ctx_{};
};

[[nodiscard]] inline AutoSonifyBuilder<> auto_sonify(SimProfile profile, ResourceKey key = kWorldResource) {
    return AutoSonifyBuilder<>(profile, key);
}

} // namespace pebble::spandana::edsl
