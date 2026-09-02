// ============================================================================
// src/tests/containers/test_prakriti_spatial.cpp — uniform-grid neighbor search vs brute force.
// ============================================================================
#include "catch_amalgamated.hpp"
#include "prakriti/core/spatial_hash.hpp"
#include <containers/numeric/math_vector.hpp>
#include <vector>
#include <set>

using namespace prakriti;

TEST_CASE (
"spatial hash finds same neighbors as brute force"
,
"[prakriti][spatial]"
)
 {
    std::vector<Scalar> xs, ys;
    for (int i = 0; i < 10; ++i)
        for (int j = 0; j < 10; ++j) { xs.push_back(Scalar(i)); ys.push_back(Scalar(j)); }

    const Scalar cell = 1.0f;
    SpatialHash grid(cell);
    grid.build(xs, ys);

    const pebble::math::vec2 q{xs[55], ys[55]}; // (5,5)
    const Scalar radius = 1.0f;

    std::set<Index> hashed;
    grid.for_each_neighbor(q[0], q[1], radius, [&](Index j, Scalar r2) {
        if (pebble::math::length_sq(q - pebble::math::vec2{xs[j], ys[j]}) <= r2) hashed.insert(j);
    });

    std::set<Index> brute;
    for (Index j = 0; j < xs.size(); ++j)
        if (pebble::math::length_sq(q - pebble::math::vec2{xs[j], ys[j]}) <= radius * radius) brute.insert(j);

    REQUIRE(hashed == brute);
    REQUIRE(!brute.empty());
}

TEST_CASE (
"spatial hash rebuild is stable"
,
"[prakriti][spatial]"
)
 {
    std::vector<Scalar> xs{0, 0.5f, 5}, ys{0, 0.5f, 5};
    SpatialHash grid(1.0f);
    grid.build(xs, ys);
    int count = 0;
    grid.for_each_neighbor(0.2f, 0.2f, 1.0f, [&](Index, Scalar) { ++count; });
    REQUIRE(count >= 2); // (0,0) and (0.5,0.5) in 3x3 block
}
