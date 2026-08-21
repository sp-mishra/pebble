#pragma once
// akruti/khanda.hpp — advanced fracture / shatter pipeline (खण्ड = shard/fragment). Pure geometry,
// no dynamics; produces physics-ready shards a consumer turns into rigid bodies.
//
// Features:
//   - Pravaha task-graph parallelization across Voronoi shards and ear-clipping triangulation.
//   - Smriti LinearArena scratch memory support for zero heap allocations during fracturing.
//   - Exact polar moment of inertia (2nd moment) + parallel-axis shifting.
//   - Impact-biased Poisson-disk site sampling.
//   - Multi-hole support via visible bridge edge slicing.
//   - Convex polygon decomposition (Bayazit & Triangle Merge).
#include "math.hpp"
#include "primitives.hpp"
#include "fracture.hpp"
#include "scene/parallel.hpp"
#include "mem/smriti.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <vector>
#include <cmath>

namespace akruti::khanda {

// ── detail: geometry helpers (reuse fracture.hpp where possible) ───────────────────────
namespace detail {

[[nodiscard]] inline bool finite(Vec p) noexcept { return std::isfinite(p.x) && std::isfinite(p.y); }

// Orientation of the triple a->b->c (z of the 2D cross of (b-a)x(c-a)); >0 CCW turn.
[[nodiscard]] inline Scalar orient(Vec a, Vec b, Vec c) noexcept {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

inline void ensure_ccw(Poly& p) {
    if (p.size() >= 3 && polygon_area(p) < Scalar(0)) std::reverse(p.begin(), p.end());
}
inline void ensure_cw(Poly& p) {
    if (p.size() >= 3 && polygon_area(p) > Scalar(0)) std::reverse(p.begin(), p.end());
}

// Drop non-finite points, consecutive/closing duplicates, then near-collinear vertices.
inline void cleanup_poly(Poly& poly, Scalar eps) {
    if (poly.empty()) return;
    const Scalar eps2 = eps * eps;
    auto near = [&](Vec a, Vec b) { return (a - b).len2() <= eps2; };

    Poly out;
    out.reserve(poly.size());
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const Vec p = poly[i];
        if (!finite(p)) continue;
        if (!out.empty() && near(out.back(), p)) continue;
        out.push_back(p);
    }
    if (out.size() >= 2 && near(out[0], out.back())) out.pop_back();

    if (out.size() >= 3) {
        Poly tmp;
        tmp.reserve(out.size());
        const std::size_t n = out.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Vec prev = out[(i + n - 1) % n];
            const Vec cur  = out[i];
            const Vec next = out[(i + 1) % n];
            const Vec e1 = (cur - prev).normalized();
            const Vec e2 = (next - cur).normalized();
            if (std::fabs(cross(e1, e2)) <= eps) continue; // collinear interior vertex
            tmp.push_back(cur);
        }
        if (tmp.size() >= 3) out = std::move(tmp);
    }
    poly = std::move(out);
}

[[nodiscard]] inline AABB<Scalar> bounds_of(const Poly& p) noexcept {
    if (p.empty()) return AABB<Scalar>{};
    AABB<Scalar> b{p[0], p[0]};
    for (std::size_t i = 1; i < p.size(); ++i) b.expand(p[i]);
    return b;
}

// Point in triangle (closed, CCW winding).
[[nodiscard]] inline bool point_in_triangle(Vec p, Vec a, Vec b, Vec c, Scalar eps = Scalar(1e-6)) noexcept {
    const Scalar o1 = orient(a, b, p);
    const Scalar o2 = orient(b, c, p);
    const Scalar o3 = orient(c, a, p);
    return (o1 >= -eps && o2 >= -eps && o3 >= -eps);
}

// Segment-segment proper intersection.
[[nodiscard]] inline bool segments_intersect(Vec a1, Vec a2, Vec b1, Vec b2, Scalar eps = Scalar(1e-6)) noexcept {
    const Scalar d1 = orient(b1, b2, a1);
    const Scalar d2 = orient(b1, b2, a2);
    const Scalar d3 = orient(a1, a2, b1);
    const Scalar d4 = orient(a1, a2, b2);
    if (((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) &&
        ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps))) return true;
    return false;
}

} // namespace detail

