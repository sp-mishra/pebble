#include "catch_amalgamated.hpp"
#include "rekha/rekha.hpp"

#include <span>
#include <string_view>

namespace {

struct TestBackend {
    int lines = 0;
    int polylines = 0;
    int circles = 0;
    int rects = 0;
    int texts = 0;

    void begin_frame(std::uint32_t, std::uint32_t, rekha::Color) {}
    void draw_line(rekha::Scalar, rekha::Scalar, rekha::Scalar, rekha::Scalar, rekha::StrokeStyle) { ++lines; }
    void draw_polyline(std::span<const rekha::Vec2>, rekha::StrokeStyle) { ++polylines; }
    void draw_circle(rekha::Scalar, rekha::Scalar, rekha::Scalar, rekha::Color) { ++circles; }
    void draw_rect(rekha::Scalar, rekha::Scalar, rekha::Scalar, rekha::Scalar, rekha::Color) { ++rects; }
    void draw_text(std::string_view, rekha::Scalar, rekha::Scalar, rekha::Color) { ++texts; }
    void end_frame() {}
};

} // namespace

TEST_CASE("rekha: mixed plot dispatch hits backend primitives", "[rekha][plot][backend]") {
    rekha::XYSeries line("line");
    line.add(0.0f, 0.0f).add(1.0f, 2.0f).add(2.0f, 1.0f);

    rekha::XYSeries scatter("scatter");
    scatter.add(0.5f, 0.2f).add(1.5f, 1.7f).marker({kalpana::colors::red(), 4.0f});

    rekha::XYSeries bars("bars");
    bars.add(0.0f, 3.0f).add(1.0f, 2.0f).add(2.0f, 5.0f);

    rekha::Figure fig;
    fig.add(rekha::LinePlot{line})
       .add(rekha::ScatterPlot{scatter})
       .add(rekha::BarPlot{bars})
       .add(rekha::HistogramPlot{{0.1f, 0.2f, 0.4f, 1.2f, 1.0f, 0.8f}, 4});

    TestBackend backend;
    fig.render(backend);

    REQUIRE(backend.polylines >= 1);
    REQUIRE(backend.circles >= 2);
    REQUIRE(backend.rects >= 1);
    REQUIRE(backend.lines >= 2);  // axes + ticks + graph edges if any
    REQUIRE(backend.texts >= 2);  // axis labels
}

TEST_CASE("rekha: force-directed spring layout keeps graph in viewport", "[rekha][graph][layout]") {
    rekha::Graph g;
    g.edges = {
        {0, 1, 1.0f}, {1, 2, 1.0f}, {2, 3, 1.0f}, {3, 0, 1.0f}, {0, 2, 0.5f}
    };

    rekha::ForceDirectedLayout<> layout;
    layout.config().iterations = 60;
    layout.initialize(g, 400, 300, 7);
    layout.solve(g, 400, 300);

    REQUIRE(g.nodes.size() == 4);
    for (const auto& p : g.nodes) {
        REQUIRE(p.x >= 0.0f);
        REQUIRE(p.x <= 400.0f);
        REQUIRE(p.y >= 0.0f);
        REQUIRE(p.y <= 300.0f);
    }
}

TEST_CASE("rekha: kalpana backend rasterizes frame", "[rekha][kalpana][raster]") {
    rekha::XYSeries s("wave");
    s.add(0.0f, 0.0f).add(1.0f, 1.0f).add(2.0f, 0.0f);

    rekha::Figure fig;
    fig.viewport({320, 200, {30.0f, 10.0f, 10.0f, 20.0f}}).add(rekha::LinePlot{s});

    rekha::KalpanaBackend backend;
    fig.render(backend);
    const auto pixels = backend.rasterize();

    REQUIRE(pixels.size() == 320 * 200);
}

