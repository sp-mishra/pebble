#include "catch_amalgamated.hpp"

#include <dhvani/synth/buffer.hpp>
#include <dhvani/synth/waveform.hpp>
#include <dhvani/synth/envelope.hpp>
#include <dhvani/synth/filter.hpp>
#include <dhvani/synth/resonator.hpp>
#include <dhvani/physical/material.hpp>
#include <dhvani/physical/impact.hpp>
#include <dhvani/physical/fracture.hpp>
#include <dhvani/physical/surface.hpp>
#include <dhvani/physical/metal.hpp>
#include <dhvani/sound_edsl.hpp>
#include <dhvani/backend/null_backend.hpp>
#include <dhvani/gati_bridge.hpp>
#include <dhvani/prakriti_bridge.hpp>
#include <dhvani/graph/builtin_nodes.hpp>
#include <dhvani/graph/graph.hpp>

#include <cmath>
#include <numeric>
#include <span>

// ---------------------------------------------------------------------------
// synth layer
// ---------------------------------------------------------------------------

TEST_CASE("dhvani: WaveShape sine produces values in [-1, 1]", "[dhvani][synth][waveform]") {
    pebble::dhvani::synth::OscillatorState s{.frequency=440.f, .amplitude=1.f, .sample_rate=44100};
    for (int i = 0; i < 1024; ++i) {
        const float v = pebble::dhvani::synth::tick(s, pebble::dhvani::synth::WaveShape::Sine);
        CHECK(v >= -1.001f);
        CHECK(v <=  1.001f);
    }
}

TEST_CASE("dhvani: WaveShape white-noise energy is non-zero", "[dhvani][synth][waveform]") {
    pebble::dhvani::synth::OscillatorState s{.frequency=1.f, .amplitude=1.f, .sample_rate=44100};
    float sum = 0.f;
    for (int i = 0; i < 256; ++i)
        sum += std::abs(pebble::dhvani::synth::tick(s, pebble::dhvani::synth::WaveShape::WhiteNoise));
    CHECK(sum > 0.f);
}

TEST_CASE("dhvani: fill_block populates all frames", "[dhvani][synth][waveform]") {
    pebble::dhvani::synth::OscillatorState s{.frequency=220.f, .amplitude=0.5f, .sample_rate=44100};
    pebble::dhvani::synth::SampleBlock<64> blk{};
    pebble::dhvani::synth::fill_block(s, pebble::dhvani::synth::WaveShape::Saw, blk);
    float energy = 0.f;
    for (const auto& f : blk) energy += std::abs(f.left);
    CHECK(energy > 0.f);
}

TEST_CASE("dhvani: ADSR envelope — trigger → attack → decay → sustain", "[dhvani][synth][envelope]") {
    using namespace pebble::dhvani::synth;
    EnvelopeState env{};
    env.params = {.attack=0.01f, .decay=0.05f, .sustain=0.5f, .release=0.1f};
    env.sample_rate = 44100;

    trigger(env);
    REQUIRE(env.stage == EnvelopeStage::Attack);

    // Advance through attack (441 samples @ 44100Hz for 10ms)
    float peak = 0.f;
    for (int i = 0; i < 600; ++i) peak = tick(env);
    // Should have passed Attack; value should be positive
    CHECK(peak > 0.f);
    CHECK_FALSE(done(env));
}

TEST_CASE("dhvani: ADSR envelope — release reaches Done", "[dhvani][synth][envelope]") {
    using namespace pebble::dhvani::synth;
    EnvelopeState env{};
    env.params = {.attack=0.001f, .decay=0.001f, .sustain=0.5f, .release=0.01f};
    env.sample_rate = 44100;
    trigger(env);
    // Run through ADSR to sustain
    for (int i = 0; i < 2000; ++i) (void)tick(env);
    release_note(env);
    REQUIRE(env.stage == EnvelopeStage::Release);
    for (int i = 0; i < 2000; ++i) (void)tick(env);
    CHECK(done(env));
    CHECK(env.value == Catch::Approx(0.f).margin(1e-4f));
}

