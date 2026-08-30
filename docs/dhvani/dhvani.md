# Dhvani (ध्वनि) — Physically-Based Procedural Sound Engine

Header-only C++23/C++26. No virtual, no macros. Zero dynamic heap allocation on audio synthesis paths.
Dhvani provides physically-based procedural sound generation, modal synthesis, spatial 2D audio, composable signal graphs, and physics engine integration bridges.

Include: `#include <dhvani/dhvani.hpp>`, `#include <dhvani/sound_edsl.hpp>`

---

## 1. Features

1. **2D Spatial Audio (`dhvani/spatial.hpp`)**:
   - Inverse-distance logarithmic attenuation relative to `AudioListener2D`.
   - Equal-power stereo panning relative to listener forward orientation.
2. **SPSC `SoundBus` Queue (`dhvani/dhvani.hpp`)**:
   - Zero-allocation sound event queue decoupling simulation threads from platform audio sinks.
3. **PCM Synthesis Primitives (`dhvani/synth/`)**:
   - Oscillators: sine, saw, square, triangle, white noise (`waveform.hpp`).
   - ADSR envelope state machine (`envelope.hpp`).
   - RBJ cookbook biquad filters — LP/HP/BP/Notch, compile-time tag dispatch (`filter.hpp`).
   - Karplus-Strong string model and N-mode modal resonators (`resonator.hpp`).
4. **Physical Sound Models (`dhvani/physical/`)**:
   - `PhysicalMaterial` concept + presets: steel, glass, wood, rubber, cloth, concrete, ceramic (`material.hpp`).
   - Impact voice: force + material → modal resonator excitation + ADSR (`impact.hpp`).
   - Fracture voice: crack/shatter burst — brittleness-driven HP-filtered noise (`fracture.hpp`).
   - Friction voice: velocity-modulated band-pass filtered noise (`surface.hpp`).
   - Tear voice: periodic noise bursts, rate controlled by tear speed (`surface.hpp`).
   - Metallic modal presets: Bell, Cymbal, Plate, Pipe, Spring with empirical inharmonic ratios (`metal.hpp`).
5. **Fluent EDSL (`dhvani/sound_edsl.hpp`)**:
   - `impact()`, `fracture()`, `friction()`, `tear()`, `metal_hit()` entry points.
   - Method-chaining: `.material()`, `.force()`, `.velocity()`, `.metal()`, `.sample_rate()`.
   - Terminal: `.build()` → `SoundEvent`, `.render<N>()` → stack-allocated `SampleBlock<N>`.
6. **Signal Graph (`dhvani/graph/`)**:
   - `AudioNode` concept — any type with `process(out, in, sr)` and `reset()`.
   - `SoundGraph` — type-erased DAG evaluated in insertion order.
   - Built-in nodes: `OscillatorNode`, `GainNode`, `LowPassNode`, `MixerNode`, `EnvelopeNode`.
7. **Audio Backends (`dhvani/backend/`)**:
   - `AudioBackend` concept — `sample_rate()`, `start(callback)`, `stop()`, `is_running()`.
   - `NullBackend` — PCM capture to `std::vector` for testing and offline export.
   - `MiniAudioBackend` — hardware playback via miniaudio (gated on `DHVANI_USE_MINIAUDIO`).
8. **Physics Bridges**:
   - `GatiSoundBridge` (`dhvani/gati_bridge.hpp`) — Gati collision events → SoundBus cues.
   - `from_prakriti_material()` (`dhvani/prakriti_bridge.hpp`) — Prakriti particle state → `MaterialParams`.
9. **Spandana Timeline Integration (`dhvani/edsl.hpp`)**:
   - `audio_cue(sound_bus, name)` directive for declarative Spandana timelines.

---

## 2. Quick Start — Spatial Sound (existing)

```cpp
#include <dhvani/dhvani.hpp>
#include <dhvani/spatial.hpp>

pebble::dhvani::SoundBus bus;
bus.play("menu_select.wav", 0.8f, 1.0f);

pebble::dhvani::AudioListener2D listener{
    .position = {0.f, 0.f}, .forward = {0.f, 1.f}, .max_distance = 500.f
};
bus.play_spatial("explosion.wav", {120.f, 50.f}, listener, 1.f);
bus.drain([](const pebble::dhvani::SoundCue& cue) { /* send to hardware */ });
```

---

## 3. Physical Sound Models

### 3.1 Material Presets

