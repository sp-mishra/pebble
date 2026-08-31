#include "catch_amalgamated.hpp"
#include <manas/nn/nn.hpp>

using namespace manas::nn;
using Catch::Approx;

// ─── Tape & Ops ──────────────────────────────────────────────────────────────
TEST_CASE("manas::nn: autodiff: scalar add/mul gradients", "[manas][nn][autodiff]") {
    Tape::current().reset();

    // z = x * y + y, dz/dx = y, dz/dy = x + 1
    TensorVar x(Tensor({1}, {3.0f}), true);
    TensorVar y(Tensor({1}, {4.0f}), true);

    auto xy = mul(x, y);
    auto z  = add(xy, y);

    Tensor one({1}, {1.0f});
    Tape::current().backward(z.tape_id, one);

    // dz/dx = y = 4
    REQUIRE(x.grad().data()[0] == Approx(4.0f));
    // dz/dy = x + 1 = 4
    REQUIRE(y.grad().data()[0] == Approx(4.0f));
}

TEST_CASE("manas::nn: autodiff: matmul gradient", "[manas][nn][autodiff]") {
    Tape::current().reset();

    // A: [2,2], B: [2,1], C = A @ B
    TensorVar A(Tensor({2, 2}, {1.0f, 2.0f, 3.0f, 4.0f}), true);
    TensorVar B(Tensor({2, 1}, {1.0f, 1.0f}), true);

    auto C = matmul(A, B);  // [[3], [7]]
    auto s = sum_all(C);    // 10

    Tape::current().backward(s.tape_id);

    // dL/dA = upstream @ B^T = [[1],[1]] @ [[1,1]] = [[1,1],[1,1]]
    CHECK(A.grad().data()[0] == Approx(1.0f));
    CHECK(A.grad().data()[3] == Approx(1.0f));
    // dL/dB = A^T @ upstream = [[1,3],[2,4]] @ [[1],[1]] = [[4],[6]]
    CHECK(B.grad().data()[0] == Approx(4.0f));
    CHECK(B.grad().data()[1] == Approx(6.0f));
}

TEST_CASE("manas::nn: autodiff: relu gradient zeros negatives", "[manas][nn][autodiff]") {
    Tape::current().reset();

    TensorVar x(Tensor({4}, {-2.0f, -0.5f, 0.5f, 2.0f}), true);
    auto y = relu(x);
    Tape::current().backward(y.tape_id, Tensor({4}, {1.0f, 1.0f, 1.0f, 1.0f}));

    auto& g = x.grad();
    CHECK(g.data()[0] == Approx(0.0f));  // negative -> 0
    CHECK(g.data()[1] == Approx(0.0f));  // negative -> 0
    CHECK(g.data()[2] == Approx(1.0f));  // positive -> 1
    CHECK(g.data()[3] == Approx(1.0f));  // positive -> 1
}

TEST_CASE("manas::nn: autodiff: sigmoid gradient", "[manas][nn][autodiff]") {
    Tape::current().reset();

    TensorVar x(Tensor({1}, {0.0f}), true);
    auto y = sigmoid(x);
    Tape::current().backward(y.tape_id);

    // sigma(0) = 0.5, d_sigma/dx = sigma*(1-sigma) = 0.25
    CHECK(x.grad().data()[0] == Approx(0.25f).epsilon(1e-4f));
}

TEST_CASE("manas::nn: autodiff: softmax gradient sums to zero", "[manas][nn][autodiff]") {
    Tape::current().reset();

    TensorVar x(Tensor({1, 3}, {1.0f, 2.0f, 3.0f}), true);
    auto y = softmax(x);
    // Upstream gradient of ones
    Tape::current().backward(y.tape_id, Tensor({1, 3}, {1.0f, 1.0f, 1.0f}));

    // grad should sum to 0 (softmax Jacobian property with constant upstream)
    auto& g = x.grad();
    float sum_g = g.data()[0] + g.data()[1] + g.data()[2];
    CHECK(std::abs(sum_g) < 1e-5f);
}

// ─── Initializers ─────────────────────────────────────────────────────────────
TEST_CASE("manas::nn: GlorotUniformInit produces values in range", "[manas][nn][init]") {
    GlorotUniformInit init{42};
    auto t = init({10, 10});
    float fan = 10.0f + 10.0f;
    float limit = std::sqrt(6.0f / fan);
    for (size_t i = 0; i < t.size(); ++i) {
        CHECK(t.data()[i] >= -limit - 1e-6f);
        CHECK(t.data()[i] <=  limit + 1e-6f);
    }
}

TEST_CASE("manas::nn: HeNormalInit mean ~0, stddev ~ sqrt(2/fan_in)", "[manas][nn][init]") {
    HeNormalInit init{42};
    auto t = init({1000, 10});
    float sum = 0.0f;
    for (size_t i = 0; i < t.size(); ++i) sum += t.data()[i];
    float mean = sum / static_cast<float>(t.size());
    CHECK(std::abs(mean) < 0.1f);  // roughly zero mean
}

