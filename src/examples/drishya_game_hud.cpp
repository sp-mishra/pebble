// // ============================================================================
// // drishya_game_hud.cpp — Drishya example: a game HUD with live input + reflow.
// // ----------------------------------------------------------------------------
// // Builds a heads-up display (health/energy gauges, a crosshair, an ability bar)
// // and drives a few input frames through the router to show pointer routing and
// // a reactive Signal updating a gauge. Uses SpringReflow so moved widgets ease to
// // their new rects instead of snapping. Painted headless via a kalpana canvas.
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
//     struct DrishyaGameHudExample {
//         static constexpr std::string_view name() { return "drishya_game_hud"; }
//
//         static constexpr std::string_view description() {
//             return "Drishya game HUD: health/energy gauges bound to a reactive "
//                 "Signal, a crosshair, and an ability row, with pointer routing.";
//         }
//
//         static constexpr std::array<std::string_view, 3> tag_data{"drishya", "ui", "game"};
//         static constexpr std::span<const std::string_view> tags() { return tag_data; }
//
//         static testfw::Result run() {
//             using M = MonospaceMetrics;
//             using P = DefaultPainter;
//
//             M metrics;
//             // SpringReflow: widgets ease to new rects when the layout shifts.
//             App<M, P, SpringReflow> app(metrics);
//
//             auto root = w::vstack(8.0f);
//             root.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//             root.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
//             const NodeId root_id = app.set_root(std::move(root));
//
//             // Top-left status column: health + energy gauges.
//             auto status = w::vstack(6.0f);
//             status.style_.width = akruti::layout::SizeSpec::Px(220.0f);
//             status.style_.height = akruti::layout::SizeSpec::Px(48.0f);
//             const NodeId status_id = app.add_child(root_id, std::move(status));
//
//             // Health drives a reactive Signal<float>. Widgets are value-typed and
//             // type-erased in the tree (no RTTI reach-in), so the retained-mode idiom
//             // is: mutate the Signal, rebuild the affected node from its current
//             // value, and mark it dirty. Here we keep the health node id and rebuild
//             // its gauge each time the Signal changes.
//             Signal<float> health{1.0f};
//             auto hp = w::health_bar(health.get());
//             hp.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//             const NodeId hp_id = app.add_child(status_id, std::move(hp));
//
//             w::Gauge energy{0.7f};
//             energy.fill = 0xFF38BDF8u; // cyan energy
//             energy.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//             app.add_child(status_id, std::move(energy));
//
//             // Center crosshair spacer + crosshair.
//             auto center = w::hstack();
//             center.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//             center.style_.flex_grow = 1.0f;
//             center.style_.justify_content = akruti::layout::Justify::Center;
//             center.style_.align_items = akruti::layout::Align::Center;
//             const NodeId center_id = app.add_child(root_id, std::move(center));
//
//             w::Crosshair reticle;
//             reticle.center_dot = true;
//             reticle.color = 0xFF9AE6B4u;
//             app.add_child(center_id, std::move(reticle));
//
//             // Ability bar along the bottom.
//             auto bar = w::hstack(6.0f);
//             bar.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//             bar.style_.height = akruti::layout::SizeSpec::Px(40.0f);
//             const NodeId bar_id = app.add_child(root_id, std::move(bar));
//             for (int i = 0; i < 5; ++i) {
//                 auto slot = w::card(0xC0202832u, 6.0f);
//                 slot.style_.width = akruti::layout::SizeSpec::Fr(1.0f);
//                 slot.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
//                 app.add_child(bar_id, std::move(slot));
//             }
//
//             // Rebuild the health gauge from the Signal whenever it changes, then
//             // flag the node for repaint. bind() primes with the current value.
//             using Widget = typename App<M, P, SpringReflow>::widget_type;
//             bind(health, [&](const float& hpv) {
//                 auto g = w::health_bar(hpv);
//                 g.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
//                 app.tree().widget(hp_id) = Widget{std::move(g)};
//                 app.tree().mark_dirty(hp_id, kDirtyPaint);
//             });
//
//             kalpana::DefaultCanvas canvas(800, 450);
//             app.set_viewport(Rect2D{0.0f, 0.0f, 800.0f, 450.0f});
//
//             // Simulate three frames: idle, a pointer press, and a damage tick.
//             P painter(canvas, metrics);
//             for (int frame = 0; frame < 3; ++frame) {
//                 if (frame == 2) health.set(0.35f); // took a hit
//
//                 InputFrame input;
//                 input.pointer = Vec2{400.0f, 225.0f};
//                 if (frame == 1) {
//                     input.buttons = kPointerLeft;
//                     input.prev_buttons = kPointerNone;
//                 }
//                 app.pump(input);
//                 app.tick(1.0f / 60.0f);
//
//                 painter.begin_frame();
//                 painter.set_color(0xFF05070Au);
//                 painter.fill_rect(Rect2D{0.0f, 0.0f, 800.0f, 450.0f});
//                 app.paint(painter);
//                 painter.present();
//             }
//
//             const std::vector<std::uint32_t> px = canvas.snapshot();
//             if (px.size() != 800u * 450u) return testfw::fail("unexpected canvas size");
//             return {};
//         }
//     };
// } // namespace
