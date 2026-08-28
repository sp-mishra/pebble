// src/tests/containers/test_akruti_scene.cpp — akruti scene layer: SoA batches, AABBTree broadphase, bulk ops.
#include "Catch2-3.9.0/extras/catch_amalgamated.hpp"
#include "akruti/akruti.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace akruti;
using namespace akruti::scene;
using V = Vec2<Scalar>;

TEST_CASE("akruti: payload pack/unpack round-trips", "[akruti][scene][payload]") {
    for (std::uint32_t t = 0; t < 6; ++t) {
        for (std::uint32_t i : {0u, 1u, 123u, kIndexMask}) {
            const std::uint32_t p = pack(t, i);
            REQUIRE(unpack_type(p) == t);
            REQUIRE(unpack_index(p) == i);
        }
    }
}

TEST_CASE("akruti: scene add + count + tree size", "[akruti][scene][build]") {
    Scene scene;
    int n = 0;
    for (int gx = 0; gx < 10; ++gx)
        for (int gy = 0; gy < 10; ++gy) {
            (void)scene.add(Circle{{Scalar(gx) * 3, Scalar(gy) * 3}, Scalar(1)});
            ++n;
        }
    (void)scene.add(Box{{100, 100}, {2, 2}});
    ++n;
    REQUIRE(scene.count() == std::size_t(n));
    REQUIRE(scene.tree().size() == std::size_t(n));
    REQUIRE(scene.count<Circle>() == 100u);
    REQUIRE(scene.count<Box>() == 1u);
}

TEST_CASE("akruti: broadphase pairs: overlapping chain", "[akruti][scene][broadphase]") {
    // Circles r=1 spaced 1.5 apart on x: each overlaps only its immediate neighbors.
    Scene scene;
    constexpr int N = 8;
    for (int i = 0; i < N; ++i) (void)scene.add(Circle{{Scalar(i) * Scalar(1.5), 0}, Scalar(1)});

    auto pairs = broadphase_pairs(scene);

    // No self-pairs, no duplicates (a<b enforced).
    for (auto& p : pairs) REQUIRE(p.a < p.b);
    REQUIRE(pairs.size() >= std::size_t(N - 1));
    REQUIRE(pairs.size() <= std::size_t(N) * std::size_t(N));
}

TEST_CASE("akruti: broadphase: isolated shape yields no pair", "[akruti][scene][broadphase]") {
    Scene scene;
    (void)scene.add(Circle{{0, 0}, Scalar(1)});
    (void)scene.add(Circle{{1000, 1000}, Scalar(1)});   // far away
    auto pairs = broadphase_pairs(scene);
    REQUIRE(pairs.empty());
}

TEST_CASE("akruti: bulk narrowphase matches direct epa", "[akruti][scene][narrowphase]") {
    Scene scene;
    (void)scene.add(Circle{{0, 0}, Scalar(1)});          // payload 0
    (void)scene.add(Circle{{Scalar(1.5), 0}, Scalar(1)}); // overlaps first by 0.5

    auto pairs = broadphase_pairs(scene);
    REQUIRE(pairs.size() == 1);

    auto contacts = bulk_narrowphase(scene, pairs);
    REQUIRE(contacts.size() == 1);
    REQUIRE(contacts[0].hit);

    // Compare to direct epa on the reconstructed prims.
    Circle a{{0, 0}, 1}, b{{Scalar(1.5), 0}, 1};
    Contact ref = epa(a, b);
    REQUIRE(contacts[0].depth == Catch::Approx(ref.depth).margin(0.02));
}

