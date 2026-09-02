#pragma once
// ============================================================================
// kalpana/geom/path_modifier.hpp — Path Geometry Modifiers & Contour Transformers
// ============================================================================
// Zero-virtual modifier pipeline on Path contours: offset, outline, roughen,
// smooth (Chaikin subdivision), simplify (Ramer-Douglas-Peucker), warp, dash,
// and round corners.
// ============================================================================

#include "path.hpp"
#include "../fill/noise.hpp"
#include "akruti/poly_ops.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include <cmath>
#include <vector>
#include <functional>
#include <algorithm>

namespace kalpana {namespace path_ops {
        // 1. Path Offset (delegates geometric offset to Akruti; Kalpana owns only join-style intent)
        [[nodiscard]] inline Path offset(const Path& p, float distance,
                                         akruti::JoinStyle join = akruti::JoinStyle::Miter) {
            if (p.empty() || std::fabs(distance) < 1e-4f) return p;
            const auto poly = p.to_poly();
            if (poly.size() < 2) return p;
            const auto offset_result = akruti::offset_polygon(poly, distance, join);
            if (offset_result.size() < 3) return p;
            return Path::from_poly(offset_result);
        }

        // 2. Roughen (organic contour jitter using Simplex noise)
        [[nodiscard]] inline Path roughen(const Path& p, float amount, float scale = 1.0f) {
            if (p.empty() || amount <= 1e-4f) return p;
            Path out;
            const auto& pts = p.points();
            const auto& verbs = p.verbs();

            std::size_t pt_idx = 0;
            for (auto v : verbs) {
                switch (v) {
                case PathVerb::Move: {
                    if (pt_idx < pts.size()) {
                        const float jx = noise::simplex(pts[pt_idx][0] * scale, pts[pt_idx][1] * scale) * amount;
                        const float jy = noise::simplex(pts[pt_idx][1] * scale, pts[pt_idx][0] * scale) * amount;
                        out.move_to(pts[pt_idx][0] + jx, pts[pt_idx][1] + jy);
                        pt_idx++;
                    }
                    break;
                }
                case PathVerb::Line: {
                    if (pt_idx < pts.size() && pt_idx > 0) {
                        const auto& p0 = pts[pt_idx - 1];
                        const auto& p1 = pts[pt_idx];
                        const float dx = p1[0] - p0[0];
                        const float dy = p1[1] - p0[1];
                        const float dist = std::sqrt(dx * dx + dy * dy);
                        const int subs = std::clamp(static_cast<int>(dist / 10.0f), 1, 8);

                        for (int s = 1; s <= subs; ++s) {
                            const float t = float(s) / float(subs);
                            const float x = p0[0] + dx * t;
                            const float y = p0[1] + dy * t;
                            const float jx = noise::simplex(x * scale, y * scale) * amount;
                            const float jy = noise::simplex(y * scale, x * scale) * amount;
                            out.line_to(x + jx, y + jy);
                        }
                        pt_idx++;
                    }
                    break;
                }
                case PathVerb::Quad: {
                    if (pt_idx + 1 < pts.size()) {
                        out.quad_to(pts[pt_idx][0], pts[pt_idx][1], pts[pt_idx + 1][0], pts[pt_idx + 1][1]);
                        pt_idx += 2;
                    }
                    break;
                }
                case PathVerb::Cubic: {
                    if (pt_idx + 2 < pts.size()) {
                        out.cubic_to(pts[pt_idx][0], pts[pt_idx][1], pts[pt_idx + 1][0], pts[pt_idx + 1][1],
                                     pts[pt_idx + 2][0], pts[pt_idx + 2][1]);
                        pt_idx += 3;
                    }
                    break;
                }
                case PathVerb::Close: {
                    out.close();
                    break;
                }
                }
            }
            return out;
        }

        // 3. Smooth (Chaikin's corner-cutting subdivision)
        [[nodiscard]] inline Path smooth(const Path& p, int iterations = 1) {
            if (p.empty() || iterations <= 0) return p;
            const auto& pts = p.points();
            if (pts.size() < 3) return p;

            std::vector<pebble::math::vec2> current(pts.begin(), pts.end());
            for (int iter = 0; iter < iterations; ++iter) {
                std::vector<pebble::math::vec2> next;
                next.reserve(current.size() * 2);
                for (std::size_t i = 0; i + 1 < current.size(); ++i) {
                    const auto& p0 = current[i];
                    const auto& p1 = current[i + 1];
                    next.push_back(pebble::math::vec2(0.75f * p0[0] + 0.25f * p1[0], 0.75f * p0[1] + 0.25f * p1[1]));
                    next.push_back(pebble::math::vec2(0.25f * p0[0] + 0.75f * p1[0], 0.25f * p0[1] + 0.75f * p1[1]));
                }
                current = std::move(next);
            }

            Path out;
            if (!current.empty()) {
                out.move_to(current[0][0], current[0][1]);
                for (std::size_t i = 1; i < current.size(); ++i) {
                    out.line_to(current[i][0], current[i][1]);
                }
            }
            return out;
        }

