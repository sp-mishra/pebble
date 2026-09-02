#ifndef LITEGRAPH_HIGHWAY_HPP
#define LITEGRAPH_HIGHWAY_HPP

#include "LiteGraphAlgorithms.hpp"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#ifdef LITEGRAPH_ENABLE_HIGHWAY
#include <hwy/highway.h>
#endif

namespace litegraph::highway { namespace detail {
        inline void fill_vector(std::vector<double>& v, const double value) {
#ifdef LITEGRAPH_ENABLE_HIGHWAY
            const HWY_FULL (double) d;
            const std::size_t lanes = hwy::Lanes(d);
            const auto vv = hwy::Set(d, value);

            std::size_t i = 0;
            for (; i + lanes <= v.size(); i += lanes) {
                hwy::StoreU(vv, d, v.data() + i);
            }
            for (; i < v.size(); ++i) {
                v[i] = value;
            }
#else
            std::fill(v.begin(), v.end(), value);
#endif
        }

        inline void scale_vector(std::vector<double>& v, const double factor) {
#ifdef LITEGRAPH_ENABLE_HIGHWAY
            const HWY_FULL (double) d;
            const std::size_t lanes = hwy::Lanes(d);
            const auto vf = hwy::Set(d, factor);

            std::size_t i = 0;
            for (; i + lanes <= v.size(); i += lanes) {
                const auto x = hwy::LoadU(d, v.data() + i);
                hwy::StoreU(hwy::Mul(x, vf), d, v.data() + i);
            }
            for (; i < v.size(); ++i) {
                v[i] *= factor;
            }
#else
            for (double& x : v) x *= factor;
#endif
        }

        inline void add_scaled(std::vector<double>& dst,
                               const std::vector<double>& src,
                               const double scale) {
#ifdef LITEGRAPH_ENABLE_HIGHWAY
            const HWY_FULL (double) d;
            const std::size_t lanes = hwy::Lanes(d);
            const auto vs = hwy::Set(d, scale);

            std::size_t i = 0;
            for (; i + lanes <= dst.size(); i += lanes) {
                const auto vd = hwy::LoadU(d, dst.data() + i);
                const auto vx = hwy::LoadU(d, src.data() + i);
                hwy::StoreU(hwy::Add(vd, hwy::Mul(vx, vs)), d, dst.data() + i);
            }
            for (; i < dst.size(); ++i) {
                dst[i] += src[i] * scale;
            }
#else
            for (std::size_t i = 0; i < dst.size(); ++i) {
                dst[i] += src[i] * scale;
            }
#endif
        }

        [[nodiscard]] inline double l1_delta(const std::vector<double>& a,
                                             const std::vector<double>& b) {
#ifdef LITEGRAPH_ENABLE_HIGHWAY
            const HWY_FULL (double) d;
            const std::size_t lanes = hwy::Lanes(d);
            auto vacc = hwy::Zero(d);

            std::size_t i = 0;
            for (; i + lanes <= a.size(); i += lanes) {
                const auto va = hwy::LoadU(d, a.data() + i);
                const auto vb = hwy::LoadU(d, b.data() + i);
                vacc = hwy::Add(vacc, hwy::Abs(hwy::Sub(va, vb)));
            }

            double sum = hwy::GetLane(hwy::SumOfLanes(d, vacc));
            for (; i < a.size(); ++i) {
                sum += std::abs(a[i] - b[i]);
            }
            return sum;
#else
            double sum = 0.0;
            for (std::size_t i = 0; i < a.size(); ++i) {
                sum += std::abs(a[i] - b[i]);
            }
            return sum;
#endif
        }
    } // namespace detail

    [[nodiscard]] constexpr bool enabled() noexcept {
#ifdef LITEGRAPH_ENABLE_HIGHWAY
        return true;
#else
        return false;
#endif
    }

    [[nodiscard]] inline std::int64_t supported_targets_mask() noexcept {
#ifdef LITEGRAPH_ENABLE_HIGHWAY
        return hwy::SupportedTargets();
#else
        return 0;
#endif
    }

