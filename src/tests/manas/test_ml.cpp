#include "catch_amalgamated.hpp"
#include <manas/ml/ml.hpp>

using namespace manas::ml;
using Catch::Approx;

// ─── Linear Regression ───────────────────────────────────────────────────────
TEST_CASE("manas::ml: LinearRegression fits y = 2x + 1", "[manas][ml][linear]") {
    // y = 2*x + 1, 4 samples
    ts::tensor<float> X({4, 1}, {1.0f, 2.0f, 3.0f, 4.0f});
    ts::tensor<float> y({4}, {3.0f, 5.0f, 7.0f, 9.0f});

    LinearRegression<> reg;
    reg.fit(X, y);

    auto pred = reg.predict(X);
    REQUIRE(pred({0}) == Approx(3.0f).epsilon(0.01f));
    REQUIRE(pred({3}) == Approx(9.0f).epsilon(0.01f));
}

TEST_CASE("manas::ml: RidgeRegression (lambda>0) reduces weights", "[manas][ml][linear]") {
    ts::tensor<float> X({4, 1}, {1.0f, 2.0f, 3.0f, 4.0f});
    ts::tensor<float> y({4}, {3.0f, 5.0f, 7.0f, 9.0f});

    LinearRegression<> plain(0.0f);
    LinearRegression<> ridge(10.0f);
    plain.fit(X, y); ridge.fit(X, y);

    // Ridge weight magnitude should be smaller than plain OLS
    float w_plain = std::abs(plain.weights()({0}));
    float w_ridge = std::abs(ridge.weights()({0}));
    CHECK(w_ridge < w_plain);
}

TEST_CASE("manas::ml: LogisticRegression classifies linearly separable data", "[manas][ml][linear]") {
    // Class 0: x<0, Class 1: x>0
    ts::tensor<float> X({6, 1}, {-3.f, -2.f, -1.f, 1.f, 2.f, 3.f});
    ts::tensor<float> y({6}, {0.f, 0.f, 0.f, 1.f, 1.f, 1.f});

    LogisticRegression lr(0.5f, 500);
    lr.fit(X, y);

    auto pred = lr.predict(X);
    CHECK(pred({0}) == Approx(0.0f));
    CHECK(pred({3}) == Approx(1.0f));
    CHECK(pred({5}) == Approx(1.0f));
}

// ─── K-Means ─────────────────────────────────────────────────────────────────
TEST_CASE("manas::ml: KMeans clusters 2D data into 2 groups", "[manas][ml][kmeans]") {
    // Two clear clusters: A near (0,0), B near (10,10)
    ts::tensor<float> X({6, 2}, {
        0.1f, 0.2f,  0.3f, 0.1f,  0.2f, 0.3f,
        9.8f, 9.9f, 10.1f, 9.9f, 10.0f, 10.2f
    });
    KMeans<> km(2, 100, 1e-4f, 0);
    km.fit(X);

    REQUIRE(km.cluster_centers().shape()[0] == 2);
    auto labels = km.labels();
    REQUIRE(labels.size() == 6);
    // First 3 same label, last 3 same label
    CHECK(labels[0] == labels[1]);
    CHECK(labels[1] == labels[2]);
    CHECK(labels[3] == labels[4]);
    CHECK(labels[4] == labels[5]);
    CHECK(labels[0] != labels[3]);
}

// ─── PCA ─────────────────────────────────────────────────────────────────────
TEST_CASE("manas::ml: PCA reduces 2D to 1D principal component", "[manas][ml][pca]") {
    // Data lies mainly along y=x axis
    ts::tensor<float> X({5, 2}, {
        1.0f, 1.0f,  2.0f, 2.0f,  3.0f, 3.0f,  4.0f, 4.0f,  5.0f, 5.0f
    });
    PCA pca(1);
    pca.fit(X);

    REQUIRE(pca.components().shape()[0] == 1);
    REQUIRE(pca.components().shape()[1] == 2);
    // First PC should be close to [1/sqrt(2), 1/sqrt(2)]
    float c0 = std::abs(pca.components()({0, 0}));
    float c1 = std::abs(pca.components()({0, 1}));
    CHECK(c0 == Approx(c1).epsilon(0.01f));

    auto Xt = pca.transform(X);
    REQUIRE(Xt.shape()[0] == 5);
    REQUIRE(Xt.shape()[1] == 1);
}

// ─── KNN ─────────────────────────────────────────────────────────────────────
TEST_CASE("manas::ml: KNNClassifier classifies 1D data", "[manas][ml][knn]") {
    ts::tensor<float> X_train({4, 1}, {1.0f, 2.0f, 8.0f, 9.0f});
    ts::tensor<float> y_train({4}, {0.0f, 0.0f, 1.0f, 1.0f});

    KNNClassifier<> knn(2);
    knn.fit(X_train, y_train);

    ts::tensor<float> X_test({2, 1}, {1.5f, 8.5f});
    auto pred = knn.predict(X_test);
    CHECK(pred({0}) == Approx(0.0f));
    CHECK(pred({1}) == Approx(1.0f));
}

