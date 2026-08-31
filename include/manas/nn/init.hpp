#pragma once
// Weight initializers: Zeros, Ones, Normal, Uniform, GlorotUniform, HeNormal
// All initializers satisfy Initializer concept: operator()(shape) -> Tensor
#include <cmath>
#include <random>
#include <containers/tensor/tensor.hpp>

namespace manas::nn {

using Tensor = ts::tensor<float>;

template<typename I>
concept Initializer = requires(const I& init, const ts::TensorShape& shape) {
    { init(shape) } -> std::same_as<Tensor>;
};

struct ZerosInit {
    Tensor operator()(const ts::TensorShape& shape) const {
        Tensor t(shape);
        for (size_t i = 0; i < t.size(); ++i) t.data()[i] = 0.0f;
        return t;
    }
};

struct OnesInit {
    Tensor operator()(const ts::TensorShape& shape) const {
        Tensor t(shape);
        for (size_t i = 0; i < t.size(); ++i) t.data()[i] = 1.0f;
        return t;
    }
};

struct NormalInit {
    float mean = 0.0f;
    float stddev = 0.01f;
    uint64_t seed = 42;
    Tensor operator()(const ts::TensorShape& shape) const {
        Tensor t(shape);
        std::mt19937_64 rng(seed);
        std::normal_distribution<float> dist(mean, stddev);
        for (size_t i = 0; i < t.size(); ++i) t.data()[i] = dist(rng);
        return t;
    }
};

struct UniformInit {
    float lo = -0.1f, hi = 0.1f;
    uint64_t seed = 42;
    Tensor operator()(const ts::TensorShape& shape) const {
        Tensor t(shape);
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<float> dist(lo, hi);
        for (size_t i = 0; i < t.size(); ++i) t.data()[i] = dist(rng);
        return t;
    }
};

// Glorot (Xavier) uniform: U(-limit, limit), limit = sqrt(6 / (fan_in + fan_out))
struct GlorotUniformInit {
    uint64_t seed = 42;
    Tensor operator()(const ts::TensorShape& shape) const {
        float fan_in  = shape.size() >= 2 ? static_cast<float>(shape[shape.size()-2]) : 1.0f;
        float fan_out = static_cast<float>(shape.back());
        float limit   = std::sqrt(6.0f / (fan_in + fan_out));
        Tensor t(shape);
        std::mt19937_64 rng(seed);
        std::uniform_real_distribution<float> dist(-limit, limit);
        for (size_t i = 0; i < t.size(); ++i) t.data()[i] = dist(rng);
        return t;
    }
};

// He (Kaiming) Normal: N(0, sqrt(2 / fan_in))
struct HeNormalInit {
    uint64_t seed = 42;
    Tensor operator()(const ts::TensorShape& shape) const {
        float fan_in = shape.size() >= 2 ? static_cast<float>(shape[shape.size()-2]) : 1.0f;
        float stddev = std::sqrt(2.0f / fan_in);
        Tensor t(shape);
        std::mt19937_64 rng(seed);
        std::normal_distribution<float> dist(0.0f, stddev);
        for (size_t i = 0; i < t.size(); ++i) t.data()[i] = dist(rng);
        return t;
    }
};

// Orthogonal initialization via QR decomposition approximation
struct OrthogonalInit {
    float gain = 1.0f;
    uint64_t seed = 42;
    Tensor operator()(const ts::TensorShape& shape) const {
        // For rectangular matrices: generate random normal then orthogonalize via Gram-Schmidt
        const size_t rows = shape.size() >= 2 ? shape[shape.size()-2] : 1;
        const size_t cols = shape.back();
        Tensor t(shape);
        std::mt19937_64 rng(seed);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        // Fill with random normal
        for (size_t i = 0; i < t.size(); ++i) t.data()[i] = dist(rng);
        // Gram-Schmidt over rows
        const size_t k = std::min(rows, cols);
        for (size_t i = 0; i < k; ++i) {
            // Normalize row i
            float norm = 0.0f;
            for (size_t j = 0; j < cols; ++j) { float v = t.data()[i*cols+j]; norm += v*v; }
            norm = std::sqrt(norm + 1e-12f);
            for (size_t j = 0; j < cols; ++j) t.data()[i*cols+j] /= norm;
            // Orthogonalize subsequent rows against row i
            for (size_t ii = i + 1; ii < rows; ++ii) {
                float dot = 0.0f;
                for (size_t j = 0; j < cols; ++j) dot += t.data()[ii*cols+j] * t.data()[i*cols+j];
                for (size_t j = 0; j < cols; ++j) t.data()[ii*cols+j] -= dot * t.data()[i*cols+j];
            }
        }
        if (gain != 1.0f)
            for (size_t i = 0; i < t.size(); ++i) t.data()[i] *= gain;
        return t;
    }
};

} // namespace manas::nn