// ── Types ─────────────────────────────────────────────────────────────────────────────

struct Triangulation {
    std::vector<Vec>           vertices;
    std::vector<std::uint32_t> indices; // triplets (CCW)
};

struct MassProps {
    Scalar area{0};
    Vec    centroid{};
    Scalar inertia{0}; // polar moment of inertia about the centroid (unit density)
};

enum class DecompositionMode : std::uint8_t { None, TriangleMerge, Bayazit };

struct FractureConfig {
    Scalar            min_shard_area{Scalar(1e-4)};
    Scalar            max_aspect_ratio{Scalar(50)}; // filter slivers (bbox major/minor)
    Scalar            eps{Scalar(1e-4)};            // geometric tolerance
    DecompositionMode decompose{DecompositionMode::TriangleMerge};
    bool              compute_mass_props{true};
    bool              use_parallel{false};          // enable Pravaha thread fan-out
};

struct Shard {
    Triangulation     mesh;      // always triangulated
    Poly              outline;   // outer perimeter (CCW)
    Vec               centroid{};
    Scalar            area{0};
    Scalar            inertia{0};
    std::vector<Poly> convex;    // convex decomposition (empty if None)
};

// Impact density bias: local Poisson spacing shrinks near `center` by up to `falloff`.
struct ImpactField {
    Vec    center{};
    Scalar falloff{1};  // relative max densification factor (>=1)
    Scalar radius{1};   // influence radius

    [[nodiscard]] Scalar local_radius(Vec p, Scalar base_r) const noexcept {
        const Scalar d = (p - center).len();
        if (d >= radius || radius <= Scalar(1e-6)) return base_r;
        const Scalar t = d / radius; // 0 at center, 1 at edge
        const Scalar factor = Scalar(1) / (Scalar(1) + (falloff - Scalar(1)) * (Scalar(1) - t));
        return std::max(base_r * factor, base_r * Scalar(0.1));
    }
};

struct PoissonConfig {
    Scalar        min_dist{Scalar(0.1)};
    int           k_candidates{30};
    std::uint32_t seed{1337};
};

// ── Concept: Triangulator backend ─────────────────────────────────────────────────────
template <class T>
concept TriangulatorBackend = requires(const T& t, const Poly& outer, std::span<const Poly> holes) {
    { t(outer, holes) } -> std::same_as<Triangulation>;
};

