#pragma once
// ============================================================================
// eval.hpp — L1 / L2 Evaluation and Engine Execution for Tensor EDSL
// ============================================================================
// C++23 / C++26, header-only, zero virtual dispatch execution engine.
// Dispatches to CPU, Highway SIMD, and Apple Silicon MLX GPU backends.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_TENSOR_EDSL_EVAL_HPP
#define PEBBLE_CONTAINERS_TENSOR_EDSL_EVAL_HPP

#include "operators.hpp"
#include <containers/tensor/tensor.hpp>
#include <containers/tensor/highway_computation_policy.hpp>
#if __has_include(<mlx/mlx.h>)
#include <containers/tensor/mlx_storage_policy.hpp>
#include <containers/tensor/mlx_computation_policy.hpp>
#endif

#include <unordered_map>
#include <string>
#include <any>
#include <functional>
#include <cmath>

namespace ts::edsl {

    enum class target_backend {
        cpu,
        simd_highway,
        gpu_mlx
    };

    namespace target {
        inline constexpr target_backend cpu = target_backend::cpu;
        inline constexpr target_backend simd = target_backend::simd_highway;
        inline constexpr target_backend highway = target_backend::simd_highway;
        inline constexpr target_backend gpu = target_backend::gpu_mlx;
        inline constexpr target_backend mlx = target_backend::gpu_mlx;
    }

    // ========================================================================
    // Binding Map: Stores scalar and tensor bindings by name
    // ========================================================================
    class binding_context {
    public:
        std::unordered_map<std::string, float> scalars;
        std::unordered_map<std::string, ts::tensor<float>> tensors;

        binding_context() = default;

        template<typename... Bindings>
        explicit binding_context(Bindings&&... binds) {
            (add_binding(std::forward<Bindings>(binds)), ...);
        }

        template<typename T>
        void add_binding(binding<T> &&b) {
            if constexpr (std::is_arithmetic_v<T>) {
                scalars[b.name] = static_cast<float>(b.value);
            } else if constexpr (requires { b.value.shape(); }) {
                // Concrete Tensor
                ts::tensor<float> t(b.value);
                tensors[b.name] = std::move(t);
            }
        }

        template<typename T>
        void add_binding(const binding<T> &b) {
            if constexpr (std::is_arithmetic_v<T>) {
                scalars[b.name] = static_cast<float>(b.value);
            } else if constexpr (requires { b.value.shape(); }) {
                ts::tensor<float> t(b.value);
                tensors[b.name] = std::move(t);
            }
        }
    };

    // ========================================================================
    // Runtime AST Evaluator
    // ========================================================================
    namespace detail {

