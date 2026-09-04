// // ============================================================================
// // drishya_ml_dashboard.cpp — Drishya example: an ML/AI training dashboard.
// // ----------------------------------------------------------------------------
// // Composes data widgets (StatTile KPIs, Sparkline loss curve, Progress bar) into
// // a card-based dashboard, lays it out into a headless kalpana capture canvas,
// // and reads the pixels back. Demonstrates the full retained-mode path:
// //   build tree -> set viewport -> solve layout -> paint -> present -> snapshot.
// // Backend-agnostic: swap DefaultCanvas for a sokol/terminal canvas unchanged.
// // ============================================================================
//
// #include "test/example_registry.hpp"
//
// #include "drishya/drishya.hpp"
//
// #include <array>
// #include <string_view>
// #include <vector>
//
// namespace {
//     using namespace pebble::drishya;
//     namespace w = pebble::drishya::widgets;
//
//     struct DrishyaMlDashboardExample {
//         static constexpr std::string_view name() { return "drishya_ml_dashboard"; }
//
//         static constexpr std::string_view description() {
//             return "Drishya retained-mode ML training dashboard: KPI tiles, a loss "
//                 "sparkline, and a progress bar composed onto a headless canvas.";
//         }
//
//         static constexpr std::array<std::string_view, 3> tag_data{"drishya", "ui", "dashboard"};
//         static constexpr std::span<const std::string_view> tags() { return tag_data; }
//
//         static testfw::Result run() {
//             using M = MonospaceMetrics;
//             using P = DefaultPainter;
//
//             M metrics;
//             App<M, P> app(metrics);
//
//             // Root: a full-viewport vertical stack with padding.
//             auto root = w::vstack(16.0f);
//             root.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//             root.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
//             const NodeId root_id = app.set_root(std::move(root));
//
//             // A row of KPI stat tiles.
//             auto kpi_row = w::hstack(12.0f);
//             kpi_row.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//             kpi_row.style_.height = akruti::layout::SizeSpec::Px(72.0f);
//             const NodeId kpi_id = app.add_child(root_id, std::move(kpi_row));
//
//             w::StatTile loss_tile{"train/loss", "0.184"};
//             loss_tile.delta = "-0.012";
//             loss_tile.style_.width = akruti::layout::SizeSpec::Fr(1.0f);
//             app.add_child(kpi_id, std::move(loss_tile));
//
//             w::StatTile acc_tile{"val/acc", "94.7%"};
//             acc_tile.delta = "+0.9%";
//             acc_tile.style_.width = akruti::layout::SizeSpec::Fr(1.0f);
//             app.add_child(kpi_id, std::move(acc_tile));
//
//             w::StatTile lr_tile{"lr", "3.0e-4"};
//             lr_tile.style_.width = akruti::layout::SizeSpec::Fr(1.0f);
//             app.add_child(kpi_id, std::move(lr_tile));
//
//             // The loss curve, in a card that fills remaining space.
//             auto chart_card = w::card(0xFF11161Cu);
//             chart_card.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//             chart_card.style_.flex_grow = 1.0f;
//             const NodeId chart_id = app.add_child(root_id, std::move(chart_card));
//
//             std::vector<float> loss;
//             loss.reserve(64);
//             float v = 2.0f;
//             for (int i = 0; i < 64; ++i) {
//                 v = v * 0.955f + 0.02f; // decaying curve toward a floor
//                 loss.push_back(v);
//             }
//             w::Sparkline curve{std::move(loss)};
//             curve.color = 0xFF60A5FAu;
//             curve.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//             curve.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
//             app.add_child(chart_id, std::move(curve));
//
//             // Epoch progress along the bottom.
//             w::Progress bar{0.62f};
//             bar.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//             app.add_child(root_id, std::move(bar));
//
//             // Solve + paint into a headless capture canvas.
//             kalpana::DefaultCanvas canvas(640, 400);
//             app.set_viewport(Rect2D{0.0f, 0.0f, 640.0f, 400.0f});
//
//             P painter(canvas, metrics);
//             painter.begin_frame();
//             painter.set_color(0xFF0B0E12u);
//             painter.fill_rect(Rect2D{0.0f, 0.0f, 640.0f, 400.0f});
//             app.paint(painter);
//             painter.present();
//
//             const std::vector<std::uint32_t> px = canvas.snapshot();
//             if (px.size() != 640u * 400u) return testfw::fail("unexpected canvas size");
//             if (app.tree().node_count() != 8) return testfw::fail("unexpected node count");
//             return {};
//         }
//     };
// } // namespace
