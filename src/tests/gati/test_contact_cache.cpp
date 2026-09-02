#include "catch_amalgamated.hpp"
#include "gati/contact_cache.hpp"

TEST_CASE (
"Gati: ContactCache"
,
"[gati][contact_cache]"
)
 {
    gati::ContactCache cache;
    akruti::Manifold m;
    m.hit = true;
    m.depth = 0.5f;

    cache.store(1, 2, m, {0, 0}, {0, 1}, 0.0f, 0.0f);
    auto found = cache.find(1, 2);

    REQUIRE(found.has_value());
    REQUIRE(found->manifold.depth == 0.5f);

    cache.update_impulses(1, 2, 10.0f, 2.0f);
    found = cache.find(1, 2);
    REQUIRE(found->normal_impulse == 10.0f);
}