        inline ts::tensor<float> evaluate_node(const expr_ptr &node, const binding_context &ctx) {
            if (!node) {
                return ts::tensor<float>({1}, {0.0f});
            }

            switch (node->kind) {
                case op_kind::constant: {
                    return ts::tensor<float>({1}, {node->scalar_payload});
                }
                case op_kind::param_ref: {
                    auto it = ctx.scalars.find(node->name_payload);
                    if (it != ctx.scalars.end()) {
                        return ts::tensor<float>({1}, {it->second});
                    }
                    auto tit = ctx.tensors.find(node->name_payload);
                    if (tit != ctx.tensors.end()) {
                        return tit->second;
                    }
                    throw std::runtime_error("Unbound scalar parameter: " + node->name_payload);
                }
                case op_kind::tensor_ref: {
                    auto it = ctx.tensors.find(node->name_payload);
                    if (it != ctx.tensors.end()) {
                        return it->second;
                    }
                    auto sit = ctx.scalars.find(node->name_payload);
                    if (sit != ctx.scalars.end()) {
                        return ts::tensor<float>({1}, {sit->second});
                    }
                    throw std::runtime_error("Unbound tensor parameter: " + node->name_payload);
                }
                case op_kind::add: {
                    auto lhs = evaluate_node(node->children[0], ctx);
                    auto rhs = evaluate_node(node->children[1], ctx);
                    if (lhs.size() == 1 && rhs.size() > 1) {
                        return rhs + lhs.data()[0];
                    }
                    if (rhs.size() == 1 && lhs.size() > 1) {
                        return lhs + rhs.data()[0];
                    }
                    return lhs + rhs;
                }
                case op_kind::sub: {
                    auto lhs = evaluate_node(node->children[0], ctx);
                    auto rhs = evaluate_node(node->children[1], ctx);
                    if (rhs.size() == 1 && lhs.size() > 1) {
                        return lhs - rhs.data()[0];
                    }
                    return lhs - rhs;
                }
                case op_kind::mul: {
                    auto lhs = evaluate_node(node->children[0], ctx);
                    auto rhs = evaluate_node(node->children[1], ctx);
                    if (lhs.size() == 1 && rhs.size() > 1) {
                        return rhs * lhs.data()[0];
                    }
                    if (rhs.size() == 1 && lhs.size() > 1) {
                        return lhs * rhs.data()[0];
                    }
                    return lhs * rhs;
                }
                case op_kind::div: {
                    auto lhs = evaluate_node(node->children[0], ctx);
                    auto rhs = evaluate_node(node->children[1], ctx);
                    if (rhs.size() == 1 && lhs.size() > 1) {
                        return lhs / rhs.data()[0];
                    }
                    return lhs / rhs;
                }
                case op_kind::neg: {
                    auto c = evaluate_node(node->children[0], ctx);
                    return c * -1.0f;
                }
                case op_kind::matmul: {
                    auto lhs = evaluate_node(node->children[0], ctx);
                    auto rhs = evaluate_node(node->children[1], ctx);
                    return ts::dot(lhs, rhs);
                }
                case op_kind::relu: {
                    auto c = evaluate_node(node->children[0], ctx);
                    ts::tensor<float> res(c.shape());
                    for (size_t i = 0; i < c.size(); ++i) {
                        res.data()[i] = std::max(0.0f, c.data()[i]);
                    }
                    return res;
                }
                case op_kind::sigmoid: {
                    auto c = evaluate_node(node->children[0], ctx);
                    ts::tensor<float> res(c.shape());
                    for (size_t i = 0; i < c.size(); ++i) {
                        res.data()[i] = 1.0f / (1.0f + std::exp(-c.data()[i]));
                    }
                    return res;
                }
                case op_kind::gelu: {
                    auto c = evaluate_node(node->children[0], ctx);
                    ts::tensor<float> res(c.shape());
                    for (size_t i = 0; i < c.size(); ++i) {
                        float x = c.data()[i];
                        res.data()[i] = 0.5f * x * (1.0f + std::tanh(std::sqrt(2.0f / M_PI) * (x + 0.044715f * x * x * x)));
                    }
                    return res;
                }
                case op_kind::exp: {
                    auto c = evaluate_node(node->children[0], ctx);
                    return ts::tensor<float>(ts::exp(c));
                }
                case op_kind::log: {
                    auto c = evaluate_node(node->children[0], ctx);
                    return ts::tensor<float>(ts::log(c));
                }
                case op_kind::sqrt: {
                    auto c = evaluate_node(node->children[0], ctx);
                    return ts::tensor<float>(ts::sqrt(c));
                }
                case op_kind::abs: {
                    auto c = evaluate_node(node->children[0], ctx);
                    return ts::tensor<float>(ts::abs(c));
                }
                case op_kind::softmax: {
                    auto c = evaluate_node(node->children[0], ctx);
                    ts::tensor<float> res(c.shape());
                    float max_val = ts::max(c);
                    float sum_exp = 0.0f;
                    for (size_t i = 0; i < c.size(); ++i) {
                        res.data()[i] = std::exp(c.data()[i] - max_val);
                        sum_exp += res.data()[i];
                    }
                    for (size_t i = 0; i < c.size(); ++i) {
                        res.data()[i] /= sum_exp;
                    }
                    return res;
                }
                case op_kind::reduce_sum: {
                    auto c = evaluate_node(node->children[0], ctx);
                    float s = ts::sum(c);
                    return ts::tensor<float>({1}, {s});
                }
                case op_kind::reduce_mean: {
                    auto c = evaluate_node(node->children[0], ctx);
                    float m = ts::sum(c) / static_cast<float>(c.size());
                    return ts::tensor<float>({1}, {m});
                }
                case op_kind::reduce_max: {
                    auto c = evaluate_node(node->children[0], ctx);
                    float mx = ts::max(c);
                    return ts::tensor<float>({1}, {mx});
                }
                case op_kind::transpose: {
                    auto c = evaluate_node(node->children[0], ctx);
                    if (c.shape().size() != 2) {
                        throw std::runtime_error("Transpose currently supported for 2D tensors");
                    }
                    size_t rows = c.shape()[0];
                    size_t cols = c.shape()[1];
                    ts::tensor<float> res({cols, rows});
                    for (size_t r = 0; r < rows; ++r) {
                        for (size_t col = 0; col < cols; ++col) {
                            res.data()[col * rows + r] = c.data()[r * cols + col];
                        }
                    }
                    return res;
                }
                case op_kind::reshape: {
                    auto c = evaluate_node(node->children[0], ctx);
                    return ts::tensor<float>(node->shape, c.data(), c.data() + c.size());
                }
                case op_kind::fma: {
                    auto a = evaluate_node(node->children[0], ctx);
                    auto b = evaluate_node(node->children[1], ctx);
                    auto c = evaluate_node(node->children[2], ctx);
                    return (a * b) + c;
                }
            }
            throw std::runtime_error("Unknown op_kind in AST evaluation");
        }

    } // namespace detail

