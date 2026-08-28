#pragma once
// ============================================================================
// operators.hpp — Mathematical, Neural Network, and EDSL Operators
// ============================================================================
// C++23 / C++26, header-only AST node definitions for Tensor EDSL.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_TENSOR_EDSL_OPERATORS_HPP
#define PEBBLE_CONTAINERS_TENSOR_EDSL_OPERATORS_HPP

#include "sym_leaf.hpp"
#include "shape_inference.hpp"
#include <memory>
#include <string>
#include <vector>
#include <cmath>
#include <iostream>

namespace ts::edsl {

    enum class op_kind {
        constant,
        param_ref,
        tensor_ref,
        add,
        sub,
        mul,
        div,
        matmul,
        neg,
        abs,
        exp,
        log,
        sqrt,
        relu,
        sigmoid,
        gelu,
        softmax,
        reduce_sum,
        reduce_mean,
        reduce_max,
        transpose,
        reshape,
        fma
    };

    // ========================================================================
    // AST Expression Node Base
    // ========================================================================
    struct expr_node {
        op_kind kind;
        tensor_shape shape;
        std::vector<expr_ptr> children;
        std::string name_payload;
        float scalar_payload = 0.0f;
        int axis_payload = -1;

        expr_node(op_kind k, tensor_shape s)
            : kind(k), shape(std::move(s)), children() {}

        expr_node(op_kind k, tensor_shape s, std::vector<expr_ptr> ch)
            : kind(k), shape(std::move(s)), children(std::move(ch)) {}
    };

    // ========================================================================
    // Symbolic Expression Wrapper (`expr`)
    // ========================================================================
    class expr {
    public:
        expr_ptr node;

        expr() = default;
        explicit expr(expr_ptr n) : node(std::move(n)) {}

        // Implicit constructors from leaves & literals
        expr(float val)
            : node(std::make_shared<expr_node>(op_kind::constant, tensor_shape{})) {
            const_cast<expr_node*>(node.get())->scalar_payload = val;
        }

        expr(double val) : expr(static_cast<float>(val)) {}
        expr(int val) : expr(static_cast<float>(val)) {}

        expr(const param &p)
            : node(std::make_shared<expr_node>(op_kind::param_ref, tensor_shape{})) {
            const_cast<expr_node*>(node.get())->name_payload = p.name;
        }

        template<size_t Rank>
        expr(const sym_tensor<Rank> &st)
            : node(std::make_shared<expr_node>(op_kind::tensor_ref, st.shape)) {
            const_cast<expr_node*>(node.get())->name_payload = st.name;
        }

        template<size_t Rank>
        expr(const tensor_param<Rank> &tp)
            : node(std::make_shared<expr_node>(op_kind::tensor_ref, tp.expected_shape)) {
            const_cast<expr_node*>(node.get())->name_payload = tp.name;
        }

        // Lift concrete Tensor into Expression
        template<typename T, typename SP, typename CP>
        expr(const DynamicTensor<T, SP, CP> &t)
            : node(std::make_shared<expr_node>(op_kind::tensor_ref, t.shape())) {
            // Unnamed concrete tensor payload
        }

        [[nodiscard]] const tensor_shape& shape() const noexcept {
            return node ? node->shape : s_empty_shape;
        }

    private:
        inline static const tensor_shape s_empty_shape{};
    };

    inline param::operator expr() const {
        return expr(*this);
    }

    template<size_t Rank>
    tensor_param<Rank>::operator expr() const {
        return expr(*this);
    }

    template<size_t Rank>
    sym_tensor<Rank>::operator expr() const {
        return expr(*this);
    }

    // ========================================================================
    // Binary Arithmetic Operator Overloads
    // ========================================================================
    inline expr operator+(const expr &lhs, const expr &rhs) {
        tensor_shape s = infer_broadcast_shape(lhs.shape(), rhs.shape());
        return expr(std::make_shared<expr_node>(op_kind::add, std::move(s), std::vector{lhs.node, rhs.node}));
    }

    inline expr operator-(const expr &lhs, const expr &rhs) {
        tensor_shape s = infer_broadcast_shape(lhs.shape(), rhs.shape());
        return expr(std::make_shared<expr_node>(op_kind::sub, std::move(s), std::vector{lhs.node, rhs.node}));
    }

