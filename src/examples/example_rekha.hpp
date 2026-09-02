#pragma once

// src/examples/example_rekha.hpp - Rekha plotting demo rendered via Kalpana backends.

#include "rekha/rekha.hpp"
#include <cmath>
#include <cstdio>
#include <string_view>

inline int rekha_demo(std::string_view backend_flag = "capture") {
    rekha::XYSeries line("latency");
    line.stroke({kalpana::colors::blue(), 2.0f});
    for (int i = 0; i < 20; ++i) {
        const float x = static_cast<float>(i);
        const float y = 6.0f + std::sin(0.35f * x) * 1.8f;
        line.add(x, y);
    }

    rekha::Graph graph;
    graph.edges = {{0, 1, 1.0f}, {1, 2, 1.0f}, {2, 3, 1.0f}, {3, 4, 1.0f}, {4, 0, 1.0f}, {0, 2, 0.7f}};

    rekha::ForceDirectedLayout<> layout;
    layout.config().iterations = 100;
    layout.initialize(graph, 1000, 640, 42);
    layout.solve(graph, 1000, 640);

    rekha::Figure fig;
    fig.viewport({1000, 640, {60.0f, 20.0f, 20.0f, 40.0f}})
       .axes({"sample", "ms", 6})
       .add(rekha::LinePlot{line})
       .add(rekha::GraphPlot{graph});

    if (backend_flag == "capture") {
        rekha::KalpanaBackend backend;
        fig.render(backend);
        const auto pixels = backend.rasterize();
        std::printf("rekha demo (capture): rendered %zu pixels\n", pixels.size());
        return pixels.empty() ? 1 : 0;
    }

#if defined(KALPANA_ENABLE_SOKOL_BACKEND) && KALPANA_ENABLE_SOKOL_BACKEND
    if (backend_flag == "sokol") {
        kalpana::Canvas<kalpana::sokol_backend> canvas(1000, 640);
        rekha::KalpanaBackend backend;
        fig.render(backend);
        canvas.render(backend.scene());
        std::printf("rekha demo (sokol): scene rendered to GPU backend\n");
        return 0;
    }
#endif

#if defined(KALPANA_ENABLE_NOTCURSES_BACKEND) && KALPANA_ENABLE_NOTCURSES_BACKEND
    if (backend_flag == "notcurses") {
        kalpana::Canvas<kalpana::notcurses_backend> canvas(180, 60);
        rekha::KalpanaBackend backend;
        fig.viewport({180, 60, {10.0f, 4.0f, 4.0f, 8.0f}});
        fig.render(backend);
        canvas.render(backend.scene());
        std::printf("rekha demo (notcurses): frame pushed to terminal\n");
        return 0;
    }
#endif

    std::printf("rekha demo: unknown or disabled backend '%.*s' (use capture",
                static_cast<int>(backend_flag.size()), backend_flag.data());
#if defined(KALPANA_ENABLE_SOKOL_BACKEND) && KALPANA_ENABLE_SOKOL_BACKEND
    std::printf(", sokol");
#endif
#if defined(KALPANA_ENABLE_NOTCURSES_BACKEND) && KALPANA_ENABLE_NOTCURSES_BACKEND
    std::printf(", notcurses");
#endif
    std::printf(")\n");
    return 2;
}

