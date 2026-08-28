#pragma once
// ============================================================================
// gati/reactive_cues.hpp — Impact-Sensitive Reactive Collision Cue Triggers
// ============================================================================
// Dispatches visual, particle, audio, and camera shakes in response to
// Gati ContactEvents based on penetration depth and impulse thresholds.
// ============================================================================

#include "event.hpp"
#include "spandana/edsl/spandana_edsl.hpp"
#include <functional>
#include <vector>

#if __has_include("dhvani/dhvani.hpp")
#include "dhvani/dhvani.hpp"
#include "dhvani/spatial.hpp"
#endif

namespace gati {

struct ImpactCueTrigger {
    float                                          min_depth = 0.1f;
    std::function<void(const ContactEvent&)>       handler;
};

class ReactiveCueManager {
public:
    ReactiveCueManager() = default;

    void on_impact(float min_depth, std::function<void(const ContactEvent&)> handler) {
        triggers_.push_back(ImpactCueTrigger{
            .min_depth = min_depth,
            .handler = std::move(handler)
        });
    }

    void process_events(EventBus& bus) {
        bus.drain<ContactEvent>([&](const ContactEvent& ce) {
            for (const auto& trig : triggers_) {
                if (ce.depth >= trig.min_depth) {
                    if (trig.handler) trig.handler(ce);
                }
            }
        });
    }

private:
    std::vector<ImpactCueTrigger> triggers_;
};

#if __has_include("dhvani/dhvani.hpp")
// ECS Audio Emitter Component
struct AudioEmitter {
    std::string_view name;
    float            volume = 1.0f;
    float            pitch = 1.0f;
    bool             trigger_play = false;
    bool             is_spatial = true;
};

// ECS Audio Listener Component
struct AudioListener {
    pebble::dhvani::AudioListener2D listener;
};

// Spatial Audio Dispatch System
struct SpatialAudioSystem {
    void run(World& world, StepContext&, pebble::dhvani::SoundBus& sound_bus) {
        // 1. Locate active listener in world if available
        pebble::dhvani::AudioListener2D active_listener;
        bool has_listener = false;
        world.view<AudioListener>([&](Entity, AudioListener& al) {
            if (!has_listener) {
                active_listener = al.listener;
                has_listener = true;
            }
        });

        // 2. Dispatch audio cues from emitters
        world.view<Transform, AudioEmitter>([&](Entity, Transform& tr, AudioEmitter& ae) {
            if (ae.trigger_play) {
                if (ae.is_spatial && has_listener) {
                    sound_bus.play_spatial(ae.name, tr.position, active_listener, ae.volume, ae.pitch);
                } else {
                    sound_bus.play(ae.name, ae.volume, ae.pitch);
                }
                ae.trigger_play = false; // Reset trigger
            }
        });
    }
};
#endif

} // namespace gati