TEST_CASE("rekha: extended chart set renders", "[rekha][plot][extended]") {
    rekha::XYSeries area("area");
    area.add(0.0f, 1.0f).add(1.0f, 2.0f).add(2.0f, 1.4f).add(3.0f, 2.2f);

    rekha::XYSeries step("step");
    step.add(0.0f, 0.2f).add(1.0f, 0.7f).add(2.0f, 0.4f).add(3.0f, 1.0f);

    rekha::XYSeries stem("stem");
    stem.add(0.5f, 0.8f).add(1.5f, 1.1f).add(2.5f, 0.6f).marker({kalpana::colors::yellow(), 2.0f});

    rekha::BubblePlot bubbles;
    bubbles.points = {
        {0.2f, 1.6f, 2.0f, kalpana::colors::cyan()},
        {1.2f, 1.0f, 3.0f, kalpana::colors::magenta()},
        {2.2f, 1.8f, 2.4f, kalpana::colors::green()}
    };

    rekha::Figure fig;
    fig.add(rekha::AreaPlot{area, 0.0f, 0.25f})
       .add(rekha::StepPlot{step})
       .add(rekha::StemPlot{stem, 0.0f})
       .add(std::move(bubbles));

    TestBackend backend;
    fig.render(backend);

    REQUIRE(backend.polylines >= 2);
    REQUIRE(backend.lines >= 3);
    REQUIRE(backend.circles >= 3);
    REQUIRE(backend.rects >= 1);
}

TEST_CASE("rekha: errorbar pie heatmap render paths", "[rekha][plot][phase1]") {
    rekha::ErrorBarPlot eb;
    eb.points = {
        {0.5f, 1.0f, 0.2f, 0.3f},
        {1.5f, 1.8f, 0.1f, 0.2f},
        {2.5f, 1.3f, 0.3f, 0.1f}
    };

    rekha::PiePlot pie;
    pie.cx = 2.0f;
    pie.cy = 3.0f;
    pie.radius = 0.8f;
    pie.inner_radius = 0.25f;
    pie.slices = {
        {3.0f, kalpana::colors::cyan(), "A"},
        {2.0f, kalpana::colors::magenta(), "B"},
        {1.0f, kalpana::colors::yellow(), "C"}
    };

    rekha::HeatmapPlot hm;
    hm.rows = 3;
    hm.cols = 4;
    hm.x_extent = {0.0f, 4.0f};
    hm.y_extent = {0.0f, 3.0f};
    hm.values = {
        0.1f, 0.2f, 0.3f, 0.4f,
        0.4f, 0.5f, 0.6f, 0.7f,
        0.7f, 0.8f, 0.9f, 1.0f
    };

    rekha::Figure fig;
    fig.legend(true)
       .add(std::move(eb))
       .add(std::move(pie))
       .add(std::move(hm));

    TestBackend backend;
    fig.render(backend);

    REQUIRE(backend.lines >= 6);   // axes + errorbars
    REQUIRE(backend.circles >= 3); // errorbar markers
    REQUIRE(backend.rects >= 10);  // heatmap cells + pie wedges + legend
    REQUIRE(backend.texts >= 2);   // axes text + legend labels
}

TEST_CASE("rekha: subplot shared axes and annotations", "[rekha][subplot][annotation]") {
    rekha::XYSeries s0("cpu");
    s0.add(0.0f, 10.0f).add(1.0f, 14.0f).add(2.0f, 12.0f);
    rekha::XYSeries s1("mem");
    s1.add(0.0f, 20.0f).add(1.0f, 24.0f).add(2.0f, 22.0f);

    rekha::Figure fig;
    fig.subplots(1, 2)
       .share_axes(true, true)
       .legend(true)
       .legend_position(rekha::LegendPosition::BottomLeft)
       .select_subplot(0, 0)
       .axes({"t", "cpu", 4})
       .add(rekha::LinePlot{s0})
       .annotate(1.0f, 14.0f, "peak")
       .select_subplot(0, 1)
       .axes({"t", "mem", 4})
       .add(rekha::StepPlot{s1})
       .annotate(1.0f, 24.0f, "max");

    TestBackend backend;
    fig.render(backend);

    REQUIRE(backend.polylines >= 2);
    REQUIRE(backend.texts >= 8); // axes labels + annotations + legends
    REQUIRE(backend.lines >= 6); // axes/ticks
}

