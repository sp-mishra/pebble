#pragma once
// ============================================================================
// kalpana/geom/path.hpp — Vector Path Geometry & Contour Stream
// ============================================================================
// Supports move_to, line_to, cubic_to, quad_to, close commands, primitive
// builders (rect, round_rect, circle, ellipse, polygon), and Akruti spline import.
// Configurable sequence storage with SmallVector defaults.
// ============================================================================

#include "transform.hpp"
#include "containers/numeric/math_vector.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include "akruti/spline.hpp"
#include "akruti/fracture.hpp"
#include <vector>
#include <cmath>
#include <algorithm>
#include <string_view>
#include <charconv>

namespace kalpana {
    enum class PathVerb : std::uint8_t {
        Move,
        Line,
        Quad,
        Cubic,
        Close
    };

    template <
        typename VerbContainer = containers::dynamic::SmallVector<PathVerb, 64>,
        typename PointContainer = containers::dynamic::SmallVector<pebble::math::vec2, 128>>
    class BasicPath {
    public:
        using verb_container_type = VerbContainer;
        using point_container_type = PointContainer;

        BasicPath() = default;

        BasicPath& move_to(float x, float y) {
            verbs_.push_back(PathVerb::Move);
            pts_.push_back(pebble::math::vec2(x, y));
            return *this;
        }

        BasicPath& line_to(float x, float y) {
            verbs_.push_back(PathVerb::Line);
            pts_.push_back(pebble::math::vec2(x, y));
            return *this;
        }

        BasicPath& quad_to(float cx, float cy, float x, float y) {
            verbs_.push_back(PathVerb::Quad);
            pts_.push_back(pebble::math::vec2(cx, cy));
            pts_.push_back(pebble::math::vec2(x, y));
            return *this;
        }

        BasicPath& cubic_to(float c1x, float c1y, float c2x, float c2y, float x, float y) {
            verbs_.push_back(PathVerb::Cubic);
            pts_.push_back(pebble::math::vec2(c1x, c1y));
            pts_.push_back(pebble::math::vec2(c2x, c2y));
            pts_.push_back(pebble::math::vec2(x, y));
            return *this;
        }

        BasicPath& close() {
            verbs_.push_back(PathVerb::Close);
            return *this;
        }

        // ── Primitive Shapes ────────────────────────────────────────────────────────
        BasicPath& rect(float x, float y, float w, float h) {
            move_to(x, y);
            line_to(x + w, y);
            line_to(x + w, y + h);
            line_to(x, y + h);
            return close();
        }

        BasicPath& round_rect(float x, float y, float w, float h, float rx, float ry) {
            rx = std::min(rx, w * 0.5f);
            ry = std::min(ry, h * 0.5f);
            constexpr float k = 0.5522847498f; // Cubic constant for circle arc
            const float kx = rx * k, ky = ry * k;

            move_to(x + rx, y);
            line_to(x + w - rx, y);
            cubic_to(x + w - rx + kx, y, x + w, y + ry - ky, x + w, y + ry);
            line_to(x + w, y + h - ry);
            cubic_to(x + w, y + h - ry + ky, x + w - rx + kx, y + h, x + w - rx, y + h);
            line_to(x + rx, y + h);
            cubic_to(x + rx - kx, y + h, x, y + h - ry + ky, x, y + h - ry);
            line_to(x, y + ry);
            cubic_to(x, y + ry - ky, x + rx - kx, y, x + rx, y);
            return close();
        }

        BasicPath& circle(float cx, float cy, float r) {
            return ellipse(cx, cy, r, r);
        }

