#include <catch_amalgamated.hpp>
#include <containers/tensor/tensor.hpp>
#include <containers/tensor/highway_computation_policy.hpp>
#include <containers/tensor/tensor_utils.hpp>
#if __has_include(<mlx/mlx.h>)
#include <containers/tensor/mlx_storage_policy.hpp>
#include <containers/tensor/mlx_computation_policy.hpp>
#endif

#include <numeric>
#include <vector>

using cpu_tensor = ts::tensor<float, ts::default_storage_policy, ts::default_computation_policy>;
using simd_tensor = ts::tensor<float, ts::highway_storage_policy, ts::highway_computation_policy>;

#if __has_include(<mlx/mlx.h>)
using gpu_tensor_f32 = ts::gpu_tensor<float>;
#endif

TEST_CASE("tensor: CPU tensor basic ops", "[tensor][cpu][basic]") {
    SECTION("Addition, multiplication, sum, mean, max") {
        std::vector<size_t> shape = {4, 4};
        cpu_tensor t1(shape), t2(shape);
        for (size_t i = 0; i < t1.size(); ++i) {
            t1.data()[i] = static_cast<float>(i);
            t2.data()[i] = static_cast<float>(i * 2);
        }
        auto t3 = t1 + t2;
        REQUIRE(t3.size() == t1.size());
        for (size_t i = 0; i < t3.size(); ++i) {
            REQUIRE(t3.data()[i] == Catch::Approx(t1.data()[i] + t2.data()[i]));
        }
        auto t4 = t1 * t2;
        for (size_t i = 0; i < t4.size(); ++i) {
            REQUIRE(t4.data()[i] == Catch::Approx(t1.data()[i] * t2.data()[i]));
        }
        float s = ts::sum(t1);
        REQUIRE(s == Catch::Approx(120.0f));
        float m = ts::mean(t1);
        REQUIRE(m == Catch::Approx(7.5f));
        float mx = ts::max(t2);
        REQUIRE(mx == Catch::Approx(30.0f));

        // Format to string test
        std::string s_formatted = ts::tensor_to_string(t1);
        REQUIRE_FALSE(s_formatted.empty());
        REQUIRE(s_formatted.front() == '[');
        REQUIRE(s_formatted.back() == ']');
    }
}

TEST_CASE("tensor: SIMD tensor elementwise ops", "[tensor][simd][elementwise]") {
    SECTION("Addition and division") {
        std::vector<size_t> shape = {8};
        simd_tensor t1(shape), t2(shape);
        for (size_t i = 0; i < t1.size(); ++i) {
            t1.data()[i] = static_cast<float>(i + 1);
            t2.data()[i] = static_cast<float>(2 * (i + 1));
        }
        auto t3 = t1 + t2;
        for (size_t i = 0; i < t3.size(); ++i) {
            REQUIRE(t3.data()[i] == Catch::Approx(t1.data()[i] + t2.data()[i]));
        }
        auto t4 = t1 / (t2 + 1.0f);
        for (size_t i = 0; i < t4.size(); ++i) {
            REQUIRE(t4.data()[i] == Catch::Approx(t1.data()[i] / (t2.data()[i] + 1.0f)));
        }
    }

    SECTION("SIMD reductions: sum, mean, max, min") {
        std::vector<size_t> shape = {16};
        simd_tensor t(shape);
        for (size_t i = 0; i < t.size(); ++i) {
            t.data()[i] = static_cast<float>(i + 1);
        }
        float s = ts::sum(t);
        REQUIRE(s == Catch::Approx(136.0f));
        float m = ts::mean(t);
        REQUIRE(m == Catch::Approx(8.5f));
        float mx = ts::max(t);
        REQUIRE(mx == Catch::Approx(16.0f));
        float mn = ts::min(t);
        REQUIRE(mn == Catch::Approx(1.0f));
    }
}

TEST_CASE("tensor: Dot product (CPU & SIMD)", "[tensor][cpu][dot][matmul]") {
    SECTION("Matrix-matrix dot") {
        std::vector<size_t> shapeA = {2, 3};
        std::vector<size_t> shapeB = {3, 2};
        cpu_tensor a(shapeA), b(shapeB);
        for (size_t i = 0; i < a.size(); ++i) a.data()[i] = static_cast<float>(i + 1);
        for (size_t i = 0; i < b.size(); ++i) b.data()[i] = static_cast<float>(i + 1);
        auto result = ts::dot(a, b);
        REQUIRE(result.shape() == std::vector<size_t>({2, 2}));
        REQUIRE(result({0, 0}) == Catch::Approx(22.0f));
        REQUIRE(result({0, 1}) == Catch::Approx(28.0f));
        REQUIRE(result({1, 0}) == Catch::Approx(49.0f));
        REQUIRE(result({1, 1}) == Catch::Approx(64.0f));
    }

    SECTION("Vector-vector dot") {
        std::vector<size_t> shape = {4};
        cpu_tensor v1(shape), v2(shape);
        for (size_t i = 0; i < 4; ++i) {
            v1.data()[i] = float(i + 1);
            v2.data()[i] = float(2 * (i + 1));
        }
        auto result = ts::dot(v1, v2);
        REQUIRE(result.size() == 1);
        REQUIRE(result.data()[0] == Catch::Approx(1*2 + 2*4 + 3*6 + 4*8));
    }

    SECTION("Matrix-vector dot") {
        std::vector<size_t> shapeA = {2, 3};
        std::vector<size_t> shapeB = {3};
        cpu_tensor a(shapeA), b(shapeB);
        for (size_t i = 0; i < a.size(); ++i) a.data()[i] = static_cast<float>(i + 1);
        for (size_t i = 0; i < b.size(); ++i) b.data()[i] = static_cast<float>(i + 1);
        auto result = ts::dot(a, b);
        REQUIRE(result.shape() == std::vector<size_t>({2}));
        REQUIRE(result({0}) == Catch::Approx(1*1 + 2*2 + 3*3));
        REQUIRE(result({1}) == Catch::Approx(4*1 + 5*2 + 6*3));
    }
}