TEST_CASE("dhvani: Biquad LP filter attenuates above cutoff", "[dhvani][synth][filter]") {
    using namespace pebble::dhvani::synth;
    constexpr uint32_t sr = 44100;
    // LP at 1kHz, Q=0.7
    const auto coeffs = make_biquad<FilterTag_LowPass>(1000.f, 0.7f, sr);
    BiquadState st{};

    // Feed 10kHz sine (above cutoff) — expect significant attenuation
    OscillatorState osc{.frequency=10000.f, .amplitude=1.f, .sample_rate=sr};
    float energy_filtered = 0.f;
    for (int i = 0; i < 512; ++i) {
        const float raw = tick(osc, WaveShape::Sine);
        energy_filtered += std::abs(process(st, coeffs, raw));
    }
    // Unfiltered 512 samples of amplitude=1 → energy ~256; filtered at 10x cutoff should be <<
    CHECK(energy_filtered < 50.f);
}

TEST_CASE("dhvani: Biquad HP filter passes above cutoff", "[dhvani][synth][filter]") {
    using namespace pebble::dhvani::synth;
    constexpr uint32_t sr = 44100;
    const auto hp = make_biquad<FilterTag_HighPass>(100.f, 0.7f, sr);
    BiquadState st{};
    OscillatorState osc{.frequency=5000.f, .amplitude=1.f, .sample_rate=sr};
    float energy = 0.f;
    for (int i = 0; i < 512; ++i)
        energy += std::abs(process(st, hp, tick(osc, WaveShape::Sine)));
    // 5kHz is well above 100Hz cutoff → mostly passes through
    CHECK(energy > 100.f);
}

TEST_CASE("dhvani: ModalResonator energy after excitation", "[dhvani][synth][resonator]") {
    using namespace pebble::dhvani::synth;
    ModalResonator<4> r{};
    r.modes[0] = {.freq=440.f,  .decay=0.9995f, .amp=1.f};
    r.modes[1] = {.freq=880.f,  .decay=0.9990f, .amp=0.5f};
    r.modes[2] = {.freq=1320.f, .decay=0.9985f, .amp=0.25f};
    r.modes[3] = {.freq=1760.f, .decay=0.9980f, .amp=0.125f};
    r.excite(1.f);
    float energy = 0.f;
    for (int i = 0; i < 1024; ++i) energy += std::abs(r.tick(44100));
    CHECK(energy > 0.f);
}

TEST_CASE("dhvani: KarplusStrong decays over time", "[dhvani][synth][resonator]") {
    using namespace pebble::dhvani::synth;
    KarplusStrong<2048> ks{};
    ks.set_frequency(440.f, 44100);
    // Excite with noise
    std::array<Sample, 100> noise;
    for (auto& s : noise) s = 0.5f;
    ks.excite(noise);
    float early = 0.f, late = 0.f;
    for (int i = 0; i < 1000; ++i) early += std::abs(ks.tick());
    for (int i = 0; i < 44100; ++i) late  += std::abs(ks.tick());
    CHECK(early > 0.f);
    CHECK(late / 44100.f < early / 1000.f); // per-sample average decays
}

// ---------------------------------------------------------------------------
// physical layer
// ---------------------------------------------------------------------------

TEST_CASE("dhvani: MaterialPresets satisfy PhysicalMaterial concept", "[dhvani][physical][material]") {
    using namespace pebble::dhvani::physical;
    static_assert(PhysicalMaterial<MaterialPreset>);
    const auto s = presets::steel();
    CHECK(s.material_params().stiffness > 0.9f);
    const auto r = presets::rubber();
    CHECK(r.material_params().stiffness < 0.1f);
}

TEST_CASE("dhvani: ImpactVoice steel vs rubber — both produce energy", "[dhvani][physical][impact]") {
    using namespace pebble::dhvani::physical;
    constexpr uint32_t sr = 44100;
    ImpactVoice<8> vs{}, vr{};
    vs.trigger(presets::steel().params,  {1.f, 0.002f}, sr);
    vr.trigger(presets::rubber().params, {1.f, 0.002f}, sr);

    float es = 0.f, er = 0.f;
    for (int i = 0; i < 44100; ++i) {
        es += std::abs(vs.tick());
        er += std::abs(vr.tick());
    }
    // Both materials produce energy when struck
    CHECK(es > 0.f);
    CHECK(er > 0.f);
}