    inline expr operator*(const expr &lhs, const expr &rhs) {
        tensor_shape s = infer_broadcast_shape(lhs.shape(), rhs.shape());
        return expr(std::make_shared<expr_node>(op_kind::mul, std::move(s), std::vector{lhs.node, rhs.node}));
    }

    inline expr operator/(const expr &lhs, const expr &rhs) {
        tensor_shape s = infer_broadcast_shape(lhs.shape(), rhs.shape());
        return expr(std::make_shared<expr_node>(op_kind::div, std::move(s), std::vector{lhs.node, rhs.node}));
    }

    inline expr operator-(const expr &e) {
        return expr(std::make_shared<expr_node>(op_kind::neg, e.shape(), std::vector{e.node}));
    }

    // ========================================================================
    // Core Tensor & Neural Network Builders
    // ========================================================================
    inline expr matmul(const expr &a, const expr &b) {
        tensor_shape s = infer_matmul_shape(a.shape(), b.shape());
        return expr(std::make_shared<expr_node>(op_kind::matmul, std::move(s), std::vector{a.node, b.node}));
    }

    inline expr relu(const expr &e) {
        return expr(std::make_shared<expr_node>(op_kind::relu, e.shape(), std::vector{e.node}));
    }

    inline expr sigmoid(const expr &e) {
        return expr(std::make_shared<expr_node>(op_kind::sigmoid, e.shape(), std::vector{e.node}));
    }

    inline expr gelu(const expr &e) {
        return expr(std::make_shared<expr_node>(op_kind::gelu, e.shape(), std::vector{e.node}));
    }

    inline expr exp(const expr &e) {
        return expr(std::make_shared<expr_node>(op_kind::exp, e.shape(), std::vector{e.node}));
    }

    inline expr log(const expr &e) {
        return expr(std::make_shared<expr_node>(op_kind::log, e.shape(), std::vector{e.node}));
    }

    inline expr sqrt(const expr &e) {
        return expr(std::make_shared<expr_node>(op_kind::sqrt, e.shape(), std::vector{e.node}));
    }

    inline expr abs(const expr &e) {
        return expr(std::make_shared<expr_node>(op_kind::abs, e.shape(), std::vector{e.node}));
    }

    inline expr softmax(const expr &e, int axis = -1) {
        auto n = std::make_shared<expr_node>(op_kind::softmax, e.shape(), std::vector{e.node});
        n->axis_payload = axis;
        return expr(n);
    }

    inline expr reduce_sum(const expr &e, int axis = -1, bool keepdims = false) {
        tensor_shape s = infer_reduction_shape(e.shape(), axis, keepdims);
        auto n = std::make_shared<expr_node>(op_kind::reduce_sum, std::move(s), std::vector{e.node});
        n->axis_payload = axis;
        return expr(n);
    }

    inline expr reduce_mean(const expr &e, int axis = -1, bool keepdims = false) {
        tensor_shape s = infer_reduction_shape(e.shape(), axis, keepdims);
        auto n = std::make_shared<expr_node>(op_kind::reduce_mean, std::move(s), std::vector{e.node});
        n->axis_payload = axis;
        return expr(n);
    }

    inline expr reduce_max(const expr &e, int axis = -1, bool keepdims = false) {
        tensor_shape s = infer_reduction_shape(e.shape(), axis, keepdims);
        auto n = std::make_shared<expr_node>(op_kind::reduce_max, std::move(s), std::vector{e.node});
        n->axis_payload = axis;
        return expr(n);
    }

    inline expr transpose(const expr &e) {
        tensor_shape s = infer_transpose_shape(e.shape());
        return expr(std::make_shared<expr_node>(op_kind::transpose, std::move(s), std::vector{e.node}));
    }

    inline expr reshape(const expr &e, tensor_shape target_shape) {
        return expr(std::make_shared<expr_node>(op_kind::reshape, std::move(target_shape), std::vector{e.node}));
    }

    // ========================================================================
    // Converting helper to convert any leaf to expr
    // ========================================================================
    template<typename T>
    inline expr to_expr(const T &val) {
        if constexpr (std::is_same_v<T, expr>) {
            return val;
        } else if constexpr (requires { expr(val); }) {
            return expr(val);
        } else {
            return static_cast<expr>(val);
        }
    }