        BasicPath& ellipse(float cx, float cy, float rx, float ry) {
            constexpr float k = 0.5522847498f;
            const float kx = rx * k, ky = ry * k;

            move_to(cx + rx, cy);
            cubic_to(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
            cubic_to(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
            cubic_to(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
            cubic_to(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
            return close();
        }

        // ── Akruti Spline & Polygon Import ──────────────────────────────────────────
        static BasicPath from_bezier(const akruti::CubicBezierCurve& curve) {
            BasicPath p;
            p.move_to(curve.p0[0], curve.p0[1]);
            p.cubic_to(curve.p1[0], curve.p1[1], curve.p2[0], curve.p2[1], curve.p3[0], curve.p3[1]);
            return p;
        }

        static BasicPath from_catmull_rom(const akruti::CatmullRomSpline& spline) {
            BasicPath p;
            if (spline.points.empty()) return p;
            p.move_to(spline.points[0][0], spline.points[0][1]);
            for (std::size_t i = 1; i < spline.points.size(); ++i) {
                p.line_to(spline.points[i][0], spline.points[i][1]);
            }
            if (spline.closed) p.close();
            return p;
        }

        template <std::size_t N>
        static BasicPath from_chain(const akruti::ChainShape<N>& chain) {
            BasicPath p;
            if (chain.verts.empty()) return p;
            p.move_to(chain.verts[0][0], chain.verts[0][1]);
            for (std::size_t i = 1; i < chain.verts.size(); ++i) {
                p.line_to(chain.verts[i][0], chain.verts[i][1]);
            }
            if (chain.is_loop) p.close();
            return p;
        }

        static BasicPath from_poly(const akruti::Poly& poly) {
            BasicPath p;
            if (poly.empty()) return p;
            p.move_to(poly[0][0], poly[0][1]);
            for (std::size_t i = 1; i < poly.size(); ++i) {
                p.line_to(poly[i][0], poly[i][1]);
            }
            p.close();
            return p;
        }

        [[nodiscard]] akruti::Poly to_poly() const {
            akruti::Poly poly;
            for (const auto& pt : pts_) {
                poly.push_back(pt);
            }
            return poly;
        }

        [[nodiscard]] const VerbContainer& verbs() const noexcept { return verbs_; }
        [[nodiscard]] const PointContainer& points() const noexcept { return pts_; }
        [[nodiscard]] bool empty() const noexcept { return verbs_.empty(); }

        void clear() noexcept {
            verbs_.clear();
            pts_.clear();
        }

        // ── SVG path `d` string parser ──────────────────────────────────────────
        // Parses M/m L/l C/c Q/q A/a Z/z commands from an SVG `d` attribute string.
        // Arc (A/a) is approximated as a line to the arc endpoint (sufficient for import).
        [[nodiscard]] static BasicPath from_svg(std::string_view d) {
            BasicPath p;
            const char* it = d.data();
            const char* end = d.data() + d.size();

            auto skip_ws = [&]() {
                while (it < end && (*it == ' ' || *it == '\t' || *it == '\n' || *it == '\r' || *it == ',')) ++it;
            };

            auto parse_float = [&](float& out) -> bool {
                skip_ws();
                if (it >= end) return false;
                auto [ptr, ec] = std::from_chars(it, end, out);
                if (ec != std::errc{}) return false;
                it = ptr;
                return true;
            };

            float cx = 0.0f, cy = 0.0f;
            char cmd = 0;

            while (it < end) {
                skip_ws();
                if (it >= end) break;
                if ((*it >= 'A' && *it <= 'Z') || (*it >= 'a' && *it <= 'z')) {
                    cmd = *it++;
                }
                bool rel = (cmd >= 'a' && cmd <= 'z');
                char base = rel ? (char)(cmd - 32) : cmd;
                float x, y;

                if (base == 'M') {
                    if (!parse_float(x) || !parse_float(y)) break;
                    if (rel) {
                        x += cx;
                        y += cy;
                    }
                    p.move_to(x, y);
                    cx = x;
                    cy = y;
                    cmd = rel ? 'l' : 'L'; // subsequent coords are implicit LineTo
                }
                else if (base == 'L') {
                    if (!parse_float(x) || !parse_float(y)) break;
                    if (rel) {
                        x += cx;
                        y += cy;
                    }
                    p.line_to(x, y);
                    cx = x;
                    cy = y;
                }
                else if (base == 'C') {
                    float x1, y1, x2, y2, xe, ye;
                    if (!parse_float(x1) || !parse_float(y1) ||
                        !parse_float(x2) || !parse_float(y2) ||
                        !parse_float(xe) || !parse_float(ye))
                        break;
                    if (rel) {
                        x1 += cx;
                        y1 += cy;
                        x2 += cx;
                        y2 += cy;
                        xe += cx;
                        ye += cy;
                    }
                    p.cubic_to(x1, y1, x2, y2, xe, ye);
                    cx = xe;
                    cy = ye;
                }
                else if (base == 'Q') {
                    float x1, y1, xe, ye;
                    if (!parse_float(x1) || !parse_float(y1) ||
                        !parse_float(xe) || !parse_float(ye))
                        break;
                    if (rel) {
                        x1 += cx;
                        y1 += cy;
                        xe += cx;
                        ye += cy;
                    }
                    p.quad_to(x1, y1, xe, ye);
                    cx = xe;
                    cy = ye;
                }
                else if (base == 'A') {
                    // rx ry x-rot large-arc-flag sweep-flag x y — approximate as line to endpoint
                    float rx, ry, xrot, laf, sf;
                    if (!parse_float(rx) || !parse_float(ry) || !parse_float(xrot) ||
                        !parse_float(laf) || !parse_float(sf) ||
                        !parse_float(x) || !parse_float(y))
                        break;
                    if (rel) {
                        x += cx;
                        y += cy;
                    }
                    p.line_to(x, y);
                    cx = x;
                    cy = y;
                }
                else if (base == 'Z') {
                    p.close();
                }
                else {
                    ++it; // skip unknown command
                }
            }
            return p;
        }

        friend bool operator==(const BasicPath&, const BasicPath&) = default;

    private:
        VerbContainer verbs_;
        PointContainer pts_;
    };

    // Default Path using SmallVector storage policy
    using Path = BasicPath<>;
} // namespace kalpana
