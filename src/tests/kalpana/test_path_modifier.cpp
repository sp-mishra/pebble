#include "catch_amalgamated.hpp"
#include "kalpana/kalpana.hpp"

TEST_CASE("Kalpana: Geometry Modifiers and Shape Builders", "[kalpana][geom]") {
    using namespace kalpana;

    SECTION("Free-function Shape Constructors") {
        Path c = circle(50.0f, 50.0f, 25.0f);
        REQUIRE_FALSE(c.empty());

        Path r = rect(0.0f, 0.0f, 120.0f, 80.0f);
        REQUIRE_FALSE(r.empty());

        Path s = star(0.0f, 0.0f, 50.0f, 20.0f, 5);
        REQUIRE(s.points().size() == 10);
    }

    SECTION("Path Modifiers and Pipe Composition") {
        Path p = circle(0.0f, 0.0f, 40.0f);

        Path roughened = p | roughen(2.0f, 1.0f);
        REQUIRE_FALSE(roughened.empty());

        Path smoothed = roughened | smooth(2);
        REQUIRE_FALSE(smoothed.empty());

        Path offset_p = p | offset(5.0f);
        REQUIRE_FALSE(offset_p.empty());
    }

    SECTION("Chained PathModifier Object") {
        PathModifier mod = roughen(1.5f) | smooth(1) | offset(2.0f);
        Path p = rect(10.0f, 10.0f, 100.0f, 50.0f);
        Path transformed = p | mod;
        REQUIRE_FALSE(transformed.empty());
    }

    SECTION("Geometric Repetition") {
        Path p = circle(0.0f, 0.0f, 10.0f);
        Transform step = Transform::translate(20.0f, 0.0f);
        auto repeated = repeat(p, 5, step);
        REQUIRE(repeated.size() == 5);
    }
}
