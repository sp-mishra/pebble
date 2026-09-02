// ============================================================================
// drishya_terminal_dashboard.cpp — Drishya example: a low-res "terminal" board.
// ----------------------------------------------------------------------------
// The same widget vocabulary as the ML/game examples, sized to a small blocky
// canvas that stands in for a text/terminal backend (notcurses, etc.). Shows a
// two-column split: a status panel of stat tiles on the left, a scrolling log
// list on the right. Painter is backend-agnostic, so nothing here is terminal-
// specific except the resolution.
// ============================================================================

#include "test/example_registry.hpp"

#include "drishya/drishya.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace pebble::drishya;
    namespace w = pebble::drishya::widgets;

    struct DrishyaTerminalDashboardExample {
        static constexpr std::string_view name() { return "drishya_terminal_dashboard"; }

        static constexpr std::string_view description() {
            return "Drishya dashboard sized for a terminal-cell backend: a split "
                "layout with a status column and a virtualized log list.";
        }

        static constexpr std::array<std::string_view, 3> tag_data{"drishya", "ui", "terminal"};
        static constexpr std::span<const std::string_view> tags() { return tag_data; }

        static testfw::Result run() {
            using M = MonospaceMetrics;
            using P = DefaultPainter;

            M metrics;
            App<M, P> app(metrics);

            // Root split: 1 fr status | 2 fr log.
            auto root = w::hstack(4.0f);
            root.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
            root.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
            const NodeId root_id = app.set_root(std::move(root));

            // Left status column.
            auto status = w::vstack(4.0f);
            status.style_.width = akruti::layout::SizeSpec::Fr(1.0f);
            status.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
            const NodeId status_id = app.add_child(root_id, std::move(status));

            w::StatTile up{"uptime", "12:47:03"};
            up.value_size = 14.0f;
            up.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
            app.add_child(status_id, std::move(up));

            w::StatTile rps{"req/s", "1 284"};
            rps.value_size = 14.0f;
            rps.delta = "+42";
            rps.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
            app.add_child(status_id, std::move(rps));

            w::StatTile err{"errors", "3"};
            err.value_size = 14.0f;
            err.value_color = 0xFFEF4444u;
            err.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
            app.add_child(status_id, std::move(err));

            // Right log column: a virtualized list view inside a card.
            auto log_card = w::card(0xFF0D1117u);
            log_card.style_.width = akruti::layout::SizeSpec::Fr(2.0f);
            log_card.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
            const NodeId log_id = app.add_child(root_id, std::move(log_card));

            w::ListView log;
            log.row_count = 200; // model is large; only visible rows are painted
            log.row_height = 14.0f;
            log.scroll_y = 120.0f; // scrolled partway down
            log.style_.width = akruti::layout::SizeSpec::Percent(100.0f);
            log.style_.height = akruti::layout::SizeSpec::Percent(100.0f);
            app.add_child(log_id, std::move(log));

            // Small canvas: 320x180 stands in for a terminal cell grid.
            kalpana::DefaultCanvas canvas(320, 180);
            app.set_viewport(Rect2D{0.0f, 0.0f, 320.0f, 180.0f});

            P painter(canvas, metrics);
            painter.begin_frame();
            painter.set_color(0xFF000000u);
            painter.fill_rect(Rect2D{0.0f, 0.0f, 320.0f, 180.0f});
            app.paint(painter);
            painter.present();

            const std::vector<std::uint32_t> px = canvas.snapshot();
            if (px.size() != 320u * 180u) return testfw::fail("unexpected canvas size");
            if (app.tree().node_count() != 7) return testfw::fail("unexpected node count");
            return {};
        }
    };
} // namespace
