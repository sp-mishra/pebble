#pragma once
// Parameter: a named TensorVar that tracks gradients and is owned by a layer.
// Parameters are stored in a SmallVector inside each layer (inline for small nets).
// ParamGroup: a flat view over all parameters for optimizers.
#include <string>
#include <string_view>
#include <containers/dynamic/SmallVector.hpp>
#include "ops.hpp"

namespace manas::nn {

struct Parameter {
    std::string name;
    TensorVar   var;     // data + tape_id (tape_id refreshed each forward pass)
    bool        trainable = true;

    Parameter(std::string n, Tensor data, bool trainable_ = true)
        : name(std::move(n))
        , var(std::move(data), false)  // don't push on tape at construction
        , trainable(trainable_)
        , grad_(var.data.shape()) {
        for (size_t i = 0; i < grad_.size(); ++i) grad_.data()[i] = 0.0f;
    }

    Tensor&       data()       noexcept { return var.data; }
    const Tensor& data() const noexcept { return var.data; }

    // Register this parameter on the current tape (call at start of forward pass)
    void register_on_tape() {
        if (!trainable) { var.tape_id = Tape::kNoGrad; return; }
        var.tape_id = Tape::current().push(true, var.data,
            [this](const Tensor& g) {
                // Accumulate into our own grad buffer
                if (grad_.shape() == g.shape()) {
                    for (size_t i = 0; i < g.size(); ++i) grad_.data()[i] += g.data()[i];
                } else {
                    grad_ = g;
                }
            });
    }

    const Tensor& grad() const noexcept { return grad_; }
    void zero_grad() {
        for (size_t i = 0; i < grad_.size(); ++i) grad_.data()[i] = 0.0f;
    }
    uint32_t tape_id() const noexcept { return var.tape_id; }

private:
    Tensor grad_;
};

// Flat list of raw pointers to parameters (borrowed, not owned)
// Passed to optimizers for update steps.
using ParamList = containers::dynamic::SmallVector<Parameter*, 64>;

// Layer concept: every layer must provide forward(TensorVar) -> TensorVar
// and parameters() -> ParamList
template<typename L>
concept Layer = requires(L& layer, const TensorVar& x, bool training) {
    { layer.forward(x, training) } -> std::same_as<TensorVar>;
    { layer.parameters() }        -> std::convertible_to<ParamList>;
    { layer.name() }              -> std::convertible_to<std::string_view>;
};

} // namespace manas::nn
