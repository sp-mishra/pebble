#pragma once
// ============================================================================
// gati/anim.hpp — High-Performance Keyframed Animation Subsystem
// ============================================================================
// Zero-allocation sampling, Catmull-Rom cubic splines, clip state machines.
// ============================================================================

#include "math.hpp"
#include "ecs.hpp"
#include "transform.hpp"
#include "system.hpp"
#include "containers/static/static_vector.hpp"

#include <cstddef>
#include <cstdint>
#include <cmath>

namespace gati {

enum class Interp : std::uint8_t { Step, Linear, Cubic };

[[nodiscard]] inline Scalar blend(Scalar a, Scalar b, Scalar t) noexcept {
    return lerp(a, b, t);
}

[[nodiscard]] inline Vec2 blend(const Vec2& a, const Vec2& b, Scalar t) noexcept {
    return lerp(a, b, t);
}

template <typename T, std::size_t N = 16>
struct Curve {
    struct Key {
        Scalar time;
        T      value;
        Interp interp = Interp::Linear;
    };
    containers::static_vector<Key, N> keys;

    void add(Scalar t, T v, Interp i = Interp::Linear) {
        (void)keys.push_back({t, v, i});
    }

    [[nodiscard]] T sample(Scalar t) const noexcept {
        const std::size_t n = keys.size();
        if (n == 0) return T{};
        if (t <= keys[0].time || n == 1) return keys[0].value;
        if (t >= keys[n - 1].time)       return keys[n - 1].value;

        // Binary search for segment [lo, lo + 1)
        std::size_t lo = 0, hi = n - 1;
        while (hi - lo > 1) {
            const std::size_t mid = (lo + hi) >> 1;
            (keys[mid].time <= t ? lo : hi) = mid;
        }
        const Key& k0 = keys[lo];
        const Key& k1 = keys[lo + 1];
        const Scalar span = k1.time - k0.time;
        const Scalar u = span > Scalar(1e-9) ? (t - k0.time) / span : Scalar(0);

        switch (k0.interp) {
            case Interp::Step:   return k0.value;
            case Interp::Linear: return blend(k0.value, k1.value, u);
            case Interp::Cubic: {
                const T p0 = lo > 0 ? keys[lo - 1].value : k0.value;
                const T p3 = (lo + 2) < n ? keys[lo + 2].value : k1.value;
                const Scalar u2 = u * u, u3 = u2 * u;
                return catmull(p0, k0.value, k1.value, p3, u, u2, u3);
            }
        }
        return k0.value;
    }

private:
    [[nodiscard]] static T catmull(T p0, T p1, T p2, T p3,
                                   Scalar u, Scalar u2, Scalar u3) noexcept {
        const T c0 = p1 * Scalar(2);
        const T c1 = (p2 - p0) * u;
        const T c2 = (p0 * Scalar(2) - p1 * Scalar(5) + p2 * Scalar(4) - p3) * u2;
        const T c3 = (p1 * Scalar(3) - p0 - p2 * Scalar(3) + p3) * u3;
        return (c0 + c1 + c2 + c3) * Scalar(0.5);
    }
};

enum class Channel : std::uint8_t { PosX, PosY, Angle, ScaleX, ScaleY };

struct TrackScalar {
    Channel       channel;
    Curve<Scalar> curve;
};

struct Clip {
    containers::static_vector<TrackScalar, 8> tracks;
    Scalar duration = 1.0f;
    bool   loop = true;

    void sample_into(Transform& tr, Scalar time) const noexcept {
        const Scalar t = (loop && duration > Scalar(1e-9))
                       ? time - duration * std::floor(time / duration)
                       : time;
        for (const auto& trk : tracks) {
            const Scalar v = trk.curve.sample(t);
            switch (trk.channel) {
                case Channel::PosX:   tr.position[0] = v; break;
                case Channel::PosY:   tr.position[1] = v; break;
                case Channel::Angle:  tr.angle       = v; break;
                case Channel::ScaleX: tr.scale[0]    = v; break;
                case Channel::ScaleY: tr.scale[1]    = v; break;
            }
        }
    }
};

struct Animator {
    const Clip* clip = nullptr;
    Scalar      time = 0.0f;
    Scalar      speed = 1.0f;
    Scalar      weight = 1.0f;
    bool        playing = true;
};

// System: samples animators into transforms (optionally parallelized via Pravaha)
struct AnimationSystem {
    void run(World& w, StepContext ctx) {
        w.par_view<Animator, Transform>(ctx.executor, [&](Entity, Animator& a, Transform& tr) {
            if (!a.playing || !a.clip) return;
            a.time += ctx.dt * a.speed;
            a.clip->sample_into(tr, a.time);
        });
    }
};

// StateMachine for cross-fading clips
template <std::size_t NStates = 8, std::size_t NTrans = 16>
struct StateMachine {
    using Predicate = bool (*)(const World&, Entity);
    struct State { const Clip* clip; };
    struct Transition {
        std::uint8_t from;
        std::uint8_t to;
        Predicate    when;
        Scalar       blend_time;
    };

    containers::static_vector<State, NStates>      states;
    containers::static_vector<Transition, NTrans>  transitions;

    std::uint8_t current = 0;
    std::uint8_t blending_to = 0;
    Scalar       blend = 0.0f;
    Scalar       blend_time = 0.0f;
    Scalar       time = 0.0f;
    bool         in_transition = false;

    std::uint8_t add_state(const Clip* c) {
        (void)states.push_back({c});
        return static_cast<std::uint8_t>(states.size() - 1);
    }

    void add_transition(std::uint8_t from, std::uint8_t to, Predicate when, Scalar bt) {
        (void)transitions.push_back({from, to, when, bt});
    }

    void update(const World& w, Entity e, Transform& tr, Scalar dt) {
        time += dt;
        if (!in_transition) {
            for (const auto& t : transitions) {
                if (t.from == current && t.when && t.when(w, e)) {
                    blending_to = t.to;
                    blend_time = t.blend_time;
                    blend = 0.0f;
                    in_transition = t.blend_time > Scalar(1e-6);
                    if (!in_transition) {
                        current = t.to;
                        time = 0.0f;
                    }
                    break;
                }
            }
        }
        if (in_transition) {
            blend += dt / blend_time;
            if (blend >= Scalar(1)) {
                current = blending_to;
                blend = 0.0f;
                in_transition = false;
            }
        }

        const Clip* a = states[current].clip;
        if (in_transition) {
            Transform ta = tr, tb = tr;
            if (a) a->sample_into(ta, time);
            if (const Clip* b = states[blending_to].clip) b->sample_into(tb, time);
            tr.position = lerp(ta.position, tb.position, blend);
            tr.angle    = angle_lerp(ta.angle, tb.angle, blend);
            tr.scale    = lerp(ta.scale, tb.scale, blend);
        } else if (a) {
            a->sample_into(tr, time);
        }
    }
};

} // namespace gati
