# Dhvani (ध्वनि) — Lightweight Audio Cue & Spatial Sound Engine

Header-only C++23/C++26. No virtual, no macros. Zero dynamic heap allocation on audio playback paths.
Dhvani provides spatial 2D sound attenuation, stereo equal-power panning, audio listener orientation, and timeline sound cue synchronization for Spandana.

Include: `#include <dhvani/dhvani.hpp>` and `#include <dhvani/edsl.hpp>`

---

## 1. Features

1. **2D Spatial Audio (`dhvani/spatial.hpp`)**:
   - Inverse-distance logarithmic and linear rolloff attenuation.
   - Equal-power stereo panning relative to `AudioListener2D` forward orientation.
2. **SPSC `SoundBus` Queue (`dhvani/dhvani.hpp`)**:
   - Zero-allocation sound event queue decoupling simulation threads from platform audio sinks.
3. **Spandana Timeline Synchronization (`dhvani/edsl.hpp`)**:
   - Direct integration into declarative Spandana timelines:
     ```cpp
     timeline.add(
         tween(position).to({100, 0}),
         audio_cue(sound_bus, "dash_woosh.wav").pitch(1.2f).volume(0.8f)
     );
     ```

---

## 2. Quick Start Example

```cpp
#include <dhvani/dhvani.hpp>
#include <dhvani/spatial.hpp>

pebble::dhvani::SoundBus sound_bus;

// 1. Play non-spatial 2D sound cue
sound_bus.play("menu_select.wav", /*volume*/ 0.8f, /*pitch*/ 1.0f);

// 2. Play 2D spatialized audio cue relative to listener
pebble::dhvani::AudioListener2D listener{
    .position = {0.0f, 0.0f},
    .forward = {0.0f, 1.0f},
    .max_distance = 500.0f
};

sound_bus.play_spatial("explosion.wav", /*emitter_pos*/ {120.0f, 50.0f}, listener, /*volume*/ 1.0f);

// 3. Drain pending cues into audio backend (e.g. miniaudio, SDL, OpenAL)
sound_bus.drain([](const pebble::dhvani::SoundCue& cue) {
    // Send to hardware audio mixer
});
```
