#pragma once
// ============================================================================
// src/examples/example_prakriti.hpp — Prakriti physics engine demo.
// A steel bar swings under gravity; a hot fluid column breaks and spreads.
// Call prakriti_demo() from a driver; header-only so it never defines main().
// Directly reuses pebble::math::vec2.
// ============================================================================
#include "prakriti/prakriti.hpp"
#include <containers/numeric/math_vector.hpp>
#include <cstdio>

inline void prakriti_demo() {
    using namespace prakriti;

    WorldConfig cfg;
    cfg.bounds   = {{-50.0f, 0.0f}, {50.0f, 100.0f}}; // floor at y=0
    cfg.cell_size = 1.0f;
    World<> w(cfg);
    w.thermal().cfg.enabled = false;

    const auto steel = w.materials().add(MaterialRegistry::steel());
    const auto water = w.materials().add(MaterialRegistry::water());

    // Pinned steel bar of 6 particles hanging from a static anchor.
    Index prev = w.particles().add({.position = {0.0f, 40.0f}, .mass = 0, .material = steel});
    for (int i = 1; i < 6; ++i) {
        Index cur = w.particles().add({.position = {Scalar(i), 40.0f}, .material = steel});
        w.edges().add(prev, cur, 1.0f);
        prev = cur;
    }

    // A 6x6 liquid block that will splash on the floor.
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < 6; ++j)
            w.particles().add({.position = {Scalar(i) * 0.5f - 10.0f, Scalar(j) * 0.5f + 5.0f},
                               .temperature = 50, .material = water,
                               .f_solid = 0, .f_liquid = 1});

    std::printf("Prakriti demo: %u particles, %u bonds\n",
                w.particles().size(), w.edges().size());

    for (int frame = 0; frame < 120; ++frame) {
        w.step();
        if (frame % 30 == 0)
            std::printf("  frame %3d: KE=%.3f  bonds=%u\n",
                        frame, w.kinetic_energy(), w.edges().size());
    }
    std::printf("Prakriti demo done.\n");
}