TEST_CASE("tensor: C++23 indexing and views", "[tensor][indexing][views]") {
    SECTION("Multidimensional operator[]") {
        ts::static_tensor<int, ts::default_storage_policy, ts::default_computation_policy, 2, 3> st = {
            1, 2, 3,
            4, 5, 6
        };
        REQUIRE(st[0, 0] == 1);
        REQUIRE(st[0, 2] == 3);
        REQUIRE(st[1, 1] == 5);
        st[1, 2] = 42;
        REQUIRE(st[1, 2] == 42);
    }

    SECTION("Dynamic tensor slicing") {
        cpu_tensor t({3, 3});
        for (size_t i = 0; i < t.size(); ++i) t.data()[i] = static_cast<float>(i + 1);

        auto view = t(ts::all, ts::slice(std::pair<size_t, size_t>{0, 2}));
        REQUIRE(view.shape() == std::vector<size_t>({3, 2}));
        REQUIRE(view(0, 0) == Catch::Approx(1.0f));
        REQUIRE(view(0, 1) == Catch::Approx(2.0f));
        REQUIRE(view(1, 0) == Catch::Approx(4.0f));
        REQUIRE(view(1, 1) == Catch::Approx(5.0f));
    }
}

TEST_CASE("tensor: Mathematical functions and statistics", "[tensor][math][stats]") {
    SECTION("Normalize, std_dev, variance") {
        cpu_tensor t({4}, {2.0f, 4.0f, 4.0f, 2.0f});
        float m = ts::mean(t);
        REQUIRE(m == Catch::Approx(3.0f));
        auto norm = ts::normalize(t);
        REQUIRE(norm.size() == 4);
        REQUIRE(ts::mean(norm) == Catch::Approx(0.0f).margin(1e-5));
    }

    SECTION("Unary math: sqrt, abs, square, clip") {
        cpu_tensor t({4}, {4.0f, 9.0f, 16.0f, 25.0f});
        auto sq = ts::sqrt(t);
        REQUIRE(sq({0}) == Catch::Approx(2.0f));
        REQUIRE(sq({1}) == Catch::Approx(3.0f));
        REQUIRE(sq({2}) == Catch::Approx(4.0f));
        REQUIRE(sq({3}) == Catch::Approx(5.0f));

        auto clipped = ts::clip(t, 5.0f, 20.0f);
        REQUIRE(clipped({0}) == Catch::Approx(5.0f));
        REQUIRE(clipped({3}) == Catch::Approx(20.0f));
    }

    SECTION("Reshape, flatten, transpose") {
        cpu_tensor t({2, 3}, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f});
        auto tr = ts::transpose(t);
        REQUIRE(tr.shape() == std::vector<size_t>({3, 2}));
        REQUIRE(tr({0, 0}) == Catch::Approx(1.0f));
        REQUIRE(tr({0, 1}) == Catch::Approx(4.0f));
        REQUIRE(tr({2, 1}) == Catch::Approx(6.0f));

        auto fl = ts::flatten(t);
        REQUIRE(fl.shape() == std::vector<size_t>({6}));
        REQUIRE(fl.size() == 6);
    }
}

TEST_CASE("tensor: Arrow-style string storage", "[tensor][arrow][strings]") {
    ts::arrow_string_storage strings({"hello", "modern", "pebble", "tensor"});
    REQUIRE(strings.size() == 4);
    REQUIRE(strings[0] == "hello");
    REQUIRE(strings[1] == "modern");
    REQUIRE(strings[2] == "pebble");
    REQUIRE(strings[3] == "tensor");

    std::vector<std::string> collected;
    for (auto sv : strings) {
        collected.push_back(std::string(sv));
    }
    REQUIRE(collected.size() == 4);
    REQUIRE(collected[2] == "pebble");
}

