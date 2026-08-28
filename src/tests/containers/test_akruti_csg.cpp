// src/tests/containers/test_akruti_csg.cpp — CSG analytic tree operations.
#include "Catch2-3.9.0/extras/catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include <cmath>

using namespace akruti;
using V = Vec2<Scalar>;

TEST_CASE("akruti: analytic CSG booleans", "[akruti][csg]") {
    auto u = csg_union(csg_leaf(Circle{{-0.5f, 0}, 1}), csg_leaf(Circle{{0.5f, 0}, 1}));
    REQUIRE(u->sdf({0, 0}) < 0); // inside both
    auto s = csg_subtract(csg_leaf(Box{{0, 0}, {1, 1}}), csg_leaf(Circle{{0, 0}, 0.5f}));
    REQUIRE(s->sdf({0, 0}) > 0);   // carved out center
    REQUIRE(s->sdf({0.9f, 0}) < 0); // still solid near edge
}

TEST_CASE("akruti: CSG smooth union and transform", "[akruti][csg]") {
    auto su = csg_smooth_union(csg_leaf(Circle{{-1.0f, 0}, 1}), csg_leaf(Circle{{1.0f, 0}, 1}), 0.5f);
    REQUIRE(su->sdf({0, 0}) <= su->sdf({0, 1}));

    auto tf = csg_transform(csg_leaf(Box{{0, 0}, {1, 1}}), Mat2<Scalar>::rotation(0.5f), V{2.0f, 3.0f});
    REQUIRE(tf->sdf({2.0f, 3.0f}) < 0);
    REQUIRE(tf->sdf({0, 0}) > 0);
}
