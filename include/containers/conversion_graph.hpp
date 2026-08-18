#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

// Weighted least-cost directed path over opaque uint32 vertices.
// Pure container — no Vākya/Sutra types may appear here.

namespace containers {
    using conversion_vertex = std::uint32_t;
    using conversion_cost = std::uint32_t;
    using conversion_edge_id = std::uint32_t;

    struct conversion_edge {
        conversion_vertex from;
        conversion_vertex to;
        conversion_cost cost;
        conversion_edge_id id; // dense index in internal edge table
    };

    // ---------------------------------------------------------------------------
    // conversion_graph
    // ---------------------------------------------------------------------------
    // Directed weighted graph with self-contained Dijkstra (non-negative uint32).
    // Allocates nothing until first add_conversion — zero-cost when unused.
    // ---------------------------------------------------------------------------
    class conversion_graph {
    public:
        conversion_graph() = default;

        // Add directed weighted edge. Multiple edges between same pair allowed.
        // Returns stable edge id (== index in internal edge table).
        conversion_edge_id add_conversion(conversion_vertex from,
                                          conversion_vertex to,
                                          conversion_cost cost) {
            auto id = static_cast<conversion_edge_id>(edges_.size());
            edges_.push_back({from, to, cost, id});
            adj_[from].push_back(id);
            // ensure 'to' is known to vertex_count (lazy insert)
            adj_.try_emplace(to);
            return id;
        }

        // Dijkstra. Returns ordered edge-id list (cheapest path from→to).
        // from==to  → empty vector (present, cost 0).
        // unreachable → nullopt.
        [[nodiscard]] std::optional<std::vector<conversion_edge_id>>
        least_cost_path(conversion_vertex from, conversion_vertex to) const {
            if (from == to)
                return std::vector<conversion_edge_id>{};

            auto it = adj_.find(from);
            if (it == adj_.end())
                return std::nullopt;

            constexpr std::uint64_t kInf = std::numeric_limits<std::uint64_t>::max();

            std::unordered_map<conversion_vertex, std::uint64_t> dist;
            std::unordered_map<conversion_vertex, conversion_edge_id> prev_edge;

            dist.reserve(adj_.size());
            dist[from] = 0;

            // min-heap: (cost, vertex) — tie-break by vertex id for determinism
            using Entry = std::pair<std::uint64_t, conversion_vertex>;
            std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;
            heap.push({0, from});

            while (!heap.empty()) {
                auto [d, u] = heap.top();
                heap.pop();

                auto dist_u = dist.find(u);
                if (dist_u == dist.end() || d > dist_u->second)
                    continue; // stale entry

                if (u == to) {
                    // reconstruct path by back-walking prev_edge
                    std::vector<conversion_edge_id> path;
                    conversion_vertex cur = to;
                    while (cur != from) {
                        auto pe = prev_edge.find(cur);
                        if (pe == prev_edge.end()) break; // unreachable (safety)
                        conversion_edge_id eid = pe->second;
                        path.push_back(eid);
                        cur = edges_[eid].from;
                    }
                    std::ranges::reverse(path);
                    return path;
                }

                auto adj_it = adj_.find(u);
                if (adj_it == adj_.end()) continue;

                for (conversion_edge_id eid : adj_it->second) {
                    const auto& e = edges_[eid];
                    if (e.from != u) continue; // should never happen but guard

                    // accumulate in uint64 to avoid overflow
                    std::uint64_t nd = d + static_cast<std::uint64_t>(e.cost);
                    if (nd >= kInf) continue; // unreachably expensive

                    auto& dv = [&]() -> std::uint64_t& {
                        auto [pos, _] = dist.try_emplace(e.to, kInf);
                        return pos->second;
                    }();

                    if (nd < dv) {
                        dv = nd;
                        prev_edge[e.to] = eid;
                        heap.push({nd, e.to});
                    }
                }
            }

            return std::nullopt; // to never reached
        }

        // Sum of edge costs along a resolved path (uint64 accumulate, saturate to uint32).
        [[nodiscard]] conversion_cost
        path_cost(const std::vector<conversion_edge_id>& path) const noexcept {
            std::uint64_t total = 0;
            for (conversion_edge_id eid : path)
                total += static_cast<std::uint64_t>(edges_[eid].cost);
            constexpr std::uint64_t kMax = std::numeric_limits<conversion_cost>::max();
            return static_cast<conversion_cost>(total > kMax ? kMax : total);
        }

        [[nodiscard]] const conversion_edge& edge(conversion_edge_id id) const noexcept {
            return edges_[id];
        }

        [[nodiscard]] std::size_t vertex_count() const noexcept { return adj_.size(); }
        [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }
        [[nodiscard]] bool empty() const noexcept { return edges_.empty(); }

    private:
        std::unordered_map<conversion_vertex, std::vector<conversion_edge_id>> adj_;
        std::vector<conversion_edge> edges_;
    };
} // namespace containers
