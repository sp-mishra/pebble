#pragma once
// akruti/gjk.hpp — GJK boolean overlap + EPA penetration depth/normal for convex shapes.
//   Works on any pair satisfying Shape (uses support()). Plain C++ — this is irregular,
//   data-dependent narrowphase. No virtual, no macros.
#include "shape.hpp"
#include "containers/static/static_vector.hpp"
#include <cmath>
#include <cstddef>

namespace akruti {
    // Minkowski-difference support: farthest point of (A - B) along d.
    template <Shape A, Shape B>
    [[nodiscard]] Vec2<Scalar> support_diff(const A& a, const B& b, Vec2<Scalar> d) noexcept {
        return a.support(d) - b.support(-d);
    }

    struct Contact {
        bool hit{false};
        Scalar depth{0}; // penetration depth (>=0 when hit)
        Vec2<Scalar> normal{}; // unit contact normal (from A into B)
    };

    // Simplex cache for warm-starting consecutive simulation frames (reduces GJK to 1-2 iters)
    struct SimplexCache {
        containers::static_vector<Vec2<Scalar>, 3> simplex{};
        Vec2<Scalar> separating_axis{1, 0};
        bool valid{false};

        void reset() noexcept {
            simplex.clear();
            separating_axis = Vec2<Scalar>{1, 0};
            valid = false;
        }
    };

    namespace detail {
        // Deterministic signed cross for 2D vectors
        [[nodiscard]] inline constexpr Scalar cross2d(Vec2<Scalar> a, Vec2<Scalar> b) noexcept {
            return a.x * b.y - a.y * b.x;
        }

        // Vector perpendicular to ab pointing toward ao (2D cross(ab, ao) cross ab)
        inline Vec2<Scalar> triple_perp(Vec2<Scalar> ab, Vec2<Scalar> ao) noexcept {
            const Scalar z = cross2d(ab, ao);
            return {-z * ab.y, z * ab.x};
        }
    } // namespace detail

    // GJK boolean with optional SimplexCache warm-starting & Voronoi region solver
    template <Shape A, Shape B>
    [[nodiscard]] bool gjk_overlap(const A& a, const B& b,
                                   containers::static_vector<Vec2<Scalar>, 3>* out_simplex = nullptr,
                                   SimplexCache* cache = nullptr) noexcept {
        using V = Vec2<Scalar>;
        containers::static_vector<V, 3> simplex;
        V d{1, 0};

        // Warm-start from cached direction or prior simplex if available
        if (cache && cache->valid && cache->separating_axis.len2() > Scalar(1e-8)) {
            d = cache->separating_axis;
            (void)simplex.push_back(support_diff(a, b, d));
            d = -simplex[0];
        } else {
            d = b.support(V{1, 0}) - a.support(V{-1, 0});
            if (d.len2() < Scalar(1e-12)) d = V{1, 0};
            (void)simplex.push_back(support_diff(a, b, d));
            d = -simplex[0]; // Point toward origin
        }

        for (int iter = 0; iter < 32; ++iter) {
            if (d.len2() < Scalar(1e-12)) {
                if (out_simplex) { *out_simplex = simplex; }
                if (cache) { cache->simplex = simplex; cache->separating_axis = d; cache->valid = true; }
                return true;
            }
            const V p = support_diff(a, b, d);
            if (p.dot(d) < 0) {
                if (cache) { cache->separating_axis = d; cache->valid = true; }
                return false; // Separating axis found
            }
            (void)simplex.push_back(p);

            // Simplex evolution via Voronoi region classification:
            if (simplex.size() == 2) {
                const V a_pt = simplex[1];
                const V b_pt = simplex[0];
                const V ab = b_pt - a_pt;
                const V ao = -a_pt;
                d = detail::triple_perp(ab, ao);
                if (d.len2() < Scalar(1e-12)) d = ab.perp();
            }
            else { // Triangle: A (newest = simplex[2]), B (simplex[1]), C (simplex[0])
                const V a_pt = simplex[2];
                const V b_pt = simplex[1];
                const V c_pt = simplex[0];
                const V ab = b_pt - a_pt;
                const V ac = c_pt - a_pt;
                const V ao = -a_pt;

                const V ab_perp = detail::triple_perp(ab, -ac);
                const V ac_perp = detail::triple_perp(ac, -ab);

                if (ab_perp.dot(ao) > 0) {
                    simplex.clear();
                    (void)simplex.push_back(b_pt);
                    (void)simplex.push_back(a_pt);
                    d = ab_perp;
                }
                else if (ac_perp.dot(ao) > 0) {
                    simplex.clear();
                    (void)simplex.push_back(c_pt);
                    (void)simplex.push_back(a_pt);
                    d = ac_perp;
                }
                else {
                    if (out_simplex) { *out_simplex = simplex; }
                    if (cache) { cache->simplex = simplex; cache->separating_axis = d; cache->valid = true; }
                    return true; // Origin enclosed in triangle
                }
            }
        }
        return false;
    }

