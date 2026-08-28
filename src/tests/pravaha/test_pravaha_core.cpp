#include "catch_amalgamated.hpp"

#include "pravaha/pravaha.hpp"

#include <type_traits>

TEST_CASE("Pravaha core task graph remains independently usable", "[pravaha][core]") {
    auto first = pravaha::task("first", [] {});
    auto second = pravaha::task("second", [] {});
    auto pipeline = first | second;

    STATIC_REQUIRE(std::same_as<decltype(pipeline.frontend.hash), std::size_t>);
    REQUIRE(pipeline.frontend.hash != 0);
    REQUIRE_FALSE(pipeline.frontend.dump.empty());
}