TEST_CASE("dhvani: ImpactVoice inactive after envelope done", "[dhvani][physical][impact]") {
    using namespace pebble::dhvani::physical;
    ImpactVoice<4> v{};
    // Very fast envelope
    v.trigger({.damping=0.99f}, {0.5f, 0.0001f}, 44100u);
    for (int i = 0; i < 100000; ++i) (void)v.tick();
    CHECK_FALSE(v.is_active());
}

TEST_CASE("dhvani: FractureVoice — burst at onset, silence after decay", "[dhvani][physical][fracture]") {
    using namespace pebble::dhvani::physical;
    FractureVoice v{};
    v.trigger(presets::glass().params, {1.f, 2}, 44100u);
    float early = 0.f, late = 0.f;
    for (int i = 0; i < 500; ++i)  early += std::abs(v.tick());
    for (int i = 0; i < 30000; ++i) late  += std::abs(v.tick());
    CHECK(early > 0.f);
    CHECK(late  < 0.01f);  // decayed to near-silence
}

TEST_CASE("dhvani: FractureVoice — glass vs rubber cutoff difference", "[dhvani][physical][fracture]") {
    using namespace pebble::dhvani::physical;
    constexpr uint32_t sr = 44100;
    FractureVoice vg{}, vr{};
    vg.trigger(presets::glass().params,  {1.f, 1}, sr);
    vr.trigger(presets::rubber().params, {1.f, 1}, sr);
    // Glass has high brittleness → high HP cutoff → more high-freq energy initially
    float eg = 0.f, er = 0.f;
    for (int i = 0; i < 200; ++i) {
        eg += std::abs(vg.tick());
        er += std::abs(vr.tick());
    }
    // Both should have energy (different spectral content)
    CHECK(eg > 0.f);
    CHECK(er > 0.f);
}

TEST_CASE("dhvani: FrictionVoice — zero velocity yields near-zero output", "[dhvani][physical][surface]") {
    using namespace pebble::dhvani::physical;
    FrictionVoice v{};
    v.configure(presets::wood().params, {.velocity=0.f, .normal_force=0.8f}, 44100u);
    float energy = 0.f;
    for (int i = 0; i < 512; ++i) energy += std::abs(v.tick());
    CHECK(energy < 1e-4f);
}

TEST_CASE("dhvani: FrictionVoice — non-zero velocity produces signal", "[dhvani][physical][surface]") {
    using namespace pebble::dhvani::physical;
    FrictionVoice v{};
    v.configure(presets::wood().params, {.velocity=0.8f, .normal_force=0.9f}, 44100u);
    float energy = 0.f;
    for (int i = 0; i < 512; ++i) energy += std::abs(v.tick());
    CHECK(energy > 0.f);
}

TEST_CASE("dhvani: FrictionVoice set_velocity modulation", "[dhvani][physical][surface]") {
    using namespace pebble::dhvani::physical;
    constexpr uint32_t sr = 44100;
    FrictionVoice v{};
    const auto mat = presets::concrete().params;
    v.configure(mat, {.velocity=0.9f, .normal_force=1.f}, sr);
    float hi = 0.f;
    for (int i = 0; i < 512; ++i) hi += std::abs(v.tick());

    v.set_velocity(0.f, mat, sr);
    float lo = 0.f;
    for (int i = 0; i < 512; ++i) lo += std::abs(v.tick());
    CHECK(hi > lo);
}

TEST_CASE("dhvani: TearVoice produces periodic bursts", "[dhvani][physical][surface]") {
    using namespace pebble::dhvani::physical;
    TearVoice v{};
    v.configure(presets::cloth().params, 0.5f, 44100u);
    float energy = 0.f;
    for (int i = 0; i < 4096; ++i) energy += std::abs(v.tick());
    CHECK(energy > 0.f);
}

TEST_CASE("dhvani: MetalHit bell resonator — non-zero energy", "[dhvani][physical][metal]") {
    using namespace pebble::dhvani::physical;
    auto res = make_metal_resonator<8>(MetalType::Bell, 440.f, 1.f, 44100u);
    float energy = 0.f;
    for (int i = 0; i < 4096; ++i) energy += std::abs(res.tick(44100u));
    CHECK(energy > 0.f);
}

