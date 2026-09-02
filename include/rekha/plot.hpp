#pragma once

#include "backend.hpp"
#include "graph.hpp"
#include "scales.hpp"
#include "series.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rekha {
    struct Axes {
        std::string x_label = "x";
        std::string y_label = "y";
        std::size_t ticks = 6;
        std::size_t x_ticks = 0; // 0 => use ticks
        std::size_t y_ticks = 0; // 0 => use ticks
        int x_precision = 2;
        int y_precision = 2;
        bool show_tick_labels = true;
        bool x_percent = false;
        bool y_percent = false;
        bool x_range_override = false;
        bool y_range_override = false;
        Range x_range{0.0f, 1.0f};
        Range y_range{0.0f, 1.0f};
        Color axis_color = kalpana::Color{0.72f, 0.78f, 0.92f, 0.95f};
        Color tick_color = kalpana::Color{0.58f, 0.66f, 0.88f, 0.92f};
        Color label_color = kalpana::Color{0.88f, 0.92f, 1.0f, 0.96f};
    };

    enum class LegendPosition : std::uint8_t {
        Auto,
        TopRight,
        TopLeft,
        BottomRight,
        BottomLeft
    };

    struct Annotation {
        Scalar x = 0.0f;
        Scalar y = 0.0f;
        std::string text{};
        Color color = kalpana::colors::white();
        bool arrow = false;
        Scalar text_x = 0.0f;
        Scalar text_y = 0.0f;
    };

    struct LinePlot {
        XYSeries series{};
    };

    struct AreaPlot {
        XYSeries series{};
        Scalar baseline = 0.0f;
        Scalar opacity = 0.35f;
    };

    struct StepPlot {
        XYSeries series{};
    };

    struct StemPlot {
        XYSeries series{};
        Scalar baseline = 0.0f;
    };

    struct ScatterPlot {
        XYSeries series{};
    };

    struct ErrorBarPoint {
        Scalar x = 0.0f;
        Scalar y = 0.0f;
        Scalar x_err = 0.0f;
        Scalar y_err = 0.0f;
    };

    struct ErrorBarPlot {
        std::vector<ErrorBarPoint> points{};
        StrokeStyle stroke{kalpana::colors::white(), 1.0f};
        MarkerStyle marker{kalpana::colors::white(), 2.0f};
    };

    struct BarPlot {
        XYSeries series{};
        Scalar bar_width = 0.8f;
    };

    struct HistogramPlot {
        std::vector<Scalar> values{};
        std::size_t bins = 16;
        StrokeStyle stroke{kalpana::colors::blue(), 1.0f};
    };

    struct GraphPlot {
        Graph graph{};
        StrokeStyle edge_stroke{kalpana::colors::black(), 1.0f};
        MarkerStyle node_style{kalpana::colors::coral(), 4.0f};
        bool deoverlap = true;
        Scalar deoverlap_radius = 3.0f;
    };

    struct BubblePoint {
        Scalar x = 0.0f;
        Scalar y = 0.0f;
        Scalar r = 4.0f;
        Color color = kalpana::colors::cyan();
    };

    struct BubblePlot {
        std::vector<BubblePoint> points{};
        StrokeStyle stroke{kalpana::colors::white(), 1.0f};
    };

    struct PieSlice {
        Scalar value = 0.0f;
        Color color = kalpana::colors::cyan();
        std::string label = "slice";
    };

    struct PiePlot {
        std::vector<PieSlice> slices{};
        Scalar cx = 84.0f;
        Scalar cy = 24.0f;
        Scalar radius = 12.0f;
        Scalar inner_radius = 0.0f; // >0 forms donut
    };

    struct HeatmapPlot {
        std::size_t rows = 0;
        std::size_t cols = 0;
        std::vector<Scalar> values{}; // row-major
        Range x_extent{0.0f, 1.0f};
        Range y_extent{0.0f, 1.0f};
        Color low = kalpana::Color{0.05f, 0.10f, 0.35f, 1.0f};
        Color high = kalpana::Color{1.0f, 0.85f, 0.15f, 1.0f};
    };

    struct PlotTheme {
        Color background = kalpana::colors::white();
        Color axis_color = kalpana::colors::black();
        Color tick_color = kalpana::colors::black();
        Color label_color = kalpana::colors::black();
        Color legend_bg = kalpana::Color{0.95f, 0.95f, 0.95f, 0.9f};
        Color legend_text = kalpana::colors::black();
    };

    using Plot = std::variant<LinePlot, AreaPlot, StepPlot, StemPlot, ScatterPlot, ErrorBarPlot, BarPlot, HistogramPlot,
                              GraphPlot, BubblePlot, PiePlot, HeatmapPlot>;

    class Figure {
    public:
        Figure() = default;

        [[nodiscard]] static constexpr PlotTheme theme_dark_neon() noexcept {
            return PlotTheme{
                .background = kalpana::Color{0.03f, 0.04f, 0.07f, 1.0f},
                .axis_color = kalpana::Color{0.55f, 0.62f, 0.80f, 0.95f},
                .tick_color = kalpana::Color{0.45f, 0.52f, 0.72f, 0.92f},
                .label_color = kalpana::Color{0.88f, 0.92f, 1.0f, 0.96f},
                .legend_bg = kalpana::Color{0.08f, 0.10f, 0.16f, 0.82f},
                .legend_text = kalpana::colors::white()
            };
        }

        [[nodiscard]] static constexpr PlotTheme theme_scientific_light() noexcept {
            return PlotTheme{
                .background = kalpana::Color{0.99f, 0.99f, 1.0f, 1.0f},
                .axis_color = kalpana::Color{0.15f, 0.18f, 0.25f, 1.0f},
                .tick_color = kalpana::Color{0.22f, 0.25f, 0.34f, 1.0f},
                .label_color = kalpana::Color{0.10f, 0.12f, 0.18f, 1.0f},
                .legend_bg = kalpana::Color{0.94f, 0.96f, 0.99f, 0.92f},
                .legend_text = kalpana::Color{0.10f, 0.12f, 0.18f, 1.0f}
            };
        }

        [[nodiscard]] static constexpr PlotTheme theme_finance_dark() noexcept {
            return PlotTheme{
                .background = kalpana::Color{0.05f, 0.06f, 0.08f, 1.0f},
                .axis_color = kalpana::Color{0.62f, 0.66f, 0.70f, 0.95f},
                .tick_color = kalpana::Color{0.52f, 0.56f, 0.60f, 0.92f},
                .label_color = kalpana::Color{0.90f, 0.93f, 0.95f, 0.96f},
                .legend_bg = kalpana::Color{0.10f, 0.12f, 0.14f, 0.90f},
                .legend_text = kalpana::Color{0.90f, 0.93f, 0.95f, 0.96f}
            };
        }

        Figure& viewport(Viewport vp) noexcept {
            viewport_ = vp;
            return *this;
        }

        Figure& background(Color c) noexcept {
            background_ = c;
            return *this;
        }

        Figure& theme(const PlotTheme& t) noexcept {
            background_ = t.background;
            axes_.axis_color = t.axis_color;
            axes_.tick_color = t.tick_color;
            axes_.label_color = t.label_color;
            legend_bg_color_ = t.legend_bg;
            legend_text_color_ = t.legend_text;
            for (auto& cell : cells_) {
                cell.axes.axis_color = t.axis_color;
                cell.axes.tick_color = t.tick_color;
                cell.axes.label_color = t.label_color;
            }
            return *this;
        }

        Figure& axes(Axes ax) {
            if (grid_enabled_) {
                cells_[active_cell_].axes = std::move(ax);
            }
            else {
                axes_ = std::move(ax);
            }
            return *this;
        }

        Figure& subplots(std::size_t rows, std::size_t cols) {
            rows_ = std::max<std::size_t>(rows, 1);
            cols_ = std::max<std::size_t>(cols, 1);
            grid_enabled_ = true;
            active_cell_ = 0;
            cells_.assign(rows_ * cols_, Cell{});
            for (auto& c : cells_) {
                c.axes.axis_color = axes_.axis_color;
                c.axes.tick_color = axes_.tick_color;
                c.axes.label_color = axes_.label_color;
            }
            return *this;
        }

        Figure& select_subplot(std::size_t row, std::size_t col) {
            if (!grid_enabled_) return *this;
            const std::size_t r = std::min(row, rows_ - 1);
            const std::size_t c = std::min(col, cols_ - 1);
            active_cell_ = r * cols_ + c;
            return *this;
        }

        Figure& share_axes(bool share_x = true, bool share_y = true) noexcept {
            share_x_ = share_x;
            share_y_ = share_y;
            return *this;
        }

        Figure& add(LinePlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(ScatterPlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(AreaPlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(StepPlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(StemPlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(BarPlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(ErrorBarPlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(HistogramPlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(GraphPlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(BubblePlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(PiePlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& add(HeatmapPlot plot) {
            add_plot(std::move(plot));
            return *this;
        }

        Figure& legend(bool enabled = true) noexcept {
            legend_enabled_ = enabled;
            return *this;
        }

        Figure& legend_position(LegendPosition pos) noexcept {
            legend_pos_ = pos;
            return *this;
        }

        Figure& legend_auto(bool enabled = true) noexcept {
            legend_auto_ = enabled;
            return *this;
        }

        Figure& annotate(Scalar x, Scalar y, std::string text, Color color = kalpana::colors::white()) {
            if (grid_enabled_) {
                cells_[active_cell_].annotations.push_back(Annotation{x, y, std::move(text), color});
            }
            else {
                annotations_.push_back(Annotation{x, y, std::move(text), color});
            }
            return *this;
        }

        Figure& annotate_arrow(Scalar x, Scalar y, Scalar text_x, Scalar text_y,
                               std::string text, Color color = kalpana::colors::white()) {
            Annotation a{};
            a.x = x;
            a.y = y;
            a.text_x = text_x;
            a.text_y = text_y;
            a.text = std::move(text);
            a.color = color;
            a.arrow = true;
            if (grid_enabled_) {
                cells_[active_cell_].annotations.push_back(std::move(a));
            }
            else {
                annotations_.push_back(std::move(a));
            }
            return *this;
        }

        Figure& subplot_gap(Scalar x_gap, Scalar y_gap) noexcept {
            subplot_gap_x_ = std::max<Scalar>(0.0f, x_gap);
            subplot_gap_y_ = std::max<Scalar>(0.0f, y_gap);
            return *this;
        }

        Figure& constrained_layout(bool enabled = true) noexcept {
            constrained_layout_ = enabled;
            return *this;
        }

        [[nodiscard]] const std::vector<Plot>& plots() const noexcept { return plots_; }

        template <PlotBackend Backend>
        void render(Backend& backend) const {
            const Rect rect = viewport_.plot_rect();

            backend.begin_frame(viewport_.width, viewport_.height, background_);
            if (!grid_enabled_) {
                render_cell(backend, rect, axes_, plots_, annotations_, infer_ranges_for(plots_));
            }
            else {
                const Scalar gap_x = constrained_layout_ ? subplot_gap_x_ : 0.0f;
                const Scalar gap_y = constrained_layout_ ? subplot_gap_y_ : 0.0f;
                const Scalar cw = (rect.w - gap_x * static_cast<Scalar>(cols_ - 1)) / static_cast<Scalar>(cols_);
                const Scalar ch = (rect.h - gap_y * static_cast<Scalar>(rows_ - 1)) / static_cast<Scalar>(rows_);

                std::pair<Range, Range> shared_ranges{};
                bool have_shared = false;
                if (share_x_ || share_y_) {
                    std::vector<Plot> merged;
                    for (const auto& cell : cells_) {
                        merged.insert(merged.end(), cell.plots.begin(), cell.plots.end());
                    }
                    if (!merged.empty()) {
                        shared_ranges = infer_ranges_for(merged);
                        have_shared = true;
                    }
                }

                for (std::size_t r = 0; r < rows_; ++r) {
                    for (std::size_t c = 0; c < cols_; ++c) {
                        const std::size_t idx = r * cols_ + c;
                        const Rect cell_rect{
                            rect.x + static_cast<Scalar>(c) * (cw + gap_x),
                            rect.y + static_cast<Scalar>(r) * (ch + gap_y),
                            cw,
                            ch
                        };
                        auto local = infer_ranges_for(cells_[idx].plots);
                        if (have_shared) {
                            if (share_x_) local.first = shared_ranges.first;
                            if (share_y_) local.second = shared_ranges.second;
                        }
                        render_cell(backend, cell_rect, cells_[idx].axes, cells_[idx].plots, cells_[idx].annotations,
                                    local);
                    }
                }
            }

            backend.end_frame();
        }

    private:
        struct Cell {
            Axes axes{};
            std::vector<Plot> plots{};
            std::vector<Annotation> annotations{};
        };

        void add_plot(Plot plot) {
            if (grid_enabled_) {
                cells_[active_cell_].plots.push_back(std::move(plot));
            }
            else {
                plots_.push_back(std::move(plot));
            }
        }

        template <PlotBackend Backend>
        void render_cell(Backend& backend, const Rect& rect, const Axes& axes,
                         const std::vector<Plot>& plots, const std::vector<Annotation>& ann,
                         std::pair<Range, Range> ranges) const {
            if (axes.x_range_override) ranges.first = axes.x_range;
            if (axes.y_range_override) ranges.second = axes.y_range;
            const auto& [x_range, y_range] = ranges;
            draw_axes(backend, rect, axes, x_range, y_range);
            const LinearScale x_scale{x_range, Range{rect.x, rect.x + rect.w}};
            const LinearScale y_scale{y_range, Range{rect.y + rect.h, rect.y}};

            for (const auto& plot : plots) {
                std::visit([&](const auto& typed) {
                    draw_plot(backend, typed, x_scale, y_scale, rect, x_range, y_range);
                }, plot);
            }

            for (const auto& a : ann) {
                const Scalar px = x_scale.map(a.x);
                const Scalar py = y_scale.map(a.y);
                if (a.arrow) {
                    const Scalar tx = x_scale.map(a.text_x);
                    const Scalar ty = y_scale.map(a.text_y);
                    const StrokeStyle as{a.color, 1.0f};
                    backend.draw_line(tx, ty, px, py, as);
                    // Small V arrowhead near the target point.
                    const Scalar dx = px - tx;
                    const Scalar dy = py - ty;
                    const Scalar len = std::sqrt(dx * dx + dy * dy);
                    if (len > 1e-4f) {
                        const Scalar ux = dx / len;
                        const Scalar uy = dy / len;
                        const Scalar hx = -uy;
                        const Scalar hy = ux;
                        const Scalar al = 6.0f;
                        const Scalar aw = 3.0f;
                        backend.draw_line(px, py, px - ux * al + hx * aw, py - uy * al + hy * aw, as);
                        backend.draw_line(px, py, px - ux * al - hx * aw, py - uy * al - hy * aw, as);
                    }
                    backend.draw_circle(px, py, 2.0f, a.color);
                    backend.draw_text(a.text, tx, ty, a.color);
                }
                else {
                    backend.draw_text(a.text, px, py, a.color);
                }
            }

            if (legend_enabled_) {
                draw_legend(backend, rect, plots, x_range, y_range);
            }
        }

        [[nodiscard]] static std::string format_tick(Scalar value, bool as_percent, int precision) {
            std::ostringstream oss;
            oss.setf(std::ios::fixed);
            oss << std::setprecision(std::max(0, precision));
            if (as_percent) {
                oss << (value * 100.0f) << "%";
            }
            else {
                oss << value;
            }
            return oss.str();
        }

        [[nodiscard]] static std::vector<Scalar> nice_ticks(const Range& r, std::size_t target_count) {
            std::vector<Scalar> out;
            const std::size_t n = std::max<std::size_t>(target_count, 2);
            const Scalar span = r.span();
            if (!(span > 0.0f)) {
                out.push_back(r.min);
                out.push_back(r.max);
                return out;
            }

            const Scalar raw = span / static_cast<Scalar>(n - 1);
            const Scalar p10 = std::pow(10.0f, std::floor(std::log10(std::max(raw, 1e-12f))));
            const Scalar norm = raw / p10;
            Scalar step = 1.0f;
            if (norm > 5.0f) step = 10.0f;
            else if (norm > 2.0f) step = 5.0f;
            else if (norm > 1.0f) step = 2.0f;
            step *= p10;

            const Scalar lo = std::floor(r.min / step) * step;
            const Scalar hi = std::ceil(r.max / step) * step;
            for (Scalar v = lo; v <= hi + step * 0.5f; v += step) {
                out.push_back(v);
                if (out.size() > 4096) break;
            }
            if (out.empty()) {
                out.push_back(r.min);
                out.push_back(r.max);
            }
            return out;
        }

        template <PlotBackend Backend>
        void draw_axes(Backend& backend, const Rect& rect, const Axes& axes,
                       const Range& xr, const Range& yr) const {
            const StrokeStyle axis_stroke{axes.axis_color, 1.0f};
            backend.draw_line(rect.x, rect.y + rect.h, rect.x + rect.w, rect.y + rect.h, axis_stroke);
            backend.draw_line(rect.x, rect.y, rect.x, rect.y + rect.h, axis_stroke);

            const std::size_t xt = axes.x_ticks == 0 ? axes.ticks : axes.x_ticks;
            const std::size_t yt = axes.y_ticks == 0 ? axes.ticks : axes.y_ticks;
            const auto x_ticks = nice_ticks(xr, xt + 1);
            const auto y_ticks = nice_ticks(yr, yt + 1);

            for (const Scalar xv : x_ticks) {
                const Scalar t = (xv - xr.min) / std::max<Scalar>(xr.span(), 1e-12f);
                const Scalar tx = rect.x + t * rect.w;
                backend.draw_line(tx, rect.y + rect.h, tx, rect.y + rect.h + 4.0f, StrokeStyle{axes.tick_color, 1.0f});
                if (axes.show_tick_labels) {
                    const auto s = format_tick(xv, axes.x_percent, axes.x_precision);
                    backend.draw_text(s, tx - 10.0f, rect.y + rect.h + 16.0f, axes.label_color);
                }
            }

            for (const Scalar yv : y_ticks) {
                const Scalar t = (yv - yr.min) / std::max<Scalar>(yr.span(), 1e-12f);
                const Scalar ty = rect.y + rect.h - t * rect.h;
                backend.draw_line(rect.x - 4.0f, ty, rect.x, ty, StrokeStyle{axes.tick_color, 1.0f});
                if (axes.show_tick_labels) {
                    const auto s = format_tick(yv, axes.y_percent, axes.y_precision);
                    backend.draw_text(s, rect.x - 36.0f, ty + 4.0f, axes.label_color);
                }
            }

            backend.draw_text(axes.x_label, rect.x + rect.w * 0.5f, rect.y + rect.h + 20.0f, axes.label_color);
            backend.draw_text(axes.y_label, rect.x - 22.0f, rect.y - 6.0f, axes.label_color);
        }

        static std::array<std::size_t, 4> quadrant_density(const std::vector<Plot>& plots,
                                                           const Range& xr, const Range& yr) {
            // 0: TR, 1: TL, 2: BR, 3: BL
            std::array<std::size_t, 4> d{};
            const Scalar mx = xr.min + xr.span() * 0.5f;
            const Scalar my = yr.min + yr.span() * 0.5f;
            auto bump = [&](Scalar x, Scalar y) {
                const bool right = x >= mx;
                const bool top = y >= my;
                if (right && top) ++d[0];
                else if (!right && top) ++d[1];
                else if (right && !top) ++d[2];
                else ++d[3];
            };
            for (const auto& plot : plots) {
                std::visit([&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::same_as<T, LinePlot> || std::same_as<T, AreaPlot> || std::same_as<T, StepPlot> ||
                        std::same_as<T, StemPlot> || std::same_as<T, ScatterPlot> || std::same_as<T, BarPlot>) {
                        for (const auto& pt : p.series.points()) bump(pt.x, pt.y);
                    }
                    else if constexpr (std::same_as<T, GraphPlot>) {
                        for (const auto& n : p.graph.nodes) bump(n.x(), n.y());
                    }
                    else if constexpr (std::same_as<T, BubblePlot>) {
                        for (const auto& bp : p.points) bump(bp.x, bp.y);
                    }
                    else if constexpr (std::same_as<T, ErrorBarPlot>) {
                        for (const auto& ep : p.points) bump(ep.x, ep.y);
                    }
                    else if constexpr (std::same_as<T, PiePlot>) {
                        bump(p.cx, p.cy);
                    }
                    else if constexpr (std::same_as<T, HeatmapPlot>) {
                        bump((p.x_extent.min + p.x_extent.max) * 0.5f, (p.y_extent.min + p.y_extent.max) * 0.5f);
                    }
                    else if constexpr (std::same_as<T, HistogramPlot>) {
                        if (!p.values.empty()) {
                            auto [vmin, vmax] = std::minmax_element(p.values.begin(), p.values.end());
                            bump((*vmin + *vmax) * 0.5f, static_cast<Scalar>(p.values.size()) * 0.5f);
                        }
                    }
                }, plot);
            }
            return d;
        }

        template <PlotBackend Backend>
        void draw_legend(Backend& backend, const Rect& rect, const std::vector<Plot>& plots,
                         const Range& xr, const Range& yr) const {
            const Scalar w = 150.0f;
            const Scalar h = 20.0f + 14.0f * static_cast<Scalar>(plots.size());
            LegendPosition pos = legend_pos_;
            if ((legend_auto_ || legend_pos_ == LegendPosition::Auto) && !plots.empty()) {
                const auto d = quadrant_density(plots, xr, yr);
                std::size_t min_i = 0;
                for (std::size_t i = 1; i < d.size(); ++i) {
                    if (d[i] < d[min_i]) min_i = i;
                }
                pos = (min_i == 0)
                          ? LegendPosition::TopRight
                          : (min_i == 1)
                          ? LegendPosition::TopLeft
                          : (min_i == 2)
                          ? LegendPosition::BottomRight
                          : LegendPosition::BottomLeft;
            }

            Scalar x = rect.x + rect.w - 160.0f;
            Scalar y = rect.y + 12.0f;
            if (pos == LegendPosition::TopLeft) {
                x = rect.x + 10.0f;
                y = rect.y + 12.0f;
            }
            else if (pos == LegendPosition::BottomRight) {
                x = rect.x + rect.w - 160.0f;
                y = rect.y + rect.h - h - 12.0f;
            }
            else if (pos == LegendPosition::BottomLeft) {
                x = rect.x + 10.0f;
                y = rect.y + rect.h - h - 12.0f;
            }
            backend.draw_rect(x - 8.0f, y - 8.0f, w,
                              h,
                              legend_bg_color_);

            std::size_t row = 0;
            for (const auto& plot : plots) {
                const Scalar yy = y + static_cast<Scalar>(row++) * 14.0f;
                std::visit([&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    Color c = kalpana::colors::white();
                    std::string name = "plot";
                    if constexpr (std::same_as<T, LinePlot> || std::same_as<T, AreaPlot> || std::same_as<T, StepPlot> ||
                        std::same_as<T, StemPlot> || std::same_as<T, ScatterPlot> || std::same_as<T, BarPlot>) {
                        c = p.series.stroke_style().color;
                        name = std::string(p.series.name());
                    }
                    else if constexpr (std::same_as<T, ErrorBarPlot>) {
                        c = p.stroke.color;
                        name = "errorbar";
                    }
                    else if constexpr (std::same_as<T, HistogramPlot>) {
                        c = p.stroke.color;
                        name = "hist";
                    }
                    else if constexpr (std::same_as<T, GraphPlot>) {
                        c = p.node_style.color;
                        name = "graph";
                    }
                    else if constexpr (std::same_as<T, BubblePlot>) {
                        c = p.points.empty() ? kalpana::colors::white() : p.points.front().color;
                        name = "bubble";
                    }
                    else if constexpr (std::same_as<T, PiePlot>) {
                        c = p.slices.empty() ? kalpana::colors::white() : p.slices.front().color;
                        name = "pie";
                    }
                    else if constexpr (std::same_as<T, HeatmapPlot>) {
                        c = p.high;
                        name = "heatmap";
                    }
                    backend.draw_rect(x, yy, 10.0f, 10.0f, c);
                    backend.draw_text(name, x + 14.0f, yy + 9.0f, legend_text_color_);
                }, plot);
            }
        }

        [[nodiscard]] static std::pair<Range, Range> infer_ranges_for(const std::vector<Plot>& plots) {
            Range xr{0.0f, 1.0f};
            Range yr{0.0f, 1.0f};
            bool has_any = false;

            auto include_point = [&](Scalar x, Scalar y) {
                if (!has_any) {
                    xr = Range{x, x};
                    yr = Range{y, y};
                    has_any = true;
                    return;
                }
                xr.include(x);
                yr.include(y);
            };

            for (const auto& plot : plots) {
                std::visit([&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::same_as<T, LinePlot> || std::same_as<T, AreaPlot> || std::same_as<T, StepPlot> ||
                        std::same_as<T, StemPlot> || std::same_as<T, ScatterPlot> || std::same_as<T, BarPlot>) {
                        for (const auto& pt : p.series.points()) include_point(pt.x, pt.y);
                        if constexpr (std::same_as<T, AreaPlot> || std::same_as<T, StemPlot>) {
                            for (const auto& pt : p.series.points()) include_point(pt.x, p.baseline);
                        }
                    }
                    else if constexpr (std::same_as<T, HistogramPlot>) {
                        if (!p.values.empty()) {
                            auto [vmin, vmax] = std::minmax_element(p.values.begin(), p.values.end());
                            include_point(*vmin, 0.0f);
                            include_point(*vmax, static_cast<Scalar>(p.values.size()));
                        }
                    }
                    else if constexpr (std::same_as<T, GraphPlot>) {
                        for (const auto& n : p.graph.nodes) include_point(n.x(), n.y());
                    }
                    else if constexpr (std::same_as<T, BubblePlot>) {
                        for (const auto& bp : p.points) {
                            include_point(bp.x - bp.r, bp.y - bp.r);
                            include_point(bp.x + bp.r, bp.y + bp.r);
                        }
                    }
                    else if constexpr (std::same_as<T, ErrorBarPlot>) {
                        for (const auto& ep : p.points) {
                            include_point(ep.x - ep.x_err, ep.y - ep.y_err);
                            include_point(ep.x + ep.x_err, ep.y + ep.y_err);
                        }
                    }
                    else if constexpr (std::same_as<T, PiePlot>) {
                        include_point(p.cx - p.radius, p.cy - p.radius);
                        include_point(p.cx + p.radius, p.cy + p.radius);
                    }
                    else if constexpr (std::same_as<T, HeatmapPlot>) {
                        include_point(p.x_extent.min, p.y_extent.min);
                        include_point(p.x_extent.max, p.y_extent.max);
                    }
                }, plot);
            }

            if (xr.degenerate()) {
                xr.min -= 1.0f;
                xr.max += 1.0f;
            }
            if (yr.degenerate()) {
                yr.min -= 1.0f;
                yr.max += 1.0f;
            }
            return {xr, yr};
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const LinePlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            std::vector<Vec2> pts;
            pts.reserve(plot.series.points().size());
            for (const auto& p : plot.series.points()) {
                pts.push_back(Vec2{xs.map(p.x), ys.map(p.y)});
            }
            backend.draw_polyline(std::span<const Vec2>(pts.data(), pts.size()), plot.series.stroke_style());
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const AreaPlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            const auto& points = plot.series.points();
            if (points.size() < 2) return;
            const Scalar y0 = ys.map(plot.baseline);
            Color fill = plot.series.stroke_style().color;
            fill.a = std::clamp(plot.opacity, 0.05f, 1.0f);

            for (std::size_t i = 1; i < points.size(); ++i) {
                const Scalar x0 = xs.map(points[i - 1].x);
                const Scalar x1 = xs.map(points[i].x);
                const Scalar y_prev = ys.map(points[i - 1].y);
                const Scalar y_curr = ys.map(points[i].y);
                const Scalar y_top = std::min(std::min(y_prev, y_curr), y0);
                const Scalar y_bot = std::max(std::max(y_prev, y_curr), y0);
                backend.draw_rect(std::min(x0, x1), y_top, std::max<Scalar>(1.0f, std::abs(x1 - x0)),
                                  std::max<Scalar>(1.0f, y_bot - y_top), fill);
            }

            std::vector<Vec2> line_pts;
            line_pts.reserve(points.size());
            for (const auto& p : points) {
                line_pts.push_back(Vec2{xs.map(p.x), ys.map(p.y)});
            }
            backend.draw_polyline(std::span<const Vec2>(line_pts.data(), line_pts.size()), plot.series.stroke_style());
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const StepPlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            const auto& points = plot.series.points();
            if (points.empty()) return;

            std::vector<Vec2> pts;
            pts.reserve(points.size() * 2);
            pts.push_back(Vec2{xs.map(points[0].x), ys.map(points[0].y)});
            for (std::size_t i = 1; i < points.size(); ++i) {
                const Scalar py = ys.map(points[i - 1].y);
                const Scalar cx = xs.map(points[i].x);
                const Scalar cy = ys.map(points[i].y);
                pts.push_back(Vec2{cx, py});
                pts.push_back(Vec2{cx, cy});
            }
            backend.draw_polyline(std::span<const Vec2>(pts.data(), pts.size()), plot.series.stroke_style());
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const StemPlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            const Scalar y0 = ys.map(plot.baseline);
            for (const auto& p : plot.series.points()) {
                const Scalar sx = xs.map(p.x);
                const Scalar sy = ys.map(p.y);
                backend.draw_line(sx, y0, sx, sy, plot.series.stroke_style());
                backend.draw_circle(sx, sy, plot.series.marker_style().radius, plot.series.marker_style().color);
            }
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const ScatterPlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            for (const auto& p : plot.series.points()) {
                backend.draw_circle(xs.map(p.x), ys.map(p.y), plot.series.marker_style().radius,
                                    plot.series.marker_style().color);
            }
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const ErrorBarPlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            for (const auto& p : plot.points) {
                const Scalar x = xs.map(p.x);
                const Scalar y = ys.map(p.y);
                const Scalar x0 = xs.map(p.x - p.x_err);
                const Scalar x1 = xs.map(p.x + p.x_err);
                const Scalar y0 = ys.map(p.y - p.y_err);
                const Scalar y1 = ys.map(p.y + p.y_err);
                backend.draw_line(x0, y, x1, y, plot.stroke);
                backend.draw_line(x, y0, x, y1, plot.stroke);
                backend.draw_circle(x, y, plot.marker.radius, plot.marker.color);
            }
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const BarPlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            const Scalar y0 = ys.map(0.0f);
            for (const auto& p : plot.series.points()) {
                const Scalar x = xs.map(p.x);
                const Scalar y = ys.map(p.y);
                const Scalar w = std::max<Scalar>(2.0f, plot.bar_width * 8.0f);
                const Scalar top = std::min(y, y0);
                const Scalar h = std::abs(y - y0);
                backend.draw_rect(x - 0.5f * w, top, w, std::max<Scalar>(1.0f, h),
                                  plot.series.stroke_style().color.with_alpha(0.6f));
            }
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const HistogramPlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            if (plot.values.empty() || plot.bins == 0) return;

            const auto [vmin_it, vmax_it] = std::minmax_element(plot.values.begin(), plot.values.end());
            const Scalar min_v = *vmin_it;
            const Scalar max_v = *vmax_it;
            const Scalar span = std::max<Scalar>(max_v - min_v, 1e-6f);
            std::vector<std::size_t> counts(plot.bins, 0);

            for (const Scalar v : plot.values) {
                const Scalar t = (v - min_v) / span;
                const std::size_t idx = std::min<std::size_t>(plot.bins - 1,
                                                              static_cast<std::size_t>(t * static_cast<Scalar>(plot.
                                                                  bins)));
                ++counts[idx];
            }

            const Scalar bin_w = span / static_cast<Scalar>(plot.bins);
            for (std::size_t i = 0; i < plot.bins; ++i) {
                const Scalar x0 = min_v + static_cast<Scalar>(i) * bin_w;
                const Scalar x1 = x0 + bin_w;
                const Scalar y = static_cast<Scalar>(counts[i]);
                const Scalar sx0 = xs.map(x0);
                const Scalar sx1 = xs.map(x1);
                const Scalar sy = ys.map(y);
                const Scalar sy0 = ys.map(0.0f);
                const Scalar top = std::min(sy, sy0);
                backend.draw_rect(sx0, top, std::max<Scalar>(1.0f, sx1 - sx0), std::abs(sy0 - sy),
                                  plot.stroke.color.with_alpha(0.5f));
            }
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const GraphPlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            std::vector<Vec2> nodes;
            nodes.reserve(plot.graph.nodes.size());
            for (const auto& n : plot.graph.nodes) {
                nodes.push_back(Vec2{n.x(), n.y()});
            }
            if (plot.deoverlap && !nodes.empty()) {
                const Scalar min_d2 = plot.deoverlap_radius * plot.deoverlap_radius;
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    for (std::size_t j = i + 1; j < nodes.size(); ++j) {
                        Vec2 d = nodes[j] - nodes[i];
                        const Scalar l2 = akruti::length_sq(d);
                        if (l2 < min_d2) {
                            const Scalar a = static_cast<Scalar>((i * 97 + j * 57) % 360) * 0.0174532925f;
                            const Vec2 push{std::cos(a), std::sin(a)};
                            nodes[j] += push * (plot.deoverlap_radius * 0.5f);
                        }
                    }
                }
            }
            for (const auto& e : plot.graph.edges) {
                if (e.from >= nodes.size() || e.to >= nodes.size()) continue;
                const Vec2& a = nodes[e.from];
                const Vec2& b = nodes[e.to];
                backend.draw_line(xs.map(a.x()), ys.map(a.y()), xs.map(b.x()), ys.map(b.y()), plot.edge_stroke);
            }
            for (const auto& n : nodes) {
                backend.draw_circle(xs.map(n.x()), ys.map(n.y()), plot.node_style.radius, plot.node_style.color);
            }
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const BubblePlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            for (const auto& p : plot.points) {
                const Scalar sx = xs.map(p.x);
                const Scalar sy = ys.map(p.y);
                backend.draw_circle(sx, sy, std::max<Scalar>(1.0f, p.r), p.color.with_alpha(0.55f));
                backend.draw_circle(sx, sy, std::max<Scalar>(1.0f, p.r * 0.33f), p.color);
            }
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const PiePlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            if (plot.slices.empty()) return;
            Scalar total = 0.0f;
            for (const auto& s : plot.slices) total += std::max<Scalar>(0.0f, s.value);
            if (!(total > 0.0f)) return;

            const Scalar cx = xs.map(plot.cx);
            const Scalar cy = ys.map(plot.cy);
            const Scalar rp = std::max<Scalar>(2.0f, plot.radius);
            const Scalar ri = std::max<Scalar>(0.0f, std::min(plot.inner_radius, rp - 1.0f));

            Scalar accum = 0.0f;
            for (const auto& s : plot.slices) {
                const Scalar frac = std::max<Scalar>(0.0f, s.value) / total;
                const Scalar a0 = accum * 6.28318530718f;
                const Scalar a1 = (accum + frac) * 6.28318530718f;
                accum += frac;

                const int radial_steps = std::max(4, static_cast<int>(rp - ri));
                const int angle_steps = std::max(12, static_cast<int>((a1 - a0) * rp * 0.9f));
                for (int rr = 0; rr <= radial_steps; ++rr) {
                    const Scalar r = ri + (rp - ri) * (static_cast<Scalar>(rr) / static_cast<Scalar>(radial_steps));
                    for (int aa = 0; aa <= angle_steps; ++aa) {
                        const Scalar t = static_cast<Scalar>(aa) / static_cast<Scalar>(angle_steps);
                        const Scalar a = a0 + (a1 - a0) * t;
                        const Scalar x = cx + std::cos(a) * r;
                        const Scalar y = cy + std::sin(a) * r;
                        const Scalar dot_r = std::max<Scalar>(0.6f, (rp - ri) * 0.035f);
                        backend.draw_circle(x, y, dot_r, s.color.with_alpha(0.96f));
                    }
                }
            }

            // Donut hole cleanup for crisper center.
            if (ri > 0.5f) {
                backend.draw_circle(cx, cy, ri * 0.98f, kalpana::Color{0.03f, 0.04f, 0.07f, 1.0f});
            }
        }

        template <PlotBackend Backend>
        static void draw_plot(Backend& backend, const HeatmapPlot& plot,
                              const LinearScale& xs, const LinearScale& ys,
                              const Rect&, const Range&, const Range&) {
            if (plot.rows == 0 || plot.cols == 0) return;
            if (plot.values.size() < plot.rows * plot.cols) return;

            auto [min_it, max_it] = std::minmax_element(plot.values.begin(), plot.values.end());
            const Scalar vmin = *min_it;
            const Scalar vmax = *max_it;
            const Scalar span = std::max<Scalar>(1e-6f, vmax - vmin);
            const Scalar dx = (plot.x_extent.max - plot.x_extent.min) / static_cast<Scalar>(plot.cols);
            const Scalar dy = (plot.y_extent.max - plot.y_extent.min) / static_cast<Scalar>(plot.rows);

            for (std::size_t r = 0; r < plot.rows; ++r) {
                for (std::size_t c = 0; c < plot.cols; ++c) {
                    const std::size_t idx = r * plot.cols + c;
                    const Scalar v = (plot.values[idx] - vmin) / span;
                    const Color col = plot.low.lerp(plot.high, std::clamp(v, 0.0f, 1.0f));
                    const Scalar x0 = plot.x_extent.min + static_cast<Scalar>(c) * dx;
                    const Scalar y0 = plot.y_extent.min + static_cast<Scalar>(r) * dy;
                    const Scalar x1 = x0 + dx;
                    const Scalar y1 = y0 + dy;
                    const Scalar sx0 = xs.map(x0);
                    const Scalar sx1 = xs.map(x1);
                    const Scalar sy0 = ys.map(y0);
                    const Scalar sy1 = ys.map(y1);
                    const Scalar left = std::min(sx0, sx1);
                    const Scalar top = std::min(sy0, sy1);
                    backend.draw_rect(left, top, std::max<Scalar>(1.0f, std::abs(sx1 - sx0)),
                                      std::max<Scalar>(1.0f, std::abs(sy1 - sy0)), col);
                }
            }
        }

        Viewport viewport_{};
        Axes axes_{};
        Color background_ = kalpana::colors::white();
        bool legend_enabled_ = false;
        LegendPosition legend_pos_ = LegendPosition::Auto;
        bool legend_auto_ = true;
        Color legend_bg_color_ = kalpana::Color{0.08f, 0.10f, 0.16f, 0.82f};
        Color legend_text_color_ = kalpana::colors::white();
        bool grid_enabled_ = false;
        bool constrained_layout_ = false;
        Scalar subplot_gap_x_ = 16.0f;
        Scalar subplot_gap_y_ = 16.0f;
        bool share_x_ = false;
        bool share_y_ = false;
        std::size_t rows_ = 1;
        std::size_t cols_ = 1;
        std::size_t active_cell_ = 0;
        std::vector<Cell> cells_{};
        std::vector<Annotation> annotations_{};
        std::vector<Plot> plots_{};
    };
} // namespace rekha

