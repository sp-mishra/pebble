#pragma once
// ============================================================================
// spandana/edsl/effect_edsl.hpp — Camera Shake, Squash/Stretch & FX Directives
// ============================================================================

#include "../timeline.hpp"
#include "../procedural.hpp"
#include "../flipbook.hpp"

namespace pebble::spandana::edsl {
    // Camera Shake Action
    class CameraShakeAction {
    public:
        CameraShakeAction(ScreenShake2D& cam, float trauma, float duration, ResourceKey key)
            : cam_(cam), trauma_(trauma), duration_(duration), key_(key) {}

        void on_start() {
            cam_.add_trauma(trauma_);
        }

        void update(float, float dt) noexcept {
            cam_.update(dt);
        }

        [[nodiscard]] float duration() const noexcept { return duration_; }
        [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

    private:
        ScreenShake2D& cam_;
        float trauma_;
        float duration_;
        ResourceKey key_;
    };

    class CameraShakeBuilder {
    public:
        explicit CameraShakeBuilder(ScreenShake2D& cam, ResourceKey key = kCameraResource)
            : cam_(cam), key_(key) {}

        CameraShakeBuilder& trauma(float amount) {
            trauma_ = amount;
            return *this;
        }

        CameraShakeAction duration(float d = 0.3f) {
            return CameraShakeAction(cam_, trauma_, d, key_);
        }

    private:
        ScreenShake2D& cam_;
        float trauma_ = 0.5f;
        ResourceKey key_;
    };

    inline CameraShakeBuilder shake_camera(ScreenShake2D& cam, ResourceKey key = kCameraResource) {
        return CameraShakeBuilder(cam, key);
    }

    // Flipbook Play Action
    class FlipbookPlayAction {
    public:
        FlipbookPlayAction(SpriteAnimator& anim, const FlipbookClip* clip, ResourceKey key)
            : anim_(anim), clip_(clip), key_(key) {}

        void on_start() {
            if (clip_) anim_.play(clip_);
        }

        void update(float, float dt) noexcept {
            anim_.update(dt);
        }

        [[nodiscard]] float duration() const noexcept {
            return clip_ ? clip_->duration() : 0.0f;
        }

        [[nodiscard]] ResourceKey resource_key() const noexcept { return key_; }

    private:
        SpriteAnimator& anim_;
        const FlipbookClip* clip_;
        ResourceKey key_;
    };

    struct FlipbookBuilder {
        SpriteAnimator& anim;
        ResourceKey key;

        [[nodiscard]] FlipbookPlayAction play(const FlipbookClip* clip) const {
            return FlipbookPlayAction(anim, clip, key);
        }
    };

    inline FlipbookBuilder flipbook(SpriteAnimator& anim, ResourceKey key = kWorldResource) {
        return FlipbookBuilder{anim, key};
    }
} // namespace pebble::spandana::edsl
