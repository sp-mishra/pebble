#pragma once
// ============================================================================
// TensorUtils.hpp — Pretty Printing & Debug Utilities for Tensor
// ============================================================================
// C++23 / C++26, header-only, zero virtual dispatch.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_TENSOR_UTILS_HPP
#define PEBBLE_CONTAINERS_TENSOR_UTILS_HPP

#include <containers/tensor/tensor.hpp>
#include <iostream>
#include <string>
#include <vector>

namespace ts {

    template<typename E, typename T, typename S, typename C>
    void print_tensor(const TensorExpression<E, T, S, C> &expr, const size_t indent = 0) {
        const auto &tensor = expr.self();
        auto shape = get_shape(tensor);
        if (shape.empty()) {
            std::cout << tensor(std::vector<size_t>{}) << std::endl;
            return;
        }
        std::vector<size_t> idx(shape.size(), 0);
        auto print_rec = [&](auto &self, size_t dim) -> void {
            if (dim == shape.size() - 1) {
                std::cout << std::string(indent, ' ') << "[";
                for (size_t i = 0; i < shape[dim]; ++i) {
                    idx[dim] = i;
                    std::cout << tensor(idx);
                    if (i + 1 < shape[dim]) std::cout << ", ";
                }
                std::cout << "]";
            } else {
                std::cout << std::string(indent, ' ') << "[\n";
                for (size_t i = 0; i < shape[dim]; ++i) {
                    idx[dim] = i;
                    self(self, dim + 1);
                    if (i + 1 < shape[dim]) std::cout << ",\n";
                }
                std::cout << "\n" << std::string(indent, ' ') << "]";
            }
        };
        print_rec(print_rec, 0);
        std::cout << std::endl;
    }

    template<typename T>
        requires std::is_arithmetic_v<T>
    void print_tensor(const T &scalar) {
        std::cout << scalar << std::endl;
    }

    template<typename E, typename T, typename S, typename C>
    [[nodiscard]] std::string tensor_to_string(const TensorExpression<E, T, S, C> &expr) {
        const auto &tensor = expr.self();
        auto shape = get_shape(tensor);
        if (shape.empty()) {
            return std::to_string(tensor(std::vector<size_t>{}));
        }
        std::string result;
        std::vector<size_t> idx(shape.size(), 0);
        auto format_rec = [&](auto &self, size_t dim) -> void {
            if (dim == shape.size() - 1) {
                result += "[";
                for (size_t i = 0; i < shape[dim]; ++i) {
                    idx[dim] = i;
                    result += std::to_string(tensor(idx));
                    if (i + 1 < shape[dim]) result += ", ";
                }
                result += "]";
            } else {
                result += "[";
                for (size_t i = 0; i < shape[dim]; ++i) {
                    idx[dim] = i;
                    self(self, dim + 1);
                    if (i + 1 < shape[dim]) result += ", ";
                }
                result += "]";
            }
        };
        format_rec(format_rec, 0);
        return result;
    }

} // namespace ts

namespace containers::tensor {
    using ts::print_tensor;
    using ts::tensor_to_string;
}

#endif // PEBBLE_CONTAINERS_TENSOR_UTILS_HPP
