#pragma once
// ============================================================================
// spandana/skeleton.hpp — 2D Bone Hierarchy & Linear Blend Skinning Engine
// ============================================================================
// Zero-virtual, cache-friendly forward kinematics and vertex skinning deformation.
// ============================================================================

#include "containers/numeric/math_vector.hpp"
#include "containers/static/static_vector.hpp"
#include <vector>
#include <cmath>
#include <string_view>

namespace pebble::spandana {
    // 2D Transform (Position + Rotation Angle in Radians)
    struct BoneTransform2D {
        pebble::math::vec2 position{0.0f, 0.0f};
        float rotation = 0.0f; // radians
        pebble::math::vec2 scale{1.0f, 1.0f};

        [[nodiscard]] pebble::math::vec2 transform_point(const pebble::math::vec2& p) const noexcept {
            const float c = std::cos(rotation);
            const float s = std::sin(rotation);
            const float sx = p[0] * scale[0];
            const float sy = p[1] * scale[1];
            return pebble::math::vec2(position[0] + (c * sx - s * sy),
                                      position[1] + (s * sx + c * sy));
        }

        [[nodiscard]] BoneTransform2D combine(const BoneTransform2D& child) const noexcept {
            const float c = std::cos(rotation);
            const float s = std::sin(rotation);
            const float child_x = child.position[0] * scale[0];
            const float child_y = child.position[1] * scale[1];

            return BoneTransform2D{
                .position = pebble::math::vec2(position[0] + (c * child_x - s * child_y),
                                               position[1] + (s * child_x + c * child_y)),
                .rotation = rotation + child.rotation,
                .scale = pebble::math::vec2(scale[0] * child.scale[0], scale[1] * child.scale[1])
            };
        }
    };

    struct Bone2D {
        std::string_view name;
        int parent_index = -1; // -1 for root bone
        BoneTransform2D local_transform;
        BoneTransform2D world_transform;
        BoneTransform2D bind_pose_inv;
        float length = 20.0f;
    };

    // Skinning vertex weight link
    struct SkinWeight2D {
        std::uint32_t bone_index = 0;
        float weight = 1.0f;
    };

    struct SkinnedVertex2D {
        pebble::math::vec2 bind_pos{0.0f, 0.0f};
        containers::static_vector<SkinWeight2D, 4> weights;
    };

    class Skeleton2D {
    public:
        Skeleton2D() = default;

        int add_bone(std::string_view name, int parent_index, BoneTransform2D local_tf, float length = 20.0f) {
            const int idx = static_cast<int>(bones_.size());
            bones_.push_back(Bone2D{
                .name = name,
                .parent_index = parent_index,
                .local_transform = local_tf,
                .world_transform = local_tf,
                .bind_pose_inv = local_tf,
                .length = length
            });
            return idx;
        }

        void set_bone_rotation(int index, float angle_rad) noexcept {
            if (index >= 0 && index < static_cast<int>(bones_.size())) {
                bones_[index].local_transform.rotation = angle_rad;
            }
        }

        void set_bone_position(int index, const pebble::math::vec2& pos) noexcept {
            if (index >= 0 && index < static_cast<int>(bones_.size())) {
                bones_[index].local_transform.position = pos;
            }
        }

        // Forward Kinematics (FK) Hierarchy Update
        void update_fk() noexcept {
            for (std::size_t i = 0; i < bones_.size(); ++i) {
                auto& b = bones_[i];
                if (b.parent_index >= 0 && b.parent_index < static_cast<int>(i)) {
                    b.world_transform = bones_[b.parent_index].world_transform.combine(b.local_transform);
                }
                else {
                    b.world_transform = b.local_transform;
                }
            }
        }

        // Linear Blend Skinning (LBS) for vertices
        [[nodiscard]] pebble::math::vec2 skin_vertex(const SkinnedVertex2D& v) const noexcept {
            if (v.weights.empty()) return v.bind_pos;

            float out_x = 0.0f;
            float out_y = 0.0f;
            float total_w = 0.0f;

            for (const auto& w : v.weights) {
                if (w.bone_index < bones_.size()) {
                    const auto& bone = bones_[w.bone_index];
                    const auto skinned_pt = bone.world_transform.transform_point(v.bind_pos);
                    out_x += skinned_pt[0] * w.weight;
                    out_y += skinned_pt[1] * w.weight;
                    total_w += w.weight;
                }
            }

            if (total_w > 0.0f) {
                return pebble::math::vec2(out_x / total_w, out_y / total_w);
            }
            return v.bind_pos;
        }

        [[nodiscard]] const std::vector<Bone2D>& bones() const noexcept { return bones_; }
        [[nodiscard]] std::size_t bone_count() const noexcept { return bones_.size(); }

    private:
        std::vector<Bone2D> bones_;
    };
} // namespace pebble::spandana
