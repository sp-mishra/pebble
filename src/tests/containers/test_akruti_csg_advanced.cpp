// src/tests/containers/test_akruti_csg_advanced.cpp — Expression CSG, Flat Arena CSG, and extended operators.
#include "Catch2-3.9.0/extras/catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include <cmath>

using namespace akruti;
using V = Vec2<Scalar>;

TEST_CASE (
"akruti: expression-template inlined CSG EDSL"
,
"[akruti][csg][expr]"
)
 {
    using namespace akruti::expr;
    Circle c1{{0, 0}, 1.0f};
    Box b1{{0.5f, 0}, {0.5f, 0.5f}};

    // Operator overloads: | for union, - for diff, & for intersect
    auto expr_diff = c1 - b1;
    REQUIRE(expr_diff.sdf({-0.5f, 0}) < 0); // inside circle, outside box
    REQUIRE(expr_diff.sdf({0.5f, 0}) > 0);  // carved out by box

    auto expr_union = c1 | b1;
    REQUIRE(expr_union.sdf({0.5f, 0}) < 0); // inside union

    auto expr_shell = csg_shell(c1, 0.1f);
    REQUIRE(expr_shell.sdf({1.0f, 0}) == Catch::Approx(-0.1f).margin(1e-3)); // on circle boundary => shell center
    REQUIRE(expr_shell.sdf({0, 0}) > 0);                                    // hollow inside
}

TEST_CASE (
"akruti: flat arena contiguous CSG tree"
,
"[akruti][csg][flat]"
)
 {
    FlatCsgTree tree;
    auto l1 = tree.add_leaf(Circle{{0, 0}, 1.0f});
    auto l2 = tree.add_leaf(Box{{0.5f, 0}, {0.5f, 0.5f}});
    auto root = tree.add_op(CsgOp::Subtract, l1, l2);

    REQUIRE(tree.eval(root, {-0.5f, 0}) < 0);
    REQUIRE(tree.eval(root, {0.5f, 0}) > 0);
}

TEST_CASE (
"akruti: extended CSG operators (chamfer, morph, shell)"
,
"[akruti][csg][ops]"
)
 {
    auto c1 = csg_leaf(Circle{{-0.5f, 0}, 1.0f});
    auto c2 = csg_leaf(Circle{{0.5f, 0}, 1.0f});

    auto chamfer = csg_chamfer_union(std::move(c1), std::move(c2), 0.2f);
    REQUIRE(chamfer->sdf({0, 0}) < 0);

    auto box = csg_leaf(Box{{0, 0}, {1, 1}});
    auto circ = csg_leaf(Circle{{0, 0}, 1.0f});
    auto morph = csg_morph(std::move(box), std::move(circ), 0.5f);
    REQUIRE(morph->sdf({0, 0}) < 0);
}
