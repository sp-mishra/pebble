#pragma once
// ============================================================================
// MlxComputationPolicy.hpp — Apple Silicon MLX Accelerated Computation Policy
// ============================================================================
// C++23 / C++26, header-only, zero virtual dispatch.
// Dispatches operations to Apple Silicon GPU / Neural Engine via MLX.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MLX_COMPUTATION_POLICY_HPP
#define PEBBLE_CONTAINERS_MLX_COMPUTATION_POLICY_HPP

#include <containers/tensor/tensor.hpp>
#include <containers/tensor/mlx_storage_policy.hpp>

#if __has_include(<mlx/mlx.h>)
#include <mlx/mlx.h>
#include <vector>
#include <stdexcept>

namespace ts {
    struct MlxComputationPolicy {
    private:
        template <typename E>
        static mlx::core::array get_mlx_array(
            const TensorExpression<E, typename E::value_type, typename E::storage_policy, typename
                                   E::computation_policy>& expr) {
            auto shape_vec = get_shape(expr.self());
            if (shape_vec.empty()) {
                using T = typename E::value_type;
                T scalar_val = expr.self()({});
                return mlx::core::array(scalar_val);
            }

            if constexpr (requires { expr.self().storage(); }) {
                return expr.self().storage().get();
            }
            else if constexpr (requires { expr.self().operator()(std::vector<size_t>{}); }) {
                mlx::core::Shape shape_vec_int(shape_vec.begin(), shape_vec.end());
                using T = typename E::value_type;
                size_t total = calculate_size_dyn(shape_vec);
                std::vector<T> buffer(total);
                std::vector<size_t> idx(shape_vec.size());
                for (size_t i = 0; i < total; ++i) {
                    size_t temp_i = i;
                    for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                        if (shape_vec[d] > 0) {
                            idx[d] = temp_i % shape_vec[d];
                            temp_i /= shape_vec[d];
                        }
                    }
                    buffer[i] = expr.self()(idx);
                }
                return mlx::core::array(buffer.data(), shape_vec_int, MlxDtype<T>::value);
            }
            else {
                mlx::core::Shape shape_vec_int(shape_vec.begin(), shape_vec.end());
                using T = typename E::value_type;
                return mlx::core::array(const_cast<T*>(expr.self().data()), shape_vec_int, MlxDtype<T>::value);
            }
        }

        template <typename T>
        static auto wrap_result(mlx::core::array result_array, const TensorShape& expected_shape = {}) {
            MlxStorage<T> result_storage(result_array);
            TensorShape result_shape(result_array.shape().begin(), result_array.shape().end());

            if (!expected_shape.empty() &&
                result_shape.size() == 1 &&
                calculate_size_dyn(expected_shape) == result_storage.size()) {
                result_shape = expected_shape;
            }

            return DynamicTensor<T, MlxStoragePolicy, MlxComputationPolicy>(result_shape, result_storage);
        }

        template <typename E1, typename E2, typename BinaryOp>
        static auto binary_op_helper(const E1& a, const E2& b, BinaryOp op) {
            auto arr_a = get_mlx_array(a);
            auto arr_b = get_mlx_array(b);
            auto result_arr = op(arr_a, arr_b);

            const TensorShape shape_a = get_shape(a);
            const TensorShape shape_b = get_shape(b);
            TensorShape broadcasted = broadcast_shapes_unif(shape_a, shape_b);

            using T = typename E1::value_type;
            return wrap_result<T>(result_arr, broadcasted);
        }

    public:
        template <typename E1, typename E2>
        static auto add(const E1& a, const E2& b) {
            return binary_op_helper(a, b, [](auto arr_a, auto arr_b) {
                return mlx::core::add(arr_a, arr_b);
            });
        }

        template <typename E1, typename E2>
        static auto subtract(const E1& a, const E2& b) {
            return binary_op_helper(a, b, [](auto arr_a, auto arr_b) {
                return mlx::core::subtract(arr_a, arr_b);
            });
        }

        template <typename E1, typename E2>
        static auto multiply(const E1& a, const E2& b) {
            return binary_op_helper(a, b, [](auto arr_a, auto arr_b) {
                return mlx::core::multiply(arr_a, arr_b);
            });
        }

        template <typename E1, typename E2>
        static auto divide(const E1& a, const E2& b) {
            return binary_op_helper(a, b, [](auto arr_a, auto arr_b) {
                return mlx::core::divide(arr_a, arr_b);
            });
        }

