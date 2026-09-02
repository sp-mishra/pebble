#pragma once
// ============================================================================
// sym_leaf.hpp — Symbolic Leaves, Parameter Literals, and Binding Types
// ============================================================================
// C++23 / C++26, header-only, zero-overhead metadata carriers for Tensor EDSL.
// Inspired by Sūtra & Vākya expression designs.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_TENSOR_EDSL_SYM_LEAF_HPP
#define PEBBLE_CONTAINERS_TENSOR_EDSL_SYM_LEAF_HPP

#include <containers/tensor/tensor.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_map>
#include <memory>
#include <any>
#include <optional>
#include <stdexcept>

namespace ts::edsl {
    // Forward declarations
    struct expr_node;
    using expr_ptr = std::shared_ptr<const expr_node>;
    class expr;

    // ========================================================================
    // 1. Parameter / Variable Identity Key
    // ========================================================================
    struct param_key {
        std::string name;

        constexpr explicit param_key(std::string_view n) : name(n) {}

        bool operator==(const param_key& other) const noexcept {
            return name == other.name;
        }
    };
} // namespace ts::edsl

template <>
struct std::hash<ts::edsl::param_key> {
    std::size_t operator()(const ts::edsl::param_key& k) const noexcept {
        return std::hash<std::string>{}(k.name);
    }
};

namespace ts::edsl {
    // ========================================================================
    // 2. Binding Pair (e.g. "param"_p = 3.14f or "weight"_t = tensor_w)
    // ========================================================================
    template <typename T>
    struct binding {
        std::string name;
        T value;

        binding(std::string_view n, T val) : name(n), value(std::move(val)) {}
    };

    // Forward declaration of expr
    class expr;

    // ========================================================================
    // 3. Scalar Parameter Handle
    // ========================================================================
    struct param {
        std::string name;

        explicit constexpr param(std::string_view n) : name(n) {}

        template <typename ValT>
        auto operator=(ValT&& val) const {
            return binding<std::decay_t<ValT>>(name, std::forward<ValT>(val));
        }

        operator expr() const;
    };

    // ========================================================================
    // 4. Tensor Parameter Handle
    // ========================================================================
    template <size_t Rank = 0>
    struct tensor_param {
        std::string name;
        tensor_shape expected_shape;

        explicit tensor_param(std::string_view n, tensor_shape shape = {})
            : name(n), expected_shape(std::move(shape)) {}

        template <typename TensorT>
        auto operator=(TensorT&& tensor_val) const {
            return binding<std::decay_t<TensorT>>(name, std::forward<TensorT>(tensor_val));
        }

        operator expr() const;
    };

    // ========================================================================
    // 5. User-Defined Literals ("_p" and "_t")
    // ========================================================================
    namespace literals {
        inline param operator""_p(const char* str, std::size_t len) {
            return param(std::string_view(str, len));
        }

        inline tensor_param<0> operator""_t(const char* str, std::size_t len) {
            return tensor_param<0>(std::string_view(str, len));
        }
    } // namespace literals

    using namespace literals;

    // ========================================================================
    // 6. Symbolic Tensor Leaf Carrier (sym_tensor<Rank>)
    // ========================================================================
    template <size_t Rank = 0>
    class sym_tensor {
    public:
        std::string name;
        tensor_shape shape;

        explicit sym_tensor(std::string_view n, tensor_shape s = {})
            : name(n), shape(std::move(s)) {
            if constexpr (Rank > 0) {
                if (!shape.empty() && shape.size() != Rank) {
                    throw std::invalid_argument("sym_tensor rank mismatch with provided shape");
                }
            }
        }

        [[nodiscard]] size_t rank() const noexcept {
            return Rank > 0 ? Rank : shape.size();
        }

        template <typename TensorT>
        auto operator=(TensorT&& tensor_val) const {
            return binding<std::decay_t<TensorT>>(name, std::forward<TensorT>(tensor_val));
        }

        operator expr() const;
    };

    // Deduction guide
    sym_tensor(std::string_view, tensor_shape) -> sym_tensor<0>;
} // namespace ts::edsl

#endif // PEBBLE_CONTAINERS_TENSOR_EDSL_SYM_LEAF_HPP
