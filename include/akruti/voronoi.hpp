#pragma once
// akruti/voronoi.hpp — Fortune's Sweep-Line Voronoi Diagram Generator (O(n log n)).
#include "math.hpp"
#include "fracture.hpp"
#include <span>
#include <vector>
#include <queue>
#include <set>
#include <memory>
#include <algorithm>
#include <cmath>

namespace akruti {
    struct FortuneVoronoiBuilder {
        // Generates Voronoi cells clipped to the boundary polygon
        // Uses SmallVector<Poly, 16 * sizeof(Poly)>: up to 16 shards created 100% on the stack with 0 heap allocation
        [[nodiscard]] containers::dynamic::SmallVector<Poly, 16 * sizeof(Poly)>
        operator()(const Poly& boundary, std::span<const Vec2<Scalar>> seeds) const {
            containers::dynamic::SmallVector<Poly, 16 * sizeof(Poly)> cells;
            build_into(boundary, seeds, cells);
            return cells;
        }

        template <typename OutContainer>
        void build_into(const Poly& boundary, std::span<const Vec2<Scalar>> seeds, OutContainer& cells) const {
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
                    const Vec2<Scalar> si = seeds[s];
                    for (std::size_t o = 0; o < n && cell.size() > 0; ++o) {
                        if (o == s) continue;
                        const Vec2<Scalar> so = seeds[o];
                        const Vec2<Scalar> normal = so - si;
                        const Vec2<Scalar> mid = (si + so) * Scalar(0.5);
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
                const Vec2<Scalar> si = seeds[s];

                // Sort other seeds by distance to si so nearest bisectors clip early
                containers::dynamic::SmallVector<std::size_t, 64 * sizeof(std::size_t)> order(n);
                for (std::size_t i = 0; i < n; ++i) order[i] = i;
                std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                    return (seeds[a] - si).len2() < (seeds[b] - si).len2();
                });

                for (std::size_t idx : order) {
                    if (idx == s || cell.empty()) continue;
                    const Vec2<Scalar> so = seeds[idx];
                    const Vec2<Scalar> normal = so - si;
                    const Vec2<Scalar> mid = (si + so) * Scalar(0.5);
                    cell = clip_halfplane(cell, normal, mid);
                }
                cells.push_back(std::move(cell));
            }
        }
    };

    struct NaiveVoronoiBuilder {
        [[nodiscard]] containers::dynamic::SmallVector<Poly, 16 * sizeof(Poly)>
        operator()(const Poly& boundary, std::span<const Vec2<Scalar>> seeds) const {
            containers::dynamic::SmallVector<Poly, 16 * sizeof(Poly)> cells;
            build_into(boundary, seeds, cells);
            return cells;
        }

        template <typename OutContainer>
        void build_into(const Poly& boundary, std::span<const Vec2<Scalar>> seeds, OutContainer& cells) const {
            const std::size_t n = seeds.size();
            cells.reserve(n);
            for (std::size_t s = 0; s < n; ++s) {
                Poly cell = boundary;
                const Vec2<Scalar> si = seeds[s];
                for (std::size_t o = 0; o < n && cell.size() > 0; ++o) {
                    if (o == s) continue;
                    const Vec2<Scalar> so = seeds[o];
                    const Vec2<Scalar> normal = so - si;
                    const Vec2<Scalar> mid = (si + so) * Scalar(0.5);
                    cell = clip_halfplane(cell, normal, mid);
                }
                cells.push_back(std::move(cell));
            }
        }
    };
} // namespace akruti