TEST_CASE("rekha: constrained layout tick labels and arrows", "[rekha][subplot][phase2]") {
    rekha::XYSeries s("util");
    s.add(0.0f, 0.2f).add(0.5f, 0.8f).add(1.0f, 0.4f);

    rekha::Figure fig;
    fig.subplots(2, 1)
       .constrained_layout(true)
       .subplot_gap(18.0f, 24.0f)
       .share_axes(true, true)
       .legend(true)
       .legend_position(rekha::LegendPosition::TopLeft)
       .select_subplot(0, 0)
       .axes({"x", "y", 4, 4, 4, 0, 0, true, true, true})
       .add(rekha::LinePlot{s})
       .annotate_arrow(0.5f, 0.8f, 0.15f, 0.95f, "peak", kalpana::colors::yellow())
       .select_subplot(1, 0)
       .axes({"x", "y", 4, 4, 4, 1, 1, true, false, false})
       .add(rekha::AreaPlot{s, 0.0f, 0.25f})
       .annotate(0.1f, 0.2f, "baseline", kalpana::colors::cyan());

    TestBackend backend;
    fig.render(backend);

    REQUIRE(backend.polylines >= 2);
    REQUIRE(backend.lines >= 10); // includes arrow + axes + ticks
    REQUIRE(backend.texts >= 12); // tick labels + legends + annotations
}

TEST_CASE("rekha: data tick labels and auto legend", "[rekha][axis][legend]") {
    rekha::XYSeries s("trend");
    s.add(10.0f, 100.0f).add(20.0f, 180.0f).add(30.0f, 120.0f);

    rekha::Figure fig;
    fig.axes({"time", "load", 5, 5, 5, 1, 0, true, false, false})
       .legend(true)
       .legend_auto(true)
       .add(rekha::LinePlot{s})
       .annotate_arrow(30.0f, 120.0f, 24.0f, 175.0f, "drop", kalpana::colors::yellow());

    TestBackend backend;
    fig.render(backend);

    REQUIRE(backend.polylines >= 1);
    REQUIRE(backend.lines >= 6);  // axes/ticks + arrow + arrowhead
    REQUIRE(backend.texts >= 6);  // axis labels + tick labels + legend + annotation
}

TEST_CASE("rekha: axis override and graph deoverlap", "[rekha][axis][graph]") {
    rekha::Graph g;
    g.nodes = {{10.0f, 10.0f}, {10.1f, 10.1f}, {10.2f, 10.1f}};
    g.edges = {{0, 1, 1.0f}, {1, 2, 1.0f}, {2, 0, 1.0f}};

    rekha::Figure fig;
    fig.axes({
            "x", "y", 4, 0, 0, 1, 1, true, false, false,
            true, true, {0.0f, 20.0f}, {0.0f, 20.0f}
        })
       .add(rekha::GraphPlot{g, {kalpana::colors::white(), 1.0f}, {kalpana::colors::yellow(), 3.0f}, true, 2.5f});

    TestBackend backend;
    fig.render(backend);

    REQUIRE(backend.lines >= 3);
    REQUIRE(backend.circles >= 3);
    REQUIRE(backend.texts >= 2);
}

TEST_CASE("rekha: theme presets apply cleanly", "[rekha][theme]") {
    rekha::XYSeries s("theme");
    s.add(0.0f, 0.1f).add(0.5f, 0.9f).add(1.0f, 0.4f);

    rekha::Figure fig;
    fig.theme(rekha::Figure::theme_dark_neon())
       .legend(true)
       .add(rekha::LinePlot{s})
       .annotate(0.5f, 0.9f, "t");

    TestBackend backend;
    fig.render(backend);

    REQUIRE(backend.polylines >= 1);
    REQUIRE(backend.lines >= 2);
    REQUIRE(backend.texts >= 3);
}

