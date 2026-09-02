#include <catch_amalgamated.hpp>
#include <containers/tensor/tensor_edsl.hpp>

using namespace ts;
using namespace ts::edsl;
using namespace ts::edsl::literals;

TEST_CASE (
"Tensor EDSL: Level 1 One-Shot Evaluation with _p scalar literals"
,
"[tensor_edsl][l1][scalar]"
)
 {
    SECTION("Basic polynomial evaluation: x^2 + 2x + 1") {
        auto x = "x"_p;
        auto expr = (x * x) + (2.0f * x) + 1.0f;

        float res1 = ts::eval_scalar(expr, "x"_p = 3.0f);
        REQUIRE(res1 == Catch::Approx(16.0f));

        float res2 = ts::eval_scalar(expr, "x"_p = 5.0f);
        REQUIRE(res2 == Catch::Approx(36.0f));
    }

    SECTION("Multiple named parameter bindings") {
        auto a = "a"_p;
        auto b = "b"_p;
        auto c = "c"_p;
        auto formula = (a * b) - (c / 2.0f);

        float res = ts::eval_scalar(formula, "a"_p = 10.0f, "b"_p = 4.0f, "c"_p = 6.0f);
        REQUIRE(res == Catch::Approx(37.0f));
    }
}

TEST_CASE (
"Tensor EDSL: Symbolic Tensor Leaves and Shape Inference"
,
"[tensor_edsl][shapes]"
)
 {
    SECTION("2D Matrix Multiplication Shape Propagation") {
        auto W = sym_tensor<2>("W", {4, 8});
        auto x = sym_tensor<2>("x", {8, 1});

        auto y = ts::edsl::matmul(W, x);
        REQUIRE(y.shape().size() == 2);
        REQUIRE(y.shape()[0] == 4);
        REQUIRE(y.shape()[1] == 1);
    }

    SECTION("Reduction Shape Propagation") {
        auto A = sym_tensor<2>("A", {10, 20});
        auto sum_all = ts::edsl::reduce_sum(A); // scalar
        REQUIRE(sum_all.shape().empty());

        auto sum_axis0 = ts::edsl::reduce_sum(A, 0); // [20]
        REQUIRE(sum_axis0.shape().size() == 1);
        REQUIRE(sum_axis0.shape()[0] == 20);

        auto sum_keep = ts::edsl::reduce_sum(A, 1, /*keepdims=*/true); // [10, 1]
        REQUIRE(sum_keep.shape().size() == 2);
        REQUIRE(sum_keep.shape()[0] == 10);
        REQUIRE(sum_keep.shape()[1] == 1);
    }

    SECTION("Broadcast Arithmetic Shape Propagation") {
        auto A = sym_tensor<2>("A", {1, 64});
        auto B = sym_tensor<2>("B", {32, 1});
        auto C = A + B; // broadcast -> [32, 64]
        REQUIRE(C.shape().size() == 2);
        REQUIRE(C.shape()[0] == 32);
        REQUIRE(C.shape()[1] == 64);
    }
}