    // EPA: expand the GJK simplex along the Minkowski boundary to recover the minimum
    // penetration vector (normal + depth). Requires an overlapping pair.
    template <Shape A, Shape B>
    [[nodiscard]] Contact epa(const A& a, const B& b) noexcept {
        using V = Vec2<Scalar>;
        containers::static_vector<V, 3> simp;
        if (!gjk_overlap(a, b, &simp) || simp.size() < 3) return Contact{};

        // Ensure CCW winding for the initial simplex
        if ((simp[1].x - simp[0].x) * (simp[2].y - simp[0].y) - (simp[1].y - simp[0].y) * (simp[2].x - simp[0].x) < 0) {
            std::swap(simp[1], simp[2]);
        }

        // Polytope as a growable vertex ring in CCW order
        containers::static_vector<V, 64> poly;
        for (std::size_t i = 0; i < simp.size(); ++i) (void)poly.push_back(simp[i]);

        for (int iter = 0; iter < 48; ++iter) {
            // Find the closest edge of the CCW polytope to the origin.
            Scalar best_dist = Scalar(1e18);
            std::size_t best_i = 0;
            V best_normal{};
            const std::size_t n = poly.size();

            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t j = (i + 1) % n;
                const V e = poly[j] - poly[i];
                // In CCW order, the outward right-hand normal is (e.y, -e.x)
                V nrm{e.y, -e.x};
                Scalar len_n = nrm.len();
                if (len_n < Scalar(1e-12)) continue;
                nrm = nrm / len_n;

                Scalar dist = nrm.dot(poly[i]);
                if (dist < 0) {
                    // If distance is negative, origin is outside this edge due to numerical precision
                    dist = 0;
                }
                if (dist < best_dist) {
                    best_dist = dist;
                    best_i = i;
                    best_normal = nrm;
                }
            }

            if (best_dist >= Scalar(1e17)) break;

            const V p = support_diff(a, b, best_normal);

            if (const Scalar d = p.dot(best_normal); d - best_dist < Scalar(1e-4) || poly.size() >= 63) {
                // best_normal points outward from Minkowski difference (A - B), which means from A toward B.
                return Contact{true, best_dist, best_normal};
            }

            // Insert new support point between best_i and (best_i+1)%n maintaining CCW order in-place.
            const std::size_t insert_idx = best_i + 1;
            (void)poly.push_back(p); // Grow size by 1
            for (std::size_t k = poly.size() - 1; k > insert_idx; --k) {
                poly[k] = poly[k - 1];
            }
            poly[insert_idx] = p;
        }
        return Contact{false, 0, {}};
    }

    // GJK distance: closest separation between two convex shapes (0 if overlapping). Also reports
    // the closest point on the Minkowski difference boundary as the separation direction. Used by
    // conservative-advancement CCD. Standard 2D GJK closest-point sub-distance on 1- and 2-simplices.
    struct Separation {
        Scalar distance{0}; // >=0; 0 means overlapping
        Vec2<Scalar> dir{}; // unit direction from A toward B (valid when distance>0)
    };

    template <Shape A, Shape B>
    [[nodiscard]] Separation gjk_distance(const A& a, const B& b) noexcept {
        using V = Vec2<Scalar>;
        V d{1, 0};
        containers::static_vector<V, 3> s;
        (void)s.push_back(support_diff(a, b, d));
        d = -s[0];
        for (int iter = 0; iter < 32; ++iter) {
            if (d.len2() < Scalar(1e-18)) return Separation{0, {}}; // origin on boundary => touching
            const V p = support_diff(a, b, d);
            if (p.dot(d.normalized()) - s[0].dot(d.normalized()) < Scalar(1e-6) && s.size() >= 1) {
                // No progress toward origin: closest feature found.
                break;
            }
            (void)s.push_back(p);
            // Reduce simplex to the feature closest to the origin; set d to origin-ward normal.
            if (s.size() == 2) {
                const V a0 = s[1], b0 = s[0];
                const V ab = b0 - a0, ao = -a0;
                const Scalar t = std::clamp(ao.dot(ab) / std::max(ab.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));
                const V closest = a0 + ab * t;
                d = -closest;
                if (closest.len2() < Scalar(1e-18)) return Separation{0, {}}; // origin inside => overlap
            }
            else { // triangle: check if origin enclosed => overlap
                const V a2 = s[2], b2 = s[1], c2 = s[0];
                const V ab = b2 - a2, ac = c2 - a2, ao = -a2;
                const V abp0 = detail::triple_perp(ab, -ac);
                const V acp0 = detail::triple_perp(ac, -ab);
                if (abp0.dot(ao) > 0) {
                    s.clear();
                    (void)s.push_back(b2);
                    (void)s.push_back(a2);
                    d = abp0;
                }
                else if (acp0.dot(ao) > 0) {
                    s.clear();
                    (void)s.push_back(c2);
                    (void)s.push_back(a2);
                    d = acp0;
                }
                else {
                    return Separation{0, {}}; // origin enclosed => overlapping
                }
            }
        }
        // Closest point on the final simplex to the origin gives the separation.
        V closest{};
        if (s.size() == 1) closest = s[0];
        else {
            const V a0 = s[s.size() - 1], b0 = s[0];
            const V ab = b0 - a0, ao = -a0;
            const Scalar t = std::clamp(ao.dot(ab) / std::max(ab.len2(), Scalar(1e-12)), Scalar(0), Scalar(1));
            closest = a0 + ab * t;
        }
        const Scalar dist = closest.len();
        if (dist < Scalar(1e-9)) return Separation{0, {}};
        return Separation{dist, (closest * Scalar(-1)).normalized()}; // dir A->B points opposite closest
    }
} // namespace akruti
