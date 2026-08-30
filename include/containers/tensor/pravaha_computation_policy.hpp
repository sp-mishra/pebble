#pragma once
// ============================================================================
// pravaha_computation_policy.hpp — Pravaha Accelerated Computation Policy
// ============================================================================
// C++23 / C++26, header-only, zero virtual dispatch.
// Multi-core parallel execution powered by Pravaha task graphs and runner.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_PRAVAHA_COMPUTATION_POLICY_HPP
#define PEBBLE_CONTAINERS_PRAVAHA_COMPUTATION_POLICY_HPP

#include <containers/tensor/tensor.hpp>
#include <pravaha/pravaha.hpp>
#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ts {

    struct PravahaComputationPolicy {
    private:
        template<typename E>
        struct is_scalar_wrapper : std::false_type {};

        template<typename T, typename SP, typename CP>
        struct is_scalar_wrapper<ScalarWrapper<T, SP, CP>> : std::true_type {};

        static inline std::size_t optimal_chunk_size(std::size_t total_elements) noexcept {
            if (total_elements == 0) return 1;
            const std::size_t num_threads = std::max<std::size_t>(1, std::thread::hardware_concurrency());
            // Target ~4 chunks per worker thread for balanced scheduling
            const std::size_t target_chunks = num_threads * 4;
            const std::size_t calculated = (total_elements + target_chunks - 1) / target_chunks;
            return std::max<std::size_t>(1024, calculated);
        }

        template<typename E1, typename E2, typename OpScalar>
        static auto parallel_binary_op(const E1 &a, const E2 &b, OpScalar op_scalar) {
            using T = typename E1::value_type;
            const auto &tensor_a = a.self();
            const auto &tensor_b = b.self();

            auto shape = get_shape(tensor_a);
            const size_t total_size = calculate_size_dyn(shape);
            std::vector<T> result_data(total_size);
            T *result_ptr = result_data.data();

            constexpr bool a_is_scalar = is_scalar_wrapper<E1>::value;
            constexpr bool b_is_scalar = is_scalar_wrapper<E2>::value;

            if (total_size < 2048) {
                // Fast path for small tensors: single-threaded execution without thread pool overhead
                if constexpr (!a_is_scalar && !b_is_scalar) {
                    const T *data_a = tensor_a.data();
                    const T *data_b = tensor_b.data();
                    if (data_a && data_b) {
                        for (size_t i = 0; i < total_size; ++i) {
                            result_ptr[i] = op_scalar(data_a[i], data_b[i]);
                        }
                        return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(shape, std::move(result_data));
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
                return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(shape, std::move(result_data));
            }

            // Pravaha parallel execution for large tensors
            const std::size_t chunk_sz = optimal_chunk_size(total_size);
            auto chunks = pravaha::StaticChunkingPolicy::chunks(total_size, chunk_sz);
            pravaha::Runner<pravaha::JThreadBackend> runner;

            if constexpr (!a_is_scalar && !b_is_scalar) {
                const T *data_a = tensor_a.data();
                const T *data_b = tensor_b.data();
                if (data_a && data_b) {
                    for (std::size_t i = 0; i < chunks.size(); ++i) {
                        const auto range = chunks[i];
                        auto chunk_cmd = pravaha::TaskCommand::make([data_a, data_b, result_ptr, range, op_scalar]() {
                            for (std::size_t k = range.begin; k < range.end; ++k) {
                                result_ptr[k] = op_scalar(data_a[k], data_b[k]);
                            }
                        });
                        runner.backend_ref().submit(std::move(chunk_cmd));
                    }
                    runner.backend_ref().drain();
                    return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(shape, std::move(result_data));
                }
            }

            // General indexed fallback with Pravaha chunking
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                const auto range = chunks[i];
                auto chunk_cmd = pravaha::TaskCommand::make([&tensor_a, &tensor_b, result_ptr, range, shape, op_scalar]() {
                    std::vector<size_t> idx(shape.size(), 0);
                    for (size_t k = range.begin; k < range.end; ++k) {
                        size_t temp_k = k;
                        for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                            if (shape[d] > 0) {
                                idx[d] = temp_k % shape[d];
                                temp_k /= shape[d];
                            }
                        }
                        result_ptr[k] = op_scalar(tensor_a(idx), tensor_b(idx));
                    }
                });
                runner.backend_ref().submit(std::move(chunk_cmd));
            }
            runner.backend_ref().drain();

            return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(shape, std::move(result_data));
        }

        template<typename E, typename OpScalar>
        static auto parallel_unary_op(const E &a, OpScalar op_scalar) {
            using T = typename E::value_type;
            const auto &tensor_a = a.self();
            auto shape = get_shape(tensor_a);
            const size_t total_size = calculate_size_dyn(shape);
            std::vector<T> result_data(total_size);
            T *result_ptr = result_data.data();

            constexpr bool a_is_scalar = is_scalar_wrapper<E>::value;

            if (total_size < 2048) {
                if constexpr (!a_is_scalar) {
                    const T *data_a = tensor_a.data();
                    if (data_a) {
                        for (size_t i = 0; i < total_size; ++i) {
                            result_ptr[i] = op_scalar(data_a[i]);
                        }
                        return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(shape, std::move(result_data));
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
                return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(shape, std::move(result_data));
            }

            const std::size_t chunk_sz = optimal_chunk_size(total_size);
            auto chunks = pravaha::StaticChunkingPolicy::chunks(total_size, chunk_sz);
            pravaha::Runner<pravaha::JThreadBackend> runner;

            if constexpr (!a_is_scalar) {
                const T *data_a = tensor_a.data();
                if (data_a) {
                    for (std::size_t i = 0; i < chunks.size(); ++i) {
                        const auto range = chunks[i];
                        auto chunk_cmd = pravaha::TaskCommand::make([data_a, result_ptr, range, op_scalar]() {
                            for (std::size_t k = range.begin; k < range.end; ++k) {
                                result_ptr[k] = op_scalar(data_a[k]);
                            }
                        });
                        runner.backend_ref().submit(std::move(chunk_cmd));
                    }
                    runner.backend_ref().drain();
                    return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(shape, std::move(result_data));
                }
            }

            for (std::size_t i = 0; i < chunks.size(); ++i) {
                const auto range = chunks[i];
                auto chunk_cmd = pravaha::TaskCommand::make([&tensor_a, result_ptr, range, shape, op_scalar]() {
                    std::vector<size_t> idx(shape.size(), 0);
                    for (size_t k = range.begin; k < range.end; ++k) {
                        size_t temp_k = k;
                        for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                            if (shape[d] > 0) {
                                idx[d] = temp_k % shape[d];
                                temp_k /= shape[d];
                            }
                        }
                        result_ptr[k] = op_scalar(tensor_a(idx));
                    }
                });
                runner.backend_ref().submit(std::move(chunk_cmd));
            }
            runner.backend_ref().drain();

            return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(shape, std::move(result_data));
        }

    public:
        template<typename E1, typename E2>
        static auto add(const E1 &a, const E2 &b) {
            return parallel_binary_op(a, b, [](auto x, auto y) { return x + y; });
        }

        template<typename E1, typename E2>
        static auto subtract(const E1 &a, const E2 &b) {
            return parallel_binary_op(a, b, [](auto x, auto y) { return x - y; });
        }

        template<typename E1, typename E2>
        static auto multiply(const E1 &a, const E2 &b) {
            return parallel_binary_op(a, b, [](auto x, auto y) { return x * y; });
        }

        template<typename E1, typename E2>
        static auto divide(const E1 &a, const E2 &b) {
            return parallel_binary_op(a, b, [](auto x, auto y) { return x / y; });
        }

        template<typename E>
        static auto abs(const E &e) {
            return parallel_unary_op(e, [](auto x) { return std::abs(x); });
        }

        template<typename E>
        static auto sqrt(const E &e) {
            return parallel_unary_op(e, [](auto x) { return std::sqrt(x); });
        }

        template<typename E>
        static auto exp(const E &e) {
            return parallel_unary_op(e, [](auto x) { return std::exp(x); });
        }

        template<typename E>
        static auto log(const E &e) {
            return parallel_unary_op(e, [](auto x) { return std::log(x); });
        }

        template<typename E>
        static auto sin(const E &e) {
            return parallel_unary_op(e, [](auto x) { return std::sin(x); });
        }

        template<typename E>
        static auto cos(const E &e) {
            return parallel_unary_op(e, [](auto x) { return std::cos(x); });
        }

        template<typename E>
        static auto tan(const E &e) {
            return parallel_unary_op(e, [](auto x) { return std::tan(x); });
        }

        template<typename E>
        static auto square(const E &e) {
            return parallel_unary_op(e, [](auto x) { return x * x; });
        }

        template<typename E1, typename E2>
        static auto power(const E1 &a, const E2 &b) {
            return parallel_binary_op(a, b, [](auto x, auto y) { return std::pow(x, y); });
        }

        template<typename E>
        static auto sum(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto dyn_shape = get_shape(tensor);
            const size_t total_size = calculate_size_dyn(dyn_shape);
            if (total_size == 0) return T{0};

            constexpr bool is_scalar = is_scalar_wrapper<E>::value;

            if (total_size < 2048) {
                if constexpr (!is_scalar) {
                    const T *data = tensor.data();
                    if (data) {
                        return std::accumulate(data, data + total_size, T{0});
                    }
                }
                T total = T{0};
                std::vector<size_t> idx(dyn_shape.size(), 0);
                for (size_t i = 0; i < total_size; ++i) {
                    size_t temp_i = i;
                    for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                        if (dyn_shape[d] > 0) {
                            idx[d] = temp_i % dyn_shape[d];
                            temp_i /= dyn_shape[d];
                        }
                    }
                    total += tensor(idx);
                }
                return total;
            }

            // Pravaha chunked reduction
            const std::size_t chunk_sz = optimal_chunk_size(total_size);
            auto chunks = pravaha::StaticChunkingPolicy::chunks(total_size, chunk_sz);
            std::vector<T> partials(chunks.size(), T{0});
            pravaha::Runner<pravaha::JThreadBackend> runner;

            if constexpr (!is_scalar) {
                const T *data = tensor.data();
                if (data) {
                    for (std::size_t i = 0; i < chunks.size(); ++i) {
                        const auto range = chunks[i];
                        auto chunk_cmd = pravaha::TaskCommand::make([data, &partials, i, range]() {
                            T chunk_sum = T{0};
                            for (std::size_t k = range.begin; k < range.end; ++k) {
                                chunk_sum += data[k];
                            }
                            partials[i] = chunk_sum;
                        });
                        runner.backend_ref().submit(std::move(chunk_cmd));
                    }
                    runner.backend_ref().drain();
                    return std::accumulate(partials.begin(), partials.end(), T{0});
                }
            }

            for (std::size_t i = 0; i < chunks.size(); ++i) {
                const auto range = chunks[i];
                auto chunk_cmd = pravaha::TaskCommand::make([&tensor, &partials, i, range, dyn_shape]() {
                    T chunk_sum = T{0};
                    std::vector<size_t> idx(dyn_shape.size(), 0);
                    for (size_t k = range.begin; k < range.end; ++k) {
                        size_t temp_k = k;
                        for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                            if (dyn_shape[d] > 0) {
                                idx[d] = temp_k % dyn_shape[d];
                                temp_k /= dyn_shape[d];
                            }
                        }
                        chunk_sum += tensor(idx);
                    }
                    partials[i] = chunk_sum;
                });
                runner.backend_ref().submit(std::move(chunk_cmd));
            }
            runner.backend_ref().drain();

            return std::accumulate(partials.begin(), partials.end(), T{0});
        }

        template<typename E>
        static auto mean(const E &expr) {
            const auto &tensor = expr.self();
            auto dyn_shape = get_shape(tensor);
            const size_t size = calculate_size_dyn(dyn_shape);
            if (size == 0) throw std::runtime_error("mean() on empty tensor not supported");
            return sum(expr) / static_cast<double>(size);
        }

        template<typename E>
        static auto max(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto dyn_shape = get_shape(tensor);
            const size_t total_size = calculate_size_dyn(dyn_shape);
            if (total_size == 0) throw std::runtime_error("max() on empty tensor not supported");

            constexpr bool is_scalar = is_scalar_wrapper<E>::value;

            if (total_size < 2048) {
                if constexpr (!is_scalar) {
                    const T *data = tensor.data();
                    if (data) {
                        return *std::max_element(data, data + total_size);
                    }
                }
                T result = tensor(std::vector<size_t>(dyn_shape.size(), 0));
                std::vector<size_t> idx(dyn_shape.size(), 0);
                for (size_t i = 1; i < total_size; ++i) {
                    size_t temp_i = i;
                    for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                        if (dyn_shape[d] > 0) {
                            idx[d] = temp_i % dyn_shape[d];
                            temp_i /= dyn_shape[d];
                        }
                    }
                    result = std::max(result, tensor(idx));
                }
                return result;
            }

            const std::size_t chunk_sz = optimal_chunk_size(total_size);
            auto chunks = pravaha::StaticChunkingPolicy::chunks(total_size, chunk_sz);
            std::vector<T> partial_maxs(chunks.size());
            pravaha::Runner<pravaha::JThreadBackend> runner;

            if constexpr (!is_scalar) {
                const T *data = tensor.data();
                if (data) {
                    for (std::size_t i = 0; i < chunks.size(); ++i) {
                        const auto range = chunks[i];
                        auto chunk_cmd = pravaha::TaskCommand::make([data, &partial_maxs, i, range]() {
                            T local_max = data[range.begin];
                            for (std::size_t k = range.begin + 1; k < range.end; ++k) {
                                if (data[k] > local_max) local_max = data[k];
                            }
                            partial_maxs[i] = local_max;
                        });
                        runner.backend_ref().submit(std::move(chunk_cmd));
                    }
                    runner.backend_ref().drain();
                    return *std::max_element(partial_maxs.begin(), partial_maxs.end());
                }
            }

            for (std::size_t i = 0; i < chunks.size(); ++i) {
                const auto range = chunks[i];
                auto chunk_cmd = pravaha::TaskCommand::make([&tensor, &partial_maxs, i, range, dyn_shape]() {
                    std::vector<size_t> idx(dyn_shape.size(), 0);
                    auto get_val = [&](std::size_t k) {
                        size_t temp_k = k;
                        for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                            if (dyn_shape[d] > 0) {
                                idx[d] = temp_k % dyn_shape[d];
                                temp_k /= dyn_shape[d];
                            }
                        }
                        return tensor(idx);
                    };
                    T local_max = get_val(range.begin);
                    for (std::size_t k = range.begin + 1; k < range.end; ++k) {
                        T val = get_val(k);
                        if (val > local_max) local_max = val;
                    }
                    partial_maxs[i] = local_max;
                });
                runner.backend_ref().submit(std::move(chunk_cmd));
            }
            runner.backend_ref().drain();

            return *std::max_element(partial_maxs.begin(), partial_maxs.end());
        }

        template<typename E>
        static auto min(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto dyn_shape = get_shape(tensor);
            const size_t total_size = calculate_size_dyn(dyn_shape);
            if (total_size == 0) throw std::runtime_error("min() on empty tensor not supported");

            constexpr bool is_scalar = is_scalar_wrapper<E>::value;

            if (total_size < 2048) {
                if constexpr (!is_scalar) {
                    const T *data = tensor.data();
                    if (data) {
                        return *std::min_element(data, data + total_size);
                    }
                }
                T result = tensor(std::vector<size_t>(dyn_shape.size(), 0));
                std::vector<size_t> idx(dyn_shape.size(), 0);
                for (size_t i = 1; i < total_size; ++i) {
                    size_t temp_i = i;
                    for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                        if (dyn_shape[d] > 0) {
                            idx[d] = temp_i % dyn_shape[d];
                            temp_i /= dyn_shape[d];
                        }
                    }
                    result = std::min(result, tensor(idx));
                }
                return result;
            }

            const std::size_t chunk_sz = optimal_chunk_size(total_size);
            auto chunks = pravaha::StaticChunkingPolicy::chunks(total_size, chunk_sz);
            std::vector<T> partial_mins(chunks.size());
            pravaha::Runner<pravaha::JThreadBackend> runner;

            if constexpr (!is_scalar) {
                const T *data = tensor.data();
                if (data) {
                    for (std::size_t i = 0; i < chunks.size(); ++i) {
                        const auto range = chunks[i];
                        auto chunk_cmd = pravaha::TaskCommand::make([data, &partial_mins, i, range]() {
                            T local_min = data[range.begin];
                            for (std::size_t k = range.begin + 1; k < range.end; ++k) {
                                if (data[k] < local_min) local_min = data[k];
                            }
                            partial_mins[i] = local_min;
                        });
                        runner.backend_ref().submit(std::move(chunk_cmd));
                    }
                    runner.backend_ref().drain();
                    return *std::min_element(partial_mins.begin(), partial_mins.end());
                }
            }

            for (std::size_t i = 0; i < chunks.size(); ++i) {
                const auto range = chunks[i];
                auto chunk_cmd = pravaha::TaskCommand::make([&tensor, &partial_mins, i, range, dyn_shape]() {
                    std::vector<size_t> idx(dyn_shape.size(), 0);
                    auto get_val = [&](std::size_t k) {
                        size_t temp_k = k;
                        for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                            if (dyn_shape[d] > 0) {
                                idx[d] = temp_k % dyn_shape[d];
                                temp_k /= dyn_shape[d];
                            }
                        }
                        return tensor(idx);
                    };
                    T local_min = get_val(range.begin);
                    for (std::size_t k = range.begin + 1; k < range.end; ++k) {
                        T val = get_val(k);
                        if (val < local_min) local_min = val;
                    }
                    partial_mins[i] = local_min;
                });
                runner.backend_ref().submit(std::move(chunk_cmd));
            }
            runner.backend_ref().drain();

            return *std::min_element(partial_mins.begin(), partial_mins.end());
        }

        template<typename E>
        static auto variance(const E &expr) {
            const auto &tensor = expr.self();
            auto dyn_shape = get_shape(tensor);
            const size_t size = calculate_size_dyn(dyn_shape);
            if (size <= 1) return 0.0;
            
            const double mean_val = mean(expr);
            using T = typename E::value_type;
            const T* data = tensor.data();

            if (data && size >= 2048) {
                const std::size_t chunk_sz = optimal_chunk_size(size);
                auto chunks = pravaha::StaticChunkingPolicy::chunks(size, chunk_sz);
                std::vector<double> partial_sq_diff(chunks.size(), 0.0);
                pravaha::Runner<pravaha::JThreadBackend> runner;

                for (std::size_t i = 0; i < chunks.size(); ++i) {
                    const auto range = chunks[i];
                    auto chunk_cmd = pravaha::TaskCommand::make([data, &partial_sq_diff, i, range, mean_val]() {
                        double local_sum = 0.0;
                        for (std::size_t k = range.begin; k < range.end; ++k) {
                            const double diff = static_cast<double>(data[k]) - mean_val;
                            local_sum += diff * diff;
                        }
                        partial_sq_diff[i] = local_sum;
                    });
                    runner.backend_ref().submit(std::move(chunk_cmd));
                }
                runner.backend_ref().drain();
                const double total_sq_diff = std::accumulate(partial_sq_diff.begin(), partial_sq_diff.end(), 0.0);
                return total_sq_diff / static_cast<double>(size - 1);
            }

            return DefaultComputationPolicy::variance(expr);
        }

        template<typename E>
        static auto std_dev(const E &expr) {
            return std::sqrt(variance(expr));
        }

        template<typename E>
        static auto normalize(const E &expr) {
            using T = typename E::value_type;
            const auto &tensor = expr.self();
            auto shape = get_shape(tensor);
            const auto mean_val = mean(expr);
            const auto std_val = std_dev(expr);

            if (std_val == 0.0) {
                return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(
                    shape, std::vector<T>(calculate_size_dyn(shape), T{0}));
            }

            return parallel_unary_op(expr, [mean_val, std_val](auto x) {
                return static_cast<T>((static_cast<double>(x) - mean_val) / std_val);
            });
        }

        template<typename E1, typename E2>
        static auto dot(const E1 &a, const E2 &b) {
            const auto &A = a.self();
            const auto &B = b.self();
            auto ashape = get_shape(A);
            auto bshape = get_shape(B);
            using T = typename E1::value_type;

            if (ashape.size() == 2 && bshape.size() == 2) {
                if (ashape[1] != bshape[0]) {
                    throw std::invalid_argument("dot: Inner dimensions must match");
                }
                const size_t M = ashape[0];
                const size_t K = ashape[1];
                const size_t N = bshape[1];

                DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy> result({M, N});
                const T *a_ptr = A.data();
                const T *b_ptr = B.data();
                T *r_ptr = result.data();

                if (a_ptr && b_ptr && r_ptr && (M * N >= 1024)) {
                    // Parallelize row slices across worker threads with Pravaha
                    const std::size_t chunk_rows = std::max<std::size_t>(1, M / (std::max<std::size_t>(1, std::thread::hardware_concurrency()) * 2));
                    auto chunks = pravaha::StaticChunkingPolicy::chunks(M, chunk_rows);
                    pravaha::Runner<pravaha::JThreadBackend> runner;

                    for (std::size_t c = 0; c < chunks.size(); ++c) {
                        const auto range = chunks[c];
                        auto chunk_cmd = pravaha::TaskCommand::make([a_ptr, b_ptr, r_ptr, range, K, N]() {
                            for (std::size_t i = range.begin; i < range.end; ++i) {
                                for (std::size_t j = 0; j < N; ++j) {
                                    T sum_val = T{0};
                                    for (std::size_t k = 0; k < K; ++k) {
                                        sum_val += a_ptr[i * K + k] * b_ptr[k * N + j];
                                    }
                                    r_ptr[i * N + j] = sum_val;
                                }
                            }
                        });
                        runner.backend_ref().submit(std::move(chunk_cmd));
                    }
                    runner.backend_ref().drain();
                    return result;
                }
            }

            auto res = DefaultComputationPolicy::dot(a, b);
            return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(res.shape(), res.data(), res.data() + res.size());
        }

        template<typename E1, typename E2>
        static auto greater(const E1 &a, const E2 &b) {
            const auto &tensor_a = a.self();
            const auto &tensor_b = b.self();

            auto shape = get_shape(tensor_a);
            const size_t total_size = calculate_size_dyn(shape);

            std::vector<bool> bool_result(total_size);
            std::vector<size_t> idx(shape.size(), 0);
            for (size_t i = 0; i < total_size; ++i) {
                size_t temp_i = i;
                for (int d = static_cast<int>(idx.size()) - 1; d >= 0; --d) {
                    if (shape[d] > 0) {
                        idx[d] = temp_i % shape[d];
                        temp_i /= shape[d];
                    }
                }
                bool_result[i] = (tensor_a(idx) > tensor_b(idx));
            }

            return DynamicTensor<bool, DefaultStoragePolicy, DefaultComputationPolicy>(
                shape, bool_result.begin(), bool_result.end());
        }

        template<typename E>
        static auto reshape(const E &expr, const TensorShape &new_shape) {
            auto res = DefaultComputationPolicy::reshape(expr, new_shape);
            using T = typename E::value_type;
            return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(res.shape(), res.data(), res.data() + res.size());
        }

        template<typename E>
        static auto flatten(const E &expr) {
            auto shape = get_shape(expr.self());
            auto size = calculate_size_dyn(shape);
            return reshape(expr, TensorShape{size});
        }

        template<typename E>
        static auto transpose(const E &expr) {
            auto res = DefaultComputationPolicy::transpose(expr);
            using T = typename E::value_type;
            return DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>(res.shape(), res.data(), res.data() + res.size());
        }

        template<typename E, typename T>
        static auto clip(const E &expr, T min_val, T max_val) {
            return parallel_unary_op(expr, [min_val, max_val](auto x) {
                return std::clamp(x, min_val, max_val);
            });
        }

        // ----------------------------------------------------------------
        // BLAS primitives — row-parallel via Pravaha task graph
        // ----------------------------------------------------------------

        // gemm: C ← α·A·B + β·C  (row-parallel outer loop)
        template<typename T, typename SP, typename CP>
        static void gemm(T alpha,
                         const DynamicTensor<T,SP,CP>& A,
                         const DynamicTensor<T,SP,CP>& B,
                         T beta,
                         DynamicTensor<T,SP,CP>& C) {
            const auto& as = A.shape();
            const auto& bs = B.shape();
            const auto& cs = C.shape();
            if (as.size() != 2 || bs.size() != 2 || cs.size() != 2)
                throw std::invalid_argument("gemm: all tensors must be rank-2");
            const size_t M = as[0], K = as[1], N = bs[1];
            if (bs[0] != K || cs[0] != M || cs[1] != N)
                throw std::invalid_argument("gemm: incompatible shapes");

            const T* a = A.data();
            const T* b = B.data();
            T*       c = C.data();
            if (!a || !b || !c) throw std::runtime_error("gemm: null data pointer");

            if (beta == T{0}) std::fill(c, c + M * N, T{0});
            else if (beta != T{1}) for (size_t i = 0; i < M * N; ++i) c[i] *= beta;

            if (M * N < 1024) {
                DefaultComputationPolicy::template gemm<T,SP,CP>(alpha, A, B, T{1}, C);
                return;
            }

            const size_t chunk_rows = std::max<size_t>(1, M / (std::max<size_t>(1, std::thread::hardware_concurrency()) * 2));
            auto chunks = pravaha::StaticChunkingPolicy::chunks(M, chunk_rows);
            pravaha::Runner<pravaha::JThreadBackend> runner;

            for (size_t ci = 0; ci < chunks.size(); ++ci) {
                const auto range = chunks[ci];
                auto cmd = pravaha::TaskCommand::make([a, b, c, range, K, N, alpha]() {
                    constexpr size_t KC = 256, NC = 128;
                    for (size_t i = range.begin; i < range.end; ++i) {
                        for (size_t kk = 0; kk < K; kk += KC) {
                            const size_t kb = std::min(KC, K - kk);
                            for (size_t jj = 0; jj < N; jj += NC) {
                                const size_t nb = std::min(NC, N - jj);
                                const T* ar = a + i * K + kk;
                                T*       cr = c + i * N + jj;
                                for (size_t j = 0; j < nb; ++j) {
                                    T sum = T{0};
                                    for (size_t k = 0; k < kb; ++k)
                                        sum += ar[k] * b[(kk + k) * N + (jj + j)];
                                    cr[j] += alpha * sum;
                                }
                            }
                        }
                    }
                });
                runner.backend_ref().submit(std::move(cmd));
            }
            runner.backend_ref().drain();
        }

        // gemv: y ← α·A·x + β·y  (row-parallel)
        template<typename T, typename SP, typename CP>
        static void gemv(T alpha,
                         const DynamicTensor<T,SP,CP>& A,
                         const DynamicTensor<T,SP,CP>& x,
                         T beta,
                         DynamicTensor<T,SP,CP>& y) {
            const auto& as = A.shape();
            const auto& xs = x.shape();
            const auto& ys = y.shape();
            if (as.size() != 2 || xs.size() != 1 || ys.size() != 1)
                throw std::invalid_argument("gemv: A must be rank-2, x and y rank-1");
            const size_t M = as[0], N = as[1];
            if (xs[0] != N || ys[0] != M)
                throw std::invalid_argument("gemv: incompatible shapes");

            const T* ap = A.data();
            const T* xp = x.data();
            T*       yp = y.data();
            if (!ap || !xp || !yp) throw std::runtime_error("gemv: null data pointer");

            if (beta == T{0}) std::fill(yp, yp + M, T{0});
            else if (beta != T{1}) for (size_t i = 0; i < M; ++i) yp[i] *= beta;

            if (M < 256) {
                DefaultComputationPolicy::template gemv<T,SP,CP>(alpha, A, x, T{1}, y);
                return;
            }

            const size_t chunk_rows = std::max<size_t>(1, M / (std::max<size_t>(1, std::thread::hardware_concurrency()) * 2));
            auto chunks = pravaha::StaticChunkingPolicy::chunks(M, chunk_rows);
            pravaha::Runner<pravaha::JThreadBackend> runner;

            for (size_t ci = 0; ci < chunks.size(); ++ci) {
                const auto range = chunks[ci];
                auto cmd = pravaha::TaskCommand::make([ap, xp, yp, range, N, alpha]() {
                    for (size_t i = range.begin; i < range.end; ++i) {
                        T sum = T{0};
                        const T* row = ap + i * N;
                        for (size_t j = 0; j < N; ++j) sum += row[j] * xp[j];
                        yp[i] += alpha * sum;
                    }
                });
                runner.backend_ref().submit(std::move(cmd));
            }
            runner.backend_ref().drain();
        }

        // axpy: y ← α·x + y  (parallel chunks)
        template<typename T, typename SP, typename CP>
        static void axpy(T alpha,
                         const DynamicTensor<T,SP,CP>& x,
                         DynamicTensor<T,SP,CP>& y) {
            const size_t n = x.size();
            if (y.size() != n)
                throw std::invalid_argument("axpy: x and y must have the same size");
            const T* xp = x.data();
            T*       yp = y.data();
            if (!xp || !yp) throw std::runtime_error("axpy: null data pointer");

            if (n < 2048) {
                for (size_t i = 0; i < n; ++i) yp[i] += alpha * xp[i];
                return;
            }

            const size_t chunk_sz = optimal_chunk_size(n);
            auto chunks = pravaha::StaticChunkingPolicy::chunks(n, chunk_sz);
            pravaha::Runner<pravaha::JThreadBackend> runner;

            for (size_t ci = 0; ci < chunks.size(); ++ci) {
                const auto range = chunks[ci];
                auto cmd = pravaha::TaskCommand::make([xp, yp, range, alpha]() {
                    for (size_t i = range.begin; i < range.end; ++i)
                        yp[i] += alpha * xp[i];
                });
                runner.backend_ref().submit(std::move(cmd));
            }
            runner.backend_ref().drain();
        }

        // nrm2: ‖x‖₂  (parallel partial sums)
        template<typename T, typename SP, typename CP>
        static T nrm2(const DynamicTensor<T,SP,CP>& x) {
            const size_t n = x.size();
            const T* xp = x.data();
            if (!xp) throw std::runtime_error("nrm2: null data pointer");

            if (n < 2048) {
                T sum = T{0};
                for (size_t i = 0; i < n; ++i) sum += xp[i] * xp[i];
                return static_cast<T>(std::sqrt(static_cast<double>(sum)));
            }

            const size_t chunk_sz = optimal_chunk_size(n);
            auto chunks = pravaha::StaticChunkingPolicy::chunks(n, chunk_sz);
            std::vector<T> partial(chunks.size(), T{0});
            pravaha::Runner<pravaha::JThreadBackend> runner;

            for (size_t ci = 0; ci < chunks.size(); ++ci) {
                const auto range = chunks[ci];
                auto cmd = pravaha::TaskCommand::make([xp, &partial, ci, range]() {
                    T s = T{0};
                    for (size_t i = range.begin; i < range.end; ++i) s += xp[i] * xp[i];
                    partial[ci] = s;
                });
                runner.backend_ref().submit(std::move(cmd));
            }
            runner.backend_ref().drain();
            T sum = std::accumulate(partial.begin(), partial.end(), T{0});
            return static_cast<T>(std::sqrt(static_cast<double>(sum)));
        }

        // syrk: C ← α·A·Aᵀ + β·C  (delegates to scalar; symmetric pattern)
        template<typename T, typename SP, typename CP>
        static void syrk(T alpha,
                         const DynamicTensor<T,SP,CP>& A,
                         T beta,
                         DynamicTensor<T,SP,CP>& C,
                         bool upper = true) {
            DefaultComputationPolicy::template syrk<T,SP,CP>(alpha, A, beta, C, upper);
        }

        // matmul: C = A·B
        template<typename T, typename SP, typename CP>
        static DynamicTensor<T,SP,CP> matmul(
                const DynamicTensor<T,SP,CP>& A,
                const DynamicTensor<T,SP,CP>& B) {
            const auto& as = A.shape();
            const auto& bs = B.shape();
            if (as.size() != 2 || bs.size() != 2)
                throw std::invalid_argument("matmul: both tensors must be rank-2");
            DynamicTensor<T,SP,CP> C({as[0], bs[1]});
            gemm<T,SP,CP>(T{1}, A, B, T{0}, C);
            return C;
        }
    };

    using pravaha_computation_policy = PravahaComputationPolicy;

    template<typename T>
    using parallel_tensor = DynamicTensor<T, DefaultStoragePolicy, PravahaComputationPolicy>;

    template<typename T>
    using PravahaTensor = parallel_tensor<T>;

} // namespace ts

namespace containers::tensor {
    using ts::PravahaComputationPolicy;
    using ts::pravaha_computation_policy;
    using ts::parallel_tensor;
    using ts::PravahaTensor;
}

#endif // PEBBLE_CONTAINERS_PRAVAHA_COMPUTATION_POLICY_HPP