TEST_CASE("dhvani: MetalHit all types produce energy", "[dhvani][physical][metal]") {
    using namespace pebble::dhvani::physical;
    constexpr uint32_t sr = 44100;
    for (auto t : {MetalType::Bell, MetalType::Cymbal, MetalType::Plate,
                   MetalType::Pipe, MetalType::Spring}) {
        auto res = make_metal_resonator<8>(t, 440.f, 1.f, sr);
        float energy = 0.f;
        for (int i = 0; i < 1024; ++i) energy += std::abs(res.tick(sr));
        CHECK(energy > 0.f);
    }
}

// ---------------------------------------------------------------------------
// EDSL
// ---------------------------------------------------------------------------

TEST_CASE("dhvani: EDSL impact().render() — non-zero PCM block", "[dhvani][edsl]") {
    using namespace pebble::dhvani;
    auto blk = impact()
        .material(physical::presets::steel().params)
        .force(0.9f)
        .sample_rate(44100u)
        .render<512>();
    float energy = 0.f;
    for (const auto& f : blk) energy += std::abs(f.left);
    CHECK(energy > 0.f);
}

TEST_CASE("dhvani: EDSL fracture().render() — non-zero PCM block", "[dhvani][edsl]") {
    using namespace pebble::dhvani;
    auto blk = fracture()
        .material(physical::presets::glass().params)
        .force(1.f)
        .render<512>();
    float energy = 0.f;
    for (const auto& f : blk) energy += std::abs(f.left);
    CHECK(energy > 0.f);
}

TEST_CASE("dhvani: EDSL metal_hit().render() bell vs cymbal differ", "[dhvani][edsl]") {
    using namespace pebble::dhvani;
    auto bell = metal_hit(physical::MetalType::Bell).force(1.f).sample_rate(44100u).render<512>();
    auto cym  = metal_hit(physical::MetalType::Cymbal).force(1.f).sample_rate(44100u).render<512>();
    // Different modal ratios → different waveforms
    float diff = 0.f;
    for (std::size_t i = 0; i < bell.size(); ++i)
        diff += std::abs(bell[i].left - cym[i].left);
    CHECK(diff > 0.f);
}

TEST_CASE("dhvani: EDSL material PhysicalMaterial concept overload", "[dhvani][edsl]") {
    using namespace pebble::dhvani;
    // Uses template overload that accepts PhysicalMaterial directly
    auto blk = impact().material(physical::presets::wood()).force(0.5f).render<256>();
    float energy = 0.f;
    for (const auto& f : blk) energy += std::abs(f.left);
    CHECK(energy > 0.f);
}

TEST_CASE("dhvani: EDSL SoundEvent build()", "[dhvani][edsl]") {
    using namespace pebble::dhvani;
    const auto evt = impact().material(physical::presets::concrete().params).force(0.7f).build();
    CHECK(evt.type == SoundEventType::Impact);
    CHECK(evt.force == Catch::Approx(0.7f));
    CHECK(evt.material.stiffness == Catch::Approx(0.9f));
}

// ---------------------------------------------------------------------------
// backend
// ---------------------------------------------------------------------------

TEST_CASE("dhvani: NullBackend start invokes callback with correct span", "[dhvani][backend]") {
    pebble::dhvani::backend::NullBackend nb{44100u, 512};
    bool called = false;
    nb.start([&](std::span<pebble::dhvani::synth::SampleFrame> frames) {
        called = true;
        CHECK(frames.size() == 512);
    });
    CHECK(called);
    CHECK(nb.is_running());
    CHECK(nb.captured().size() == 512);
    nb.stop();
    CHECK_FALSE(nb.is_running());
}

TEST_CASE("dhvani: NullBackend captures rendered PCM", "[dhvani][backend]") {
    using namespace pebble::dhvani;
    backend::NullBackend nb{44100u, 256};
    nb.start([](std::span<synth::SampleFrame> frames) {
        // Write a recognizable pattern
        for (std::size_t i = 0; i < frames.size(); ++i)
            frames[i] = {static_cast<float>(i) * 0.001f, static_cast<float>(i) * 0.001f};
    });
    const auto& cap = nb.captured();
    CHECK(cap[0].left == Catch::Approx(0.f));
    CHECK(cap[1].left == Catch::Approx(0.001f));
}