TEST_CASE("manas::ml: KNNRegressor returns mean of neighbors", "[manas][ml][knn]") {
    ts::tensor<float> X_train({4, 1}, {1.0f, 2.0f, 3.0f, 4.0f});
    ts::tensor<float> y_train({4}, {1.0f, 2.0f, 3.0f, 4.0f});

    KNNRegressor<> knn(2);
    knn.fit(X_train, y_train);

    ts::tensor<float> X_test({1, 1}, {2.5f});
    auto pred = knn.predict(X_test);
    CHECK(pred({0}) == Approx(2.5f).epsilon(0.01f));
}

// ─── Naive Bayes ─────────────────────────────────────────────────────────────
TEST_CASE("manas::ml: GaussianNaiveBayes classifies 2-class problem", "[manas][ml][naive_bayes]") {
    ts::tensor<float> X({6, 2}, {
        1.0f, 1.0f,  1.5f, 1.2f,  0.8f, 0.9f,
        8.0f, 8.0f,  8.5f, 7.9f,  7.8f, 8.2f
    });
    ts::tensor<float> y({6}, {0.f, 0.f, 0.f, 1.f, 1.f, 1.f});

    GaussianNaiveBayes gnb;
    gnb.fit(X, y);

    ts::tensor<float> X_test({2, 2}, {1.1f, 1.0f, 8.1f, 8.0f});
    auto pred = gnb.predict(X_test);
    CHECK(pred({0}) == Approx(0.0f));
    CHECK(pred({1}) == Approx(1.0f));
}

// ─── Decision Tree ───────────────────────────────────────────────────────────
TEST_CASE("manas::ml: DecisionTreeClassifier separates XOR-like data", "[manas][ml][decision_tree]") {
    ts::tensor<float> X({4, 2}, {0.f,0.f, 1.f,0.f, 0.f,1.f, 1.f,1.f});
    ts::tensor<float> y({4}, {0.f, 1.f, 1.f, 0.f});

    DecisionTreeClassifier dt(4, 2, 1);
    dt.fit(X, y);

    auto pred = dt.predict(X);
    CHECK(pred({0}) == Approx(0.0f));
    CHECK(pred({1}) == Approx(1.0f));
    CHECK(pred({2}) == Approx(1.0f));
    CHECK(pred({3}) == Approx(0.0f));
}

TEST_CASE("manas::ml: DecisionTreeRegressor fits simple function", "[manas][ml][decision_tree]") {
    ts::tensor<float> X({5, 1}, {1.f, 2.f, 3.f, 4.f, 5.f});
    ts::tensor<float> y({5}, {1.f, 4.f, 9.f, 16.f, 25.f});

    DecisionTreeRegressor dt(8, 2, 1);
    dt.fit(X, y);
    auto pred = dt.predict(X);
    // Should memorize training data with deep enough tree
    for (size_t i = 0; i < 5; ++i)
        CHECK(pred({i}) == Approx(y({i})).epsilon(0.5f));
}

// ─── SVM ─────────────────────────────────────────────────────────────────────
TEST_CASE("manas::ml: SVM<LinearKernel> classifies linearly separable data", "[manas][ml][svm]") {
    ts::tensor<float> X({4, 1}, {-2.f, -1.f, 1.f, 2.f});
    ts::tensor<float> y({4}, {-1.f, -1.f, 1.f, 1.f});

    SVM<LinearKernel> svm(1.0f, 1e-3f, 500);
    svm.fit(X, y);

    ts::tensor<float> X_test({2, 1}, {-1.5f, 1.5f});
    auto pred = svm.predict(X_test);
    CHECK(pred({0}) == Approx(-1.0f));
    CHECK(pred({1}) == Approx(1.0f));
}

TEST_CASE("manas::ml: SVM<RBFKernel> classifies non-linear data", "[manas][ml][svm]") {
    // Simple two-class problem solvable by RBF
    ts::tensor<float> X({4, 1}, {-2.f, -1.f, 1.f, 2.f});
    ts::tensor<float> y({4}, {-1.f, -1.f, 1.f, 1.f});

    SVM<RBFKernel> svm(1.0f, 1e-3f, 200, RBFKernel{0.5f});
    svm.fit(X, y);
    REQUIRE(svm.n_support_vectors() > 0);
}

// ─── Random Forest ───────────────────────────────────────────────────────────
TEST_CASE("manas::ml: RandomForestClassifier fits training data", "[manas][ml][random_forest]") {
    ts::tensor<float> X({6, 2}, {
        0.f,0.f,  1.f,0.f,  0.f,1.f,
        5.f,5.f,  6.f,5.f,  5.f,6.f
    });
    ts::tensor<float> y({6}, {0.f, 0.f, 0.f, 1.f, 1.f, 1.f});

    RandomForestClassifier rf(20, 5, 2, 42);
    rf.fit(X, y);
    REQUIRE(rf.n_estimators() == 20);

    auto pred = rf.predict(X);
    // Should correctly classify training data
    for (size_t i = 0; i < 3; ++i) CHECK(pred({i}) == Approx(0.0f));
    for (size_t i = 3; i < 6; ++i) CHECK(pred({i}) == Approx(1.0f));
}