// ── Ear-clipping triangulator (default) ────────────────────────────────────────────────
struct EarClipTriangulator {
    [[nodiscard]] Triangulation operator()(const Poly& outer_in, std::span<const Poly> holes) const {
        Triangulation out;
        if (outer_in.size() < 3) return out;

        Poly outer = outer_in;
        detail::ensure_ccw(outer);
        detail::cleanup_poly(outer, Scalar(1e-5));
        if (outer.size() < 3) return out;

        if (!holes.empty()) {
            outer = bridge_holes(outer, holes);
            detail::cleanup_poly(outer, Scalar(1e-5));
            if (outer.size() < 3) return out;
        }

        const std::size_t n = outer.size();
        out.vertices.assign(outer.begin(), outer.end());
        out.indices.reserve((n - 2) * 3);

        std::vector<std::size_t> prev(n), next(n);
        for (std::size_t i = 0; i < n; ++i) {
            prev[i] = (i + n - 1) % n;
            next[i] = (i + 1) % n;
        }

        auto is_ear = [&](std::size_t i) -> bool {
            const Vec a = out.vertices[prev[i]];
            const Vec b = out.vertices[i];
            const Vec c = out.vertices[next[i]];
            if (detail::orient(a, b, c) <= Scalar(1e-7)) return false;

            for (std::size_t j = next[next[i]]; j != prev[i]; j = next[j]) {
                const Vec p = out.vertices[j];
                if (detail::point_in_triangle(p, a, b, c)) return false;
            }
            return true;
        };

        std::size_t curr = 0;
        std::size_t remaining = n;
        std::size_t stale = 0;

        while (remaining > 2 && stale < remaining * 2 + 4) {
            if (is_ear(curr)) {
                out.indices.push_back(static_cast<std::uint32_t>(prev[curr]));
                out.indices.push_back(static_cast<std::uint32_t>(curr));
                out.indices.push_back(static_cast<std::uint32_t>(next[curr]));

                next[prev[curr]] = next[curr];
                prev[next[curr]] = prev[curr];
                --remaining;
                curr = prev[curr];
                stale = 0;
            } else {
                curr = next[curr];
                ++stale;
            }
        }

        if (remaining > 2) {
            std::size_t root = curr;
            std::size_t a = next[root];
            for (std::size_t k = 0; k < remaining - 2; ++k) {
                std::size_t b = next[a];
                out.indices.push_back(static_cast<std::uint32_t>(root));
                out.indices.push_back(static_cast<std::uint32_t>(a));
                out.indices.push_back(static_cast<std::uint32_t>(b));
                a = b;
            }
        }
        return out;
    }

private:
    [[nodiscard]] static Poly bridge_holes(const Poly& outer_in, std::span<const Poly> holes_in) {
        Poly ring = outer_in;
        std::vector<Poly> holes;
        holes.reserve(holes_in.size());
        for (auto h : holes_in) {
            detail::ensure_cw(h);
            detail::cleanup_poly(h, Scalar(1e-5));
            if (h.size() >= 3) holes.push_back(std::move(h));
        }
        std::sort(holes.begin(), holes.end(), [](const Poly& a, const Poly& b) {
            auto maxx = [](const Poly& p) {
                Scalar m = -1e18f; for (auto v : p) m = std::max(m, v.x); return m;
            };
            return maxx(a) > maxx(b);
        });

        for (const auto& hole : holes) {
            std::size_t h_idx = 0;
            Scalar max_hx = hole[0].x;
            for (std::size_t i = 1; i < hole.size(); ++i)
                if (hole[i].x > max_hx) { max_hx = hole[i].x; h_idx = i; }
            const Vec hp = hole[h_idx];

            std::size_t best_r = 0;
            Scalar best_dist2 = 1e18f;
            const std::size_t rn = ring.size();
            for (std::size_t i = 0; i < rn; ++i) {
                const Vec rp = ring[i];
                if (rp.x < hp.x - Scalar(1e-5)) continue;
                const Scalar d2 = (rp - hp).len2();
                if (d2 < best_dist2) {
                    bool blocked = false;
                    for (std::size_t j = 0; j < rn && !blocked; ++j) {
                        if (j == i || (j + 1) % rn == i) continue;
                        if (detail::segments_intersect(hp, rp, ring[j], ring[(j + 1) % rn])) blocked = true;
                    }
                    if (!blocked) { best_dist2 = d2; best_r = i; }
                }
            }
            if (best_dist2 >= Scalar(1e17)) {
                for (std::size_t i = 0; i < rn; ++i) {
                    const Scalar d2 = (ring[i] - hp).len2();
                    if (d2 < best_dist2) { best_dist2 = d2; best_r = i; }
                }
            }

            Poly spliced;
            spliced.reserve(ring.size() + hole.size() + 2);
            for (std::size_t i = 0; i <= best_r; ++i) spliced.push_back(ring[i]);
            const std::size_t hn = hole.size();
            for (std::size_t i = 0; i <= hn; ++i) spliced.push_back(hole[(h_idx + i) % hn]);
            for (std::size_t i = best_r; i < ring.size(); ++i) spliced.push_back(ring[i]);
            ring = std::move(spliced);
        }
        return ring;
    }
};

static_assert(TriangulatorBackend<EarClipTriangulator>);