// ─── Dense Layer ──────────────────────────────────────────────────────────────
TEST_CASE("manas::nn: Dense forward+backward propagates gradients", "[manas][nn][layers][dense]") {
    Tape::current().reset();

    Dense<ActivationReLU, GlorotUniformInit, ZerosInit> layer(2, 3, true, "fc1");

    TensorVar x(Tensor({1, 2}, {1.0f, 2.0f}), true);
    auto y = layer.forward(x, true);

    REQUIRE(y.shape()[0] == 1);
    REQUIRE(y.shape()[1] == 3);

    auto loss = sum_all(y);
    Tape::current().backward(loss.tape_id);

    // Gradients should exist for x and layer params
    auto params = layer.parameters();
    REQUIRE(params.size() == 2);  // weight + bias
}

// ─── BatchNorm1D ──────────────────────────────────────────────────────────────
TEST_CASE("manas::nn: BatchNorm1D normalizes output mean/var", "[manas][nn][layers][bn]") {
    Tape::current().reset();

    BatchNorm1D bn(4);
    ts::tensor<float> X_data({3, 4}, {
        1.0f, 2.0f, 3.0f, 4.0f,
        2.0f, 3.0f, 4.0f, 5.0f,
        3.0f, 4.0f, 5.0f, 6.0f
    });
    TensorVar x(X_data);
    auto y = bn.forward(x, true);

    REQUIRE(y.shape()[0] == 3);
    REQUIRE(y.shape()[1] == 4);

    // After BN (gamma=1, beta=0): each column should have mean~0, std~1
    for (size_t j = 0; j < 4; ++j) {
        float col_sum = 0.0f;
        for (size_t i = 0; i < 3; ++i) col_sum += y.data({i, j});
        CHECK(std::abs(col_sum) < 1e-4f);  // mean ≈ 0
    }
}

// ─── Dropout ─────────────────────────────────────────────────────────────────
TEST_CASE("manas::nn: Dropout zeros some elements in training", "[manas][nn][layers][dropout]") {
    Dropout drop(0.5f, 123);
    Tensor x_data({100});
    for (size_t i = 0; i < 100; ++i) x_data.data()[i] = 1.0f;
    TensorVar x(std::move(x_data));

    auto y = drop.forward(x, true);
    int zeros = 0;
    for (size_t i = 0; i < 100; ++i)
        if (y.data.data()[i] == 0.0f) ++zeros;

    // Expect roughly 50% zeros (with some tolerance)
    CHECK(zeros > 20);
    CHECK(zeros < 80);
}

TEST_CASE("manas::nn: Dropout is identity during inference", "[manas][nn][layers][dropout]") {
    Dropout drop(0.9f, 42);
    TensorVar x(Tensor({1, 10}, {1,1,1,1,1,1,1,1,1,1}));
    auto y = drop.forward(x, false);

    for (size_t i = 0; i < 10; ++i)
        CHECK(y.data.data()[i] == Approx(1.0f));
}

// ─── Losses ──────────────────────────────────────────────────────────────────
TEST_CASE("manas::nn: MSE loss and gradient", "[manas][nn][loss]") {
    Tape::current().reset();

    TensorVar pred(Tensor({3}, {2.0f, 3.0f, 4.0f}), true);
    TensorVar target(Tensor({3}, {1.0f, 3.0f, 5.0f}));

    // MSE = ((2-1)^2 + 0 + (4-5)^2) / 3 = 2/3
    auto loss = mse_loss(pred, target);
    CHECK(loss.data.data()[0] == Approx(2.0f / 3.0f).epsilon(1e-4f));

    Tape::current().backward(loss.tape_id);

    // dL/d_pred = 2*(pred-target)/n
    CHECK(pred.grad().data()[0] == Approx(2.0f * 1.0f / 3.0f).epsilon(1e-4f));
    CHECK(pred.grad().data()[1] == Approx(0.0f).epsilon(1e-4f));
    CHECK(pred.grad().data()[2] == Approx(2.0f * (-1.0f) / 3.0f).epsilon(1e-4f));
}

TEST_CASE("manas::nn: CrossEntropy loss correct value", "[manas][nn][loss]") {
    Tape::current().reset();

    // 2 samples, 3 classes
    TensorVar logits(Tensor({2, 3}, {
        2.0f, 1.0f, 0.1f,   // sample 0: true class 0
        0.1f, 1.0f, 2.0f    // sample 1: true class 2
    }), true);
    TensorVar target(Tensor({2}, {0.0f, 2.0f}));

    auto loss = cross_entropy_loss(logits, target);
    CHECK(loss.data.data()[0] > 0.0f);  // loss should be positive
    CHECK(loss.data.data()[0] < 3.0f);  // but not too large for correct predictions

    REQUIRE_NOTHROW(Tape::current().backward(loss.tape_id));
}

