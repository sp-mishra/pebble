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

} // namespace gati
