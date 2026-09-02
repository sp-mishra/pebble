#include <catch_amalgamated.hpp>
#include <containers/tensor/tensor.hpp>
#include <containers/tensor/pravaha_computation_policy.hpp>
#include <containers/tensor/tensor_edsl.hpp>
#include <vector>
#include <cmath>

using namespace ts;
using namespace ts::edsl;
using namespace ts::edsl::literals;

TEST_CASE (
"Pravaha Computation Policy: Basic Tensor Arithmetic"
,
"[tensor][pravaha][arithmetic]"
)
 {
    SECTION("Small Tensor Fast-Path (N < 2048)") {
        parallel_tensor<float> A({2, 3}, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f});
        parallel_tensor<float> B({2, 3}, {2.0f, 1.0f, 0.5f, 3.0f, 1.0f, 2.0f});

        auto C = A + B;
        REQUIRE(C({0, 0}) == Catch::Approx(3.0f));
        REQUIRE(C({0, 1}) == Catch::Approx(3.0f));
        REQUIRE(C({0, 2}) == Catch::Approx(3.5f));
        REQUIRE(C({1, 0}) == Catch::Approx(7.0f));
        REQUIRE(C({1, 1}) == Catch::Approx(6.0f));
        REQUIRE(C({1, 2}) == Catch::Approx(8.0f));

        auto D = A * 2.0f;
        REQUIRE(D({0, 0}) == Catch::Approx(2.0f));
        REQUIRE(D({1, 2}) == Catch::Approx(12.0f));

        auto E = A - B;
        REQUIRE(E({0, 0}) == Catch::Approx(-1.0f));
        REQUIRE(E({1, 2}) == Catch::Approx(4.0f));
    }

    SECTION("Large Tensor Multi-Core Parallel Path (N = 8192)") {
        constexpr std::size_t N = 8192;
        std::vector<float> init_a(N, 2.5f);
        std::vector<float> init_b(N, 1.5f);

        parallel_tensor<float> A({N}, init_a);
        parallel_tensor<float> B({N}, init_b);

        auto C = (A + B) * 2.0f;
        REQUIRE(C.size() == N);
        REQUIRE(C({0}) == Catch::Approx(8.0f));
        REQUIRE(C({N / 2}) == Catch::Approx(8.0f));
        REQUIRE(C({N - 1}) == Catch::Approx(8.0f));
    }
}

TEST_CASE (
"Pravaha Computation Policy: Parallel Statistical Reductions"
,
"[tensor][pravaha][reductions]"
)
 {
    SECTION("Parallel Sum and Mean on Large Buffer") {
        constexpr std::size_t N = 10000;
        std::vector<float> data(N);
        for (std::size_t i = 0; i < N; ++i) {
            data[i] = 1.0f;
        }

        parallel_tensor<float> T({N}, data);

        float total = ts::sum(T);
        REQUIRE(total == Catch::Approx(10000.0f));

        double avg = ts::mean(T);
        REQUIRE(avg == Catch::Approx(1.0));
    }

    SECTION("Parallel Min, Max and Variance") {
        constexpr std::size_t N = 4096;
        std::vector<float> data(N);
        for (std::size_t i = 0; i < N; ++i) {
            data[i] = static_cast<float>(i);
        }

        parallel_tensor<float> T({N}, data);

        float min_val = ts::min(T);
        float max_val = ts::max(T);

        REQUIRE(min_val == Catch::Approx(0.0f));
        REQUIRE(max_val == Catch::Approx(static_cast<float>(N - 1)));

        double var = ts::variance(T);
        double expected_var = (static_cast<double>(N) * static_cast<double>(N) - 1.0) / 12.0; // variance of uniform discrete 0..N-1
        REQUIRE(var == Catch::Approx(expected_var).epsilon(0.01));
    }
}

TEST_CASE (
"Pravaha Computation Policy: Parallel Matrix Multiplication"
,
"[tensor][pravaha][dot]"
)
 {
    SECTION("Parallel 2D Dot Product") {
        constexpr size_t M = 64;
        constexpr size_t K = 32;
        constexpr size_t N = 64;

        std::vector<float> a_data(M * K, 1.0f);
        std::vector<float> b_data(K * N, 2.0f);

        parallel_tensor<float> A({M, K}, a_data);
        parallel_tensor<float> B({K, N}, b_data);

        auto C = ts::dot(A, B);

        REQUIRE(C.shape().size() == 2);
        REQUIRE(C.shape()[0] == M);
        REQUIRE(C.shape()[1] == N);

        // Every cell = K * (1.0 * 2.0) = 32 * 2.0 = 64.0
        REQUIRE(C({0, 0}) == Catch::Approx(64.0f));
        REQUIRE(C({M - 1, N - 1}) == Catch::Approx(64.0f));
    }
}

TEST_CASE (
"Tensor EDSL with Pravaha Parallel Target"
,
"[tensor_edsl][pravaha][target]"
)
 {
    auto in = sym_tensor<2>("in", {4, 4});
    auto W = sym_tensor<2>("W", {4, 4});
    auto graph = ts::edsl::relu(ts::edsl::matmul(in, W) + "bias"_p);

    auto model = ts::compile(graph, ts::target::parallel);

    std::vector<float> in_data(16, 1.0f);
    std::vector<float> w_data(16, 2.0f);

    ts::tensor<float> t_in({4, 4}, in_data);
    ts::tensor<float> t_W({4, 4}, w_data);

    auto out = model("in"_t = t_in, "W"_t = t_W, "bias"_p = -2.0f);
    REQUIRE(out.shape()[0] == 4);
    REQUIRE(out.shape()[1] == 4);
    // Row dot product = 4 * (1.0 * 2.0) = 8.0, + (-2.0) = 6.0
    REQUIRE(out({0, 0}) == Catch::Approx(6.0f));
    REQUIRE(out({3, 3}) == Catch::Approx(6.0f));
}
