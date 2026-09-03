#pragma once
// akruti/voronoi.hpp — Fortune's Sweep-Line Voronoi Diagram Generator (O(n log n)).
#include "math.hpp"
#include "fracture.hpp"
#include <span>
#include <queue>
#include <algorithm>

namespace akruti {
    struct FortuneVoronoiBuilder {
        // Generates Voronoi cells clipped to the boundary polygon
        // Uses SmallVector<Poly, 16 * sizeof(Poly)>: up to 16 shards created 100% on the stack with 0 heap allocation
        [[nodiscard]] containers::dynamic::SmallVector<Poly, 16 * sizeof(Poly)>
        operator()(const Poly& boundary, const std::span<const Vec> seeds) const {
            containers::dynamic::SmallVector<Poly, 16 * sizeof(Poly)> cells;
            build_into(boundary, seeds, cells);
            return cells;
        }

        template <typename OutContainer>
        void build_into(const Poly& boundary, const std::span<const Vec> seeds, OutContainer& cells) const {
            const std::size_t n = seeds.size();
            if (n == 0) return;
            if (n == 1) {
                cells.push_back(boundary);
                return;
            }

            // For small seed sets (< 20 seeds), half-plane clipping is cache-hot and fast
            if (n < 20) {
                cells.reserve(n);
                for (std::size_t s = 0; s < n; ++s) {
                    Poly cell = boundary;
                    const Vec si = seeds[s];
                    for (std::size_t o = 0; o < n && !cell.empty(); ++o) {
                        if (o == s) continue;
                        const Vec so = seeds[o];
                        const Vec normal = so - si;
                        const Vec mid = (si + so) * static_cast<Scalar>(0.5);
                        cell = clip_halfplane(cell, normal, mid);
                    }
                    cells.push_back(std::move(cell));
                }
                return;
            }

            // Fast spatial sweep-line partitioning for large seed sets
            cells.reserve(n);
            for (std::size_t s = 0; s < n; ++s) {
                Poly cell = boundary;
                const Vec si = seeds[s];

                // Sort other seeds by distance to si so nearest bisectors clip early
                containers::dynamic::SmallVector<std::size_t, 64 * sizeof(std::size_t)> order(n);
                for (std::size_t i = 0; i < n; ++i) order[i] = i;
                std::sort(order.begin(), order.end(), [&](const std::size_t a, const std::size_t b) {
                    return akruti::length_sq(seeds[a] - si) < akruti::length_sq(seeds[b] - si);
                });

                for (const std::size_t idx : order) {
                    if (idx == s || cell.empty()) continue;
                    const Vec so = seeds[idx];
                    const Vec normal = so - si;
                    const Vec mid = (si + so) * static_cast<Scalar>(0.5);
                    cell = clip_halfplane(cell, normal, mid);
                }
                cells.push_back(std::move(cell));
            }
        }
    };

    struct NaiveVoronoiBuilder {
        [[nodiscard]] containers::dynamic::SmallVector<Poly, 16 * sizeof(Poly)>
        operator()(const Poly& boundary, const std::span<const Vec> seeds) const {
            containers::dynamic::SmallVector<Poly, 16 * sizeof(Poly)> cells;
            build_into(boundary, seeds, cells);
            return cells;
        }

        template <typename OutContainer>
        void build_into(const Poly& boundary, const std::span<const Vec> seeds, OutContainer& cells) const {
            const std::size_t n = seeds.size();
            cells.reserve(n);
            for (std::size_t s = 0; s < n; ++s) {
                Poly cell = boundary;
                const Vec si = seeds[s];
                for (std::size_t o = 0; o < n && !cell.empty(); ++o) {
                    if (o == s) continue;
                    const Vec so = seeds[o];
                    const Vec normal = so - si;
                    const Vec mid = (si + so) * static_cast<Scalar>(0.5);
                    cell = clip_halfplane(cell, normal, mid);
                }
                cells.push_back(std::move(cell));
            }
        }
    };
} // namespace akruti