        // 4. Simplify (Ramer-Douglas-Peucker tolerance reduction)
        [[nodiscard]] inline Path simplify(const Path& p, float tolerance = 1.0f) {
            if (p.empty() || tolerance <= 1e-4f) return p;
            const auto& pts = p.points();
            if (pts.size() <= 2) return p;

            std::vector<bool> keep(pts.size(), false);
            keep.front() = true;
            keep.back() = true;

            auto rdp = [&](auto& self, std::size_t i, std::size_t j) -> void {
                if (j <= i + 1) return;
                const auto& p0 = pts[i];
                const auto& p1 = pts[j];
                const float dx = p1[0] - p0[0];
                const float dy = p1[1] - p0[1];
                const float len_sq = dx * dx + dy * dy;

                float max_dist = 0.0f;
                std::size_t max_idx = i;

                for (std::size_t k = i + 1; k < j; ++k) {
                    const auto& pk = pts[k];
                    float dist = 0.0f;
                    if (len_sq < 1e-6f) {
                        dist = std::sqrt((pk[0] - p0[0]) * (pk[0] - p0[0]) + (pk[1] - p0[1]) * (pk[1] - p0[1]));
                    }
                    else {
                        const float num = std::fabs(dy * pk[0] - dx * pk[1] + p1[0] * p0[1] - p1[1] * p0[0]);
                        dist = num / std::sqrt(len_sq);
                    }
                    if (dist > max_dist) {
                        max_dist = dist;
                        max_idx = k;
                    }
                }

                if (max_dist > tolerance) {
                    keep[max_idx] = true;
                    self(self, i, max_idx);
                    self(self, max_idx, j);
                }
            };

            rdp(rdp, 0, pts.size() - 1);

            Path out;
            bool first = true;
            for (std::size_t i = 0; i < pts.size(); ++i) {
                if (keep[i]) {
                    if (first) {
                        out.move_to(pts[i][0], pts[i][1]);
                        first = false;
                    }
                    else {
                        out.line_to(pts[i][0], pts[i][1]);
                    }
                }
            }
            return out;
        }

        // 5. Warp (harmonic wave distortion)
        [[nodiscard]] inline Path warp(const Path& p, float amount, float frequency = 1.0f) {
            if (p.empty() || amount <= 1e-4f) return p;
            Path out;
            const auto& pts = p.points();
            const auto& verbs = p.verbs();

            std::size_t pt_idx = 0;
            for (auto v : verbs) {
                switch (v) {
                case PathVerb::Move: {
                    if (pt_idx < pts.size()) {
                        const float wx = std::sin(pts[pt_idx][1] * frequency * 0.05f) * amount;
                        const float wy = std::cos(pts[pt_idx][0] * frequency * 0.05f) * amount;
                        out.move_to(pts[pt_idx][0] + wx, pts[pt_idx][1] + wy);
                        pt_idx++;
                    }
                    break;
                }
                case PathVerb::Line: {
                    if (pt_idx < pts.size()) {
                        const float wx = std::sin(pts[pt_idx][1] * frequency * 0.05f) * amount;
                        const float wy = std::cos(pts[pt_idx][0] * frequency * 0.05f) * amount;
                        out.line_to(pts[pt_idx][0] + wx, pts[pt_idx][1] + wy);
                        pt_idx++;
                    }
                    break;
                }
                case PathVerb::Quad: {
                    if (pt_idx + 1 < pts.size()) {
                        out.quad_to(pts[pt_idx][0], pts[pt_idx][1], pts[pt_idx + 1][0], pts[pt_idx + 1][1]);
                        pt_idx += 2;
                    }
                    break;
                }
                case PathVerb::Cubic: {
                    if (pt_idx + 2 < pts.size()) {
                        out.cubic_to(pts[pt_idx][0], pts[pt_idx][1], pts[pt_idx + 1][0], pts[pt_idx + 1][1],
                                     pts[pt_idx + 2][0], pts[pt_idx + 2][1]);
                        pt_idx += 3;
                    }
                    break;
                }
                case PathVerb::Close: {
                    out.close();
                    break;
                }
                }
            }
            return out;
        }

        // 6. Dash
        [[nodiscard]] inline Path dash(const Path& p, float on, float off) {
            if (p.empty() || on <= 0.0f || off <= 0.0f) return p;
            Path out;
            const auto& pts = p.points();
            if (pts.size() < 2) return p;

            float acc = 0.0f;
            bool drawing = true;

            out.move_to(pts[0][0], pts[0][1]);
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                const auto& p0 = pts[i];
                const auto& p1 = pts[i + 1];
                const float dx = p1[0] - p0[0];
                const float dy = p1[1] - p0[1];
                const float dist = std::sqrt(dx * dx + dy * dy);

                float rem = dist;
                float cur_t = 0.0f;

                while (rem > 0.0f) {
                    const float target = drawing ? on : off;
                    const float needed = target - acc;

                    if (rem >= needed) {
                        cur_t += needed / dist;
                        const float nx = p0[0] + dx * cur_t;
                        const float ny = p0[1] + dy * cur_t;

                        if (drawing) {
                            out.line_to(nx, ny);
                        }
                        else {
                            out.move_to(nx, ny);
                        }
                        rem -= needed;
                        acc = 0.0f;
                        drawing = !drawing;
                    }
                    else {
                        acc += rem;
                        cur_t = 1.0f;
                        if (drawing) {
                            out.line_to(p1[0], p1[1]);
                        }
                        else {
                            out.move_to(p1[0], p1[1]);
                        }
                        rem = 0.0f;
                    }
                }
            }
            return out;
        }