TEST_CASE("akruti: bulk point_inside matches brute force", "[akruti][scene][point]") {
    Scene scene;
    std::vector<Circle> shapes = {{{0, 0}, 2}, {{10, 0}, 1}, {{0, 10}, Scalar(1.5)}};
    for (auto& c : shapes) (void)scene.add(c);

    std::vector<V> pts = {{0, 0}, {1, 0}, {10, 0}, {5, 5}, {0, 10}, {100, 100}};
    std::vector<std::uint8_t> got(pts.size());
    bulk_point_inside(scene, pts, got);

    for (std::size_t i = 0; i < pts.size(); ++i) {
        bool ref = std::any_of(shapes.begin(), shapes.end(),
                               [&](const Circle& c) { return point_inside(c, pts[i]); });
        REQUIRE(bool(got[i]) == ref);
    }
}

TEST_CASE("akruti: bulk raycast matches single-shape min-t", "[akruti][scene][ray]") {
    Scene scene;
    std::vector<Circle> shapes = {{{5, 0}, 1}, {{12, 0}, 1}};
    for (auto& c : shapes) (void)scene.add(c);

    std::vector<Ray> rays = {Ray{{-5, 0}, {1, 0}, 100}};
    std::vector<RayHit> got(rays.size());
    bulk_raycast(scene, rays, got);

    // Reference: nearest-t over per-shape raycast.
    RayHit ref{}; ref.t = 1e30f;
    for (auto& c : shapes) {
        RayHit h = raycast(c, rays[0].o, rays[0].d, rays[0].tmax);
        if (h.hit && h.t < ref.t) ref = h;
    }
    REQUIRE(got[0].hit == ref.hit);
    REQUIRE(got[0].t == Catch::Approx(ref.t).margin(1e-2));
}

TEST_CASE("akruti: bulk sdf field matches min over shapes", "[akruti][scene][sdf]") {
    Scene scene;
    std::vector<Circle> shapes = {{{0, 0}, 2}, {{6, 0}, Scalar(1.5)}, {{0, 6}, 1}};
    for (auto& c : shapes) (void)scene.add(c);

    GridSpec grid{{-4, -4}, {1, 1}, 12, 12};
    std::vector<Scalar> field(std::size_t(grid.nx) * grid.ny);
    bulk_sdf_field(scene, grid, field);

    for (std::uint32_t iy = 0; iy < grid.ny; ++iy)
        for (std::uint32_t ix = 0; ix < grid.nx; ++ix) {
            const V p{grid.origin.x + Scalar(ix) * grid.cell.x,
                      grid.origin.y + Scalar(iy) * grid.cell.y};
            Scalar ref = 1e30f;
            for (auto& c : shapes) ref = std::min(ref, c.sdf(p));
            REQUIRE(field[std::size_t(iy) * grid.nx + ix] == Catch::Approx(ref).margin(1e-3));
        }
}

TEST_CASE("akruti: bulk ops are deterministic", "[akruti][scene][determinism]") {
    Scene scene;
    for (int i = 0; i < 20; ++i)
        (void)scene.add(Circle{{Scalar(i) * Scalar(1.2), 0}, Scalar(1)});

    auto pairs = broadphase_pairs(scene);
    auto c1 = bulk_narrowphase(scene, pairs);
    auto c2 = bulk_narrowphase(scene, pairs);
    REQUIRE(c1.size() == c2.size());
    for (std::size_t i = 0; i < c1.size(); ++i) {
        REQUIRE(c1[i].hit == c2[i].hit);
        REQUIRE(c1[i].depth == c2[i].depth);
    }

    std::vector<V> pts = {{0, 0}, {3, 0}, {6, 0}};
    std::vector<Scalar> f1(pts.size()), f2(pts.size());
    bulk_sdf_field(scene, pts, f1);
    bulk_sdf_field(scene, pts, f2);
    REQUIRE(f1 == f2);
}

TEST_CASE("akruti: empty scene: bulk ops no-op", "[akruti][scene][edge]") {
    Scene scene;
    REQUIRE(scene.count() == 0);
    auto pairs = broadphase_pairs(scene);
    REQUIRE(pairs.empty());
    std::vector<V> pts = {{0, 0}};
    std::vector<std::uint8_t> got(1);
    bulk_point_inside(scene, pts, got);
    REQUIRE(got[0] == 0);
}