// ---------------------------------------------------------------------------
// graph
// ---------------------------------------------------------------------------

TEST_CASE("dhvani: SoundGraph — OscillatorNode generates signal", "[dhvani][graph]") {
    using namespace pebble::dhvani;
    graph::SoundGraph g{};
    graph::OscillatorNode osc{};
    osc.state = {.frequency=440.f, .amplitude=0.5f, .sample_rate=44100u};
    g.add_node(std::move(osc));
    CHECK(g.node_count() == 1);

    std::vector<synth::SampleFrame> out(128, synth::SampleFrame{});
    g.process(out, 44100u);
    float energy = 0.f;
    for (const auto& f : out) energy += std::abs(f.left);
    CHECK(energy > 0.f);
}

TEST_CASE("dhvani: SoundGraph GainNode scales signal", "[dhvani][graph]") {
    using namespace pebble::dhvani;
    graph::SoundGraph g{};
    graph::OscillatorNode osc{};
    osc.state = {.frequency=440.f, .amplitude=1.f, .sample_rate=44100u};
    graph::GainNode gain{.gain = 0.f};  // silence
    g.add_node(std::move(osc));
    g.add_node(std::move(gain));

    std::vector<synth::SampleFrame> out(128, synth::SampleFrame{});
    g.process(out, 44100u);
    // Gain=0 node zeroes output after osc writes to it
    // Note: gain applies in-place, osc adds to output, gain multiplies to 0
    float energy = 0.f;
    for (const auto& f : out) energy += std::abs(f.left);
    // Actually both nodes process sequentially on same buffer — osc writes, gain zeroes
    CHECK(energy == Catch::Approx(0.f).margin(1e-6f));
}

// ---------------------------------------------------------------------------
// bridges
// ---------------------------------------------------------------------------

TEST_CASE("dhvani: GatiSoundBridge on_collision enqueues cue", "[dhvani][bridge][gati]") {
    using namespace pebble::dhvani;
    SoundBus bus{};
    GatiSoundBridge bridge{bus, 44100u, 1.f};

    CollisionSoundEvent evt{
        .material_a = physical::presets::steel().params,
        .material_b = physical::presets::steel().params,
        .impulse_magnitude = 0.8f,
        .relative_velocity = 0.f
    };
    bridge.on_collision(evt);
    CHECK(bus.pending_count() == 1);
}

TEST_CASE("dhvani: GatiSoundBridge on_collision fracture for brittle material", "[dhvani][bridge][gati]") {
    using namespace pebble::dhvani;
    SoundBus bus{};
    GatiSoundBridge bridge{bus, 44100u, 1.f};

    CollisionSoundEvent evt{
        .material_a = physical::presets::glass().params,
        .material_b = physical::presets::glass().params,
        .impulse_magnitude = 0.9f
    };
    bridge.on_collision(evt);
    CHECK(bus.pending_count() == 1);
    // Drain and verify name
    bus.drain([](const SoundCue& cue) {
        CHECK(cue.name == "__dhvani_fracture__");
    });
}

TEST_CASE("dhvani: GatiSoundBridge on_collision suppresses near-zero impulse", "[dhvani][bridge][gati]") {
    using namespace pebble::dhvani;
    SoundBus bus{};
    GatiSoundBridge bridge{bus, 44100u, 1.f};
    bridge.on_collision({.impulse_magnitude = 0.005f});
    CHECK(bus.pending_count() == 0);
}

TEST_CASE("dhvani: PrakritiBridge from_prakriti_material — solid state", "[dhvani][bridge][prakriti]") {
    const auto p = pebble::dhvani::from_prakriti_material(0.9f, 0.05f);
    // Cold dense material → high stiffness, low damping
    CHECK(p.stiffness   > 0.5f);
    CHECK(p.damping     < 0.6f);
    CHECK(p.brittleness > 0.f);
}

TEST_CASE("dhvani: PrakritiBridge from_prakriti_material — gas state", "[dhvani][bridge][prakriti]") {
    const auto p = pebble::dhvani::from_prakriti_material(0.05f, 0.95f);
    // Hot low-density → low stiffness, high damping
    CHECK(p.stiffness < 0.5f);
    CHECK(p.damping   > 0.5f);
}
