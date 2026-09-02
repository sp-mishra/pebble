#ifndef PEBBLE_CONTAINERS_HYBRID_SIMD_PRAVAHA_POLICY_HPP
#define PEBBLE_CONTAINERS_HYBRID_SIMD_PRAVAHA_POLICY_HPP

// =============================================================================
// hybrid_simd_pravaha_policy.hpp — Dual-Axis Parallel Computation Policy
// =============================================================================
// Modern C++23 / C++26, Header-Only, Zero Virtual Dispatch, Zero Macros.
//
// Combines multi-core task scheduling (Pravaha JThreadBackend) with
// intra-core SIMD vector register lanes (Google Highway) to achieve complete
// hardware saturation on modern multi-core, vectorized CPUs.
// =============================================================================

#include <containers/tensor/tensor.hpp>
#include <containers/tensor/highway_computation_policy.hpp>
#include <pravaha/pravaha.hpp>
#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>
#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ts {
    struct HybridSimdPravahaComputationPolicy {
    private:
        static constexpr std::size_t k_parallel_threshold = 2048;

        template <typename E>
        struct is_scalar_wrapper : std::false_type {};

        template <typename T, typename SP, typename CP>
        struct is_scalar_wrapper<ScalarWrapper<T, SP, CP>> : std::true_type {};

        [[nodiscard]] static std::size_t optimal_chunk_size(const std::size_t total_size) noexcept {
            const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
            const std::size_t ideal = (total_size + hw_threads - 1) / hw_threads;
            return std::max<std::size_t>(512, ideal);
        }

        template <typename E1, typename E2, typename OpVec, typename OpScalar>
        static auto hybrid_binary_op(const E1& a, const E2& b, OpVec op_vec, OpScalar op_scalar) {
            using T = typename E1::value_type;
            const auto& tensor_a = a.self();
            const auto& tensor_b = b.self();

            auto shape = get_shape(tensor_a);
            const size_t total_size = calculate_size_dyn(shape);
            HighwayStoragePolicy::DynamicStorage<T> result_data(total_size);
            T* result_ptr = result_data.data();

            constexpr bool a_is_scalar = is_scalar_wrapper<E1>::value;
            constexpr bool b_is_scalar = is_scalar_wrapper<E2>::value;

            if constexpr (!a_is_scalar && !b_is_scalar) {
                const T* data_a = tensor_a.data();
                const T* data_b = tensor_b.data();

                if (data_a && data_b) {
                    // Small tensor fast-path: single-threaded Highway SIMD
                    if (total_size < k_parallel_threshold) {
                        namespace hn = hwy::HWY_NAMESPACE;
                        const hn::ScalableTag<T> d;
                        const size_t lanes = hn::Lanes(d);

                        size_t i = 0;
                        for (; i + lanes <= total_size; i += lanes) {
                            const auto va = hn::LoadU(d, data_a + i);
                            const auto vb = hn::LoadU(d, data_b + i);
                            const auto res = op_vec(d, va, vb);
                            hn::StoreU(res, d, result_ptr + i);
                        }
                        for (; i < total_size; ++i) {
                            result_ptr[i] = op_scalar(data_a[i], data_b[i]);
                        }
                        return DynamicTensor<T, HighwayStoragePolicy, HybridSimdPravahaComputationPolicy>(
                            shape, std::move(result_data));
                    }

                    // Large tensor multi-core chunked SIMD execution
                    ::pravaha::Runner<::pravaha::JThreadBackend> runner;
                    const std::size_t chunk_sz = optimal_chunk_size(total_size);
                    const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(total_size, chunk_sz);

                    for (const auto& ch : chunks) {
                        auto task_fn = [data_a, data_b, result_ptr, op_vec, op_scalar, begin = ch.begin, end = ch.end
                            ]() {
                            namespace hn = hwy::HWY_NAMESPACE;
                            const hn::ScalableTag<T> d;
                            const size_t lanes = hn::Lanes(d);

                            size_t i = begin;
                            for (; i + lanes <= end; i += lanes) {
                                const auto va = hn::LoadU(d, data_a + i);
                                const auto vb = hn::LoadU(d, data_b + i);
                                const auto res = op_vec(d, va, vb);
                                hn::StoreU(res, d, result_ptr + i);
                            }
                            for (; i < end; ++i) {
                                result_ptr[i] = op_scalar(data_a[i], data_b[i]);
                            }
                        };
                        (void)runner.submit(::pravaha::task("hybrid_binary_op", std::move(task_fn)));
                    }
                    runner.backend_ref().drain();

                    return DynamicTensor<T, HighwayStoragePolicy, HybridSimdPravahaComputationPolicy>(
                        shape, std::move(result_data));
                }
            }

            // Fallback for expression wrappers
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

            return DynamicTensor<T, HighwayStoragePolicy, HybridSimdPravahaComputationPolicy>(
                shape, std::move(result_data));
        }

        template <typename E, typename OpVec, typename OpScalar>
        static auto hybrid_unary_op(const E& a, OpVec op_vec, OpScalar op_scalar) {
            using T = typename E::value_type;
            const auto& tensor_a = a.self();
            auto shape = get_shape(tensor_a);
            const size_t total_size = calculate_size_dyn(shape);
            HighwayStoragePolicy::DynamicStorage<T> result_data(total_size);
            T* result_ptr = result_data.data();

            constexpr bool a_is_scalar = is_scalar_wrapper<E>::value;

            if constexpr (!a_is_scalar) {
                const T* data_a = tensor_a.data();
                if (data_a) {
                    if (total_size < k_parallel_threshold) {
                        namespace hn = hwy::HWY_NAMESPACE;
                        const hn::ScalableTag<T> d;
                        const size_t lanes = hn::Lanes(d);

                        size_t i = 0;
                        for (; i + lanes <= total_size; i += lanes) {
                            const auto va = hn::LoadU(d, data_a + i);
                            const auto res = op_vec(d, va);
                            hn::StoreU(res, d, result_ptr + i);
                        }
                        for (; i < total_size; ++i) {
                            result_ptr[i] = op_scalar(data_a[i]);
                        }
                        return DynamicTensor<T, HighwayStoragePolicy, HybridSimdPravahaComputationPolicy>(
                            shape, std::move(result_data));
                    }

                    ::pravaha::Runner<::pravaha::JThreadBackend> runner;
                    const std::size_t chunk_sz = optimal_chunk_size(total_size);
                    const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(total_size, chunk_sz);

                    for (const auto& ch : chunks) {
                        auto task_fn = [data_a, result_ptr, op_vec, op_scalar, begin = ch.begin, end = ch.end]() {
                            namespace hn = hwy::HWY_NAMESPACE;
                            const hn::ScalableTag<T> d;
                            const size_t lanes = hn::Lanes(d);

                            size_t i = begin;
                            for (; i + lanes <= end; i += lanes) {
                                const auto va = hn::LoadU(d, data_a + i);
                                const auto res = op_vec(d, va);
                                hn::StoreU(res, d, result_ptr + i);
                            }
                            for (; i < end; ++i) {
                                result_ptr[i] = op_scalar(data_a[i]);
                            }
                        };
                        (void)runner.submit(::pravaha::task("hybrid_unary_op", std::move(task_fn)));
                    }
                    runner.backend_ref().drain();

                    return DynamicTensor<T, HighwayStoragePolicy, HybridSimdPravahaComputationPolicy>(
                        shape, std::move(result_data));
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

            return DynamicTensor<T, HighwayStoragePolicy, HybridSimdPravahaComputationPolicy>(
                shape, std::move(result_data));
        }

    public:
        // =====================================================================
        // Elementwise Binary Arithmetic
        // =====================================================================

        template <typename E1, typename E2>
        static auto add(const E1& a, const E2& b) {
            namespace hn = hwy::HWY_NAMESPACE;
            return hybrid_binary_op(a, b,
                                    []([[maybe_unused]] auto d, auto va, auto vb) { return hn::Add(va, vb); },
                                    [](auto x, auto y) { return x + y; });
        }

        template <typename E1, typename E2>
        static auto subtract(const E1& a, const E2& b) {
            namespace hn = hwy::HWY_NAMESPACE;
            return hybrid_binary_op(a, b,
                                    []([[maybe_unused]] auto d, auto va, auto vb) { return hn::Sub(va, vb); },
                                    [](auto x, auto y) { return x - y; });
        }

        template <typename E1, typename E2>
        static auto multiply(const E1& a, const E2& b) {
            namespace hn = hwy::HWY_NAMESPACE;
            return hybrid_binary_op(a, b,
                                    []([[maybe_unused]] auto d, auto va, auto vb) { return hn::Mul(va, vb); },
                                    [](auto x, auto y) { return x * y; });
        }

        template <typename E1, typename E2>
        static auto divide(const E1& a, const E2& b) {
            namespace hn = hwy::HWY_NAMESPACE;
            return hybrid_binary_op(a, b,
                                    []([[maybe_unused]] auto d, auto va, auto vb) { return hn::Div(va, vb); },
                                    [](auto x, auto y) { return x / y; });
        }

        // =====================================================================
        // Elementwise Unary Arithmetic
        // =====================================================================

        template <typename E>
        static auto sqrt(const E& e) {
            namespace hn = hwy::HWY_NAMESPACE;
            return hybrid_unary_op(e,
                                   []([[maybe_unused]] auto d, auto va) { return hn::Sqrt(va); },
                                   [](auto x) { return std::sqrt(x); });
        }

        template <typename E>
        static auto abs(const E& e) {
            namespace hn = hwy::HWY_NAMESPACE;
            return hybrid_unary_op(e,
                                   []([[maybe_unused]] auto d, auto va) { return hn::Abs(va); },
                                   [](auto x) { return std::abs(x); });
        }

        template <typename E>
        static auto square(const E& e) {
            namespace hn = hwy::HWY_NAMESPACE;
            return hybrid_unary_op(e,
                                   []([[maybe_unused]] auto d, auto va) { return hn::Mul(va, va); },
                                   [](auto x) { return x * x; });
        }

        template <typename E>
        static auto exp(const E& e) {
            return hybrid_unary_op(e,
                                   []([[maybe_unused]] auto d, auto va) { return va; },
                                   [](auto x) { return std::exp(x); });
        }

        template <typename E>
        static auto log(const E& e) {
            return hybrid_unary_op(e,
                                   []([[maybe_unused]] auto d, auto va) { return va; },
                                   [](auto x) { return std::log(x); });
        }

        template <typename E>
        static auto sin(const E& e) {
            return hybrid_unary_op(e,
                                   []([[maybe_unused]] auto d, auto va) { return va; },
                                   [](auto x) { return std::sin(x); });
        }

        template <typename E>
        static auto cos(const E& e) {
            return hybrid_unary_op(e,
                                   []([[maybe_unused]] auto d, auto va) { return va; },
                                   [](auto x) { return std::cos(x); });
        }

        template <typename E>
        static auto tan(const E& e) {
            return hybrid_unary_op(e,
                                   []([[maybe_unused]] auto d, auto va) { return va; },
                                   [](auto x) { return std::tan(x); });
        }

        // =====================================================================
        // Statistical Reductions
        // =====================================================================

        template <typename E>
        static auto sum(const E& expr) {
            using T = typename E::value_type;
            const auto& tensor = expr.self();
            auto shape = get_shape(tensor);
            const size_t total_size = calculate_size_dyn(shape);

            if (total_size == 0) return T{0};

            constexpr bool is_scalar = is_scalar_wrapper<E>::value;

            if constexpr (!is_scalar) {
                const T* data = tensor.data();
                if (data) {
                    if (total_size < k_parallel_threshold) {
                        return HighwayComputationPolicy::sum(expr);
                    }

                    ::pravaha::Runner<::pravaha::JThreadBackend> runner;
                    const std::size_t chunk_sz = optimal_chunk_size(total_size);
                    const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(total_size, chunk_sz);
                    std::vector<T> partial_sums(chunks.size(), T{0});

                    for (std::size_t c_idx = 0; c_idx < chunks.size(); ++c_idx) {
                        const auto& ch = chunks[c_idx];
                        auto* slot = &partial_sums[c_idx];

                        auto task_fn = [data, slot, begin = ch.begin, end = ch.end]() {
                            namespace hn = hwy::HWY_NAMESPACE;
                            const hn::ScalableTag<T> d;
                            auto sum_vec = hn::Zero(d);
                            const size_t lanes = hn::Lanes(d);

                            size_t i = begin;
                            for (; i + lanes <= end; i += lanes) {
                                const auto vec = hn::LoadU(d, data + i);
                                sum_vec = hn::Add(sum_vec, vec);
                            }

                            T tail = 0;
                            for (; i < end; ++i) {
                                tail += data[i];
                            }
                            *slot = hn::ReduceSum(d, sum_vec) + tail;
                        };
                        (void)runner.submit(::pravaha::task("hybrid_sum_chunk", std::move(task_fn)));
                    }
                    runner.backend_ref().drain();

                    T total = T{0};
                    for (T val : partial_sums) total += val;
                    return total;
                }
            }

            return DefaultComputationPolicy::sum(expr);
        }

        template <typename E>
        static auto mean(const E& expr) {
            using T = typename E::value_type;
            const auto& tensor = expr.self();
            auto shape = get_shape(tensor);
            const size_t total_size = calculate_size_dyn(shape);
            if (total_size == 0) throw std::runtime_error("Mean of empty tensor");
            return sum(expr) / static_cast<T>(total_size);
        }

        template <typename E>
        static auto max(const E& expr) {
            using T = typename E::value_type;
            const auto& tensor = expr.self();
            auto shape = get_shape(tensor);
            const size_t total_size = calculate_size_dyn(shape);

            if (total_size == 0) throw std::runtime_error("Max of empty tensor");

            constexpr bool is_scalar = is_scalar_wrapper<E>::value;

            if constexpr (!is_scalar) {
                const T* data = tensor.data();
                if (data) {
                    if (total_size < k_parallel_threshold) {
                        return HighwayComputationPolicy::max(expr);
                    }

                    ::pravaha::Runner<::pravaha::JThreadBackend> runner;
                    const std::size_t chunk_sz = optimal_chunk_size(total_size);
                    const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(total_size, chunk_sz);
                    std::vector<T> partial_max(chunks.size(), data[0]);

                    for (std::size_t c_idx = 0; c_idx < chunks.size(); ++c_idx) {
                        const auto& ch = chunks[c_idx];
                        auto* slot = &partial_max[c_idx];

                        auto task_fn = [data, slot, begin = ch.begin, end = ch.end]() {
                            namespace hn = hwy::HWY_NAMESPACE;
                            const hn::ScalableTag<T> d;
                            const size_t lanes = hn::Lanes(d);

                            if (end - begin < lanes) {
                                *slot = *std::max_element(data + begin, data + end);
                                return;
                            }

                            size_t i = begin;
                            auto max_vec = hn::LoadU(d, data + i);
                            i += lanes;

                            for (; i + lanes <= end; i += lanes) {
                                const auto vec = hn::LoadU(d, data + i);
                                max_vec = hn::Max(vec, max_vec);
                            }

                            T max_val = hn::ReduceMax(d, max_vec);
                            for (; i < end; ++i) {
                                if (data[i] > max_val) max_val = data[i];
                            }
                            *slot = max_val;
                        };
                        (void)runner.submit(::pravaha::task("hybrid_max_chunk", std::move(task_fn)));
                    }
                    runner.backend_ref().drain();

                    return *std::max_element(partial_max.begin(), partial_max.end());
                }
            }

            return DefaultComputationPolicy::max(expr);
        }

        template <typename E>
        static auto min(const E& expr) {
            using T = typename E::value_type;
            const auto& tensor = expr.self();
            auto shape = get_shape(tensor);
            const size_t total_size = calculate_size_dyn(shape);

            if (total_size == 0) throw std::runtime_error("Min of empty tensor");

            constexpr bool is_scalar = is_scalar_wrapper<E>::value;

            if constexpr (!is_scalar) {
                const T* data = tensor.data();
                if (data) {
                    if (total_size < k_parallel_threshold) {
                        return HighwayComputationPolicy::min(expr);
                    }

                    ::pravaha::Runner<::pravaha::JThreadBackend> runner;
                    const std::size_t chunk_sz = optimal_chunk_size(total_size);
                    const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(total_size, chunk_sz);
                    std::vector<T> partial_min(chunks.size(), data[0]);

                    for (std::size_t c_idx = 0; c_idx < chunks.size(); ++c_idx) {
                        const auto& ch = chunks[c_idx];
                        auto* slot = &partial_min[c_idx];

                        auto task_fn = [data, slot, begin = ch.begin, end = ch.end]() {
                            namespace hn = hwy::HWY_NAMESPACE;
                            const hn::ScalableTag<T> d;
                            const size_t lanes = hn::Lanes(d);

                            if (end - begin < lanes) {
                                *slot = *std::min_element(data + begin, data + end);
                                return;
                            }

                            size_t i = begin;
                            auto min_vec = hn::LoadU(d, data + i);
                            i += lanes;

                            for (; i + lanes <= end; i += lanes) {
                                const auto vec = hn::LoadU(d, data + i);
                                min_vec = hn::Min(vec, min_vec);
                            }

                            T min_val = hn::ReduceMin(d, min_vec);
                            for (; i < end; ++i) {
                                if (data[i] < min_val) min_val = data[i];
                            }
                            *slot = min_val;
                        };
                        (void)runner.submit(::pravaha::task("hybrid_min_chunk", std::move(task_fn)));
                    }
                    runner.backend_ref().drain();

                    return *std::min_element(partial_min.begin(), partial_min.end());
                }
            }

            return DefaultComputationPolicy::min(expr);
        }

        template <typename E>
        static auto variance(const E& expr) {
            using T = typename E::value_type;
            const auto avg = mean(expr);
            const auto diff = expr - avg;
            const auto sq_diff = square(diff);
            return mean(sq_diff);
        }

        template <typename E>
        static auto std_dev(const E& expr) {
            return std::sqrt(variance(expr));
        }

        template <typename E>
        static auto normalize(const E& expr) {
            const auto avg = mean(expr);
            const auto std = std_dev(expr);
            return (expr - avg) / (std + 1e-8);
        }

        // =====================================================================
        // Blocked Multi-Threaded SIMD Matrix Multiplication (dot)
        // =====================================================================

        template <typename E1, typename E2>
        static auto dot(const E1& a, const E2& b) {
            using T = typename E1::value_type;
            const auto& tensor_a = a.self();
            const auto& tensor_b = b.self();

            auto shape_a = get_shape(tensor_a);
            auto shape_b = get_shape(tensor_b);

            if (shape_a.size() == 2 && shape_b.size() == 2) {
                const size_t M = shape_a[0];
                const size_t K = shape_a[1];
                const size_t N = shape_b[1];

                if (K != shape_b[0]) {
                    throw std::invalid_argument("Matrix dimensions mismatch for dot product");
                }

                HighwayStoragePolicy::DynamicStorage<T> result_data(M * N, T{0});
                T* C = result_data.data();
                const T* A = tensor_a.data();
                const T* B = tensor_b.data();

                if (A && B) {
                    if (M * N * K < 4096) {
                        // Small matrix fallback
                        for (size_t i = 0; i < M; ++i) {
                            for (size_t k = 0; k < K; ++k) {
                                const T r = A[i * K + k];
                                for (size_t j = 0; j < N; ++j) {
                                    C[i * N + j] += r * B[k * N + j];
                                }
                            }
                        }
                        return DynamicTensor<T, HighwayStoragePolicy, HybridSimdPravahaComputationPolicy>(
                            TensorShape{M, N}, std::move(result_data));
                    }

                    // Parallel row-chunked matrix multiplication with inner SIMD accumulation
                    ::pravaha::Runner<::pravaha::JThreadBackend> runner;
                    const unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
                    const size_t chunk_rows = std::max<size_t>(1, (M + hw_threads - 1) / hw_threads);
                    const auto chunks = ::pravaha::StaticChunkingPolicy::chunks(M, chunk_rows);

                    for (const auto& ch : chunks) {
                        auto task_fn = [A, B, C, K, N, r_begin = ch.begin, r_end = ch.end]() {
                            namespace hn = hwy::HWY_NAMESPACE;
                            const hn::ScalableTag<T> d;
                            const size_t lanes = hn::Lanes(d);

                            for (size_t i = r_begin; i < r_end; ++i) {
                                for (size_t k = 0; k < K; ++k) {
                                    const T a_val = A[i * K + k];
                                    const auto va = hn::Set(d, a_val);
                                    const T* b_row = B + k * N;
                                    T* c_row = C + i * N;

                                    size_t j = 0;
                                    for (; j + lanes <= N; j += lanes) {
                                        const auto vb = hn::LoadU(d, b_row + j);
                                        const auto vc = hn::LoadU(d, c_row + j);
                                        const auto res = hn::MulAdd(va, vb, vc);
                                        hn::StoreU(res, d, c_row + j);
                                    }
                                    for (; j < N; ++j) {
                                        c_row[j] += a_val * b_row[j];
                                    }
                                }
                            }
                        };
                        (void)runner.submit(::pravaha::task("hybrid_matmul_chunk", std::move(task_fn)));
                    }
                    runner.backend_ref().drain();

                    return DynamicTensor<T, HighwayStoragePolicy, HybridSimdPravahaComputationPolicy>(
                        TensorShape{M, N}, std::move(result_data));
                }
            }

            return DefaultComputationPolicy::dot(a, b);
        }

        template <typename E>
        static auto reshape(const E& expr, const TensorShape& new_shape) {
            return DefaultComputationPolicy::reshape(expr, new_shape);
        }

        template <typename E>
        static auto flatten(const E& expr) {
            return DefaultComputationPolicy::flatten(expr);
        }

        template <typename E>
        static auto transpose(const E& expr) {
            return DefaultComputationPolicy::transpose(expr);
        }

        template <typename E1, typename E2>
        static auto power(const E1& a, const E2& b) {
            return DefaultComputationPolicy::power(a, b);
        }

        template <typename E, typename T>
        static auto clip(const E& expr, T min_val, T max_val) {
            return hybrid_unary_op(expr,
                                   [min_val, max_val]([[maybe_unused]] auto d, auto va) {
                                       namespace hn = hwy::HWY_NAMESPACE;
                                       const auto vmin = hn::Set(d, min_val);
                                       const auto vmax = hn::Set(d, max_val);
                                       return hn::Min(hn::Max(va, vmin), vmax);
                                   },
                                   [min_val, max_val](auto x) {
                                       return std::clamp(x, min_val, max_val);
                                   });
        }
    };

    using hybrid_simd_pravaha_policy = HybridSimdPravahaComputationPolicy;

    template <typename T>
    using hybrid_tensor = DynamicTensor<T, HighwayStoragePolicy, HybridSimdPravahaComputationPolicy>;

    template <typename T>
    using HybridTensor = hybrid_tensor<T>;
} // namespace ts

namespace containers::tensor {
    using ts::HybridSimdPravahaComputationPolicy;
    using ts::hybrid_simd_pravaha_policy;
    using ts::hybrid_tensor;
    using ts::HybridTensor;
}

#endif // PEBBLE_CONTAINERS_HYBRID_SIMD_PRAVAHA_POLICY_HPP
