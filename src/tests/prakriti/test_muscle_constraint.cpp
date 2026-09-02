#include "catch_amalgamated.hpp"
#include "prakriti/prakriti.hpp"
#include <array>
#include <cmath>

TEST_CASE (
"Prakriti: MuscleStore add keeps stable SoA rows"
,
"[prakriti][muscle][store]"
)
 {
    prakriti::MuscleStore<> store;
    const auto id = store.add({
        .origin = 1,
        .insertion = 2,
        .rest_length = 0.30f,
        .tendon_slack_length = 0.04f,
        .max_isometric_force = 500.0f,
        .activation = 0.2f,
        .pennation_angle = 0.1f,
        .optimal_fiber_length = 0.12f,
    });

    REQUIRE(id == 0);
    REQUIRE(store.size() == 1);
    REQUIRE(store.origin[id] == 1);
    REQUIRE(store.insertion[id] == 2);
    REQUIRE(store.rest_length[id] == Catch::Approx(0.30f));
}

TEST_CASE (
"Prakriti: MuscleSolver shortens an activated segment"
,
"[prakriti][muscle][solver]"
)
 {
    prakriti::ParticleStore particles;
    const auto a = particles.add({.position = {0.0f, 0.0f}, .mass = 1.0f});
    const auto b = particles.add({.position = {0.40f, 0.0f}, .mass = 1.0f});

    prakriti::MuscleStore<> store;
    store.add({
        .origin = a,
        .insertion = b,
        .rest_length = 0.25f,
        .tendon_slack_length = 0.05f,
        .max_isometric_force = 800.0f,
        .activation = 1.0f,
        .pennation_angle = 0.0f,
        .optimal_fiber_length = 0.12f,
    });

    prakriti::MuscleSolver<> solver;
    const ga::Vec2<float> before_delta{
        particles.pred_x[b] - particles.pred_x[a],
        particles.pred_y[b] - particles.pred_y[a]
    };
    const auto distance_before = std::sqrt(ga::nrm2_sq(before_delta));
    constexpr float dt = 1.0f / 120.0f;

    for (int i = 0; i < 12; ++i) {
        solver.solve_substep(store, particles, dt, 1.0f / (dt * dt));
    }

    const ga::Vec2<float> after_delta{
        particles.pred_x[b] - particles.pred_x[a],
        particles.pred_y[b] - particles.pred_y[a]
    };
    const auto distance_after = std::sqrt(ga::nrm2_sq(after_delta));
    REQUIRE(std::isfinite(distance_after));
    REQUIRE(distance_after < distance_before);
    REQUIRE(store.lambda_accum[0] != 0.0f);
}

TEST_CASE (
"Prakriti: ThreeCompartmentFatigue reduces effective activation"
,
"[prakriti][muscle][fatigue]"
)
 {
    prakriti::ThreeCompartmentFatigue::State state{
        .active_motor_units = 0.5f,
        .resting_motor_units = 0.4f,
        .fatigued_motor_units = 0.1f,
    };

    const auto effective = prakriti::ThreeCompartmentFatigue::effective_activation(state, 0.8f);
    REQUIRE(effective == Catch::Approx(0.4f));
}

TEST_CASE (
"Prakriti: HillTypeFiber batch API matches scalar API"
,
"[prakriti][muscle][fiber][batch]"
)
 {
    const std::array<float, 4> activation{0.2f, 0.5f, 0.8f, 1.0f};
    const std::array<float, 4> lce{0.08f, 0.10f, 0.12f, 0.14f};
    const std::array<float, 4> vce{-0.2f, -0.1f, 0.0f, 0.1f};
    const std::array<float, 4> lopt{0.12f, 0.12f, 0.11f, 0.13f};
    const std::array<float, 4> fmax{300.0f, 400.0f, 500.0f, 600.0f};
    const std::array<float, 4> pennation{0.0f, 0.1f, 0.15f, 0.2f};
    std::array<float, 4> batch{};

    prakriti::HillTypeFiber::compute_force_batch(
        activation, lce, vce,
        lopt, 12.0f,
        fmax, pennation,
        batch);

    for (std::size_t i = 0; i < batch.size(); ++i) {
        const auto scalar = prakriti::HillTypeFiber::compute_force(
            activation[i], lce[i], vce[i], lopt[i], 12.0f, fmax[i], pennation[i]);
        REQUIRE(batch[i] == Catch::Approx(scalar));
        REQUIRE(std::isfinite(batch[i]));
    }
}

TEST_CASE (
"Prakriti: NonlinearTendon batch API matches scalar API"
,
"[prakriti][muscle][tendon][batch]"
)
 {
    const std::array<float, 4> tendon_len{0.03f, 0.05f, 0.06f, 0.08f};
    const std::array<float, 4> slack_len{0.05f, 0.05f, 0.05f, 0.05f};
    std::array<float, 4> batch{};

    prakriti::NonlinearTendon::force_batch(tendon_len, slack_len, 35.0f, 0.03f, batch);

    for (std::size_t i = 0; i < batch.size(); ++i) {
        const auto scalar = prakriti::NonlinearTendon::force(tendon_len[i], slack_len[i], 35.0f, 0.03f);
        REQUIRE(batch[i] == Catch::Approx(scalar));
        REQUIRE(batch[i] >= 0.0f);
    }
}

TEST_CASE (
"Prakriti: LinearTendon and RigidTendon batch APIs match scalar API"
,
"[prakriti][muscle][tendon][batch]"
)
 {
    const std::array<float, 4> tendon_len{0.03f, 0.05f, 0.06f, 0.08f};
    const std::array<float, 4> slack_len{0.05f, 0.05f, 0.05f, 0.05f};
    std::array<float, 4> linear_batch{};
    std::array<float, 4> rigid_batch{};

    prakriti::LinearTendon::force_batch(tendon_len, slack_len, 35.0f, 0.03f, linear_batch);
    prakriti::RigidTendon::force_batch(tendon_len, slack_len, 35.0f, 0.03f, rigid_batch);

    for (std::size_t i = 0; i < tendon_len.size(); ++i) {
        const auto linear_scalar = prakriti::LinearTendon::force(tendon_len[i], slack_len[i], 35.0f, 0.03f);
        const auto rigid_scalar = prakriti::RigidTendon::force(tendon_len[i], slack_len[i], 35.0f, 0.03f);
        REQUIRE(linear_batch[i] == Catch::Approx(linear_scalar));
        REQUIRE(rigid_batch[i] == Catch::Approx(rigid_scalar));
    }
}
