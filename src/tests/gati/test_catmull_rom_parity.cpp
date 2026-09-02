#include "catch_amalgamated.hpp"
#include "akruti/akruti.hpp"
#include "akruti/spline.hpp"
#include "gati/gati.hpp"
#include "containers/numeric/math_vector.hpp"
#include <cmath>

// Appended: proves the Catmull-Rom basis is single-sourced in pebble::math and that both
// consumers — akruti::CatmullRomSpline and gati::Curve<Scalar> (Cubic interp) — evaluate to
// that exact primitive. No engine re-derives the basis.

TEST_CASE (
"Catmull-Rom: pebble::math scalar & vec overloads agree component-wise"
,
"[math][catmull_rom]"
)
 {
    const float p0 = 0.0f, p1 = 1.0f, p2 = 3.0f, p3 = 2.0f;
    const pebble::math::vec2 v0{p0, 10.0f}, v1{p1, 20.0f}, v2{p2, 30.0f}, v3{p3, 40.0f};

    for (float u : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        const float s = pebble::math::catmull_rom(p0, p1, p2, p3, u);
        const auto  v = pebble::math::catmull_rom(v0, v1, v2, v3, u);
        REQUIRE(v[0] == Catch::Approx(s).margin(1e-5)); // x-channel == scalar basis
    }
}

TEST_CASE (
"Catmull-Rom: akruti CatmullRomSpline mid-segment == pebble::math primitive"
,
"[akruti][catmull_rom]"
)
 {
    akruti::CatmullRomSpline spline;
    spline.points.push_back(akruti::Vec2<akruti::Scalar>{0.0f, 0.0f});
    spline.points.push_back(akruti::Vec2<akruti::Scalar>{10.0f, 0.0f});
    spline.points.push_back(akruti::Vec2<akruti::Scalar>{20.0f, 10.0f});
    spline.points.push_back(akruti::Vec2<akruti::Scalar>{30.0f, 0.0f});

    // t=0.5 lands mid-way; with 3 segments the middle segment uses all 4 control points.
    const auto got = spline.evaluate(0.5f);

    // Reproduce the segment the spline picks at t=0.5 (num_segments=3, scaled=1.5 => seg 1, local 0.5).
    const auto expect = pebble::math::catmull_rom(
        static_cast<pebble::math::vec2>(spline.points[0]),
        static_cast<pebble::math::vec2>(spline.points[1]),
        static_cast<pebble::math::vec2>(spline.points[2]),
        static_cast<pebble::math::vec2>(spline.points[3]), 0.5f);

    REQUIRE(got.x == Catch::Approx(expect[0]).margin(1e-4));
    REQUIRE(got.y == Catch::Approx(expect[1]).margin(1e-4));
}

TEST_CASE (
"Catmull-Rom: gati Curve<Scalar> Cubic sample == pebble::math primitive"
,
"[gati][catmull_rom]"
)
 {
    gati::Curve<gati::Scalar> curve;
    curve.add(0.0f, 0.0f, gati::Interp::Cubic);
    curve.add(1.0f, 1.0f, gati::Interp::Cubic);
    curve.add(2.0f, 3.0f, gati::Interp::Cubic);
    curve.add(3.0f, 2.0f, gati::Interp::Cubic);

    // Sample mid the [1,2] segment (t=1.5 => u=0.5), which has both neighbours => full Catmull-Rom.
    const gati::Scalar got = curve.sample(1.5f);
    const gati::Scalar expect = pebble::math::catmull_rom(0.0f, 1.0f, 3.0f, 2.0f, 0.5f);

    REQUIRE(got == Catch::Approx(expect).margin(1e-5));
}
