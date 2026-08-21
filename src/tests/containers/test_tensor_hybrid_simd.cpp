#include <catch_amalgamated.hpp>
#include <containers/tensor/tensor.hpp>
#include <containers/tensor/hybrid_simd_pravaha_policy.hpp>
#include <vector>
#include <cmath>

using namespace ts;

TEST_CASE("Hybrid SIMD + Pravaha: Basic Arithmetic Operations", "[tensor][hybrid][arithmetic]") {
    SECTION("Small Tensor Fast-Path (N < 2048)") {
        hybrid_tensor<float> A({2, 4}, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f});
        hybrid_tensor<float> B({2, 4}, {2.0f, 1.0f, 0.5f, 3.0f, 1.0f, 2.0f, 0.5f, 1.0f});

        auto C = A + B;
        REQUIRE(C({0, 0}) == Catch::Approx(3.0f));
        REQUIRE(C({0, 1}) == Catch::Approx(3.0f));
        REQUIRE(C({0, 2}) == Catch::Approx(3.5f));
        REQUIRE(C({1, 0}) == Catch::Approx(6.0f));
        REQUIRE(C({1, 3}) == Catch::Approx(9.0f));

        auto D = A * B;
        REQUIRE(D({0, 0}) == Catch::Approx(2.0f));
        REQUIRE(D({0, 2}) == Catch::Approx(1.5f));
        REQUIRE(D({1, 3}) == Catch::Approx(8.0f));

        auto E = A - B;
        REQUIRE(E({0, 0}) == Catch::Approx(-1.0f));
        REQUIRE(E({1, 3}) == Catch::Approx(7.0f));
    }

    SECTION("Large Tensor Multi-Core SIMD Path (N = 16384)") {
        constexpr std::size_t N = 16384;
        std::vector<float> data_a(N, 3.5f);
        std::vector<float> data_b(N, 1.5f);

        hybrid_tensor<float> A({N}, data_a.begin(), data_a.end());
        hybrid_tensor<float> B({N}, data_b.begin(), data_b.end());

        auto C = A + B;
        REQUIRE(C.size() == N);
        REQUIRE(C({0}) == Catch::Approx(5.0f));
        REQUIRE(C({N / 2}) == Catch::Approx(5.0f));
        REQUIRE(C({N - 1}) == Catch::Approx(5.0f));

        auto D = A / B;
        REQUIRE(D({0}) == Catch::Approx(3.5f / 1.5f));
        REQUIRE(D({N - 1}) == Catch::Approx(3.5f / 1.5f));
    }
}

TEST_CASE("Hybrid SIMD + Pravaha: Unary Operations & Reductions", "[tensor][hybrid][reductions]") {
    SECTION("Unary Sqrt, Abs, Square & Clip") {
        hybrid_tensor<float> A({4}, {4.0f, 9.0f, 16.0f, 25.0f});
        auto sq = ts::sqrt(A);
        REQUIRE(sq({0}) == Catch::Approx(2.0f));
        REQUIRE(sq({1}) == Catch::Approx(3.0f));
        REQUIRE(sq({2}) == Catch::Approx(4.0f));
        REQUIRE(sq({3}) == Catch::Approx(5.0f));

        hybrid_tensor<float> B({4}, {-5.0f, 2.0f, 10.0f, 100.0f});
        auto clipped = ts::clip(B, 0.0f, 50.0f);
        REQUIRE(clipped({0}) == Catch::Approx(0.0f));
        REQUIRE(clipped({1}) == Catch::Approx(2.0f));
        REQUIRE(clipped({2}) == Catch::Approx(10.0f));
        REQUIRE(clipped({3}) == Catch::Approx(50.0f));
    }

    SECTION("Large Buffer Parallel SIMD Reductions") {
        constexpr std::size_t N = 32768;
        std::vector<float> data(N, 2.0f);

        hybrid_tensor<float> T({N}, data.begin(), data.end());

        float total = ts::sum(T);
        REQUIRE(total == Catch::Approx(static_cast<float>(N * 2)));

        double avg = ts::mean(T);
        REQUIRE(avg == Catch::Approx(2.0));

        float max_val = ts::max(T);
        float min_val = ts::min(T);
        REQUIRE(max_val == Catch::Approx(2.0f));
        REQUIRE(min_val == Catch::Approx(2.0f));
    }
}

TEST_CASE("Hybrid SIMD + Pravaha: Blocked Parallel Matrix Multiplication", "[tensor][hybrid][dot]") {
    constexpr size_t M = 128;
    constexpr size_t K = 64;
    constexpr size_t N = 128;

    std::vector<float> a_data(M * K, 1.5f);
    std::vector<float> b_data(K * N, 2.0f);

    hybrid_tensor<float> A({M, K}, a_data.begin(), a_data.end());
    hybrid_tensor<float> B({K, N}, b_data.begin(), b_data.end());

    auto C = ts::dot(A, B);

    REQUIRE(C.shape().size() == 2);
    REQUIRE(C.shape()[0] == M);
    REQUIRE(C.shape()[1] == N);

    // Each cell = K * (1.5 * 2.0) = 64 * 3.0 = 192.0
    REQUIRE(C({0, 0}) == Catch::Approx(192.0f));
    REQUIRE(C({M / 2, N / 2}) == Catch::Approx(192.0f));
    REQUIRE(C({M - 1, N - 1}) == Catch::Approx(192.0f));
}