        // 7. Outline
        [[nodiscard]] inline Path outline(const Path& p, float width) {
            return offset(p, width * 0.5f);
        }

        // 8. Expand
        [[nodiscard]] inline Path expand(const Path& p, float amount) {
            return offset(p, amount);
        }

        // 9. Round Corners
        [[nodiscard]] inline Path round_corners(const Path& p, float radius) {
            return smooth(p, std::max(1, static_cast<int>(radius * 0.5f)));
        }
    } // namespace path_ops

    // ── Composable PathModifier Chain ────────────────────────────────────────────

    template <typename OpContainer = containers::dynamic::SmallVector<std::function < Path(const Path &)>, 128>



    >
    class BasicPathModifier {
    public:
        BasicPathModifier() = default;

        explicit BasicPathModifier (std::function<Path(const Path &)> op){
            ops_.push_back(std::move(op));



        }

        // Factory functions
        [[nodiscard]] static BasicPathModifier offset(float distance) {
            return BasicPathModifier([distance](const Path& p) { return path_ops::offset(p, distance); });
        }

        [[nodiscard]] static BasicPathModifier roughen(float amount, float scale = 1.0f) {
            return BasicPathModifier([amount, scale](const Path& p) { return path_ops::roughen(p, amount, scale); });
        }

        [[nodiscard]] static BasicPathModifier smooth(int iterations = 1) {
            return BasicPathModifier([iterations](const Path& p) { return path_ops::smooth(p, iterations); });
        }

        [[nodiscard]] static BasicPathModifier simplify(float tolerance = 1.0f) {
            return BasicPathModifier([tolerance](const Path& p) { return path_ops::simplify(p, tolerance); });
        }

        [[nodiscard]] static BasicPathModifier warp(float amount, float frequency = 1.0f) {
            return BasicPathModifier(
                [amount, frequency](const Path& p) { return path_ops::warp(p, amount, frequency); });
        }

        [[nodiscard]] static BasicPathModifier expand(float amount) {
            return BasicPathModifier([amount](const Path& p) { return path_ops::expand(p, amount); });
        }

        [[nodiscard]] static BasicPathModifier outline(float width) {
            return BasicPathModifier([width](const Path& p) { return path_ops::outline(p, width); });
        }

        [[nodiscard]] static BasicPathModifier dash(float on, float off) {
            return BasicPathModifier([on, off](const Path& p) { return path_ops::dash(p, on, off); });
        }

        [[nodiscard]] static BasicPathModifier round_corners(float radius) {
            return BasicPathModifier([radius](const Path& p) { return path_ops::round_corners(p, radius); });
        }

        [[nodiscard]] Path apply(const Path& p) const {
            Path result = p;
            for (const auto& op : ops_) {
                result = op(result);
            }
            return result;
        }

        [[nodiscard]] BasicPathModifier operator|(BasicPathModifier other) const {
            BasicPathModifier copy = *this;
            for (auto& op : other.ops_) {
                copy.ops_.push_back(op);
            }
            return copy;
        }

    private:
        OpContainer ops_;
    };

    using PathModifier = BasicPathModifier<>;

    // Free functions for pipe operator syntax: path | roughen(2.0f)
    [[nodiscard]] inline Path operator|(const Path& p, const PathModifier& mod) {
        return mod.apply(p);
    }

    [[nodiscard]] inline PathModifier offset(float distance) { return PathModifier::offset(distance); }

    [[nodiscard]] inline PathModifier roughen(float amount, float scale = 1.0f) {
        return PathModifier::roughen(amount, scale);
    }

    [[nodiscard]] inline PathModifier smooth(int iterations = 1) { return PathModifier::smooth(iterations); }
    [[nodiscard]] inline PathModifier simplify(float tolerance = 1.0f) { return PathModifier::simplify(tolerance); }

    [[nodiscard]] inline PathModifier warp(float amount, float frequency = 1.0f) {
        return PathModifier::warp(amount, frequency);
    }

    [[nodiscard]] inline PathModifier expand(float amount) { return PathModifier::expand(amount); }
    [[nodiscard]] inline PathModifier outline(float width) { return PathModifier::outline(width); }
    [[nodiscard]] inline PathModifier dash(float on, float off) { return PathModifier::dash(on, off); }
    [[nodiscard]] inline PathModifier round_corners(float radius) { return PathModifier::round_corners(radius); }
} // namespace kalpana
