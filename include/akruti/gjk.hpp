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
[[nodiscard]] inline Vec2<Scalar> support_diff(const A& a, const B& b, Vec2<Scalar> d) noexcept {
    return a.support(d) - b.support(-d);
}

struct Contact {
    bool          hit{false};
    Scalar        depth{0};        // penetration depth (>=0 when hit)
    Vec2<Scalar>  normal{};        // unit contact normal (from A into B)
};

namespace detail {
// Origin-containing triangle test / simplex evolution for 2D GJK.
inline Vec2<Scalar> triple_perp(Vec2<Scalar> ab, Vec2<Scalar> ao) noexcept {
    // Vector perpendicular to ab, pointing toward ao (2D triple product ab x ao x ab).
    const Scalar z = cross(ab, ao);
    return {-ab.y * z, ab.x * z};
}
} // namespace detail

// GJK boolean: do convex A and B overlap? Fills the final simplex for EPA if requested.
template <Shape A, Shape B>
[[nodiscard]] inline bool gjk_overlap(const A& a, const B& b,
                                      containers::static_vector<Vec2<Scalar>, 3>* out_simplex = nullptr) noexcept {
    using V = Vec2<Scalar>;
    V d{1, 0};
    containers::static_vector<V, 3> simplex;
    (void)simplex.push_back(support_diff(a, b, d));
    d = -simplex[0];
    for (int iter = 0; iter < 32; ++iter) {
        if (d.len2() < Scalar(1e-18)) { d = V{1, 0}; }
        const V p = support_diff(a, b, d);
        if (p.dot(d) < 0) return false; // no overlap: passed the origin
        (void)simplex.push_back(p);
        // Evolve simplex toward the origin.
        const V ao = -simplex.back();
        if (simplex.size() == 2) {
            const V ab = simplex[0] - simplex[1];
            d = detail::triple_perp(ab, ao);
            if (d.len2() < Scalar(1e-18)) d = ab.perp();
        } else { // triangle
            const V a2 = simplex[2], b2 = simplex[1], c2 = simplex[0];
            const V ab = b2 - a2, ac = c2 - a2, aoo = -a2;
            const V abp = detail::triple_perp(ab, -ac);
            const V acp = detail::triple_perp(ac, -ab);
            if (abp.dot(aoo) > 0) {
                simplex.clear(); (void)simplex.push_back(c2); (void)simplex.push_back(b2);
                d = abp;
            } else if (acp.dot(aoo) > 0) {
                simplex.clear(); (void)simplex.push_back(c2); (void)simplex.push_back(a2);
                d = acp;
            } else {
                if (out_simplex) { *out_simplex = simplex; }
                return true; // origin enclosed
            }
        }
    }
    return false;
}

// EPA: expand the GJK simplex along the Minkowski boundary to recover the minimum
// penetration vector (normal + depth). Requires an overlapping pair.
template <Shape A, Shape B>
[[nodiscard]] inline Contact epa(const A& a, const B& b) noexcept {
    using V = Vec2<Scalar>;
    containers::static_vector<V, 3> simp;
    if (!gjk_overlap(a, b, &simp) || simp.size() < 3) return Contact{};

    // Polytope as a growable vertex ring.
    containers::static_vector<V, 64> poly;
    for (std::size_t i = 0; i < simp.size(); ++i) (void)poly.push_back(simp[i]);

    for (int iter = 0; iter < 48; ++iter) {
        // Find the closest edge of the current polytope to the origin.
        Scalar best_dist = Scalar(1e18);
        std::size_t best_i = 0;
        V best_normal{};
        const std::size_t n = poly.size();
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t j = (i + 1) % n;
            const V e = poly[j] - poly[i];
            V nrm{e.y, -e.x};                 // outward-ish edge normal
            nrm = nrm.normalized();
            Scalar dist = nrm.dot(poly[i]);
            if (dist < 0) { nrm = -nrm; dist = -dist; }
            if (dist < best_dist) { best_dist = dist; best_i = i; best_normal = nrm; }
        }
        const V p = support_diff(a, b, best_normal);
        const Scalar d = p.dot(best_normal);
        if (d - best_dist < Scalar(1e-4) || poly.size() >= 63) {
            return Contact{true, best_dist, best_normal};
        }
        // Insert new support point between best_i and best_i+1.
        containers::static_vector<V, 64> next;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            (void)next.push_back(poly[i]);
            if (i == best_i) (void)next.push_back(p);
        }
        poly = next;
    }
    return Contact{false, 0, {}};
}

// GJK distance: closest separation between two convex shapes (0 if overlapping). Also reports
// the closest point on the Minkowski difference boundary as the separation direction. Used by
// conservative-advancement CCD. Standard 2D GJK closest-point sub-distance on 1- and 2-simplices.
struct Separation {
    Scalar       distance{0};   // >=0; 0 means overlapping
    Vec2<Scalar> dir{};         // unit direction from A toward B (valid when distance>0)
};

template <Shape A, Shape B>
[[nodiscard]] inline Separation gjk_distance(const A& a, const B& b) noexcept {
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
        } else { // triangle: check if origin enclosed => overlap
            const V a2 = s[2], b2 = s[1], c2 = s[0];
            const V ab = b2 - a2, ac = c2 - a2, ao = -a2;
            const V abp0 = detail::triple_perp(ab, -ac);
            const V acp0 = detail::triple_perp(ac, -ab);
            if (abp0.dot(ao) > 0) {
                s.clear(); (void)s.push_back(b2); (void)s.push_back(a2);
                d = abp0;
            } else if (acp0.dot(ao) > 0) {
                s.clear(); (void)s.push_back(c2); (void)s.push_back(a2);
                d = acp0;
            } else {
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