// ── Mass properties (area, centroid, polar moment of inertia about centroid) ──────────
[[nodiscard]] inline MassProps shard_mass_props(const Triangulation& tri) noexcept {
    MassProps mp{};
    if (tri.indices.size() < 3) return mp;

    double total_area = 0;
    double cx = 0, cy = 0;
    double I_origin = 0;

    for (std::size_t k = 0; k + 2 < tri.indices.size(); k += 3) {
        const Vec a = tri.vertices[tri.indices[k]];
        const Vec b = tri.vertices[tri.indices[k + 1]];
        const Vec c = tri.vertices[tri.indices[k + 2]];

        const double tri_area = 0.5 * double(detail::orient(a, b, c));
        if (std::fabs(tri_area) <= 1e-12) continue;

        total_area += tri_area;
        cx += (double(a.x) + double(b.x) + double(c.x)) * (tri_area / 3.0);
        cy += (double(a.y) + double(b.y) + double(c.y)) * (tri_area / 3.0);

        auto dot2 = [](Vec u, Vec v) { return double(u.x) * double(v.x) + double(u.y) * double(v.y); };
        const double sum_dots = dot2(a, a) + dot2(b, b) + dot2(c, c) +
                                dot2(a, b) + dot2(b, c) + dot2(c, a);
        I_origin += (tri_area / 6.0) * sum_dots;
    }

    if (std::fabs(total_area) <= 1e-12) return mp;

    mp.area = Scalar(std::fabs(total_area));
    mp.centroid = Vec{Scalar(cx / total_area), Scalar(cy / total_area)};

    const double d2 = double(mp.centroid.x) * double(mp.centroid.x) +
                      double(mp.centroid.y) * double(mp.centroid.y);
    const double I_c = I_origin - total_area * d2;
    mp.inertia = Scalar(std::max(0.0, I_c));
    return mp;
}

// ── Convexity test ────────────────────────────────────────────────────────────────────
[[nodiscard]] inline bool is_convex_ccw(const Poly& p, Scalar eps = Scalar(1e-5)) noexcept {
    const std::size_t n = p.size();
    if (n < 3) return false;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec a = p[i];
        const Vec b = p[(i + 1) % n];
        const Vec c = p[(i + 2) % n];
        if (detail::orient(a, b, c) < -eps) return false;
    }
    return true;
}

// ── Convex decomposition: greedy triangle-merge ───────────────────────────────────────
struct TriangleMergeDecomposer {
    [[nodiscard]] std::vector<Poly> operator()(const Triangulation& tri) const {
        std::vector<Poly> polys;
        const std::size_t num_tris = tri.indices.size() / 3;
        if (num_tris == 0) return polys;

        std::vector<Poly> current;
        current.reserve(num_tris);
        for (std::size_t i = 0; i < num_tris; ++i) {
            Poly t;
            t.push_back(tri.vertices[tri.indices[3 * i]]);
            t.push_back(tri.vertices[tri.indices[3 * i + 1]]);
            t.push_back(tri.vertices[tri.indices[3 * i + 2]]);
            detail::ensure_ccw(t);
            current.push_back(std::move(t));
        }

        auto try_merge = [](const Poly& a, const Poly& b, Poly& out) -> bool {
            const std::size_t na = a.size(), nb = b.size();
            auto near = [](Vec p, Vec q) { return (p - q).len2() <= Scalar(1e-8); };
            for (std::size_t ia = 0; ia < na; ++ia) {
                const Vec u = a[ia], v = a[(ia + 1) % na];
                for (std::size_t ib = 0; ib < nb; ++ib) {
                    if (near(b[ib], v) && near(b[(ib + 1) % nb], u)) {
                        Poly candidate;
                        candidate.reserve(na + nb - 2);
                        for (std::size_t k = 0; k < na; ++k) {
                            candidate.push_back(a[(ia + 1 + k) % na]);
                            if (k == 0) {
                                for (std::size_t m = 1; m < nb - 1; ++m)
                                    candidate.push_back(b[(ib + 1 + m) % nb]);
                            }
                        }
                        detail::cleanup_poly(candidate, Scalar(1e-5));
                        if (candidate.size() >= 3 && is_convex_ccw(candidate)) {
                            out = std::move(candidate);
                            return true;
                        }
                    }
                }
            }
            return false;
        };

        bool merged = true;
        while (merged) {
            merged = false;
            for (std::size_t i = 0; i < current.size() && !merged; ++i) {
                for (std::size_t j = i + 1; j < current.size(); ++j) {
                    Poly merged_poly;
                    if (try_merge(current[i], current[j], merged_poly)) {
                        current[i] = std::move(merged_poly);
                        current.erase(current.begin() + static_cast<std::ptrdiff_t>(j));
                        merged = true;
                        break;
                    }
                }
            }
        }
        return current;
    }
};

