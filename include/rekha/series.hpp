#pragma once

#include "types.hpp"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rekha {
    struct XYPoint {
        Scalar x = 0.0f;
        Scalar y = 0.0f;
    };

    class XYSeries {
    public:
        explicit XYSeries(std::string name = "series") : name_(std::move(name)) {}

        XYSeries& reserve(std::size_t n) {
            points_.reserve(n);
            return *this;
        }

        XYSeries& add(Scalar x, Scalar y) {
            points_.push_back(XYPoint{x, y});
            return *this;
        }

        XYSeries& add(XYPoint p) {
            points_.push_back(p);
            return *this;
        }

        XYSeries& stroke(StrokeStyle style) noexcept {
            stroke_ = style;
            return *this;
        }

        XYSeries& marker(MarkerStyle style) noexcept {
            marker_ = style;
            return *this;
        }

        [[nodiscard]] std::string_view name() const noexcept { return name_; }
        [[nodiscard]] const std::vector<XYPoint>& points() const noexcept { return points_; }
        [[nodiscard]] const StrokeStyle& stroke_style() const noexcept { return stroke_; }
        [[nodiscard]] const MarkerStyle& marker_style() const noexcept { return marker_; }

    private:
        std::string name_;
        std::vector<XYPoint> points_{};
        StrokeStyle stroke_{};
        MarkerStyle marker_{};
    };

    struct Edge {
        std::size_t from = 0;
        std::size_t to = 0;
        Scalar weight = 1.0f;
    };

    struct Graph {
        std::vector<Vec2> nodes{};
        std::vector<Edge> edges{};
    };
} // namespace rekha

