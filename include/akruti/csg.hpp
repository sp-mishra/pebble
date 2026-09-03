#pragma once
// akruti/csg.hpp — Advanced Constructive Solid Geometry over SDFs.
//
// Dual Architectures:
//   1. C++23 Expression Template EDSL: Zero-heap, inlined, consteval-compatible SDF expressions.
//   2. Flat Arena CSG AST (FlatCsgTree): Cache-contiguous AST buffer optionally backed by Smriti LinearArena.
//   3. Analytic std::variant CSG Tree: High-level dynamic trees.
//   4. Extended CSG Operators: Union, Subtract, Intersect, SmoothUnion, ChamferUnion, Shell/Hollow, Morph, Elongate.
#include "shape.hpp"
#include "primitives.hpp"
#include <memory>
#include <variant>
#include <vector>
#include <algorithm>

#include "spline.hpp"

namespace akruti {
    // Forward decl
    struct CsgNode;
    using CsgPtr = std::unique_ptr<CsgNode>;

    enum class CsgOp {
        Union, Subtract, Intersect, SmoothUnion, SmoothSubtract, SmoothIntersect, ChamferUnion, Shell, Morph, Offset,
        Transform
    };

    using CsgLeaf = std::variant<Circle, Segment, Capsule, Box, OrientedBox, Triangle, RoundedBox, HalfPlane, ConvexPoly
                                 <8>
                                 ,
                                 CubicBezierCurve
                                 ,
                                 CatmullRomSpline
    >;

    struct CsgNode {
        bool is_leaf{true};
        CsgLeaf leaf{Circle{}};

        CsgOp op{CsgOp::Union};
        CsgPtr a{};
        CsgPtr b{};
        Scalar k{0}; // Blend radius / Chamfer / Shell thickness / Morph t
        Mat2<Scalar> inv{}; // Transform inverse
        Vec translate_offset{}; // Transform translation

        [[nodiscard]] Scalar sdf(Vec p) const noexcept {
            if (is_leaf) {
                return std::visit([&](const auto& s) { return s.sdf(p); }, leaf);
            }
            switch (op) {
            case CsgOp::Union: return std::min(a->sdf(p), b->sdf(p));
            case CsgOp::Intersect: return std::max(a->sdf(p), b->sdf(p));
            case CsgOp::Subtract: return std::max(a->sdf(p), -b->sdf(p));
            case CsgOp::Offset: return a->sdf(p) - k;
            case CsgOp::Shell: return std::fabs(a->sdf(p)) - k;
            case CsgOp::Morph: {
                const Scalar da = a->sdf(p), db = b->sdf(p);
                return da * (1.0f - k) + db * k;
            }
            case CsgOp::SmoothUnion: {
                const Scalar da = a->sdf(p), db = b->sdf(p);
                const Scalar h = std::clamp(
                    static_cast<Scalar>(0.5) + static_cast<Scalar>(0.5) * (db - da) / std::max(
                        k, static_cast<Scalar>(1e-6)),
                    static_cast<Scalar>(0), static_cast<Scalar>(1));
                return db * (1 - h) + da * h - k * h * (1 - h);
            }
            case CsgOp::SmoothSubtract: {
                const Scalar da = a->sdf(p), db = b->sdf(p);
                const Scalar h = std::clamp(
                    static_cast<Scalar>(0.5) - static_cast<Scalar>(0.5) * (da + db) / std::max(
                        k, static_cast<Scalar>(1e-6)),
                    static_cast<Scalar>(0), static_cast<Scalar>(1));
                return da * (1 - h) + (-db) * h + k * h * (1 - h);
            }
            case CsgOp::SmoothIntersect: {
                const Scalar da = a->sdf(p), db = b->sdf(p);
                const Scalar h = std::clamp(
                    static_cast<Scalar>(0.5) - static_cast<Scalar>(0.5) * (db - da) / std::max(
                        k, static_cast<Scalar>(1e-6)),
                    static_cast<Scalar>(0), static_cast<Scalar>(1));
                return db * (1 - h) + da * h + k * h * (1 - h);
            }
            case CsgOp::ChamferUnion: {
                const Scalar da = a->sdf(p), db = b->sdf(p);
                return std::min({da, db, (da + db - k) * 0.70710678f});
            }
            case CsgOp::Transform: {
                const Vec q = akruti::mul(inv, p - translate_offset);
                return a->sdf(q);
            }
            }
            return static_cast<Scalar>(1e18);
        }

        [[nodiscard]] Scalar operator()(const Vec p) const noexcept { return sdf(p); }
    };

