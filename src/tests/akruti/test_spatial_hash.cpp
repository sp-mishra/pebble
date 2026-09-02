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

    akruti::AABB<float> box1{pebble::math::vec2{0, 0}, pebble::math::vec2{1, 1}};
    akruti::AABB<float> box2{pebble::math::vec2{0.5f, 0.5f}, pebble::math::vec2{1.5f, 1.5f}};
    akruti::AABB<float> box3{pebble::math::vec2{10, 10}, pebble::math::vec2{11, 11}};

    sh.insert(box1, 1);
    sh.insert(box2, 2);
    sh.insert(box3, 3);

    std::vector<uint32_t> query_res;
    sh.query(akruti::AABB<float>{pebble::math::vec2{0, 0}, pebble::math::vec2{0.8f, 0.8f}}, [&](uint32_t id) {
        query_res.push_back(id);
    });

    REQUIRE(!query_res.empty());
}
