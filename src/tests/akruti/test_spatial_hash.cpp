#include "catch_amalgamated.hpp"
#include "akruti/spatial_hash.hpp"
#include <vector>

TEST_CASE (
"Akruti: SpatialHash"
,
"[akruti][spatial_hash]"
)
 {
    akruti::SpatialHashBroadphase sh(2.0f);

    akruti::Box2 box1{pebble::math::vec2{0, 0}, pebble::math::vec2{1, 1}};
    akruti::Box2 box2{pebble::math::vec2{0.5f, 0.5f}, pebble::math::vec2{1.5f, 1.5f}};
    akruti::Box2 box3{pebble::math::vec2{10, 10}, pebble::math::vec2{11, 11}};

    sh.insert(box1, 1);
    sh.insert(box2, 2);
    sh.insert(box3, 3);

    std::vector<uint32_t> query_res;
    sh.query(akruti::Box2{pebble::math::vec2{0, 0}, pebble::math::vec2{0.8f, 0.8f}}, [&](uint32_t id) {
        query_res.push_back(id);
    });

    REQUIRE(!query_res.empty());
}

TEST_CASE("Akruti: MultiGridSpatialHash for high-scale entities", "[akruti][spatial_hash][multigrid]") {
    akruti::MultiGridSpatialHash<std::uint32_t, 1024> mg(2.0f);

    akruti::Box2 box1{pebble::math::vec2{0, 0}, pebble::math::vec2{1, 1}};
    akruti::Box2 box2{pebble::math::vec2{0.5f, 0.5f}, pebble::math::vec2{1.5f, 1.5f}};
    akruti::Box2 box3{pebble::math::vec2{100, 100}, pebble::math::vec2{101, 101}};

    mg.insert(box1, 10);
    mg.insert(box2, 20);
    mg.insert(box3, 30);

    std::vector<uint32_t> results;
    mg.query(akruti::Box2{pebble::math::vec2{0, 0}, pebble::math::vec2{0.8f, 0.8f}}, [&](uint32_t id) {
        results.push_back(id);
    });

    REQUIRE(results.size() >= 2);
    REQUIRE(mg.size() == 3);
}
