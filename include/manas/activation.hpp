#pragma once
#include <cmath>
#include <functional>

namespace manas {

enum class ActivationType {
    Identity,
    Sigmoid,
    Tanh,
    ReLU,
    LeakyReLU,
    SoftSign
};

struct Identity {
    float operator()(float x) const noexcept { return x; }
};

struct Sigmoid {
    float operator()(float x) const noexcept {
        return 1.0f / (1.0f + std::exp(-x));
    }
};

struct Tanh {
    float operator()(float x) const noexcept {
        return std::tanh(x);
    }
};

struct ReLU {
    float operator()(float x) const noexcept { return x > 0.0f ? x : 0.0f; }
};

struct LeakyReLU {
    float alpha = 0.01f;
    float operator()(float x) const noexcept { return x > 0.0f ? x : alpha * x; }
};

struct SoftSign {
    float operator()(float x) const noexcept {
        return x / (1.0f + std::abs(x));
    }
};

inline float apply_activation(ActivationType type, float x, float alpha = 0.01f) noexcept {
    switch (type) {
        case ActivationType::Identity: return Identity{}(x);
        case ActivationType::Sigmoid:  return Sigmoid{}(x);
        case ActivationType::Tanh:     return Tanh{}(x);
        case ActivationType::ReLU:     return ReLU{}(x);
        case ActivationType::LeakyReLU:return LeakyReLU{alpha}(x);
        case ActivationType::SoftSign: return SoftSign{}(x);
        default: return x;
    }
}

} // namespace manas