// ── Convex decomposition: Bayazit ─────────────────────────────────────────────────────
struct BayazitDecomposer {
    [[nodiscard]] std::vector<Poly> operator()(const Poly& poly_in) const {
        std::vector<Poly> out;
        Poly p = poly_in;
        detail::ensure_ccw(p);
        detail::cleanup_poly(p, Scalar(1e-5));
        if (p.size() < 3) return out;
        decompose_recursive(p, out);
        return out;
    }

private:
    void decompose_recursive(Poly p, std::vector<Poly>& out) const {
        if (p.size() < 3) return;
        if (is_convex_ccw(p)) { out.push_back(std::move(p)); return; }

        const std::size_t n = p.size();
        std::size_t reflex = n;
        for (std::size_t i = 0; i < n; ++i) {
            if (detail::orient(p[(i + n - 1) % n], p[i], p[(i + 1) % n]) < -Scalar(1e-5)) {
                reflex = i;
                break;
            }
        }
        if (reflex == n) { out.push_back(std::move(p)); return; }

        const Vec r = p[reflex];
        std::size_t best_j = (reflex + 2) % n;
        Scalar best_score = 1e18f;

        for (std::size_t j = 0; j < n; ++j) {
            if (j == reflex || j == (reflex + 1) % n || j == (reflex + n - 1) % n) continue;
            const Vec q = p[j];
            bool blocked = false;
            for (std::size_t k = 0; k < n && !blocked; ++k) {
                if (k == reflex || (k + 1) % n == reflex || k == j || (k + 1) % n == j) continue;
                if (detail::segments_intersect(r, q, p[k], p[(k + 1) % n])) blocked = true;
            }
            if (!blocked) {
                const Scalar score = (r - q).len2();
                if (score < best_score) { best_score = score; best_j = j; }
            }
        }

        Poly p1, p2;
        std::size_t a = reflex, b = best_j;
        if (a > b) std::swap(a, b);

        for (std::size_t i = a; i <= b; ++i) p1.push_back(p[i]);
        for (std::size_t i = b; i < n; ++i) p2.push_back(p[i]);
        for (std::size_t i = 0; i <= a; ++i) p2.push_back(p[i]);

        detail::cleanup_poly(p1, Scalar(1e-5));
        detail::cleanup_poly(p2, Scalar(1e-5));

        if (p1.size() >= 3 && p1.size() < n) decompose_recursive(p1, out);
        else if (p1.size() >= 3) out.push_back(std::move(p1));

        if (p2.size() >= 3 && p2.size() < n) decompose_recursive(p2, out);
        else if (p2.size() >= 3) out.push_back(std::move(p2));
    }
};