TEST_CASE("tensor: Cross-policy assignment", "[tensor][cross][policy]") {
    std::vector<size_t> shape = {3};
    cpu_tensor cpu(shape);
    simd_tensor simd(shape);
    for (size_t i = 0; i < 3; ++i) {
        cpu.data()[i] = float(i + 1);
        simd.data()[i] = float(i + 2);
    }
    cpu_tensor cpu2(simd);
    for (size_t i = 0; i < 3; ++i) REQUIRE(cpu2.data()[i] == Catch::Approx(simd.data()[i]));

    simd_tensor simd2(cpu);
    for (size_t i = 0; i < 3; ++i) REQUIRE(simd2.data()[i] == Catch::Approx(cpu.data()[i]));
}

TEST_CASE("tensor: SmallTensor and Smriti Arena integration", "[tensor][small][smriti]") {
    SECTION("SmallTensor inline buffer (SBO)") {
        // Fits within 64-byte inline budget (16 floats)
        ts::small_tensor<float, 64> st({2, 4});
        REQUIRE(st.size() == 8);
        for (size_t i = 0; i < st.size(); ++i) st.data()[i] = static_cast<float>(i + 1);
        
        auto st2 = st * 2.0f;
        REQUIRE(st2.size() == 8);
        REQUIRE(st2({0, 0}) == Catch::Approx(2.0f));
        REQUIRE(st2({1, 3}) == Catch::Approx(16.0f));
        REQUIRE(ts::sum(st2) == Catch::Approx(72.0f));
    }

    SECTION("Smriti Arena-backed tensor") {
        smriti::pools::BumpPool<smriti::domains::SystemRAMDomain> pool{4096};
        using arena_tensor = ts::smriti_tensor<float, decltype(pool)>;

        arena_tensor at({3, 3}, pool);
        REQUIRE(at.size() == 9);
        for (size_t i = 0; i < at.size(); ++i) at.data()[i] = static_cast<float>(i + 1);

        float s = ts::sum(at);
        REQUIRE(s == Catch::Approx(45.0f));
        float mx = ts::max(at);
        REQUIRE(mx == Catch::Approx(9.0f));
    }
}

#if __has_include(<mlx/mlx.h>)
TEST_CASE("tensor: MLX Apple Silicon GPU execution", "[tensor][mlx][gpu]") {
    SECTION("MLX GPU array creation, addition, and reductions") {
        gpu_tensor_f32 g1({4}, {1.0f, 2.0f, 3.0f, 4.0f});
        gpu_tensor_f32 g2({4}, {10.0f, 20.0f, 30.0f, 40.0f});

        auto g3 = g1 + g2;
        REQUIRE(g3.size() == 4);
        REQUIRE(g3({0}) == Catch::Approx(11.0f));
        REQUIRE(g3({1}) == Catch::Approx(22.0f));
        REQUIRE(g3({2}) == Catch::Approx(33.0f));
        REQUIRE(g3({3}) == Catch::Approx(44.0f));

        float s = ts::sum(g3);
        REQUIRE(s == Catch::Approx(110.0f));
        float mx = ts::max(g3);
        REQUIRE(mx == Catch::Approx(44.0f));
    }
}
#endif

TEST_CASE("tensor: constexpr static_tensor evaluation", "[tensor][static][constexpr]") {
    SECTION("constexpr static_tensor construction and element access") {
        constexpr ts::static_tensor<float, ts::default_storage_policy, ts::default_computation_policy, 2, 2> mat(
            1.0f, 2.0f,
            3.0f, 4.0f
        );
        static_assert(mat.shape()[0] == 2 && mat.shape()[1] == 2);
        static_assert(mat[0, 0] == 1.0f);
        static_assert(mat[0, 1] == 2.0f);
        static_assert(mat[1, 0] == 3.0f);
        static_assert(mat[1, 1] == 4.0f);

        REQUIRE(mat[0, 0] == 1.0f);
        REQUIRE(mat[1, 1] == 4.0f);
    }
}

struct Particle {
    float x, y, z;
    float vx, vy, vz;
    int id;
};

TEST_CASE("tensor: Structure-of-Arrays (SoA) Reflection Storage", "[tensor][soa]") {
    SECTION("SoA column layout and reconstruction") {
        meta::soa_storage<Particle, 16> soa;
        soa.push_back(Particle{1.0f, 2.0f, 3.0f, 0.1f, 0.2f, 0.3f, 42});
        soa.push_back(Particle{4.0f, 5.0f, 6.0f, 0.4f, 0.5f, 0.6f, 100});

        REQUIRE(soa.size() == 2);
        REQUIRE(soa.column<0>()[0] == Catch::Approx(1.0f));
        REQUIRE(soa.column<0>()[1] == Catch::Approx(4.0f));
        REQUIRE(soa.column<6>()[0] == 42);
        REQUIRE(soa.column<6>()[1] == 100);

        Particle p0 = soa.get(0);
        REQUIRE(p0.x == Catch::Approx(1.0f));
        REQUIRE(p0.y == Catch::Approx(2.0f));
        REQUIRE(p0.id == 42);
    }
}

