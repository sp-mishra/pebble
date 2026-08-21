#pragma once
// ============================================================================
// HighwayComputationPolicy.hpp — SIMD Accelerated Computation Policy for Tensor
// ============================================================================
// C++23 / C++26, header-only, zero virtual dispatch.
// Portable SIMD vectorization powered by Google Highway (hwy).
// ============================================================================

#ifndef PEBBLE_CONTAINERS_HIGHWAY_COMPUTATION_POLICY_HPP
#define PEBBLE_CONTAINERS_HIGHWAY_COMPUTATION_POLICY_HPP

#include <containers/tensor/tensor.hpp>
#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>
#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace ts {

    // Custom aligned storage policy for SIMD/Highway
    struct HighwayStoragePolicy {
        template<typename T>
        using DynamicStorage = std::vector<T, hwy::AlignedAllocator<T>>;

        template<typename T, size_t Size>
        using StaticStorage = std::array<T, Size>;

        using StringStorage = ArrowStringStorage;
    };

    struct HighwayComputationPolicy {
    private:
        template<typename E>
        struct is_scalar_wrapper : std::false_type {};

        template<typename T, typename SP, typename CP>
        struct is_scalar_wrapper<ScalarWrapper<T, SP, CP>> : std::true_type {};

        template<typename E1, typename E2, typename OpVec, typename OpScalar>
        static auto highway_binary_op(const E1 &a, const E2 &b, OpVec op_vec, OpScalar op_scalar) {
            using T = typename E1::value_type;
            const auto &tensor_a = a.self();
            const auto &tensor_b = b.self();

            auto shape = get_shape(tensor_a);
            size_t total_size = calculate_size_dyn(shape);
            HighwayStoragePolicy::DynamicStorage<T> result_data(total_size);
            T *result_ptr = result_data.data();

            constexpr bool a_is_scalar = is_scalar_wrapper<E1>::value;
            constexpr bool b_is_scalar = is_scalar_wrapper<E2>::value;

            if constexpr (!a_is_scalar && !b_is_scalar) {
                const T *data_a = tensor_a.data();
                const T *data_b = tensor_b.data();

                if (data_a && data_b) {
                    namespace hn = hwy::HWY_NAMESPACE;
                    const hn::ScalableTag<T> d;
                    const size_t lanes = hn::Lanes(d);

                    size_t i = 0;
                    for (; i + lanes <= total_size; i += lanes) {
                        const auto va = hn::LoadU(d, data_a + i);
                        const auto vb = hn::LoadU(d, data_b + i);
                        const auto result = op_vec(d, va, vb);
                        hn::StoreU(result, d, result_ptr + i);
                    }

                    for (; i < total_size; ++i) {
                        result_ptr[i] = op_scalar(data_a[i], data_b[i]);
                    }

                    return DynamicTensor<T, HighwayStoragePolicy, HighwayComputationPolicy>(shape, std::move(result_data));
                }
            }

            std::vector<size_t> idx(shape.size(), 0);
            for (size_t i = 0; i < total_size; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (shape[d] > 0) {
                        idx[d] = temp_i % shape[d];
                        temp_i /= shape[d];
                    }
                }
                result_ptr[i] = op_scalar(tensor_a(idx), tensor_b(idx));
            }

            return DynamicTensor<T, HighwayStoragePolicy, HighwayComputationPolicy>(shape, std::move(result_data));
        }

        template<typename E, typename OpVec, typename OpScalar>
        static auto highway_unary_op_fallback(const E &a, OpVec op_vec, OpScalar op_scalar) {
            using T = typename E::value_type;
            const auto &tensor_a = a.self();
            auto shape = get_shape(tensor_a);
            size_t total_size = calculate_size_dyn(shape);
            HighwayStoragePolicy::DynamicStorage<T> result_data(total_size);
            T *result_ptr = result_data.data();

            constexpr bool a_is_scalar = is_scalar_wrapper<E>::value;

            if constexpr (!a_is_scalar) {
                const T *data_a = tensor_a.data();
                if (data_a) {
                    namespace hn = hwy::HWY_NAMESPACE;
                    const hn::ScalableTag<T> d;
                    const size_t lanes = hn::Lanes(d);

                    size_t i = 0;
                    for (; i + lanes <= total_size; i += lanes) {
                        const auto va = hn::LoadU(d, data_a + i);
                        const auto result = op_vec(d, va);
                        hn::StoreU(result, d, result_ptr + i);
                    }

                    for (; i < total_size; ++i) {
                        result_ptr[i] = op_scalar(data_a[i]);
                    }

                    return DynamicTensor<T, HighwayStoragePolicy, HighwayComputationPolicy>(shape, std::move(result_data));
                }
            }

            std::vector<size_t> idx(shape.size(), 0);
            for (size_t i = 0; i < total_size; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (shape[d] > 0) {
                        idx[d] = temp_i % shape[d];
                        temp_i /= shape[d];
                    }
                }
                result_ptr[i] = op_scalar(tensor_a(idx));
            }

            return DynamicTensor<T, HighwayStoragePolicy, HighwayComputationPolicy>(shape, std::move(result_data));
        }

    public:
        template<typename E1, typename E2>
        static auto add(const E1 &a, const E2 &b) {
            namespace hn = hwy::HWY_NAMESPACE;
            return highway_binary_op(a, b,
                []([[maybe_unused]] auto d, auto a_vec, auto b_vec) { return hn::Add(a_vec, b_vec); },
                [](auto a_val, auto b_val) { return a_val + b_val; });
        }

        template<typename E1, typename E2>
        static auto subtract(const E1 &a, const E2 &b) {
            namespace hn = hwy::HWY_NAMESPACE;
            return highway_binary_op(a, b,
                []([[maybe_unused]] auto d, auto a_vec, auto b_vec) { return hn::Sub(a_vec, b_vec); },
                [](auto a_val, auto b_val) { return a_val - b_val; });
        }

        template<typename E1, typename E2>
        static auto multiply(const E1 &a, const E2 &b) {
            namespace hn = hwy::HWY_NAMESPACE;
            return highway_binary_op(a, b,
                []([[maybe_unused]] auto d, auto a_vec, auto b_vec) { return hn::Mul(a_vec, b_vec); },
                [](auto a_val, auto b_val) { return a_val * b_val; });
        }

        template<typename E1, typename E2>
        static auto divide(const E1 &a, const E2 &b) {
            namespace hn = hwy::HWY_NAMESPACE;
            return highway_binary_op(a, b,
                []([[maybe_unused]] auto d, auto a_vec, auto b_vec) { return hn::Div(a_vec, b_vec); },
                [](auto a_val, auto b_val) { return a_val / b_val; });
        }

        template<typename E>
        static auto sum(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto shape = get_shape(tensor);
            const size_t total_size = calculate_size_dyn(shape);

            constexpr bool is_scalar = is_scalar_wrapper<E>::value;

            if constexpr (!is_scalar) {
                const T *data = tensor.data();
                if (data) {
                    namespace hn = hwy::HWY_NAMESPACE;
                    const hn::ScalableTag<T> d;
                    auto sum_vec = hn::Zero(d);
                    const size_t lanes = hn::Lanes(d);

                    size_t i = 0;
                    for (; i + lanes <= total_size; i += lanes) {
                        const auto vec = hn::LoadU(d, data + i);
                        sum_vec = hn::Add(sum_vec, vec);
                    }

                    T tail_sum = 0;
                    for (; i < total_size; ++i) {
                        tail_sum += data[i];
                    }

                    return hn::ReduceSum(d, sum_vec) + tail_sum;
                }
            }

            T total = T{0};
            std::vector<size_t> idx(shape.size(), 0);
            for (size_t i = 0; i < total_size; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (shape[d] > 0) {
                        idx[d] = temp_i % shape[d];
                        temp_i /= shape[d];
                    }
                }
                total += tensor(idx);
            }
            return total;
        }

        template<typename E>
        static auto mean(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto shape = get_shape(tensor);
            size_t total_size = calculate_size_dyn(shape);
            if (total_size == 0) throw std::runtime_error("Mean of empty tensor");
            return sum(expr) / static_cast<T>(total_size);
        }

        template<typename E>
        static auto sqrt(const E &e) {
            namespace hn = hwy::HWY_NAMESPACE;
            return highway_unary_op_fallback(e,
                []([[maybe_unused]] auto d, auto a_vec) { return hn::Sqrt(a_vec); },
                [](auto x) { return std::sqrt(x); });
        }

        template<typename E>
        static auto exp(const E &e) {
            return highway_unary_op_fallback(e,
                []([[maybe_unused]] auto d, auto a_vec) { return a_vec; },
                [](auto x) { return std::exp(x); });
        }

        template<typename E>
        static auto log(const E &e) {
            return highway_unary_op_fallback(e,
                []([[maybe_unused]] auto d, auto a_vec) { return a_vec; },
                [](auto x) { return std::log(x); });
        }

        template<typename E>
        static auto abs(const E &e) {
            namespace hn = hwy::HWY_NAMESPACE;
            return highway_unary_op_fallback(e,
                []([[maybe_unused]] auto d, auto a_vec) { return hn::Abs(a_vec); },
                [](auto x) { return std::abs(x); });
        }

        template<typename E1, typename E2>
        static auto dot(const E1 &a, const E2 &b) {
            return DefaultComputationPolicy::dot(a, b);
        }

        template<typename E>
        static auto max(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto shape = get_shape(tensor);
            const size_t total_size = calculate_size_dyn(shape);

            if (total_size == 0) throw std::runtime_error("Max of empty tensor");

            constexpr bool is_scalar = is_scalar_wrapper<E>::value;

            if constexpr (!is_scalar) {
                const T *data = tensor.data();
                if (data) {
                    namespace hn = hwy::HWY_NAMESPACE;
                    const hn::ScalableTag<T> d;
                    const size_t lanes = hn::Lanes(d);

                    if (total_size < lanes) {
                        return *std::max_element(data, data + total_size);
                    }

                    size_t i = 0;
                    auto max_vec = hn::LoadU(d, data);
                    i += lanes;

                    for (; i + lanes <= total_size; i += lanes) {
                        const auto vec = hn::LoadU(d, data + i);
                        max_vec = hn::Max(vec, max_vec);
                    }

                    T max_val = hn::ReduceMax(d, max_vec);

                    for (; i < total_size; ++i) {
                        if (data[i] > max_val) max_val = data[i];
                    }
                    return max_val;
                }
            }

            T result = tensor(std::vector<size_t>(shape.size(), 0));
            std::vector<size_t> idx(shape.size(), 0);
            for (size_t i = 1; i < total_size; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (shape[d] > 0) {
                        idx[d] = temp_i % shape[d];
                        temp_i /= shape[d];
                    }
                }
                result = std::max(result, tensor(idx));
            }
            return result;
        }

        template<typename E>
        static auto min(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto shape = get_shape(tensor);
            const size_t total_size = calculate_size_dyn(shape);

            if (total_size == 0) throw std::runtime_error("Min of empty tensor");

            constexpr bool is_scalar = is_scalar_wrapper<E>::value;

            if constexpr (!is_scalar) {
                const T *data = tensor.data();
                if (data) {
                    namespace hn = hwy::HWY_NAMESPACE;
                    const hn::ScalableTag<T> d;
                    const size_t lanes = hn::Lanes(d);

                    if (total_size < lanes) {
                        return *std::min_element(data, data + total_size);
                    }

                    size_t i = 0;
                    auto min_vec = hn::LoadU(d, data);
                    i += lanes;

                    for (; i + lanes <= total_size; i += lanes) {
                        const auto vec = hn::LoadU(d, data + i);
                        min_vec = hn::Min(vec, min_vec);
                    }

                    T min_val = hn::ReduceMin(d, min_vec);

                    for (; i < total_size; ++i) {
                        if (data[i] < min_val) min_val = data[i];
                    }
                    return min_val;
                }
            }

            return DefaultComputationPolicy::min(expr);
        }

        template<typename E1, typename E2>
        static auto greater(const E1 &a, const E2 &b) {
            const auto &tensor_a = a.self();
            const auto &tensor_b = b.self();

            auto shape = get_shape(tensor_a);
            const size_t total_size = calculate_size_dyn(shape);

            std::vector<bool> bool_result;
            bool_result.reserve(total_size);

            std::vector<size_t> idx(shape.size(), 0);
            for (size_t i = 0; i < total_size; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (shape[d] > 0) {
                        idx[d] = temp_i % shape[d];
                        temp_i /= shape[d];
                    }
                }
                bool_result.push_back(tensor_a(idx) > tensor_b(idx));
            }

            return DynamicTensor<bool, DefaultStoragePolicy, DefaultComputationPolicy>(
                shape, bool_result.begin(), bool_result.end());
        }

        template<typename E>
        static auto variance(const E &expr) {
            return DefaultComputationPolicy::variance(expr);
        }

        template<typename E>
        static auto std_dev(const E &expr) {
            return DefaultComputationPolicy::std_dev(expr);
        }

        template<typename E>
        static auto normalize(const E &expr) {
            return DefaultComputationPolicy::normalize(expr);
        }

        template<typename E>
        static auto reshape(const E &expr, const TensorShape &new_shape) {
            return DefaultComputationPolicy::reshape(expr, new_shape);
        }

        template<typename E>
        static auto flatten(const E &expr) {
            return DefaultComputationPolicy::flatten(expr);
        }

        template<typename E>
        static auto transpose(const E &expr) {
            return DefaultComputationPolicy::transpose(expr);
        }

        template<typename E>
        static auto sin(const E &e) { return DefaultComputationPolicy::sin(e); }

        template<typename E>
        static auto cos(const E &e) { return DefaultComputationPolicy::cos(e); }

        template<typename E>
        static auto tan(const E &e) { return DefaultComputationPolicy::tan(e); }

        template<typename E>
        static auto square(const E &e) {
            namespace hn = hwy::HWY_NAMESPACE;
            return highway_unary_op_fallback(e,
                []([[maybe_unused]] auto d, auto a_vec) { return hn::Mul(a_vec, a_vec); },
                [](auto x) { return x * x; });
        }

        template<typename E1, typename E2>
        static auto power(const E1 &a, const E2 &b) {
            return DefaultComputationPolicy::power(a, b);
        }

        template<typename E, typename T>
        static auto clip(const E &expr, T min_val, T max_val) {
            return DefaultComputationPolicy::clip(expr, min_val, max_val);
        }
    };

    using highway_storage_policy = HighwayStoragePolicy;
    using highway_computation_policy = HighwayComputationPolicy;

} // namespace ts

// Global convenience aliases
using HighwayStoragePolicy = ts::HighwayStoragePolicy;
using HighwayComputationPolicy = ts::HighwayComputationPolicy;

using highway_storage_policy = ts::highway_storage_policy;
using highway_computation_policy = ts::highway_computation_policy;

namespace containers::tensor {
    using ts::HighwayStoragePolicy;
    using ts::HighwayComputationPolicy;
    using ts::highway_storage_policy;
    using ts::highway_computation_policy;
}

#endif // PEBBLE_CONTAINERS_HIGHWAY_COMPUTATION_POLICY_HPP
