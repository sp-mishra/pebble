#pragma once

#include "series.hpp"
#include "gati/clock.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace rekha {

struct ForceSpringPolicy {
    Scalar spring_k = 0.08f;
    Scalar rest_length = 60.0f;
    Scalar repel_k = 700.0f;
    Scalar damping = 0.9f;
    Scalar min_distance = 1e-3f;
};

struct LayoutConfig {
    std::uint32_t iterations = 150;
    Scalar temperature = 14.0f;
    Scalar cooling = 0.95f;
    bool randomize_start = true;
};

template <class Policy = ForceSpringPolicy>
class ForceDirectedLayout {
public:
    explicit ForceDirectedLayout(Policy policy = {}) : policy_(policy) {}

    void initialize(Graph& graph, std::uint32_t width, std::uint32_t height, std::uint64_t seed = 0xC0FFEEULL) {
        if (!graph.nodes.empty() && !config_.randomize_start) return;

        graph.nodes.resize(std::max<std::size_t>(graph.nodes.size(), infer_node_count(graph.edges)));
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<Scalar> dist_x(0.0f, static_cast<Scalar>(width));
        std::uniform_real_distribution<Scalar> dist_y(0.0f, static_cast<Scalar>(height));
        for (auto& p : graph.nodes) {
            p = Vec2{dist_x(rng), dist_y(rng)};
        }
    }

    void solve(Graph& graph, std::uint32_t width, std::uint32_t height) {
        if (graph.nodes.empty()) return;

        std::vector<Vec2> velocity(graph.nodes.size(), Vec2{0.0f, 0.0f});
        Scalar temperature = config_.temperature;

        for (std::uint32_t iter = 0; iter < config_.iterations; ++iter) {
            std::vector<Vec2> force(graph.nodes.size(), Vec2{0.0f, 0.0f});

            for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
                for (std::size_t j = i + 1; j < graph.nodes.size(); ++j) {
                    const Vec2 d = graph.nodes[j] - graph.nodes[i];
                    const Scalar dist2 = std::max(d.len2(), policy_.min_distance);
                    const Scalar dist = std::sqrt(dist2);
                    const Vec2 dir = d / dist;
                    const Scalar fr = policy_.repel_k / dist2;
                    force[i] -= dir * fr;
                    force[j] += dir * fr;
                }
            }

            for (const auto& e : graph.edges) {
                if (e.from >= graph.nodes.size() || e.to >= graph.nodes.size()) continue;
                const Vec2 d = graph.nodes[e.to] - graph.nodes[e.from];
                const Scalar dist = std::max(d.len(), policy_.min_distance);
                const Vec2 dir = d / dist;
                const Scalar ext = dist - policy_.rest_length;
                const Scalar fs = policy_.spring_k * ext * e.weight;
                force[e.from] += dir * fs;
                force[e.to] -= dir * fs;
            }

            for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
                velocity[i] = (velocity[i] + force[i]) * policy_.damping;
                const Scalar speed = std::max(velocity[i].len(), policy_.min_distance);
                const Scalar clamp = std::min(speed, temperature) / speed;
                graph.nodes[i] += velocity[i] * clamp;
                graph.nodes[i].x = std::clamp(graph.nodes[i].x, 0.0f, static_cast<Scalar>(width));
                graph.nodes[i].y = std::clamp(graph.nodes[i].y, 0.0f, static_cast<Scalar>(height));
            }

            temperature *= config_.cooling;
        }
    }

    void step(Graph& graph, std::uint32_t width, std::uint32_t height, Scalar dt) {
        const auto saved = config_;
        config_.iterations = 1;
        config_.temperature = std::max(0.1f, saved.temperature * dt * 60.0f);
        solve(graph, width, height);
        config_ = saved;
    }

    [[nodiscard]] const LayoutConfig& config() const noexcept { return config_; }
    [[nodiscard]] LayoutConfig& config() noexcept { return config_; }

private:
    [[nodiscard]] static std::size_t infer_node_count(const std::vector<Edge>& edges) {
        std::size_t max_idx = 0;
        for (const auto& e : edges) {
            max_idx = std::max(max_idx, std::max(e.from, e.to));
        }
        return edges.empty() ? 0 : (max_idx + 1);
    }

    LayoutConfig config_{};
    [[no_unique_address]] Policy policy_{};
};

// Optional realtime driver: use Gati fixed-timestep clock to animate layout settling.
template <class LayoutPolicy = ForceSpringPolicy>
class GraphLayoutRuntime {
public:
    explicit GraphLayoutRuntime(gati::ClockConfig clock_cfg = {}) : clock_(clock_cfg) {}

    void update(Graph& graph, std::uint32_t width, std::uint32_t height, Scalar real_dt) {
        clock_.advance(real_dt);
        while (clock_.should_step()) {
            layout_.step(graph, width, height, clock_.dt());
        }
    }

    [[nodiscard]] ForceDirectedLayout<LayoutPolicy>& layout() noexcept { return layout_; }
    [[nodiscard]] const ForceDirectedLayout<LayoutPolicy>& layout() const noexcept { return layout_; }

private:
    gati::Clock clock_{};
    ForceDirectedLayout<LayoutPolicy> layout_{};
};

} // namespace rekha