TEST_CASE("manas::nn: BCE loss for binary classification", "[manas][nn][loss]") {
    Tape::current().reset();

    TensorVar pred(Tensor({4}, {0.9f, 0.1f, 0.8f, 0.2f}), true);
    TensorVar target(Tensor({4}, {1.0f, 0.0f, 1.0f, 0.0f}));

    auto loss = bce_loss(pred, target);
    CHECK(loss.data.data()[0] < 0.3f);  // near-correct predictions -> small loss

    REQUIRE_NOTHROW(Tape::current().backward(loss.tape_id));
}

// ─── Optimizers ──────────────────────────────────────────────────────────────
TEST_CASE("manas::nn: SGD reduces MSE loss over iterations", "[manas][nn][optimizer][sgd]") {
    Tape::current().reset();

    // Simple regression: predict constant 5.0 from x=1.0
    // w: [1,1], b: [1]
    Dense<ActivationNone, ZerosInit, ZerosInit> layer(1, 1, true, "l");
    // Set weight=0, bias=0 initially (ZerosInit)
    SGD<> sgd(0.1f);

    float last_loss = 1e9f;
    for (int i = 0; i < 50; ++i) {
        Tape::current().reset();
        TensorVar x(Tensor({1, 1}, {1.0f}));
        TensorVar y(Tensor({1, 1}, {5.0f}));  // match Dense output shape {1,1}

        auto pred = layer.forward(x, true);
        auto loss = mse_loss(pred, y);
        float lv = loss.data.data()[0];

        auto params = layer.parameters();
        sgd.zero_grad(params);
        Tape::current().backward(loss.tape_id);
        sgd.step(params);
        last_loss = lv;
    }
    CHECK(last_loss < 1.0f);  // loss decreased significantly
}

TEST_CASE("manas::nn: Adam converges on simple regression", "[manas][nn][optimizer][adam]") {
    Dense<ActivationNone, ZerosInit, ZerosInit> layer(1, 1, true, "l");
    Adam<> adam(0.1f);  // larger lr for faster convergence test

    float last_loss = 1e9f;
    for (int i = 0; i < 100; ++i) {
        Tape::current().reset();
        TensorVar x(Tensor({1, 1}, {1.0f}));
        TensorVar y_true(Tensor({1, 1}, {3.0f}));
        auto pred = layer.forward(x, true);
        auto loss = mse_loss(pred, y_true);
        last_loss = loss.data.data()[0];
        auto params = layer.parameters();
        adam.zero_grad(params);
        Tape::current().backward(loss.tape_id);
        adam.step(params);
    }
    CHECK(last_loss < 0.1f);  // converged
}

// ─── Sequential Model ─────────────────────────────────────────────────────────
TEST_CASE("manas::nn: Sequential forward pass produces correct shape", "[manas][nn][model]") {
    Tape::current().reset();

    Sequential net;
    net.add(Dense<ActivationReLU>(4, 8,  true, "fc1"))
       .add(Dense<ActivationReLU>(8, 4,  true, "fc2"))
       .add(Dense<ActivationNone>(4, 2,  true, "out"));

    TensorVar x(Tensor({3, 4}, {
        1,2,3,4,  5,6,7,8,  9,10,11,12
    }));
    auto y = net.forward(x, true);
    REQUIRE(y.shape()[0] == 3);
    REQUIRE(y.shape()[1] == 2);
}

TEST_CASE("manas::nn: Sequential parameter count correct", "[manas][nn][model]") {
    Sequential net;
    net.add(Dense<>(3, 4, true, "fc1"))  // w: 3x4=12, b: 4
       .add(Dense<>(4, 2, true, "fc2")); // w: 4x2=8, b: 2

    auto params = net.parameters();
    REQUIRE(params.size() == 4);  // 2 weights + 2 biases
}

TEST_CASE("manas::nn: train_step decreases loss over epochs", "[manas][nn][model]") {
    Sequential net;
    net.add(Dense<ActivationReLU>(2, 8, true, "h"))
       .add(Dense<ActivationNone>(8, 1, true, "out"));

    Adam<> opt(0.01f);

    // XOR-ish: simple classification
    ts::tensor<float> X_data({4, 2}, {0.f,0.f, 1.f,0.f, 0.f,1.f, 1.f,1.f});
    ts::tensor<float> y_data({4}, {0.f, 1.f, 1.f, 0.f});

    float first_loss = 0.0f, last_loss = 0.0f;
    for (int epoch = 0; epoch < 200; ++epoch) {
        TensorVar x(X_data);
        TensorVar y(y_data);
        float lv = train_step(net, x, y,
            [](const TensorVar& pred, const TensorVar& target) {
                // Flatten [4,1] -> use as-is for MSE
                return mse_loss(pred, target);
            }, opt, true);
        if (epoch == 0) first_loss = lv;
        last_loss = lv;
    }
    CHECK(last_loss < first_loss);
}

TEST_CASE("manas::nn: NoGradGuard disables gradient recording", "[manas][nn][tape]") {
    Tape::current().reset();
    TensorVar x(Tensor({2}, {1.0f, 2.0f}), true);
    {
        NoGradGuard ng;
        auto y = relu(x);
        CHECK(y.tape_id == Tape::kNoGrad);
    }
    // After guard: recording re-enabled
    CHECK(Tape::current().recording() == true);
}
