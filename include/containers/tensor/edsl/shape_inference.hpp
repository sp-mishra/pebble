#pragma once
// ============================================================================
// shape_inference.hpp — EDSL Shape Inference and Propagation Rules
// ============================================================================
// C++23 / C++26, header-only, eager shape calculus for Tensor EDSL.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_TENSOR_EDSL_SHAPE_INFERENCE_HPP
#define PEBBLE_CONTAINERS_TENSOR_EDSL_SHAPE_INFERENCE_HPP

#include <containers/tensor/tensor.hpp>
#include <stdexcept>
#include <algorithm>
#include <numeric>

namespace ts::edsl {

    // ========================================================================
    // 1. Elementwise / Broadcast Shape Inference
    // ========================================================================
    inline tensor_shape infer_broadcast_shape(const tensor_shape &s1, const tensor_shape &s2) {
        if (s1.empty()) return s2;
        if (s2.empty()) return s1;
        return broadcast_shapes_unif(s1, s2);
    }

    // ========================================================================
    // 2. Matrix Multiplication Shape Inference ([..., M, K] x [..., K, N] -> [..., M, N])
    // ========================================================================
    inline tensor_shape infer_matmul_shape(const tensor_shape &s1, const tensor_shape &s2) {
        // If either shape is dynamic/unspecified at build time, defer shape validation
        if (s1.empty() || s2.empty()) {
            return {};
        }

        // Vector dot product: 1D x 1D -> scalar (empty shape)
        if (s1.size() == 1 && s2.size() == 1) {
            if (s1[0] != s2[0]) {
                throw std::invalid_argument("Vector dot product dimension mismatch");
            }
            return tensor_shape{};
        }

        // 2D x 1D: [M, K] x [K] -> [M]
        if (s1.size() == 2 && s2.size() == 1) {
            if (s1[1] != s2[0]) {
                throw std::invalid_argument("Matrix-vector multiplication dimension mismatch");
            }
            return tensor_shape{s1[0]};
        }

        // 1D x 2D: [K] x [K, N] -> [N]
        if (s1.size() == 1 && s2.size() == 2) {
            if (s1[0] != s2[0]) {
                throw std::invalid_argument("Vector-matrix multiplication dimension mismatch");
            }
            return tensor_shape{s2[1]};
        }

        // Standard 2D x 2D: [M, K] x [K, N] -> [M, N]
        if (s1.size() == 2 && s2.size() == 2) {
            if (s1[1] != s2[0]) {
                throw std::invalid_argument("Matrix multiplication inner dimension mismatch: (" +
                    std::to_string(s1[0]) + "x" + std::to_string(s1[1]) + ") vs (" +
                    std::to_string(s2[0]) + "x" + std::to_string(s2[1]) + ")");
            }
            return tensor_shape{s1[0], s2[1]};
        }

        // Batched matmul: [..., M, K] x [..., K, N] -> [..., M, N]
        if (s1[s1.size() - 1] != s2[s2.size() - 2]) {
            throw std::invalid_argument("Batched matmul inner dimension mismatch");
        }

        tensor_shape batch_s1(s1.begin(), s1.end() - 2);
        tensor_shape batch_s2(s2.begin(), s2.end() - 2);
        tensor_shape out_batch = infer_broadcast_shape(batch_s1, batch_s2);

        out_batch.push_back(s1[s1.size() - 2]); // M
        out_batch.push_back(s2[s2.size() - 1]); // N
        return out_batch;
    }

    // ========================================================================
    // 3. Reduction Shape Inference
    // ========================================================================
    inline tensor_shape infer_reduction_shape(const tensor_shape &s, int axis = -1, bool keepdims = false) {
        if (s.empty()) return {};
        if (axis < 0) {
            // Full reduction across all axes -> scalar
            if (keepdims) {
                return tensor_shape(s.size(), 1);
            }
            return {};
        }
        if (static_cast<size_t>(axis) >= s.size()) {
            throw std::invalid_argument("Reduction axis out of bounds");
        }
        tensor_shape out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (static_cast<int>(i) == axis) {
                if (keepdims) out.push_back(1);
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    }

    // ========================================================================
    // 4. Transpose Shape Inference
    // ========================================================================
    inline tensor_shape infer_transpose_shape(const tensor_shape &s) {
        tensor_shape out(s.rbegin(), s.rend());
        return out;
    }

} // namespace ts::edsl

#endif // PEBBLE_CONTAINERS_TENSOR_EDSL_SHAPE_INFERENCE_HPP
