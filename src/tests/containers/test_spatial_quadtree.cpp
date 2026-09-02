#include <catch_amalgamated.hpp>
#include "containers/spatial/quadtree.hpp"
#include <vector>

using namespace containers::spatial;

TEST_CASE (
"Spatial: QuadTree Construction, Point Insertion & Morton Encoding"
,
"[quadtree][spatial]"
)
 {
    BoundingBox2D box{pebble::math::vec2{-100.0f, -100.0f}, pebble::math::vec2{100.0f, 100.0f}};
    QuadTree<std::uint32_t, 4> tree(box);

    REQUIRE(tree.empty());
    REQUIRE(tree.size() == 0);

    SECTION("Morton Z-Order Encoding Consistency") {
        const std::uint32_t code1 = morton_encode_2d(-50.0f, -50.0f, -100.0f, -100.0f, 1.0f / 200.0f, 1.0f / 200.0f);
        const std::uint32_t code2 = morton_encode_2d(50.0f, 50.0f, -100.0f, -100.0f, 1.0f / 200.0f, 1.0f / 200.0f);
        REQUIRE(code1 < code2);
    }

    SECTION("Point Insertion & Dynamic Node Subdivision") {
        tree.insert(pebble::math::vec2{-20.0f, -20.0f}, 1);
        tree.insert(pebble::math::vec2{-30.0f, -30.0f}, 2);
        tree.insert(pebble::math::vec2{-40.0f, -40.0f}, 3);
        tree.insert(pebble::math::vec2{-50.0f, -50.0f}, 4);
        // Exceeds MaxLeafElements (4) -> triggers subdivision
        tree.insert(pebble::math::vec2{-60.0f, -60.0f}, 5);

        REQUIRE(tree.size() == 5);
        REQUIRE_FALSE(tree.node(tree.root()).is_leaf);
    }

    SECTION("Spatial Range Query") {
        tree.insert(pebble::math::vec2{-10.0f, -10.0f}, 10);
        tree.insert(pebble::math::vec2{10.0f, 10.0f}, 20);
        tree.insert(pebble::math::vec2{50.0f, 50.0f}, 30);

        BoundingBox2D query{pebble::math::vec2{-20.0f, -20.0f}, pebble::math::vec2{20.0f, 20.0f}};
        std::vector<std::uint32_t> found;
        tree.query_range(query, [&](const pebble::math::vec2&, std::uint32_t payload) {
            found.push_back(payload);
        });

        REQUIRE(found.size() == 2);
        CHECK(std::find(found.begin(), found.end(), 10) != found.end());
        CHECK(std::find(found.begin(), found.end(), 20) != found.end());
        CHECK(std::find(found.begin(), found.end(), 30) == found.end());
    }

    SECTION("Spatial Radial Query") {
        tree.insert(pebble::math::vec2{0.0f, 0.0f}, 1);
        tree.insert(pebble::math::vec2{3.0f, 4.0f}, 2); // dist = 5
        tree.insert(pebble::math::vec2{10.0f, 10.0f}, 3); // dist = 14.14

        std::vector<std::uint32_t> in_circle;
        tree.query_radius(pebble::math::vec2{0.0f, 0.0f}, 6.0f, [&](const pebble::math::vec2&, std::uint32_t id) {
            in_circle.push_back(id);
        });

        REQUIRE(in_circle.size() == 2);
        CHECK(std::find(in_circle.begin(), in_circle.end(), 1) != in_circle.end());
        CHECK(std::find(in_circle.begin(), in_circle.end(), 2) != in_circle.end());
    }
}
