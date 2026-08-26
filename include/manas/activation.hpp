#pragma once
#include <cmath>

namespace manas {

struct Identity {
    float operator()(float x) const { return x; }
};

struct Sigmoid {
    float operator()(float x) const {
        return 1.0f / (1.0f + std::exp(-x));
    }
};

struct Tanh {
    float operator()(float x) const {
        return std::tanh(x);
    }
};

struct ReLU {
    float operator()(float x) const { return x > 0 ? x : 0; }
};

struct LeakyReLU {
    float alpha;
    float operator()(float x) const { return x > 0 ? x : alpha * x; }
};

struct SoftSign {
    float operator()(float x) const {
        return x / (1.0f + std::abs(x));
    }
};

} // namespace manas