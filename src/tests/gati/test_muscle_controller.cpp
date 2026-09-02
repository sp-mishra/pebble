#include "catch_amalgamated.hpp"
#include "gati/gati.hpp"
#include "prakriti/prakriti.hpp"

TEST_CASE (
"Gati: MuscleActivationSystem integrates activation toward excitation"
,
"[gati][muscle][activation]"
)
 {
    gati::World world;
    const auto e = world.spawn();
    world.add<gati::MuscleController>(e, {
        .activation = 0.0f,
        .activation_time_const = 0.02f,
        .deactivation_time_const = 0.08f,
        .prakriti_constraint_id = 0,
    });
    world.add<gati::MuscleExcitation>(e, {.value = 1.0f});

    gati::EventBus bus;
    smriti::pools::LinearArena arena(1024);
    gati::ParallelExecutor exec;
    gati::StepContext ctx{1.0f / 60.0f, 1, bus, arena, exec};

    gati::MuscleActivationSystem system;
    system.run(world, ctx);

    const auto* ctrl = world.get<gati::MuscleController>(e);
    REQUIRE(ctrl != nullptr);
    REQUIRE(ctrl->activation > 0.0f);
    REQUIRE(ctrl->activation <= 1.0f);
}

TEST_CASE (
"Gati: MuscleBridgeSystem writes activation into MuscleStore"
,
"[gati][muscle][bridge]"
)
 {
    gati::World world;
    prakriti::MuscleStore<> muscles;
    const auto id = muscles.add({
        .origin = 0,
        .insertion = 1,
        .activation = 0.0f,
    });

    const auto e = world.spawn();
    world.add<gati::MuscleController>(e, {
        .activation = 0.65f,
        .activation_time_const = 0.01f,
        .deactivation_time_const = 0.04f,
        .prakriti_constraint_id = id,
    });

    gati::EventBus bus;
    smriti::pools::LinearArena arena(1024);
    gati::ParallelExecutor exec;
    gati::StepContext ctx{1.0f / 60.0f, 2, bus, arena, exec};

    gati::MuscleBridgeSystem<> bridge{&muscles};
    bridge.run(world, ctx);

    REQUIRE(muscles.activation[id] == Catch::Approx(0.65f));
}

TEST_CASE (
"Gati: Muscle systems par_view path updates all controllers"
,
"[gati][muscle][parallel]"
)
 {
    gati::World world;
    prakriti::MuscleStore<> muscles;

    constexpr std::uint32_t kCount = 16;
    for (std::uint32_t i = 0; i < kCount; ++i) {
        const auto id = muscles.add({
            .origin = 0,
            .insertion = 1,
            .activation = 0.0f,
        });
        const auto e = world.spawn();
        world.add<gati::MuscleController>(e, {
            .activation = 0.0f,
            .activation_time_const = 0.02f,
            .deactivation_time_const = 0.08f,
            .prakriti_constraint_id = id,
        });
        world.add<gati::MuscleExcitation>(e, {.value = 1.0f});
    }

    gati::EventBus bus;
    smriti::pools::LinearArena arena(2048);
    gati::ParallelExecutor exec;
    gati::StepContext ctx{1.0f / 60.0f, 3, bus, arena, exec};

    gati::MuscleActivationSystem activation;
    gati::MuscleBridgeSystem<> bridge{&muscles};
    activation.run(world, ctx);
    bridge.run(world, ctx);

    for (std::uint32_t i = 0; i < kCount; ++i) {
        REQUIRE(muscles.activation[i] > 0.0f);
        REQUIRE(muscles.activation[i] <= 1.0f);
    }
}