    namespace experimental {
        // Experimental helper for weighted-CSR block relaxation.
        // Computes candidate = source_distance + weight[i] with SIMD where available,
        // then applies scalar indirect min-updates on distance[target[i]].
        template <typename EdgeT, DirectednessTag Directedness>
            requires std::is_arithmetic_v<EdgeT>
        void relax_weighted_edges_block(
            const CsrGraph<EdgeT, Directedness>& g,
            const std::size_t edge_begin,
            const std::size_t edge_end,
            const double source_distance,
            std::vector<double>& distances,
            const std::optional<std::size_t> source_compact = std::nullopt,
            std::vector<std::optional<std::size_t>>* predecessors = nullptr
        ) {
            if (!g.has_edge_weights()) return;
            if (edge_begin >= edge_end || edge_begin >= g.edge_count()) return;

            const std::size_t clamped_end = std::min(edge_end, g.edge_count());
            const auto& targets = g.targets();
            const auto weights = g.edge_weights();

#ifdef LITEGRAPH_ENABLE_HIGHWAY
            std::vector<double> candidates(clamped_end - edge_begin); {
                const HWY_FULL (


double) d;
                const std::size_t lanes = hwy::Lanes(d);
                const auto vs = hwy::Set(d, source_distance);

                std::size_t j = 0;
                for (; j + lanes <= candidates.size(); j += lanes) {
                    const auto w = hwy::LoadU(d, weights.data() + edge_begin + j);
                    hwy::StoreU(hwy::Add(vs, w), d, candidates.data() + j);
                }
                for (; j < candidates.size(); ++j) {
                    candidates[j] = source_distance + weights[edge_begin + j];
                }
            }

            for (std::size_t j = 0; j < candidates.size(); ++j) {
                const std::size_t idx = edge_begin + j;
                const std::size_t target = targets[idx].value;
                if (target >= distances.size()) continue;
                if (candidates[j] < distances[target]) {
                    distances[target] = candidates[j];
                    if (predecessors && source_compact) {
                        (*predecessors)[target] = *source_compact;
                    }
                }
            }
#else
            for (std::size_t idx = edge_begin; idx < clamped_end; ++idx) {
                const std::size_t target = targets[idx].value;
                if (target >= distances.size()) continue;
                const double candidate = source_distance + weights[idx];
                if (candidate < distances[target]) {
                    distances[target] = candidate;
                    if (predecessors && source_compact) {
                        (*predecessors)[target] = *source_compact;
                    }
                }
            }
#endif
        }
    } // namespace experimental

    // Optional boundary: callers can include this header and opt-in to Highway
    // without changing the core serial algorithm API.
    template <typename EdgeT, DirectednessTag Directedness>
    CsrPageRankResult pagerank(const CsrGraph<EdgeT, Directedness>& g,
                               const CsrPageRankOptions& options = {}) {
#ifdef LITEGRAPH_ENABLE_HIGHWAY
        if (options.damping_factor < 0.0 || options.damping_factor > 1.0) {
            throw std::invalid_argument("PageRank damping_factor must be in [0, 1].");
        }

        const std::size_t n = g.node_count();
        if (n == 0) {
            return {};
        }

        const auto& offsets = g.offsets();
        const auto& targets = g.targets();

        std::vector<double> rank(n, 0.0);
        std::vector<double> next_rank(n, 0.0);
        std::vector<double> ones(n, 0.0);

        detail::fill_vector(rank, 1.0);
        detail::scale_vector(rank, 1.0 / static_cast<double>(n));
        detail::fill_vector(ones, 1.0);

        CsrPageRankResult result;
        result.ranks = rank;

        const double d = options.damping_factor;

        // Gather (pull) path: read-only per-node reduction over the reverse CSR.
        // Contributions are precomputed with SIMD; the indexed sum is scalar but
        // reads a contiguous slice of in_sources per node.
        const bool gather = g.has_incoming();
        std::vector<double> contrib;
        if (gather) contrib.assign(n, 0.0);

        for (std::size_t iter = 0; iter < options.max_iterations; ++iter) {
            double dangling_mass = 0.0;
            for (std::size_t u = 0; u < n; ++u) {
                if (offsets[u] == offsets[u + 1]) {
                    dangling_mass += rank[u];
                }
            }

            const double teleport = (1.0 - d) / static_cast<double>(n);

            if (gather) {
                const auto& in_offsets = g.in_offsets();
                const auto& in_sources = g.in_sources();
                const double base = teleport + d * dangling_mass / static_cast<double>(n);

                // contrib[u] = d * rank[u] / out_degree(u), 0 for dangling nodes.
                for (std::size_t u = 0; u < n; ++u) {
                    const std::size_t out_degree = offsets[u + 1] - offsets[u];
                    contrib[u] = out_degree == 0
                                     ? 0.0
                                     : d * rank[u] / static_cast<double>(out_degree);
                }
                for (std::size_t v = 0; v < n; ++v) {
                    double acc = base;
                    for (std::size_t k = in_offsets[v]; k < in_offsets[v + 1]; ++k) {
                        acc += contrib[in_sources[k].value];
                    }
                    next_rank[v] = acc;
                }
            }
            else {
                detail::fill_vector(next_rank, teleport);

                if (dangling_mass != 0.0) {
                    detail::add_scaled(next_rank, ones, d * dangling_mass / static_cast<double>(n));
                }

                for (std::size_t u = 0; u < n; ++u) {
                    const std::size_t out_degree = offsets[u + 1] - offsets[u];
                    if (out_degree == 0) continue;

                    const double contrib_u = d * rank[u] / static_cast<double>(out_degree);
                    for (std::size_t i = offsets[u]; i < offsets[u + 1]; ++i) {
                        next_rank[targets[i].value] += contrib_u;
                    }
                }
            }

            const double delta = detail::l1_delta(next_rank, rank);
            rank.swap(next_rank);
            result.iterations = iter + 1;

            if (delta <= options.tolerance) {
                result.converged = true;
                break;
            }
        }

        result.ranks = std::move(rank);
        return result;
#endif
        // Fallback returns same CsrPageRankResult type.
        return litegraph::pagerank(g, options);
    }
} // namespace litegraph::highway

#endif // LITEGRAPH_HIGHWAY_HPP