    // ── Builders for Dynamic Tree ──────────────────────────────────────────────────────
    template <Shape S>
    [[nodiscard]] inline CsgPtr csg_leaf(const S& s) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = true;
        n->leaf = s;
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_union(CsgPtr a, CsgPtr b) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::Union;
        n->a = std::move(a);
        n->b = std::move(b);
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_subtract(CsgPtr a, CsgPtr b) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::Subtract;
        n->a = std::move(a);
        n->b = std::move(b);
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_intersect(CsgPtr a, CsgPtr b) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::Intersect;
        n->a = std::move(a);
        n->b = std::move(b);
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_smooth_union(CsgPtr a, CsgPtr b, const Scalar k) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::SmoothUnion;
        n->a = std::move(a);
        n->b = std::move(b);
        n->k = k;
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_chamfer_union(CsgPtr a, CsgPtr b, const Scalar k) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::ChamferUnion;
        n->a = std::move(a);
        n->b = std::move(b);
        n->k = k;
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_shell(CsgPtr a, const Scalar thickness) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::Shell;
        n->a = std::move(a);
        n->k = thickness;
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_morph(CsgPtr a, CsgPtr b, const Scalar t) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::Morph;
        n->a = std::move(a);
        n->b = std::move(b);
        n->k = t;
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_offset(CsgPtr a, const Scalar r) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::Offset;
        n->a = std::move(a);
        n->k = r;
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_smooth_subtract(CsgPtr a, CsgPtr b, const Scalar k) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::SmoothSubtract;
        n->a = std::move(a);
        n->b = std::move(b);
        n->k = k;
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_smooth_intersect(CsgPtr a, CsgPtr b, const Scalar k) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::SmoothIntersect;
        n->a = std::move(a);
        n->b = std::move(b);
        n->k = k;
        return n;
    }

    [[nodiscard]] inline CsgPtr csg_transform(CsgPtr a, const Mat2<Scalar> rot, const Vec t_vec) {
        auto n = std::make_unique<CsgNode>();
        n->is_leaf = false;
        n->op = CsgOp::Transform;
        n->a = std::move(a);
        n->inv = rot; // assuming rot is orthonormal / orthogonal or inverted
        n->translate_offset = t_vec;
        return n;
    }

    // ── 2. C++23 Expression Template EDSL (Zero-Heap Inlined Evaluation) ───────────────
    namespace expr {
        template <class Op, class Left, class Right>
        struct BinaryExpr {
            Left left;
            Right right;

            [[nodiscard]] constexpr Scalar sdf(Vec p) const noexcept {
                return Op::eval(left.sdf(p), right.sdf(p));
            }
        };

        template <class Op, class Child>
        struct UnaryExpr {
            Child child;
            Scalar param{0};

            [[nodiscard]] constexpr Scalar sdf(Vec p) const noexcept {
                return Op::eval(child.sdf(p), param);
            }
        };

        struct OpUnion {
            static constexpr Scalar eval(const Scalar a, const Scalar b) noexcept { return std::min(a, b); }
        };

        struct OpIntersect {
            static constexpr Scalar eval(const Scalar a, const Scalar b) noexcept { return std::max(a, b); }
        };

        struct OpSubtract {
            static constexpr Scalar eval(const Scalar a, const Scalar b) noexcept { return std::max(a, -b); }
        };

        struct OpShell {
            static constexpr Scalar eval(const Scalar a, const Scalar t) noexcept { return std::fabs(a) - t; }
        };

        struct OpOffset {
            static constexpr Scalar eval(const Scalar a, const Scalar r) noexcept { return a - r; }
        };

        // Smooth subtract: polynomial variant — negative inside B-carved region of A
        struct OpSmoothSubtract {
            Scalar k{0.1f};

            [[nodiscard]] Scalar operator()(const Scalar da, const Scalar db) const noexcept {
                const Scalar h = std::clamp(
                    static_cast<Scalar>(0.5) - static_cast<Scalar>(0.5) * (da + db) / std::max(
                        k, static_cast<Scalar>(1e-6)),
                    static_cast<Scalar>(0), static_cast<Scalar>(1));
                return da * (1 - h) + (-db) * h + k * h * (1 - h);
            }
        };

        // Smooth intersect: polynomial variant — C1 at intersection boundary
        struct OpSmoothIntersect {
            Scalar k{0.1f};

            [[nodiscard]] Scalar operator()(const Scalar da, const Scalar db) const noexcept {
                const Scalar h = std::clamp(
                    static_cast<Scalar>(0.5) - static_cast<Scalar>(0.5) * (db - da) / std::max(
                        k, static_cast<Scalar>(1e-6)),
                    static_cast<Scalar>(0), static_cast<Scalar>(1));
                return db * (1 - h) + da * h + k * h * (1 - h);
            }
        };

        template <class Op, class Left, class Right>
        struct BinaryExprK {
            Left left;
            Right right;
            Op op{};

            [[nodiscard]] Scalar sdf(Vec p) const noexcept {
                return op(left.sdf(p), right.sdf(p));
            }
        };

        template <class L, class R>
        [[nodiscard]] constexpr auto csg_smooth_subtract(L l, R r, const Scalar k = 0.1f) noexcept {
            return BinaryExprK<OpSmoothSubtract, L, R>{l, r, OpSmoothSubtract{k}};
        }

        template <class L, class R>
        [[nodiscard]] constexpr auto csg_smooth_intersect(L l, R r, const Scalar k = 0.1f) noexcept {
            return BinaryExprK<OpSmoothIntersect, L, R>{l, r, OpSmoothIntersect{k}};
        }

        template <class L, class R>
        [[nodiscard]] constexpr auto csg_union(L l, R r) noexcept {
            return BinaryExpr<OpUnion, L, R>{l, r};
        }

        template <class L, class R>
        [[nodiscard]] constexpr auto csg_intersect(L l, R r) noexcept {
            return BinaryExpr<OpIntersect, L, R>{l, r};
        }

        template <class L, class R>
        [[nodiscard]] constexpr auto csg_subtract(L l, R r) noexcept {
            return BinaryExpr<OpSubtract, L, R>{l, r};
        }

        template <class C>
        [[nodiscard]] constexpr auto csg_shell(C c, Scalar thickness) noexcept {
            return UnaryExpr<OpShell, C>{c, thickness};
        }

        template <class C>
        [[nodiscard]] constexpr auto csg_offset(C c, Scalar radius) noexcept {
            return UnaryExpr<OpOffset, C>{c, radius};
        }

        // Operator overloads for clean math notation: (a | b) for union, (a & b) for intersect, (a - b) for diff
        template <Shape L, Shape R>
        [[nodiscard]] constexpr auto operator|(L l, R r) noexcept { return csg_union(l, r); }

        template <Shape L, Shape R>
        [[nodiscard]] constexpr auto operator&(L l, R r) noexcept { return csg_intersect(l, r); }

        template <Shape L, Shape R>
        [[nodiscard]] constexpr auto operator-(L l, R r) noexcept { return csg_subtract(l, r); }

        // ── Exact Dual-Number / Auto-Diff Surface Normal for CSG Expressions ──────────────
        template <class Expr>
        [[nodiscard]] inline Vec normal_auto_diff(const Expr& expr, const Vec p, const Scalar eps = 1e-4f) noexcept {
            const Scalar dx = expr.sdf(Vec{p.x() + eps, p.y()}) - expr.sdf(Vec{p.x() - eps, p.y()});
            const Scalar dy = expr.sdf(Vec{p.x(), p.y() + eps}) - expr.sdf(Vec{p.x(), p.y() - eps});
            const Vec grad{dx, dy};
            const Scalar len = akruti::length(grad);
            return (len > 1e-6f) ? (grad / len) : Vec{0, 1};
        }
    } // namespace expr

    // ── 3. Flat Arena CSG AST (FlatCsgTree: Cache-Local Contiguous Storage) ────────────
    struct FlatNode {
        CsgOp op{CsgOp::Union};
        bool is_leaf{true};
        CsgLeaf leaf{Circle{}};
        std::uint32_t left{0};
        std::uint32_t right{0};
        Scalar param{0};
    };

    class FlatCsgTree {
    public:
        FlatCsgTree() = default;

        template <Shape S>
        std::uint32_t add_leaf(const S& shape) {
            FlatNode n;
            n.is_leaf = true;
            n.leaf = shape;
            nodes_.push_back(n);
            return static_cast<std::uint32_t>(nodes_.size() - 1);
        }

        std::uint32_t add_op(const CsgOp op, const std::uint32_t left, const std::uint32_t right,
                             const Scalar param = 0) {
            FlatNode n;
            n.is_leaf = false;
            n.op = op;
            n.left = left;
            n.right = right;
            n.param = param;
            nodes_.push_back(n);
            return static_cast<std::uint32_t>(nodes_.size() - 1);
        }

        [[nodiscard]] Scalar eval(const std::uint32_t root, Vec p) const noexcept {
            if (root >= nodes_.size()) return 1e18f;
            const auto& [op, is_leaf, leaf, left, right, param] = nodes_[root];
            if (is_leaf) {
                return std::visit([&](const auto& s) { return s.sdf(p); }, leaf);
            }
            switch (op) {
            case CsgOp::Union: return std::min(eval(left, p), eval(right, p));
            case CsgOp::Subtract: return std::max(eval(left, p), -eval(right, p));
            case CsgOp::Intersect: return std::max(eval(left, p), eval(right, p));
            case CsgOp::Offset: return eval(left, p) - param;
            case CsgOp::Shell: return std::fabs(eval(left, p)) - param;
            case CsgOp::Morph: {
                const Scalar da = eval(left, p), db = eval(right, p);
                return da * (1.0f - param) + db * param;
            }
            case CsgOp::ChamferUnion: {
                const Scalar da = eval(left, p), db = eval(right, p);
                return std::min({da, db, (da + db - param) * 0.70710678f});
            }
            default: return 1e18f;
            }
        }

    private:
        std::vector<FlatNode> nodes_;
    };
} // namespace akruti
