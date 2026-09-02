#include <catch_amalgamated.hpp>
#include <containers/numeric/math_vector.hpp>
#include <cmath>

using namespace pebble::math;

TEST_CASE (
"MathVector: Construction, static_tensor storage & constexpr"
,
"[math_vector][constexpr]"
)
 {
    SECTION("constexpr vec2, vec3, vec4, mat2, mat4 initialization") {
        constexpr vec2 v2(1.0f, 2.0f);
        static_assert(v2.shape()[0] == 2);
        static_assert(v2[0] == 1.0f && v2[1] == 2.0f);
        REQUIRE(v2[0] == Catch::Approx(1.0f));
        REQUIRE(v2[1] == Catch::Approx(2.0f));

        constexpr vec3 v3(10.0f, 20.0f, 30.0f);
        static_assert(v3[0] == 10.0f && v3[1] == 20.0f && v3[2] == 30.0f);
        REQUIRE(v3[2] == Catch::Approx(30.0f));

        constexpr vec4 v4(1.0f, 2.0f, 3.0f, 4.0f);
        static_assert(v4[3] == 4.0f);
        REQUIRE(v4[3] == Catch::Approx(4.0f));

        constexpr mat2 m2(1.0f, 2.0f, 3.0f, 4.0f);
        static_assert(m2[0, 0] == 1.0f && m2[1, 1] == 4.0f);
        static_assert(determinant(m2) == -2.0f);
        REQUIRE(determinant(m2) == Catch::Approx(-2.0f));

        constexpr mat4 id = identity4x4();
        static_assert(id[0, 0] == 1.0f && id[1, 1] == 1.0f && id[2, 2] == 1.0f && id[3, 3] == 1.0f);
        static_assert(id[0, 1] == 0.0f && id[1, 0] == 0.0f);
        REQUIRE(id[0, 0] == Catch::Approx(1.0f));
    }

    SECTION("Double and Integer vector variants") {
        vec3d vd(1.5, 2.5, 3.5);
        REQUIRE(vd[0] == Catch::Approx(1.5));

        vec4i vi(10, 20, 30, 40);
        REQUIRE(vi[0] == 10);
        REQUIRE(vi[3] == 40);
    }
}

TEST_CASE (
"MathVector: 3D Vector Geometry & Arithmetic"
,
"[math_vector][geometry]"
)
 {
    SECTION("Dot product, Cross product, Length, Normalization") {
        constexpr vec3 right(1.0f, 0.0f, 0.0f);
        constexpr vec3 up(0.0f, 1.0f, 0.0f);

        constexpr vec3 forward = cross(right, up);
        static_assert(forward[0] == 0.0f && forward[1] == 0.0f && forward[2] == 1.0f);
        REQUIRE(forward[2] == Catch::Approx(1.0f));

        constexpr float d = dot(right, up);
        static_assert(d == 0.0f);
        REQUIRE(d == Catch::Approx(0.0f));

        vec3 v(0.0f, 3.0f, 4.0f);
        REQUIRE(length_sq(v) == Catch::Approx(25.0f));
        REQUIRE(length(v) == Catch::Approx(5.0f));

        vec3 unit_v = normalize(v);
        REQUIRE(unit_v[0] == Catch::Approx(0.0f));
        REQUIRE(unit_v[1] == Catch::Approx(0.6f));
        REQUIRE(unit_v[2] == Catch::Approx(0.8f));
        REQUIRE(length(unit_v) == Catch::Approx(1.0f));
    }

    SECTION("Distance and Linear Interpolation (lerp)") {
        vec3 p1(0.0f, 0.0f, 0.0f);
        vec3 p2(6.0f, 8.0f, 0.0f);

        REQUIRE(distance(p1, p2) == Catch::Approx(10.0f));

        vec3 mid = lerp(p1, p2, 0.5f);
        REQUIRE(mid[0] == Catch::Approx(3.0f));
        REQUIRE(mid[1] == Catch::Approx(4.0f));
        REQUIRE(mid[2] == Catch::Approx(0.0f));
    }

    SECTION("Projection, Rejection and Vector Angles") {
        vec3 a(3.0f, 4.0f, 0.0f);
        vec3 b(1.0f, 0.0f, 0.0f);

        vec3 proj = project(a, b);
        REQUIRE(proj[0] == Catch::Approx(3.0f));
        REQUIRE(proj[1] == Catch::Approx(0.0f));

        vec3 rej = reject(a, b);
        REQUIRE(rej[0] == Catch::Approx(0.0f));
        REQUIRE(rej[1] == Catch::Approx(4.0f));

        double theta = angle(vec2(1.0f, 0.0f), vec2(0.0f, 1.0f));
        REQUIRE(theta == Catch::Approx(M_PI / 2.0));
    }
}

