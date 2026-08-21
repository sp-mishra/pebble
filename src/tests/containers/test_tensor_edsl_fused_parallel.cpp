#include <catch_amalgamated.hpp>
#include <containers/tensor/tensor.hpp>
#include <containers/tensor/tensor_edsl.hpp>
#include <vector>
#include <cmath>

using namespace ts;
using namespace ts::edsl;
using namespace ts::edsl::literals;

TEST_CASE("Tensor EDSL: Fused Elementwise Parallel Compilation Target", "[tensor][edsl][fused]") {
    SECTION("Small Tensor Fused Expression (N = 8)") {
        auto A = sym_tensor<2>("A", {2, 4});
        auto B = sym_tensor<2>("B", {2, 4});
        auto bias = "bias"_p;

        // Fused: relu(A * 2.0f + B - bias)
        auto graph = relu((A * 2.0f) + B - bias);

        auto model_cpu = ts::compile(graph, ts::target::cpu);
        auto model_par = ts::compile(graph, ts::target::parallel);

        tensor<float> tA({2, 4}, {1.0f, -2.0f, 3.0f, -4.0f, 5.0f, -6.0f, 7.0f, -8.0f});
        tensor<float> tB({2, 4}, {0.5f, 5.0f, -1.0f, 2.0f, 1.0f, 3.0f, -2.0f, 4.0f});

        auto res_cpu = model_cpu("A"_t = tA, "B"_t = tB, "bias"_p = 1.0f);
        auto res_par = model_par("A"_t = tA, "B"_t = tB, "bias"_p = 1.0f);

        REQUIRE(res_par.shape() == res_cpu.shape());
        for (std::size_t i = 0; i < res_cpu.size(); ++i) {
            REQUIRE(res_par.data()[i] == Catch::Approx(res_cpu.data()[i]));
        }

        // Cell 0: relu(1.0 * 2 + 0.5 - 1.0) = relu(1.5) = 1.5
        REQUIRE(res_par({0, 0}) == Catch::Approx(1.5f));
        // Cell 1: relu(-2.0 * 2 + 5.0 - 1.0) = relu(0.0) = 0.0
        REQUIRE(res_par({0, 1}) == Catch::Approx(0.0f));
    }

    SECTION("Large Tensor Fused Multi-Core Chunking (N = 65536)") {
        constexpr std::size_t N = 65536;
        auto X = sym_tensor<1>("X", {N});
        auto Y = sym_tensor<1>("Y", {N});

        // Fused sigmoid(X * Y + 0.5f)
        auto graph = sigmoid((X * Y) + 0.5f);
        auto model_par = ts::compile(graph, ts::target::pravaha);

        std::vector<float> x_data(N, 0.5f);
        std::vector<float> y_data(N, 2.0f);
        tensor<float> tX({N}, x_data);
        tensor<float> tY({N}, y_data);

        auto res = model_par("X"_t = tX, "Y"_t = tY);

        REQUIRE(res.size() == N);
        // sigmoid(0.5 * 2.0 + 0.5) = sigmoid(1.5) = 1 / (1 + exp(-1.5)) approx 0.817574
        const float expected = 1.0f / (1.0f + std::exp(-1.5f));
        REQUIRE(res({0}) == Catch::Approx(expected).epsilon(1e-4));
        REQUIRE(res({N / 2}) == Catch::Approx(expected).epsilon(1e-4));
        REQUIRE(res({N - 1}) == Catch::Approx(expected).epsilon(1e-4));
    }

    SECTION("Fused Unary Chain (sqrt, abs, gelu)") {
        auto A = sym_tensor<1>("A", {4});
        auto graph = gelu(sqrt(abs(A)));

        auto model_par = ts::compile(graph, ts::target::parallel);
        tensor<float> tA({4}, {-4.0f, -9.0f, 16.0f, 25.0f});

        auto res = model_par("A"_t = tA);
        REQUIRE(res.size() == 4);

        // sqrt(abs(-4.0)) = 2.0 -> gelu(2.0) approx 1.9545
        REQUIRE(res({0}) > 1.9f);
        // sqrt(abs(-9.0)) = 3.0 -> gelu(3.0) approx 2.996
        REQUIRE(res({1}) > 2.9f);
    }
}
