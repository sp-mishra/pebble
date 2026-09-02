#pragma once
#include <concepts>
#include <span>
#include <containers/tensor/tensor.hpp>

namespace manas::ml {
    template <typename T>
    concept Scalar = std::floating_point<T> || std::integral<T>;

    // Estimator: fit(X, y) + predict(X)
    template <typename E, typename T = float>
    concept Estimator = requires(E& e,
                                 const ts::tensor<T>& X,
                                 const ts::tensor<T>& y) {
        e.fit(X, y);
        { e.predict(X) } -> std::same_as<ts::tensor<T>>;
    };

    // Transformer: fit(X) + transform(X) -> tensor
    template <typename Tr, typename T = float>
    concept Transformer = requires(Tr& t, const ts::tensor<T>& X) {
        t.fit(X);
        { t.transform(X) } -> std::same_as<ts::tensor<T>>;
    };

    // Clustering: fit(X), predict(X) -> label tensor
    template <typename C, typename T = float>
    concept ClusteringModel = requires(C& c, const ts::tensor<T>& X) {
        c.fit(X);
        { c.predict(X) } -> std::same_as<ts::tensor<int>>;
    };

    // Kernel: k(x, y) -> scalar
    template <typename K, typename T = float>
    concept Kernel = requires(const K& k,
                              std::span<const T> a,
                              std::span<const T> b) {
        { k(a, b) } -> std::same_as<T>;
    };
} // namespace manas::ml
