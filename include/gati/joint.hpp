#pragma once
// ============================================================================
// gati/joint.hpp — Akruti + Prakriti Joint Bridge (guarded GATI_HAS_JOINTS)
// ============================================================================
// Connects entities with Akruti Joint kinematic frames, solved by Prakriti XPBD.
// ============================================================================

#include "math.hpp"
#include "ecs.hpp"
#include "system.hpp"
#include "physics.hpp"

#if defined(GATI_HAS_PRAKRITI) && defined(GATI_ENABLE_AKRUTI) && __has_include("akruti/akruti.hpp")
#define GATI_HAS_JOINTS 1
#include "akruti/joint.hpp"
#include "containers/dynamic/SmallVector.hpp"

namespace gati {

struct JointDesc {
    akruti::JointType type = akruti::JointType::Distance;
    Vec2              anchor_a{};
    Vec2              anchor_b{};
    Vec2              axis{1.0f, 0.0f};
    Scalar            rest_length = 0.0f;
    Scalar            target_angle = 0.0f;
    Scalar            compliance = 0.0f;
};

// Component: an Akruti Joint frame binding this entity's body to another
struct JointRef {
    akruti::Joint joint;
};

// Build a JointRef between two entities' BodyRef particles
[[nodiscard]] inline JointRef make_joint(World& w, Entity a, Entity b, const JointDesc& d) {
    BodyRef* ba = w.get<BodyRef>(a);
    BodyRef* bb = w.get<BodyRef>(b);
    akruti::Joint j;
    if (ba && bb) {
        switch (d.type) {
            case akruti::JointType::Distance:
                j = akruti::make_distance(ba->particle, bb->particle, {d.anchor_a[0], d.anchor_a[1]},
                                          {d.anchor_b[0], d.anchor_b[1]}, d.rest_length, d.compliance);
                break;
            case akruti::JointType::Revolute:
                j = akruti::make_revolute(ba->particle, bb->particle, {d.anchor_a[0], d.anchor_a[1]},
                                          {d.anchor_b[0], d.anchor_b[1]}, d.compliance);
                break;
            default:
                j.type = d.type;
                j.body_a = ba->particle;
                j.body_b = bb->particle;
                j.anchor_a = {d.anchor_a[0], d.anchor_a[1]};
                j.anchor_b = {d.anchor_b[0], d.anchor_b[1]};
                j.axis = {d.axis[0], d.axis[1]};
                j.rest_length = d.rest_length;
                j.target_angle = d.target_angle;
                j.compliance = d.compliance;
                break;
        }
    }
    return JointRef{j};
}

// System: gathers active joints from ECS
struct JointSystem {
    containers::dynamic::SmallVector<akruti::Joint> live;

    void run(World& w, StepContext) {
        live.clear();
        w.view<JointRef>([&](Entity, JointRef& jr) {
            live.push_back(jr.joint);
        });
    }
};

} // namespace gati
#endif // GATI_HAS_JOINTS