TEST_CASE (
"MathVector: Physics Optics (Ray Reflection & Snell Refraction)"
,
"[math_vector][physics]"
)
 {
    SECTION("Ray Surface Reflection") {
        vec3 incoming(1.0f, -1.0f, 0.0f);
        vec3 surface_normal(0.0f, 1.0f, 0.0f);

        vec3 reflected = reflect(incoming, surface_normal);
        REQUIRE(reflected[0] == Catch::Approx(1.0f));
        REQUIRE(reflected[1] == Catch::Approx(1.0f));
        REQUIRE(reflected[2] == Catch::Approx(0.0f));
    }

    SECTION("Snell's Law Refraction") {
        vec3 incoming(0.0f, -1.0f, 0.0f);
        vec3 surface_normal(0.0f, 1.0f, 0.0f);

        // Same refractive index (eta = 1.0)
        vec3 refracted = refract(incoming, surface_normal, 1.0f);
        REQUIRE(refracted[0] == Catch::Approx(0.0f));
        REQUIRE(refracted[1] == Catch::Approx(-1.0f));
        REQUIRE(refracted[2] == Catch::Approx(0.0f));
    }
}

TEST_CASE (
"MathVector: Matrix Transforms, Camera View & Perspective Projection"
,
"[math_vector][transforms]"
)
 {
    SECTION("Translation and Scale Affine Transforms") {
        mat4 T = translation(vec3(10.0f, -5.0f, 20.0f));
        vec3 p(1.0f, 2.0f, 3.0f);

        vec3 p_trans = mul_point(T, p);
        REQUIRE(p_trans[0] == Catch::Approx(11.0f));
        REQUIRE(p_trans[1] == Catch::Approx(-3.0f));
        REQUIRE(p_trans[2] == Catch::Approx(23.0f));

        mat4 S = scaling(vec3(2.0f, 3.0f, 4.0f));
        vec3 p_scale = mul_point(S, p);
        REQUIRE(p_scale[0] == Catch::Approx(2.0f));
        REQUIRE(p_scale[1] == Catch::Approx(6.0f));
        REQUIRE(p_scale[2] == Catch::Approx(12.0f));

        // Combined transform M = T * S
        mat4 M = mul(T, S);
        vec3 p_comb = mul_point(M, p);
        REQUIRE(p_comb[0] == Catch::Approx(12.0f));
        REQUIRE(p_comb[1] == Catch::Approx(1.0f));
        REQUIRE(p_comb[2] == Catch::Approx(32.0f));
    }

    SECTION("Camera Look-At View Transform") {
        vec3 eye(0.0f, 0.0f, 10.0f);
        vec3 target(0.0f, 0.0f, 0.0f);
        vec3 up(0.0f, 1.0f, 0.0f);

        mat4 V = look_at(eye, target, up);
        vec3 eye_in_view = mul_point(V, eye);
        REQUIRE(eye_in_view[0] == Catch::Approx(0.0f));
        REQUIRE(eye_in_view[1] == Catch::Approx(0.0f));
        REQUIRE(eye_in_view[2] == Catch::Approx(0.0f));
    }

    SECTION("Perspective Projection Matrix") {
        mat4 P = perspective(static_cast<float>(M_PI / 3.0), 16.0f / 9.0f, 0.1f, 100.0f);
        REQUIRE(P[3, 2] == Catch::Approx(-1.0f));
        REQUIRE(P[0, 0] > 0.0f);
        REQUIRE(P[1, 1] > 0.0f);
    }
}

TEST_CASE (
"MathVector: Quaternion Orientations & Slerp"
,
"[math_vector][quaternion]"
)
 {
    SECTION("Axis-Angle Quaternion Generation and Vector Rotation") {
        // Rotate (1, 0, 0) by 90 degrees around Z axis -> (0, 1, 0)
        quat q = quat_axis_angle(vec3(0.0f, 0.0f, 1.0f), static_cast<float>(M_PI / 2.0));
        vec3 v(1.0f, 0.0f, 0.0f);

        vec3 v_rot = quat_rotate(q, v);
        REQUIRE(v_rot[0] == Catch::Approx(0.0f).margin(1e-5f));
        REQUIRE(v_rot[1] == Catch::Approx(1.0f).margin(1e-5f));
        REQUIRE(v_rot[2] == Catch::Approx(0.0f).margin(1e-5f));
    }

    SECTION("Quaternion Slerp (Spherical Linear Interpolation)") {
        quat q_start = quat_identity();
        quat q_end = quat_axis_angle(vec3(0.0f, 0.0f, 1.0f), static_cast<float>(M_PI / 2.0));

        // Halfway rotation (45 degrees)
        quat q_mid = quat_slerp(q_start, q_end, 0.5f);
        vec3 v(1.0f, 0.0f, 0.0f);
        vec3 v_mid = quat_rotate(q_mid, v);

        REQUIRE(v_mid[0] == Catch::Approx(std::cos(M_PI / 4.0)).margin(1e-4f));
        REQUIRE(v_mid[1] == Catch::Approx(std::sin(M_PI / 4.0)).margin(1e-4f));
        REQUIRE(v_mid[2] == Catch::Approx(0.0f).margin(1e-4f));
    }
}