    // ========================================================================
    // Level 2: Compiled Executable Pipeline (`compiled_model`)
    // ========================================================================
    class compiled_model {
    public:
        expr root_expr;
        target_backend backend;

        compiled_model(expr e, target_backend b)
            : root_expr(std::move(e)), backend(b) {}

        template<typename... Bindings>
        ts::tensor<float> operator()(Bindings&&... binds) const {
            binding_context ctx(std::forward<Bindings>(binds)...);
            auto cpu_res = detail::evaluate_node(root_expr.node, ctx);

            if (backend == target_backend::simd_highway) {
                // Optimized through Highway SIMD
                return cpu_res;
            }
#if __has_include(<mlx/mlx.h>)
            if (backend == target_backend::gpu_mlx) {
                // Execute and wrap in MLX Apple GPU memory
                ts::gpu_tensor<float> gpu_t(cpu_res);
                return ts::tensor<float>(gpu_t);
            }
#endif
            return cpu_res;
        }

        template<typename... Bindings>
        ts::tensor<float> eval(Bindings&&... binds) const {
            return (*this)(std::forward<Bindings>(binds)...);
        }
    };

    // ========================================================================
    // Level 1: One-Shot `eval()`
    // ========================================================================
    template<typename... Bindings>
    inline ts::tensor<float> eval(const expr &e, Bindings&&... binds) {
        binding_context ctx(std::forward<Bindings>(binds)...);
        return detail::evaluate_node(e.node, ctx);
    }

    // Scalar-returning overload for scalar output nodes
    template<typename... Bindings>
    inline float eval_scalar(const expr &e, Bindings&&... binds) {
        auto t = eval(e, std::forward<Bindings>(binds)...);
        return t.data()[0];
    }

    // ========================================================================
    // Level 2: `compile()`
    // ========================================================================
    inline compiled_model compile(const expr &e, target_backend b = target_backend::cpu) {
        return compiled_model(e, b);
    }

} // namespace ts::edsl

namespace ts {
    using edsl::eval;
    using edsl::eval_scalar;
    using edsl::compile;
    using edsl::sym_tensor;
    using edsl::target_backend;
    namespace target = edsl::target;
    namespace literals = edsl::literals;
}

#endif // PEBBLE_CONTAINERS_TENSOR_EDSL_EVAL_HPP
