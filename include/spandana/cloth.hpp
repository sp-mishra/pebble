#pragma once
// ============================================================================
// spandana/cloth.hpp — 2D Verlet Soft-Body Cloth, Hair & Cape Dynamics
// ============================================================================
// Lightweight secondary dynamics for moving entities:
//   - Fast Verlet integration with distance constraints
//   - Aerodynamic drag and gravity
//   - Anchored to parent Transform
// ============================================================================

#include "containers/static/static_vector.hpp"
#include "containers/numeric/math_vector.hpp"
#include "akruti/primitives.hpp"
#include <algorithm>
#include <cmath>

namespace pebble::spandana {

struct VerletParticle2D {
    pebble::math::vec2 pos{};
    pebble::math::vec2 prev_pos{};
    bool               pinned = false;
};

class VerletCloth2D {
public:
    explicit VerletCloth2D(std::size_t segments = 8, float segment_length = 6.0f,
                           pebble::math::vec2 gravity = {0.0f, -98.0f}, float drag = 0.02f)
        : segment_length_(segment_length), gravity_(gravity), drag_(drag) {
        for (std::size_t i = 0; i <= segments && i < 32; ++i) {
            pebble::math::vec2 p(0.0f, -static_cast<float>(i) * segment_length);
            (void)particles_.push_back(VerletParticle2D{
                .pos = p,
                .prev_pos = p,
                .pinned = (i == 0) // Root particle is pinned to anchor
            });
        }
    }

    void set_anchor(const pebble::math::vec2& anchor_pos) noexcept {
        if (!particles_.empty()) {
            particles_[0].pos = anchor_pos;
            particles_[0].prev_pos = anchor_pos;
        }
    }

    void update(const pebble::math::vec2& anchor_pos, float dt, std::size_t constraint_iterations = 4) noexcept {
        if (particles_.empty()) return;

        particles_[0].pos = anchor_pos;

        // 1. Verlet Integration
        const float dt_sq = dt * dt;
        for (std::size_t i = 1; i < particles_.size(); ++i) {
            auto& p = particles_[i];
            const pebble::math::vec2 vel = (p.pos - p.prev_pos) * (1.0f - drag_);
            p.prev_pos = p.pos;
            p.pos = p.pos + vel + gravity_ * dt_sq;
        }

        // 2. Distance Constraint Relaxation
        for (std::size_t iter = 0; iter < constraint_iterations; ++iter) {
            for (std::size_t i = 0; i + 1 < particles_.size(); ++i) {
                auto& p1 = particles_[i];
                auto& p2 = particles_[i + 1];

                const pebble::math::vec2 delta = p2.pos - p1.pos;
                const float dist_sq = delta[0] * delta[0] + delta[1] * delta[1];
                const float dist = std::sqrt(dist_sq);
                if (dist > 1e-6f) {
                    const float diff = (dist - segment_length_) / dist;
                    const pebble::math::vec2 corr = delta * (0.5f * diff);

                    if (!p1.pinned) p1.pos = p1.pos + corr;
                    if (!p2.pinned) p2.pos = p2.pos - corr;
                }
            }
        }
    }

    [[nodiscard]] const containers::static_vector<VerletParticle2D, 32>& particles() const noexcept {
        return particles_;
    }

    // Convert verlet particles into an Akruti ChainShape for terrain/rope collision
    template <std::size_t N = 32>
    [[nodiscard]] auto to_chain(float radius = 0.5f) const noexcept {
        akruti::ChainShape<N> chain;
        chain.radius = radius;
        chain.is_loop = false;
        for (const auto& pt : particles_) {
            (void)chain.verts.push_back(akruti::Vec(pt.pos));
        }
        return chain;
    }

private:
    float                                           segment_length_ = 6.0f;
    pebble::math::vec2                              gravity_{0.0f, -98.0f};
    float                                           drag_ = 0.02f;
    containers::static_vector<VerletParticle2D, 32> particles_;
};

} // namespace pebble::spandana
