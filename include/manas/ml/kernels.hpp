#pragma once
#include <cmath>
#include <span>

namespace manas::ml {
    // Linear kernel: k(a,b) = a·b
    struct LinearKernel {
        float operator()(std::span<const float> a, std::span<const float> b) const noexcept {
            float s = 0.0f;
            for (size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
            return s;
        }
    };

    // Polynomial kernel: k(a,b) = (a·b + c)^d
    struct PolynomialKernel {
        float c = 1.0f;
        int d = 2;

        float operator()(std::span<const float> a, std::span<const float> b) const noexcept {
            float dot = 0.0f;
            for (size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
            return std::pow(dot + c, static_cast<float>(d));
        }
    };

    // RBF (Gaussian) kernel: k(a,b) = exp(-gamma * ||a-b||^2)
    struct RBFKernel {
        float gamma = 1.0f;

        float operator()(std::span<const float> a, std::span<const float> b) const noexcept {
            float sq = 0.0f;
            for (size_t i = 0; i < a.size(); ++i) {
                float d = a[i] - b[i];
                sq += d * d;
            }
            return std::exp(-gamma * sq);
        }
    };

    // Sigmoid kernel: k(a,b) = tanh(alpha * a·b + beta)
    struct SigmoidKernel {
        float alpha = 0.01f;
        float beta = 0.0f;

        float operator()(std::span<const float> a, std::span<const float> b) const noexcept {
            float dot = 0.0f;
            for (size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
            return std::tanh(alpha * dot + beta);
        }
    };
} // namespace manas::ml