        template <typename E1, typename E2>
        static auto dot(const E1& a, const E2& b) {
            auto result_arr = mlx::core::matmul(get_mlx_array(a), get_mlx_array(b));
            return wrap_result<typename E1::value_type>(result_arr);
        }

        template <typename E>
        static auto abs(const E& e) {
            auto result_arr = mlx::core::abs(get_mlx_array(e));
            const TensorShape original_shape = get_shape(e);
            return wrap_result<typename E::value_type>(result_arr, original_shape);
        }

        template <typename E>
        static auto sqrt(const E& e) {
            auto result_arr = mlx::core::sqrt(get_mlx_array(e));
            const TensorShape original_shape = get_shape(e);
            return wrap_result<typename E::value_type>(result_arr, original_shape);
        }

        template <typename E>
        static auto exp(const E& e) {
            auto result_arr = mlx::core::exp(get_mlx_array(e));
            const TensorShape original_shape = get_shape(e);
            return wrap_result<typename E::value_type>(result_arr, original_shape);
        }

        template <typename E>
        static auto log(const E& e) {
            auto result_arr = mlx::core::log(get_mlx_array(e));
            const TensorShape original_shape = get_shape(e);
            return wrap_result<typename E::value_type>(result_arr, original_shape);
        }

        template <typename E>
        static auto sum(const E& expr) {
            auto result_arr = mlx::core::sum(get_mlx_array(expr));
            result_arr.eval();
            return result_arr.template item<typename E::value_type>();
        }

        template <typename E>
        static auto mean(const E& expr) {
            auto result_arr = mlx::core::mean(get_mlx_array(expr));
            result_arr.eval();
            return result_arr.template item<typename E::value_type>();
        }

        template <typename E>
        static auto max(const E& expr) {
            auto arr = get_mlx_array(expr);
            arr.eval();
            TensorShape shape = get_shape(expr);
            size_t expected_size = calculate_size_dyn(shape);
            if (arr.size() == expected_size && arr.shape().size() == 1 && shape.size() > 1) {
                mlx::core::Shape shape_vec(shape.begin(), shape.end());
                arr = mlx::core::reshape(arr, shape_vec);
                arr.eval();
            }
            auto result_arr = mlx::core::max(arr);
            result_arr.eval();
            return result_arr.template item<typename E::value_type>();
        }

        template <typename E1, typename E2>
        static auto greater(const E1& a, const E2& b) {
            auto arr_a = get_mlx_array(a);
            auto arr_b = get_mlx_array(b);
            auto result_arr = mlx::core::greater(arr_a, arr_b);
            result_arr.eval();

            const TensorShape shape_a = get_shape(a);
            const TensorShape shape_b = get_shape(b);
            const TensorShape broadcasted = broadcast_shapes_unif(shape_a, shape_b);

            std::vector<bool> bool_data;
            bool_data.reserve(result_arr.size());

            if (result_arr.dtype() == mlx::core::bool_) {
                const auto* raw_data = result_arr.template data<bool>();
                for (size_t i = 0; i < result_arr.size(); ++i) {
                    bool_data.push_back(raw_data[i]);
                }
            }
            else {
                throw std::runtime_error("MLX greater did not return bool dtype");
            }

            return DynamicTensor<bool, DefaultStoragePolicy, DefaultComputationPolicy>(
                broadcasted, bool_data.begin(), bool_data.end());
        }
    };

    using mlx_computation_policy = MlxComputationPolicy;

    // --- Convenient MLX Apple Silicon GPU Aliases ---
    template <typename T>
    using gpu_tensor = DynamicTensor<T, MlxStoragePolicy, MlxComputationPolicy>;

    template <typename T>
    using mlx_tensor = DynamicTensor<T, MlxStoragePolicy, MlxComputationPolicy>;

    template <typename T>
    using GpuTensor = gpu_tensor<T>;

    template <typename T>
    using MlxTensor = mlx_tensor<T>;
} // namespace ts

using MlxComputationPolicy = ts::MlxComputationPolicy;
using mlx_computation_policy = ts::mlx_computation_policy;

template <typename T>
using gpu_tensor = ts::gpu_tensor<T>;

template <typename T>
using mlx_tensor = ts::mlx_tensor<T>;

namespace containers::tensor {
    using ts::MlxComputationPolicy;
    using ts::mlx_computation_policy;
    using ts::gpu_tensor;
    using ts::mlx_tensor;
    using ts::GpuTensor;
    using ts::MlxTensor;
}

#endif // __has_include(<mlx/mlx.h>)

#endif // PEBBLE_CONTAINERS_MLX_COMPUTATION_POLICY_HPP