TEST_CASE (
"Tensor EDSL: Level 1 Concrete Tensor Expression Evaluation"
,
"[tensor_edsl][l1][tensor]"
)
 {
    SECTION("Linear Layer: y = relu(X * W + b)") {
        ts::tensor<float> X({2, 3}, {
            1.0f, -2.0f, 3.0f,
           -4.0f,  5.0f, -6.0f
        });

        ts::tensor<float> W({3, 2}, {
            1.0f, 2.0f,
            0.5f, -1.0f,
            2.0f, 1.0f
        });

        ts::tensor<float> b({2}, {0.5f, -0.5f});

        auto x_leaf = "X"_t;
        auto w_leaf = "W"_t;
        auto b_leaf = "b"_t;

        auto lin_expr = ts::edsl::relu(ts::edsl::matmul(x_leaf, w_leaf) + b_leaf);

        auto out = ts::eval(lin_expr, "X"_t = X, "W"_t = W, "b"_t = b);
        REQUIRE(out.shape().size() == 2);
        REQUIRE(out.shape()[0] == 2);
        REQUIRE(out.shape()[1] == 2);

        // Row 0: X[0]*W = [1*1 + -2*0.5 + 3*2, 1*2 + -2*-1 + 3*1] = [6.0, 7.0]
        // + b = [6.5, 6.5] -> relu -> [6.5, 6.5]
        REQUIRE(out({0, 0}) == Catch::Approx(6.5f));
        REQUIRE(out({0, 1}) == Catch::Approx(6.5f));

        // Row 1: X[1]*W = [-4*1 + 5*0.5 + -6*2, -4*2 + 5*-1 + -6*1] = [-13.5, -19.0]
        // + b = [-13.0, -19.5] -> relu -> [0.0, 0.0]
        REQUIRE(out({1, 0}) == Catch::Approx(0.0f));
        REQUIRE(out({1, 1}) == Catch::Approx(0.0f));
    }

    SECTION("Activation Functions: Sigmoid, GeLU, Softmax") {
        ts::tensor<float> v({3}, {-1.0f, 0.0f, 1.0f});

        auto sig_out = ts::eval(ts::edsl::sigmoid("v"_t), "v"_t = v);
        REQUIRE(sig_out({1}) == Catch::Approx(0.5f)); // sigmoid(0) == 0.5

        auto sm_out = ts::eval(ts::edsl::softmax("v"_t), "v"_t = v);
        float sm_sum = sm_out({0}) + sm_out({1}) + sm_out({2});
        REQUIRE(sm_sum == Catch::Approx(1.0f)); // Softmax sum is 1.0
    }
}

TEST_CASE (
"Tensor EDSL: Level 2 Compile Once, Run Many"
,
"[tensor_edsl][l2][compile]"
)
 {
    auto X = sym_tensor<2>("X", {2, 2});
    auto W = sym_tensor<2>("W", {2, 2});
    auto formula = ts::edsl::matmul(X, W) * "scale"_p;

    auto model = ts::compile(formula, ts::target::cpu);

    ts::tensor<float> tX({2, 2}, {1.0f, 2.0f, 3.0f, 4.0f});
    ts::tensor<float> tW({2, 2}, {2.0f, 0.0f, 1.0f, 2.0f});

    // Run iteration 1 with scale = 1.0
    auto res1 = model("X"_t = tX, "W"_t = tW, "scale"_p = 1.0f);
    REQUIRE(res1({0, 0}) == Catch::Approx(4.0f));
    REQUIRE(res1({0, 1}) == Catch::Approx(4.0f));
    REQUIRE(res1({1, 0}) == Catch::Approx(10.0f));
    REQUIRE(res1({1, 1}) == Catch::Approx(8.0f));

    // Run iteration 2 with scale = 2.5
    auto res2 = model("X"_t = tX, "W"_t = tW, "scale"_p = 2.5f);
    REQUIRE(res2({0, 0}) == Catch::Approx(10.0f));
    REQUIRE(res2({1, 0}) == Catch::Approx(25.0f));
}

#if __has_include(<mlx/mlx.h>)
TEST_CASE ("Tensor EDSL: Level 2 Apple Silicon MLX GPU Target", "[tensor_edsl][gpu][mlx]") {
    auto A = sym_tensor<2>("A", {2, 2});
    auto B = sym_tensor<2>("B", {2, 2});
    auto graph = ts::edsl::matmul(A, B) + "bias"_p;

    auto gpu_model = ts::compile(graph, ts::target::gpu);

    ts::tensor<float> tA({2, 2}, {1.0f, 0.0f, 0.0f, 1.0f});
    ts::tensor<float> tB({2, 2}, {5.0f, 6.0f, 7.0f, 8.0f});

    auto gpu_res = gpu_model("A"_t = tA, "B"_t = tB, "bias"_p = 2.0f);
    REQUIRE(gpu_res({0, 0}) == Catch::Approx(7.0f));
    REQUIRE(gpu_res({0, 1}) == Catch::Approx(8.0f));
    REQUIRE(gpu_res({1, 0}) == Catch::Approx(9.0f));
    REQUIRE(gpu_res({1, 1}) == Catch::Approx(10.0f));
}
#endif
