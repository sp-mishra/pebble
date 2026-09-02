#pragma once
// ============================================================================
// MLXStoragePolicy.hpp — Apple Silicon MLX GPU Storage Policy for Tensor
// ============================================================================
// C++23 / C++26, header-only, zero virtual dispatch.
// Adapts mlx::core::array into Tensor storage policy architecture.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MLX_STORAGE_POLICY_HPP
#define PEBBLE_CONTAINERS_MLX_STORAGE_POLICY_HPP

#include <containers/tensor/tensor.hpp>

#if __has_include(<mlx/mlx.h>)
#include <mlx/mlx.h>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace ts {
    template <typename T>
    struct MlxDtype {
        static_assert(always_false<T>, "This C++ type is not supported by MlxPolicy.");
    };

    template <>
    struct MlxDtype<float> {
        static constexpr mlx::core::Dtype value = mlx::core::float32;
    };

    template <>
    struct MlxDtype<int> {
        static constexpr mlx::core::Dtype value = mlx::core::int32;
    };

    template <>
    struct MlxDtype<unsigned int> {
        static constexpr mlx::core::Dtype value = mlx::core::uint32;
    };

    template <>
    struct MlxDtype<double> {
        static constexpr mlx::core::Dtype value = mlx::core::float32;
    };

    template <>
    struct MlxDtype<bool> {
        static constexpr mlx::core::Dtype value = mlx::core::bool_;
    };

    // Generic MlxStorage<T>
    template <typename T>
    class MlxStorage {
    public:
        using value_type = T;

        MlxStorage()
            : array_(mlx::core::full(mlx::core::Shape{0}, T{0}, MlxDtype<T>::value)) {
            array_.eval();
        }

        template <typename InputIt>
        MlxStorage(InputIt first, InputIt last)
            : array_([&] {
                const size_t size = std::distance(first, last);
                std::vector<T> tmp(first, last);
                mlx::core::Shape shape_vec = {static_cast<int>(size)};
                if constexpr (std::is_same_v<T, bool>) {
                    std::vector<bool> tmp_bool(first, last);
                    if (size == 0) {
                        return mlx::core::full(shape_vec, false, mlx::core::bool_);
                    }
                    std::vector<uint8_t> tmp_uint8;
                    tmp_uint8.reserve(size);
                    for (const bool val : tmp_bool) tmp_uint8.push_back(val ? 1 : 0);
                    return mlx::core::array(tmp_uint8.data(), shape_vec, mlx::core::bool_);
                }
                else {
                    if (size == 0) {
                        return mlx::core::full(shape_vec, T{0}, MlxDtype<T>::value);
                    }
                    return mlx::core::array(tmp.data(), shape_vec, MlxDtype<T>::value);
                }
            }()) {
            array_.eval();
        }

        explicit MlxStorage(const size_t size)
            : array_(mlx::core::full(mlx::core::Shape{static_cast<int>(size)}, T{0},
                                     MlxDtype<T>::value)) {
            array_.eval();
        }

        explicit MlxStorage(const mlx::core::array& arr) : array_(arr) {
            array_.eval();
        }

        explicit MlxStorage(mlx::core::array&& arr) : array_(std::move(arr)) {
            array_.eval();
        }

        [[nodiscard]] size_t size() const { return array_.size(); }

        T* data() {
            if (array_.size() == 0) return nullptr;
            if constexpr (std::is_same_v<T, bool>) {
                if (array_.dtype() != mlx::core::bool_)
                    throw std::runtime_error("MLX array dtype does not match bool");
                return nullptr;
            }
            else {
                if (array_.dtype() != MlxDtype<T>::value)
                    throw std::runtime_error("MLX array dtype does not match T");
                return array_.data<T>();
            }
        }

        const T* data() const {
            if (array_.size() == 0) return nullptr;
            if constexpr (std::is_same_v<T, bool>) {
                if (array_.dtype() != mlx::core::bool_)
                    throw std::runtime_error("MLX array dtype does not match bool");
                return nullptr;
            }
            else {
                if (array_.dtype() != MlxDtype<T>::value)
                    throw std::runtime_error("MLX array dtype does not match T");
                return array_.data<T>();
            }
        }

        T& operator[](size_t i) {
            if constexpr (std::is_same_v<T, bool>) {
                throw std::runtime_error("Direct indexing not supported for MLX bool arrays");
            }
            else {
                T* ptr = data();
                if (!ptr) throw std::runtime_error("Attempt to access data of empty MLX array");
                return ptr[i];
            }
        }

        T operator[](size_t i) const {
            if constexpr (std::is_same_v<T, bool>) {
                auto indexed = mlx::core::take(array_, mlx::core::array({static_cast<int>(i)}));
                indexed.eval();
                return static_cast<bool>(indexed.template item<uint8_t>());
            }
            else {
                const T* ptr = data();
                if (!ptr) throw std::runtime_error("Attempt to access data of empty MLX array");
                return ptr[i];
            }
        }

        T get_value(size_t i) const {
            if constexpr (std::is_same_v<T, bool>) {
                auto indexed = mlx::core::take(array_, mlx::core::array({static_cast<int>(i)}));
                indexed.eval();
                return static_cast<bool>(indexed.template item<uint8_t>());
            }
            else {
                const T* ptr = data();
                if (!ptr) throw std::runtime_error("Attempt to access data of empty MLX array");
                return ptr[i];
            }
        }

        mlx::core::array& get() { return array_; }
        [[nodiscard]] const mlx::core::array& get() const { return array_; }

        void debug_print(std::ostream& os = std::cout) const {
            os << array_ << "\n";
        }

        std::vector<T> to_cpu() const {
            const T* ptr = data();
            return ptr ? std::vector<T>(ptr, ptr + size()) : std::vector<T>();
        }

    private:
        mlx::core::array array_;
    };

    // Specialization for double
    template <>
    class MlxStorage<double> {
    public:
        using value_type = double;

        MlxStorage()
            : array_(mlx::core::full(mlx::core::Shape{0}, 0.0f, mlx::core::float32)) {
            array_.eval();
        }

        template <typename InputIt>
        MlxStorage(InputIt first, InputIt last)
            : array_([&] {
                const size_t size = std::distance(first, last);
                std::vector<float> tmp;
                tmp.reserve(size);
                for (auto it = first; it != last; ++it) tmp.push_back(static_cast<float>(*it));
                const mlx::core::Shape shape_vec = {static_cast<int>(size)};
                auto arr = mlx::core::array(tmp.data(), shape_vec, mlx::core::float32);
                arr.eval();
                return arr;
            }()) {}

        explicit MlxStorage(const size_t size)
            : array_(mlx::core::full(mlx::core::Shape{static_cast<int>(size)}, 0.0f, mlx::core::float32)) {
            array_.eval();
        }

        explicit MlxStorage(const mlx::core::array& arr) : array_(arr) {
            array_.eval();
        }

        explicit MlxStorage(mlx::core::array&& arr) : array_(std::move(arr)) {
            array_.eval();
        }

        [[nodiscard]] size_t size() const { return array_.size(); }

        double* data() {
            throw std::runtime_error("Direct double* access not supported for MLX double emulation.");
        }

        const double* data() const {
            throw std::runtime_error("Direct double* access not supported for MLX double emulation.");
        }

        double operator[](const size_t i) const {
            if (array_.size() == 0) throw std::runtime_error("Attempt to access data of empty MLX array");
            if (array_.dtype() != mlx::core::float32)
                throw std::runtime_error("MLX array dtype does not match float32");
            return static_cast<double>(array_.data<float>()[i]);
        }

        class DoubleProxy {
            float* ptr;

        public:
            explicit DoubleProxy(float* p) : ptr(p) {}
            operator double() const { return static_cast<double>(*ptr); }

            DoubleProxy& operator=(double v) {
                *ptr = static_cast<float>(v);
                return *this;
            }
        };

        DoubleProxy operator[](const size_t i) {
            if (array_.size() == 0) throw std::runtime_error("Attempt to access data of empty MLX array");
            if (array_.dtype() != mlx::core::float32)
                throw std::runtime_error("MLX array dtype does not match float32");
            return DoubleProxy(&array_.data<float>()[i]);
        }

        void set(const size_t i, const double v) {
            if (array_.size() == 0) throw std::runtime_error("Attempt to access data of empty MLX array");
            if (array_.dtype() != mlx::core::float32)
                throw std::runtime_error("MLX array dtype does not match float32");
            array_.data<float>()[i] = static_cast<float>(v);
        }

        mlx::core::array& get() { return array_; }
        [[nodiscard]] const mlx::core::array& get() const { return array_; }

        void debug_print(std::ostream& os = std::cout) const {
            os << array_ << "\n";
        }

        [[nodiscard]] std::vector<double> to_cpu() const {
            if (array_.size() == 0) return {};
            const auto* ptr = array_.data<float>();
            std::vector<double> result(array_.size());
            for (size_t i = 0; i < array_.size(); ++i) result[i] = static_cast<double>(ptr[i]);
            return result;
        }

    private:
        mlx::core::array array_;
    };

    struct MlxStoragePolicy {
        template <typename T>
        using DynamicStorage = MlxStorage<T>;

        template <typename T, size_t Size>
        using StaticStorage = MlxStorage<T>;

        using StringStorage = ArrowStringStorage;
    };

    using mlx_storage_policy = MlxStoragePolicy;
    template <typename T>
    using mlx_storage = MlxStorage<T>;
} // namespace ts

using MlxPolicy = ts::MlxStoragePolicy;
using mlx_storage_policy = ts::mlx_storage_policy;

namespace containers::tensor {
    using ts::MlxStoragePolicy;
    using ts::MlxStorage;
    using ts::mlx_storage_policy;
    using ts::mlx_storage;
}

#endif // __has_include(<mlx/mlx.h>)

#endif // PEBBLE_CONTAINERS_MLX_STORAGE_POLICY_HPP
