#pragma once
// Sequential: type-erased stack of layers.
// Uses SmallVector<LayerHolder> for inline storage of small layers.
// LayerHolder: type-erased wrapper with no virtual dispatch — stores forward/params
// as std::function (single pointer indirection, better than vtable for small lambdas).
// For large nets, falls back to heap. Zero overhead for <= InlineCap layers.
// Supports training mode toggle and parameter collection.
#include <functional>
#include <string>
#include <string_view>
#include <containers/dynamic/SmallVector.hpp>
#include "parameter.hpp"
#include "tape.hpp"

namespace manas::nn {
    // Type-erased layer holder — no virtual, uses std::function internally
    struct LayerHolder {
        std::string layer_name;
        std::function<TensorVar(const TensorVar &, bool)> forward_fn;
        std::function<ParamList()> params_fn;

        template <typename L>
        explicit LayerHolder(L&& layer)
            : layer_name(std::string(layer.name()))
              , forward_fn([l = std::forward<L>(layer)](const TensorVar& x, bool training) mutable {
                  return l.forward(x, training);
              })
              , params_fn([l_ptr = &forward_fn]() -> ParamList {
                  // Can't access l through forward_fn closure — use separate capture
                  return {};
              }) {}

        // Two-lambda version for correct param capture
        template <typename L>
        static LayerHolder make(L layer) {
            LayerHolder h;
            h.layer_name = std::string(layer.name());
            auto shared = std::make_shared<L>(std::move(layer));
            h.forward_fn = [shared](const TensorVar& x, bool training) {
                return shared->forward(x, training);
            };
            h.params_fn = [shared]() -> ParamList {
                return shared->parameters();
            };
            return h;
        }

    private:
        LayerHolder() = default;
    };

    // ─── Sequential model ─────────────────────────────────────────────────────────
    class Sequential {
    public:
        Sequential() = default;

        template <typename L>
        Sequential& add(L&& layer) {
            layers_.push_back(LayerHolder::make(std::forward<L>(layer)));
            return *this;
        }

        // Forward pass: runs all layers in sequence
        TensorVar forward(const TensorVar& x, bool training = true) const {
            TensorVar out = x;
            for (const auto& l : layers_)
                out = l.forward_fn(out, training);
            return out;
        }

        // Collect all trainable parameters from all layers
        ParamList parameters() const {
            ParamList all;
            for (auto& l : layers_) {
                auto p = l.params_fn();
                for (auto* ptr : p) all.push_back(ptr);
            }
            return all;
        }

        size_t num_layers() const noexcept { return layers_.size(); }

        // Reset the autodiff tape between batches
        void zero_tape() { Tape::current().reset(); }

        std::string_view layer_name(size_t i) const { return layers_[i].layer_name; }

    private:
        containers::dynamic::SmallVector<LayerHolder, 64 * 8> layers_;
    };

    // ─── Training utilities ───────────────────────────────────────────────────────

    // Training step: forward + backward + optimizer step
    template <typename LossFn, typename OptimizerT>
    float train_step(Sequential& model,
                     const TensorVar& x,
                     const TensorVar& y,
                     LossFn&& loss_fn,
                     OptimizerT& optimizer,
                     bool reset_tape = true) {
        if (reset_tape) { Tape::current().reset(); }
        auto& tape = Tape::current();

        // Forward
        auto pred = model.forward(x, true);

        // Loss
        auto loss = loss_fn(pred, y);
        float loss_val = loss.data.data()[0];

        // Backward
        auto params = model.parameters();
        optimizer.zero_grad(params);

        // Scalar backward: loss has shape {1}
        Tensor one({1}, {1.0f});
        tape.backward(loss.tape_id, one);

        // Optimizer step
        optimizer.step(params);

        return loss_val;
    }

    // Inference (no_grad)
    inline TensorVar predict(Sequential& model, const TensorVar& x) {
        NoGradGuard ng;
        return model.forward(x, false);
    }
} // namespace manas::nn