```cpp
#include <dhvani/physical/material.hpp>
using namespace pebble::dhvani::physical;

// Built-in presets: steel, glass, wood, rubber, cloth, concrete, ceramic, wood_hollow
const auto mat = presets::steel(); // MaterialPreset satisfies PhysicalMaterial concept

// Custom material
MaterialParams my_mat{
    .density=0.7f, .stiffness=0.6f, .damping=0.3f,
    .brittleness=0.4f, .roughness=0.5f, .thickness=0.6f
};
```

### 3.2 Impact Sound

```cpp
#include <dhvani/physical/impact.hpp>
using namespace pebble::dhvani::physical;

ImpactVoice<8> voice{};
voice.trigger(presets::steel().params, {.force=0.8f, .contact_duration=0.002f}, 44100u);

// Render per-sample (call from audio thread)
while (voice.is_active()) {
    const float sample = voice.tick();
    // send sample to mixer
}
```

### 3.3 Fracture Sound

```cpp
#include <dhvani/physical/fracture.hpp>

pebble::dhvani::physical::FractureVoice v{};
v.trigger(pebble::dhvani::physical::presets::glass().params, {.force=1.f}, 44100u);
while (v.is_active()) { float s = v.tick(); }
```

### 3.4 Friction & Tear

```cpp
#include <dhvani/physical/surface.hpp>

pebble::dhvani::physical::FrictionVoice friction{};
friction.configure(mat, {.velocity=0.6f, .normal_force=0.8f}, 44100u);
for (int i = 0; i < block_size; ++i) float s = friction.tick();

// Update velocity dynamically (no reconfig needed)
friction.set_velocity(0.3f, mat, 44100u);

pebble::dhvani::physical::TearVoice tear{};
tear.configure(mat, /*speed=*/0.5f, 44100u);
for (int i = 0; i < block_size; ++i) float s = tear.tick();
```

### 3.5 Metallic Sound

```cpp
#include <dhvani/physical/metal.hpp>

auto res = pebble::dhvani::physical::make_metal_resonator<8>(
    pebble::dhvani::physical::MetalType::Bell, /*fundamental=*/440.f, /*force=*/1.f, 44100u);
for (int i = 0; i < 1024; ++i) float s = res.tick(44100u);
```

Available `MetalType` values: `Bell`, `Cymbal`, `Plate`, `Pipe`, `Spring`.

---

## 4. EDSL Fluent API

```cpp
#include <dhvani/sound_edsl.hpp>
using namespace pebble::dhvani;

// Render 512 PCM frames of a steel impact to a stack-allocated array
auto block = impact()
    .material(physical::presets::steel())
    .force(0.85f)
    .sample_rate(44100u)
    .render<512>();                    // → std::array<SampleFrame, 512>

// Fracture
auto crack = fracture()
    .material(physical::presets::glass().params)
    .force(1.f)
    .render<256>();

// Metal hit — specific archetype
auto bell = metal_hit(physical::MetalType::Bell)
    .fundamental(880.f)
    .force(0.7f)
    .render<1024>();

// Friction (continuous — render a block per audio callback)
auto rub = friction()
    .material(physical::presets::wood().params)
    .velocity(0.4f)
    .force(0.6f)
    .render<512>();

// Chain: just get the SoundEvent descriptor (no PCM rendered)
SoundEvent evt = impact().material(physical::presets::concrete()).force(0.5f).build();
```

---

## 5. Signal Graph

```cpp
#include <dhvani/graph/graph.hpp>
#include <dhvani/graph/builtin_nodes.hpp>
using namespace pebble::dhvani::graph;

SoundGraph g{};

OscillatorNode osc{};
osc.state = {.frequency=440.f, .amplitude=0.8f, .sample_rate=44100u};
osc.shape = pebble::dhvani::synth::WaveShape::Sine;

GainNode gain{.gain = 0.5f};

auto osc_id  = g.add_node(std::move(osc));
auto gain_id = g.add_node(std::move(gain));
g.connect(osc_id, gain_id);

std::vector<pebble::dhvani::synth::SampleFrame> output(512, {});
g.process(output, 44100u);   // evaluates all nodes in insertion order
```

Custom nodes only need two methods:

```cpp
struct MyNode {
    void process(std::span<pebble::dhvani::synth::SampleFrame> out,
                 std::span<const pebble::dhvani::synth::SampleFrame> in,
                 uint32_t sr) noexcept { /* ... */ }
    void reset() noexcept {}
};
static_assert(pebble::dhvani::graph::AudioNode<MyNode>);
```

---

## 6. Audio Backends

### NullBackend (testing / offline)

```cpp
#include <dhvani/backend/null_backend.hpp>

pebble::dhvani::backend::NullBackend nb{44100u, 4096};
nb.start([](std::span<pebble::dhvani::synth::SampleFrame> frames) {
    // Render your graph here
});
auto pcm = nb.captured(); // std::span<const SampleFrame>
```