    // ========================================================================
    // Free Function Overloads taking Leaf types directly
    // ========================================================================
    template<typename A, typename B>
        requires (!std::is_same_v<std::decay_t<A>, expr> || !std::is_same_v<std::decay_t<B>, expr>)
    inline expr matmul(const A &a, const B &b) {
        return matmul(to_expr(a), to_expr(b));
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr relu(const T &e) {
        return relu(to_expr(e));
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr sigmoid(const T &e) {
        return sigmoid(to_expr(e));
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr gelu(const T &e) {
        return gelu(to_expr(e));
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr exp(const T &e) {
        return exp(to_expr(e));
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr log(const T &e) {
        return log(to_expr(e));
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr sqrt(const T &e) {
        return sqrt(to_expr(e));
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr abs(const T &e) {
        return abs(to_expr(e));
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr softmax(const T &e, int axis = -1) {
        return softmax(to_expr(e), axis);
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr reduce_sum(const T &e, int axis = -1, bool keepdims = false) {
        return reduce_sum(to_expr(e), axis, keepdims);
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr reduce_mean(const T &e, int axis = -1, bool keepdims = false) {
        return reduce_mean(to_expr(e), axis, keepdims);
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr reduce_max(const T &e, int axis = -1, bool keepdims = false) {
        return reduce_max(to_expr(e), axis, keepdims);
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr transpose(const T &e) {
        return transpose(to_expr(e));
    }

    template<typename T>
        requires (!std::is_same_v<std::decay_t<T>, expr>)
    inline expr reshape(const T &e, tensor_shape target_shape) {
        return reshape(to_expr(e), std::move(target_shape));
    }

    // Leaf arithmetic overloads
    template<typename A, typename B>
        requires ((requires { to_expr(std::declval<A>()); } && requires { to_expr(std::declval<B>()); }) &&
                 (!std::is_same_v<std::decay_t<A>, expr> || !std::is_same_v<std::decay_t<B>, expr>) &&
                 (std::is_same_v<std::decay_t<A>, expr> || std::is_same_v<std::decay_t<B>, expr> ||
                  requires { std::declval<A>().name; } || requires { std::declval<B>().name; }))
    inline expr operator+(const A &a, const B &b) {
        return to_expr(a) + to_expr(b);
    }

    template<typename A, typename B>
        requires ((requires { to_expr(std::declval<A>()); } && requires { to_expr(std::declval<B>()); }) &&
                 (!std::is_same_v<std::decay_t<A>, expr> || !std::is_same_v<std::decay_t<B>, expr>) &&
                 (std::is_same_v<std::decay_t<A>, expr> || std::is_same_v<std::decay_t<B>, expr> ||
                  requires { std::declval<A>().name; } || requires { std::declval<B>().name; }))
    inline expr operator-(const A &a, const B &b) {
        return to_expr(a) - to_expr(b);
    }

    template<typename A, typename B>
        requires ((requires { to_expr(std::declval<A>()); } && requires { to_expr(std::declval<B>()); }) &&
                 (!std::is_same_v<std::decay_t<A>, expr> || !std::is_same_v<std::decay_t<B>, expr>) &&
                 (std::is_same_v<std::decay_t<A>, expr> || std::is_same_v<std::decay_t<B>, expr> ||
                  requires { std::declval<A>().name; } || requires { std::declval<B>().name; }))
    inline expr operator*(const A &a, const B &b) {
        return to_expr(a) * to_expr(b);
    }

    template<typename A, typename B>
        requires ((requires { to_expr(std::declval<A>()); } && requires { to_expr(std::declval<B>()); }) &&
                 (!std::is_same_v<std::decay_t<A>, expr> || !std::is_same_v<std::decay_t<B>, expr>) &&
                 (std::is_same_v<std::decay_t<A>, expr> || std::is_same_v<std::decay_t<B>, expr> ||
                  requires { std::declval<A>().name; } || requires { std::declval<B>().name; }))
    inline expr operator/(const A &a, const B &b) {
        return to_expr(a) / to_expr(b);
    }

} // namespace ts::edsl

#endif // PEBBLE_CONTAINERS_TENSOR_EDSL_OPERATORS_HPP
