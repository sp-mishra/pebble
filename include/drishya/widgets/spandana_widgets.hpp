#pragma once
// ============================================================================
// drishya/widgets/spandana_widgets.hpp — Spandana-powered widget wrappers
// ----------------------------------------------------------------------------
// Embeds spandana::Timeline per widget for tween/spring/particle animations
// directly inside Drishya's retained widget tree.
//
// Three concrete wrappers:
//   AnimatedLabel    — Label with FloatTweenAction fade on alpha (fade in/out).
//   ParticleOverlay  — Burst of up to 64 particles via ParticleBurstAction.
//   SpandanaWidget<W>— Generic adapter: wraps any W : WidgetBase, holds a
//                      Timeline, applies animated offset/alpha to its paint.
//
// All types remain plain value types (no virtual, no RTTI, no macros).
// spandana is expected on the include path — guard silently skips if absent.
// ============================================================================

#if __has_include("spandana/spandana.hpp")

#include "drishya/widgets/base.hpp"
#include "spandana/spandana.hpp"
#include "spandana/edsl/motion_edsl.hpp"
#include "spandana/edsl/particle_edsl.hpp"
#include "containers/static/static_vector.hpp"

#include <cstdint>
#include <cmath>

namespace pebble::drishya::widgets {
    // ============================================================================
    // AnimatedLabel — label text with a per-frame fade-in/out tween.
    // ============================================================================
    struct AnimatedLabel : WidgetBase {
        std::string text;
        float font_size = 16.0f;
        std::uint32_t base_color = 0xFFFFFFFFu; // opaque white
        float alpha = 1.0f; // driven by timeline

        spandana::Timeline timeline{};

        explicit AnimatedLabel(std::string_view t = {}, float fs = 16.0f,
                               std::uint32_t color = 0xFFFFFFFFu)
            : text(t), font_size(fs), base_color(color) {
            // Prime with a default fade-in from 0 → 1.
            using namespace spandana::edsl;
            timeline.add(tween(alpha).to(1.0f, 0.35f).ease(spandana::ease::InOutQuad{}));
            alpha = 0.0f;
            timeline.reset();
            // Replay so the timeline starts with alpha driving 0→1 on first update.
            timeline.add(tween(alpha).to(1.0f, 0.35f).ease(spandana::ease::InOutQuad{}));
        }

        // Advance the timeline by dt seconds; call from App's tick.
        void tick(float dt) { timeline.update(dt); }

        // Trigger a fade-in from zero.
        void fade_in(float duration = 0.35f) {
            using namespace spandana::edsl;
            alpha = 0.0f;
            timeline.reset();
            timeline.add(tween(alpha).to(1.0f, duration).ease(spandana::ease::InOutQuad{}));
        }

        // Trigger a fade-out to zero then back (one-shot pulse).
        void fade_out(float duration = 0.35f) {
            using namespace spandana::edsl;
            timeline.reset();
            timeline.add(tween(alpha).to(0.0f, duration).ease(spandana::ease::InOutQuad{}));
        }

        template <ITextMetrics Metrics>
        [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>& ctx) const {
            return ctx.text.measure(text.c_str(), 0.0f);
        }

        template <typename P>
            requires Painter<P>
        void paint(P& painter, Rect2D box) const {
            const float a_byte = std::clamp(alpha, 0.0f, 1.0f);
            const std::uint32_t a = static_cast<std::uint32_t>(a_byte * 255.0f);
            const std::uint32_t c = (base_color & 0x00FFFFFFu) | (a << 24);
            painter.set_color(c);
            painter.text(std::string_view{text}, Vec2{box.x + 4.0f, box.y + box.h * 0.5f}, font_size);
        }
    };

    // ============================================================================
    // ParticleOverlay — burst of particles at a point, driven by Spandana.
    // ParticleBurstAction writes into the static_vector; tick integrates gravity.
    // ============================================================================
    struct ParticleOverlay : WidgetBase {
        static constexpr std::size_t MaxParticles = 64;

        using ParticleVec = containers::static_vector<spandana::edsl::Particle, MaxParticles>;

        ParticleVec particles{};
        spandana::Timeline timeline{};
        float gravity = 120.0f; // px/s² downward