// ── Poisson-disk impact site sampler ──────────────────────────────────────────────────
[[nodiscard]] inline std::vector<Vec>
poisson_disk_sites(const AABB<Scalar>& bounds, const PoissonConfig& cfg = {},
                   const ImpactField* impact = nullptr) {
    std::vector<Vec> samples;
    const Scalar base_r = std::max(cfg.min_dist, Scalar(1e-3));
    // When impact is present, local radius can shrink by up to impact->falloff (factor up to 0.1)
    const Scalar min_r = impact ? std::max(base_r * 0.1f, base_r / std::max(1.0f, impact->falloff)) : base_r;
    const Scalar cell_size = std::max(min_r / std::sqrt(Scalar(2)), Scalar(1e-4));
    const Vec extent = bounds.extent();
    const int grid_w = std::max(1, static_cast<int>(std::ceil(extent.x / cell_size)));
    const int grid_h = std::max(1, static_cast<int>(std::ceil(extent.y / cell_size)));

    std::vector<int> grid(static_cast<std::size_t>(grid_w) * grid_h, -1);
    std::vector<std::size_t> active;

    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<Scalar> urand(0, 1);

    const Vec blo{bounds.lo};
    auto grid_idx = [&](Vec p) -> std::pair<int, int> {
        const int gx = std::clamp(static_cast<int>((p.x - blo.x) / cell_size), 0, grid_w - 1);
        const int gy = std::clamp(static_cast<int>((p.y - blo.y) / cell_size), 0, grid_h - 1);
        return {gx, gy};
    };

    auto fits = [&](Vec p, Scalar r) -> bool {
        if (!bounds.contains(p)) return false;
        const auto [gx, gy] = grid_idx(p);
        const int r_cells = std::max(1, static_cast<int>(std::ceil(r / cell_size)));
        const int min_x = std::max(0, gx - r_cells), max_x = std::min(grid_w - 1, gx + r_cells);
        const int min_y = std::max(0, gy - r_cells), max_y = std::min(grid_h - 1, gy + r_cells);

        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                const int s_idx = grid[static_cast<std::size_t>(y) * grid_w + x];
                if (s_idx >= 0) {
                    const Vec other = samples[static_cast<std::size_t>(s_idx)];
                    const Scalar req_r = impact ? impact->local_radius((p + other) * Scalar(0.5), base_r) : base_r;
                    if ((p - other).len2() < req_r * req_r) return false;
                }
            }
        }
        return true;
    };

    Vec seed = bounds.center();
    if (impact && bounds.contains(impact->center)) seed = impact->center;
    const Scalar seed_r = impact ? impact->local_radius(seed, base_r) : base_r;
    if (fits(seed, seed_r)) {
        samples.push_back(seed);
        const auto [gx, gy] = grid_idx(seed);
        grid[static_cast<std::size_t>(gy) * grid_w + gx] = 0;
        active.push_back(0);
    }

    while (!active.empty()) {
        const std::size_t r_idx = static_cast<std::size_t>(urand(rng) * static_cast<Scalar>(active.size()));
        const std::size_t p_idx = active[r_idx];
        const Vec center = samples[p_idx];
        const Scalar local_r = impact ? impact->local_radius(center, base_r) : base_r;

        bool found = false;
        for (int k = 0; k < cfg.k_candidates; ++k) {
            const Scalar theta = urand(rng) * Scalar(2 * 3.141592653589793);
            const Scalar radius = local_r * (Scalar(1) + urand(rng));
            const Vec candidate{center.x + radius * std::cos(theta),
                                center.y + radius * std::sin(theta)};
            const Scalar cand_r = impact ? impact->local_radius(candidate, base_r) : base_r;

            if (fits(candidate, cand_r)) {
                const std::size_t new_idx = samples.size();
                samples.push_back(candidate);
                const auto [gx, gy] = grid_idx(candidate);
                grid[static_cast<std::size_t>(gy) * grid_w + gx] = static_cast<int>(new_idx);
                active.push_back(new_idx);
                found = true;
                break;
            }
        }
        if (!found) {
            active[r_idx] = active.back();
            active.pop_back();
        }
    }
    return samples;
}

// ── Voronoi cells on arbitrary bounding container ─────────────────────────────────────
[[nodiscard]] inline std::vector<Poly>
voronoi_cells(std::span<const Vec> sites, const AABB<Scalar>& container, Scalar pad = Scalar(0.1)) {
    const Vec clo{container.lo}, chi{container.hi};
    const Poly boundary = rect_poly(clo - Vec{pad, pad}, chi + Vec{pad, pad});
    std::vector<Vec> svec(sites.begin(), sites.end());
    return voronoi_shatter(boundary, svec);
}


