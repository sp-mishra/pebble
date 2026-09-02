#pragma once
// Reverse-mode autodiff tape: builds dynamic computation graph for backprop.
// No virtual dispatch — backward closures stored in SmallVector<GradFn>.
// Thread-local tape for nested gradient calls.
// Policy: GradAccumPolicy controls how gradients accumulate (Add vs Set).
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
#include <containers/dynamic/SmallVector.hpp>
#include <containers/tensor/tensor.hpp>

namespace manas::nn {
    using Tensor = ts::tensor<float>;

    // ─── Node in the computation graph ────────────────────────────────────────────
    // Each node owns:
    //   - its accumulated gradient (same shape as the value)
    //   - a backward function that propagates grad to inputs
    struct TapeNode {
        Tensor grad; // accumulated gradient
        bool requires_grad = false;
        // Backward closure: receives this node's grad, propagates to inputs
        std::function<void(const Tensor&)> backward_fn;
        uint32_t id = 0;
    };

    // ─── Tape (thread-local computation graph) ────────────────────────────────────
    class Tape {
    public:
        static Tape& current() noexcept {
            thread_local Tape instance;
            return instance;
        }

        // Register a new node; returns its index
        uint32_t push(bool requires_grad,
                      Tensor grad_shape_like,
                      std::function<void(const Tensor&)> fn = {}) {
            if (!recording_) return kNoGrad;
            const uint32_t id = static_cast<uint32_t>(nodes_.size());
            auto& node = nodes_.emplace_back();
            node.id = id;
            node.requires_grad = requires_grad;
            node.grad = make_zeros_like(grad_shape_like);
            node.backward_fn = std::move(fn);
            return id;
        }

        void accumulate_grad(uint32_t id, const Tensor& g) {
            if (id == kNoGrad || id >= nodes_.size()) return;
            auto& node = nodes_[id];
            if (node.grad.shape() == g.shape()) {
                // in-place add
                for (size_t i = 0; i < g.size(); ++i)
                    node.grad.data()[i] += g.data()[i];
            }
            else {
                node.grad = g; // first assignment
            }
        }

        // Run backprop from node `root_id` with upstream gradient `upstream`
        void backward(uint32_t root_id, const Tensor& upstream) {
            if (root_id == kNoGrad || root_id >= nodes_.size()) return;
            accumulate_grad(root_id, upstream);
            // Reverse topological order
            for (int i = static_cast<int>(nodes_.size()) - 1; i >= 0; --i) {
                auto& node = nodes_[static_cast<size_t>(i)];
                if (node.backward_fn && node.requires_grad)
                    node.backward_fn(node.grad);
            }
        }

        // Convenience: backward from scalar loss node (grad = 1.0)
        void backward(uint32_t root_id) {
            Tensor one({1}, {1.0f});
            backward(root_id, one);
        }

        void reset() { nodes_.clear(); }

        TapeNode& node(uint32_t id) { return nodes_[id]; }
        const TapeNode& node(uint32_t id) const { return nodes_[id]; }

        bool recording() const noexcept { return recording_; }
        void set_recording(bool v) noexcept { recording_ = v; }

        static constexpr uint32_t kNoGrad = UINT32_MAX;

    private:
        std::vector<TapeNode> nodes_;
        bool recording_ = true;

        static Tensor make_zeros_like(const Tensor& t) {
            Tensor z(t.shape());
            for (size_t i = 0; i < z.size(); ++i) z.data()[i] = 0.0f;
            return z;
        }
    };

    // ─── RAII guard to disable gradient recording ────────────────────────────────
    struct NoGradGuard {
        NoGradGuard() { Tape::current().set_recording(false); }
        ~NoGradGuard() { Tape::current().set_recording(true); }
        NoGradGuard(const NoGradGuard&) = delete;
    };
} // namespace manas::nn