        explicit ParticleOverlay() {
            using akruti::layout::SizeSpec;
            style_.width = SizeSpec::Px(0.0f); // overlay: zero natural size
            style_.height = SizeSpec::Px(0.0f);
        }

        // Trigger a burst at (px, py) in parent-relative coordinates.
        void burst(float px, float py,
                   float speed_min = 40.0f, float speed_max = 120.0f,
                   float lifetime_s = 1.0f, std::uint32_t seed = 1337u) {
            using namespace spandana::edsl;
            timeline.reset();
            timeline.add(
                particle_burst(particles)
                .at({px, py})
                .count(MaxParticles)
                .speed(speed_min, speed_max)
                .seed(seed)
                .lifetime(lifetime_s)
            );
        }

        void tick(float dt) {
            timeline.update(dt);
            // Apply gravity per particle (timeline action updates position/age already,
            // so we only add the gravitational y component here).
            for (auto& p : particles)
                p.velocity[1] += gravity * dt;
        }

        template <typename P>
            requires Painter<P>
        void paint(P& painter, Rect2D /*box*/) const {
            for (const auto& p : particles) {
                if (p.age >= p.lifetime) continue;
                const float life_frac = 1.0f - (p.age / p.lifetime);
                const std::uint32_t a = static_cast<std::uint32_t>(life_frac * 255.0f);
                painter.set_color(0x00FF8040u | (a << 24));
                const Rect2D dot{
                    p.position[0] - p.size * 0.5f,
                    p.position[1] - p.size * 0.5f,
                    p.size, p.size
                };
                painter.fill_rect(dot);
            }
        }
    };

    // ============================================================================
    // SpandanaWidget<W> — generic wrapper: adds Timeline-driven offset + alpha
    // on top of any W : WidgetBase. Keeps W's style/measure/event pass-through.
    // ============================================================================
    template <typename W>
        requires std::is_base_of_v<WidgetBase, W>
    struct SpandanaWidget : WidgetBase {
        W inner{};
        spandana::Timeline timeline{};

        // Animated state — timeline tween actions bind directly to these floats.
        float offset_x = 0.0f;
        float offset_y = 0.0f;
        float alpha = 1.0f;

        explicit SpandanaWidget() = default;
        explicit SpandanaWidget(W w) : inner(std::move(w)) {}

        [[nodiscard]] LayoutStyle style() const noexcept { return inner.style(); }

        template <ITextMetrics Metrics>
        [[nodiscard]] Size2D measure(const MeasureCtxT<Metrics>& ctx) const {
            return inner.measure(ctx);
        }

        [[nodiscard]] EventResult on_event(EventCtx& ec) noexcept {
            return inner.on_event(ec);
        }

        void tick(float dt) { timeline.update(dt); }

        template <typename P>
            requires Painter<P>
        void paint(P& painter, Rect2D box) const {
            const Rect2D shifted{box.x + offset_x, box.y + offset_y, box.w, box.h};
            painter.push_clip(Bounds2D(
                akruti::layout::Vec2{shifted.x, shifted.y},
                akruti::layout::Vec2{shifted.x + shifted.w, shifted.y + shifted.h}
            ));
            inner.paint(painter, shifted);
            painter.pop_clip();
        }

        // Slide in from below over `duration` seconds.
        void slide_in(float distance = 40.0f, float duration = 0.4f) {
            using namespace spandana::edsl;
            offset_y = distance;
            timeline.reset();
            timeline.add(tween(offset_y).to(0.0f, duration).ease(spandana::ease::InOutQuad{}));
        }

        // Fade alpha 1→low→1 (pulse effect).
        void pulse(float low = 0.5f, float half_dur = 0.15f) {
            using namespace spandana::edsl;
            timeline.reset();
            // Two serial tweens: use separate add() calls — dependency inference
            // chains them on the same property key automatically.
            timeline.add(tween(alpha).to(low, half_dur).ease(spandana::ease::InOutQuad{}));
            timeline.add(tween(alpha).to(1.0f, half_dur).ease(spandana::ease::InOutQuad{}));
        }
    };
} // namespace pebble::drishya::widgets

#endif // __has_include("spandana/spandana.hpp")