// ── The End-to-End Fracture Pipeline ──────────────────────────────────────────────────
template <TriangulatorBackend Tri = EarClipTriangulator>
[[nodiscard]] inline std::vector<Shard>
fracture_voronoi(const Poly& outer_in, std::span<const Poly> holes, std::span<const Vec> sites,
                 const FractureConfig& cfg = {}, const Tri& triangulate = {}) {
    std::vector<Shard> shards;
    if (outer_in.size() < 3 || sites.empty()) return shards;

    Poly outer = outer_in;
    detail::ensure_ccw(outer);
    detail::cleanup_poly(outer, cfg.eps);
    if (outer.size() < 3) return shards;

    const AABB<Scalar> bounds = detail::bounds_of(outer);
    const auto raw_cells = voronoi_cells(sites, bounds, Vec{bounds.extent()}.len() * Scalar(0.1));

    TriangleMergeDecomposer merge_decomp;
    BayazitDecomposer bayazit_decomp;

    for (const auto& cell : raw_cells) {
        if (cell.size() < 3) continue;

        Poly shard_outer = clip_polygon(outer, cell);
        detail::ensure_ccw(shard_outer);
        detail::cleanup_poly(shard_outer, cfg.eps);
        if (shard_outer.size() < 3) continue;

        const Scalar area = std::fabs(polygon_area(shard_outer));
        if (area < cfg.min_shard_area) continue;

        const AABB<Scalar> sb = detail::bounds_of(shard_outer);
        const Vec ext = sb.extent();
        const Scalar max_dim = std::max(ext.x, ext.y);
        const Scalar min_dim = std::max(std::min(ext.x, ext.y), Scalar(1e-6));
        if (max_dim / min_dim > cfg.max_aspect_ratio) continue;

        std::vector<Poly> shard_holes;
        for (const auto& h : holes) {
            Poly h_clip = clip_polygon(h, cell);
            detail::ensure_cw(h_clip);
            detail::cleanup_poly(h_clip, cfg.eps);
            if (h_clip.size() >= 3 && std::fabs(polygon_area(h_clip)) >= cfg.min_shard_area * Scalar(0.1)) {
                shard_holes.push_back(std::move(h_clip));
            }
        }

        Shard s;
        s.outline = shard_outer;
        s.mesh = triangulate(shard_outer, shard_holes);

        if (cfg.compute_mass_props) {
            const MassProps mp = shard_mass_props(s.mesh);
            s.area = mp.area;
            s.centroid = mp.centroid;
            s.inertia = mp.inertia;
        } else {
            s.area = area;
            s.centroid = sb.center();
        }

        switch (cfg.decompose) {
            case DecompositionMode::None:
                break;
            case DecompositionMode::TriangleMerge:
                s.convex = merge_decomp(s.mesh);
                break;
            case DecompositionMode::Bayazit:
                s.convex = bayazit_decomp(shard_outer);
                break;
        }
        shards.push_back(std::move(s));
    }
    return shards;
}

template <TriangulatorBackend Tri = EarClipTriangulator>
[[nodiscard]] inline std::vector<Shard>
fracture_voronoi(const Poly& outer, std::span<const Vec> sites,
                 const FractureConfig& cfg = {}, const Tri& triangulate = {}) {
    return fracture_voronoi<Tri>(outer, std::span<const Poly>{}, sites, cfg, triangulate);
}

template <TriangulatorBackend Tri = EarClipTriangulator>
[[nodiscard]] inline std::vector<Shard>
fracture_voronoi_poisson(const Poly& outer, std::span<const Poly> holes,
                         const PoissonConfig& pcfg, const ImpactField* impact = nullptr,
                         const FractureConfig& fcfg = {}, const Tri& triangulate = {}) {
    const AABB<Scalar> b = detail::bounds_of(outer);
    const auto sites = poisson_disk_sites(b, pcfg, impact);
    return fracture_voronoi<Tri>(outer, holes, std::span<const Vec>(sites.data(), sites.size()), fcfg, triangulate);
}

template <TriangulatorBackend Tri = EarClipTriangulator>
[[nodiscard]] inline std::vector<Shard>
refracture(const Shard& parent, std::span<const Vec> subsites,
           const FractureConfig& cfg = {}, const Tri& triangulate = {}) {
    return fracture_voronoi<Tri>(parent.outline, std::span<const Poly>{}, subsites, cfg, triangulate);
}

} // namespace akruti::khanda