### MiniAudioBackend (hardware playback)

Define `DHVANI_USE_MINIAUDIO` before including and define `MINIAUDIO_IMPLEMENTATION` in exactly one `.cpp`.

```cpp
// in exactly one .cpp:
#define DHVANI_USE_MINIAUDIO
#define MINIAUDIO_IMPLEMENTATION
#include <dhvani/backend/miniaudio.hpp>

pebble::dhvani::backend::MiniAudioBackend audio{44100u};
audio.start([](std::span<pebble::dhvani::synth::SampleFrame> frames) {
    // Fill frames from SoundGraph or physical voices
});
// ... audio runs on background thread until audio.stop()
```

---

## 7. Physics Engine Bridges

### Gati Collision Bridge

```cpp
#include <dhvani/gati_bridge.hpp>

pebble::dhvani::SoundBus bus{};
pebble::dhvani::GatiSoundBridge bridge{bus, 44100u, /*volume_scale=*/1.f};

// In your Gati collision callback:
bridge.on_collision({
    .material_a       = pebble::dhvani::physical::presets::steel().params,
    .material_b       = pebble::dhvani::physical::presets::concrete().params,
    .impulse_magnitude = 0.8f,
    .relative_velocity = 0.f
});
// Enqueues "__dhvani_impact__" or "__dhvani_fracture__" cue to bus

bridge.on_friction({
    .material_a = pebble::dhvani::physical::presets::wood().params,
    .relative_velocity = 0.4f
});
```

### Prakriti Material Bridge

```cpp
#include <dhvani/prakriti_bridge.hpp>

// Map Prakriti particle state to acoustic material
auto mat = pebble::dhvani::from_prakriti_material(
    /*density_norm=*/0.9f,
    /*temperature_norm=*/0.1f
);
// mat.stiffness is high (solid, cold) → use with ImpactVoice
```

---

## 8. Spandana Timeline Integration (existing)

```cpp
#include <dhvani/edsl.hpp>

timeline.add(
    tween(position).to({100, 0}),
    dhvani::edsl::audio_cue(sound_bus, "dash_woosh.wav").pitch(1.2f).volume(0.8f)
);
```

---

## 9. File Reference

| Header | Contents |
|---|---|
| `dhvani/dhvani.hpp` | `SoundBus`, `SoundCue` |
| `dhvani/spatial.hpp` | `AudioListener2D`, `compute_spatial_audio()` |
| `dhvani/edsl.hpp` | `audio_cue()` for Spandana timelines |
| `dhvani/sound_edsl.hpp` | `SoundBuilder` EDSL, `impact()`, `fracture()`, `friction()`, `tear()`, `metal_hit()` |
| `dhvani/gati_bridge.hpp` | `GatiSoundBridge`, `CollisionSoundEvent` |
| `dhvani/prakriti_bridge.hpp` | `from_prakriti_material()` |
| `dhvani/synth/buffer.hpp` | `Sample`, `SampleFrame`, `SampleBlock<N>` |
| `dhvani/synth/waveform.hpp` | `OscillatorState`, `WaveShape`, `tick()`, `fill_block()` |
| `dhvani/synth/envelope.hpp` | `ADSRParams`, `EnvelopeState`, `trigger()`, `tick()`, `done()` |
| `dhvani/synth/filter.hpp` | `BiquadCoeffs`, `BiquadState`, `make_biquad<Tag>()`, `process()` |
| `dhvani/synth/resonator.hpp` | `KarplusStrong<N>`, `ModalResonator<Modes>` |
| `dhvani/physical/material.hpp` | `MaterialParams`, `PhysicalMaterial` concept, `MaterialPreset`, `presets::*` |
| `dhvani/physical/impact.hpp` | `ImpactVoice<Modes>`, `make_impact_resonator()` |
| `dhvani/physical/fracture.hpp` | `FractureVoice` |
| `dhvani/physical/surface.hpp` | `FrictionVoice`, `TearVoice` |
| `dhvani/physical/metal.hpp` | `MetalType`, `make_metal_resonator()` |
| `dhvani/graph/node.hpp` | `AudioNode` concept, `NodeId`, `NodeConnection` |
| `dhvani/graph/builtin_nodes.hpp` | `OscillatorNode`, `GainNode`, `LowPassNode`, `MixerNode`, `EnvelopeNode` |
| `dhvani/graph/graph.hpp` | `SoundGraph` |
| `dhvani/backend/backend.hpp` | `AudioBackend` concept |
| `dhvani/backend/null_backend.hpp` | `NullBackend` |
| `dhvani/backend/miniaudio.hpp` | `MiniAudioBackend` |
