#pragma once

// vakya/vakya.hpp — Vākya: generic structural construction library (EDSL core).
//
// Extracted from lithe_core.hpp. Owns expression-template construction, tags,
// tag metadata, structural hashing/equality, traversal, tree folds, and the
// shared-DAG carrier. Knows NOTHING about optimization, passes, IR, backends,
// or codegen. Reusable independently of Lithe.
//
// Design: header-only, C++23, no virtual, no macros, pay-for-what-you-use.
// Namespace: vakya (with vakya::emit, vakya::tree, vakya::graph, vakya::ir,
// vakya::lang).
//
// Phase wrappers (surface/canonical/optimized/lowered_expr) are a COMPILER
// concept and live in Lithe (edsl/lithe_core.hpp), NOT here. Vākya exposes a
// single ADL customization point — structural_unwrap(x) — so Lithe can teach
// structural_equal to see through phase wrappers without Vākya naming them.

#include <tuple>
#include <utility>
#include <type_traits>
#include <variant>
#include <string>
#include <string_view>
#include <cstdint>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <algorithm>
#include <any>
#include <typeindex>
#include <ranges>

namespace vakya {
    // -----------------------------
    // 1) Tags (operations)
    // -----------------------------
    struct add_tag {};

    struct sub_tag {}; // Binary subtraction
    struct mul_tag {};

    struct div_tag {}; // Binary division
    struct mod_tag {}; // Binary modulo
    struct neg_tag {}; // Unary minus tag
    struct subscript_tag {}; // Multidimensional subscript tag

    // Comparison operators
    struct eq_tag {}; // Equality ==
    struct ne_tag {}; // Not equal !=
    struct lt_tag {}; // Less than <
    struct le_tag {}; // Less than or equal <=
    struct gt_tag {}; // Greater than >
    struct ge_tag {}; // Greater than or equal >=

    // Logical operators
    struct and_tag {}; // Logical AND &&
    struct or_tag {}; // Logical OR ||
    struct not_tag {}; // Logical NOT !

    // Bitwise operators
    struct bit_and_tag {}; // Bitwise AND &
    struct bit_or_tag {}; // Bitwise OR |
    struct bit_xor_tag {}; // Bitwise XOR ^
    struct bit_not_tag {}; // Bitwise NOT ~
    struct shl_tag {}; // Left shift <<
    struct shr_tag {}; // Right shift >>

    // Control flow constructs
    struct if_tag {}; // Conditional if
    struct while_tag {}; // While loop
    struct for_tag {}; // For loop
    struct let_tag {}; // Variable binding
    struct seq_tag {}; // Sequence of statements
    struct call_tag {}; // Function call

    // Type system tags
    struct cast_tag {}; // Type casting
    struct sizeof_tag {}; // Size operator

    // Memory access
    struct deref_tag {}; // Pointer dereference
    struct addr_tag {}; // Address-of operator

    // Lambda and function types
    struct lambda_tag {}; // Lambda expression
    struct return_tag {}; // Return statement

    // Runtime value boxing / unboxing (bridge between JIT registers and runtime_value)
    struct box_tag {}; // lift a register value into runtime_value
    struct unbox_tag {}; // extract a typed value from runtime_value

    // Aggregate / OO memory operations (MIR Phase 2)
    struct get_element_ptr_tag {}; // GEP: compute field/element address without dereferencing
    struct extract_value_tag {}; // By-value read of an aggregate field/element
    struct insert_value_tag {}; // By-value write into an aggregate, producing a new aggregate
    struct indirect_call_tag {}; // Virtual / function-pointer dispatch (vtable call)

    // -----------------------------
    // 2) Forward declarations
    // -----------------------------
    template <class Tag, class... Children>
    struct node;

    template <class Tag, class... Args>
    [[nodiscard]] constexpr auto make_node(Args&&... args);

    // -----------------------------
    // 3) Capture policy
    //    - lvalues => reference
    //    - rvalues => decay by value
    // -----------------------------
    template <class T>
    using capture_t = std::conditional_t<
        std::is_lvalue_reference_v<T>,
        T,
        std::decay_t<T>
    >;

    // -----------------------------
    // 4) Expression concept
    // -----------------------------
    template <class T>
    concept Expression = requires {
        // must expose the marker and tag_type (only real node<> types provide these)
        typename std::decay_t<T>::is_lithe_node;
        typename std::decay_t<T>::tag_type;
        // must have a 'children' member when accessed on an lvalue of the decayed type
        { std::declval<std::decay_t<T>&>().children };
    };

    // Variant expression concept: detects std::variant-like types
    // Robust detection of std::variant specializations:
    template <class T>
    struct is_std_variant : std::false_type {};

    template <class... Ts>
    struct is_std_variant<std::variant<Ts...>> : std::true_type {};

    template <class T>
    inline constexpr bool is_std_variant_v = is_std_variant<std::decay_t<T>>::value;

    template <class T>
    concept VariantExpr = is_std_variant_v<T>;

    // -----------------------------
    // 4a) Terminal trait + concept-based detection hook.
    //
    //  Priority order (first match wins):
    //    1. T has `using vakya_terminal = void;` member  (zero-specialization opt-in)
    //    2. Explicit is_terminal<T> specialization       (legacy opt-in)
    //    3. Default: arithmetic types
    // -----------------------------

    // Detection: T declares `using vakya_terminal = void;`
    template <class T>
    concept has_vakya_terminal_tag = requires { typename std::decay_t<T>::vakya_terminal; };

    template <class T>
    struct is_terminal : std::bool_constant<
            has_vakya_terminal_tag<T> || std::is_arithmetic_v<std::decay_t<T>>> {};

    template <class T>
    inline constexpr bool is_terminal_v = is_terminal<std::decay_t<T>>::value;

    template <class T>
    concept Terminal = is_terminal_v<T>;

    // Operand remains expression or terminal
    template <class T>
    concept Operand = Expression<T> || Terminal<T>;

    // Local structural string type so .as<"alias">() works in C++20/23 NTTP form.
    template <std::size_t N>
    struct alias_string {
        char data[N]{};

        consteval alias_string(const char (&src)[N]) noexcept {
            for (std::size_t i = 0; i < N; ++i) data[i] = src[i];
        }

        constexpr bool operator==(const alias_string&) const noexcept = default;
    };

    // -----------------------------
    // 5) Syntactic interface (member operators)
    //    Inject operators via explicit object parameter (C++23)
    // -----------------------------
    template <class Derived>
    struct interface {
        // Arithmetic operators
        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator+(this Self&& self, R&& rhs) {
            return make_node<add_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator-(this Self&& self, R&& rhs) {
            return make_node<sub_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator*(this Self&& self, R&& rhs) {
            return make_node<mul_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator/(this Self&& self, R&& rhs) {
            return make_node<div_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator%(this Self&& self, R&& rhs) {
            return make_node<mod_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        // Unary operators
        template <class Self>
        constexpr auto operator-(this Self&& self) {
            return make_node<neg_tag>(std::forward<Self>(self));
        }

        template <class Self>
        constexpr auto operator!(this Self&& self) {
            return make_node<not_tag>(std::forward<Self>(self));
        }

        template <class Self>
        constexpr auto operator~(this Self&& self) {
            return make_node<bit_not_tag>(std::forward<Self>(self));
        }

        // Comparison operators
        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator==(this Self&& self, R&& rhs) {
            return make_node<eq_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator!=(this Self&& self, R&& rhs) {
            return make_node<ne_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator<(this Self&& self, R&& rhs) {
            return make_node<lt_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator<=(this Self&& self, R&& rhs) {
            return make_node<le_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator>(this Self&& self, R&& rhs) {
            return make_node<gt_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator>=(this Self&& self, R&& rhs) {
            return make_node<ge_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        // Logical operators
        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator&&(this Self&& self, R&& rhs) {
            return make_node<and_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator||(this Self&& self, R&& rhs) {
            return make_node<or_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        // Bitwise operators
        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator&(this Self&& self, R&& rhs) {
            return make_node<bit_and_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator|(this Self&& self, R&& rhs) {
            return make_node<bit_or_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator^(this Self&& self, R&& rhs) {
            return make_node<bit_xor_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator<<(this Self&& self, R&& rhs) {
            return make_node<shl_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        template <class Self, class R>
            requires Operand<R>
        constexpr auto operator>>(this Self&& self, R&& rhs) {
            return make_node<shr_tag>(std::forward<Self>(self), std::forward<R>(rhs));
        }

        // Multidimensional subscript
        template <class Self, class... I>
            requires ((Operand<I>) && ...)
        constexpr auto operator[](this Self&& self, I&&... idx) {
            return make_node<subscript_tag>(std::forward<Self>(self), std::forward<I>(idx)...);
        }

        template <auto Alias>
        [[nodiscard]] constexpr decltype(auto) as() & noexcept {
            return *static_cast<Derived*>(this);
        }

        template <alias_string Alias>
        [[nodiscard]] constexpr decltype(auto) as() & noexcept {
            return *static_cast<Derived*>(this);
        }

        template <auto Alias>
        [[nodiscard]] constexpr decltype(auto) as() const& noexcept {
            return *static_cast<const Derived*>(this);
        }

        template <alias_string Alias>
        [[nodiscard]] constexpr decltype(auto) as() const& noexcept {
            return *static_cast<const Derived*>(this);
        }

        template <auto Alias>
        [[nodiscard]] constexpr decltype(auto) as() && noexcept {
            return std::move(*static_cast<Derived*>(this));
        }

        template <alias_string Alias>
        [[nodiscard]] constexpr decltype(auto) as() && noexcept {
            return std::move(*static_cast<Derived*>(this));
        }

        template <auto Alias>
        [[nodiscard]] constexpr decltype(auto) as() const&& noexcept {
            return std::move(*static_cast<const Derived*>(this));
        }

        template <alias_string Alias>
        [[nodiscard]] constexpr decltype(auto) as() const&& noexcept {
            return std::move(*static_cast<const Derived*>(this));
        }
    };

    // -----------------------------
    // 5c) Expression wrapper helpers (forward declarations)
    // -----------------------------
    template <class T>
    struct expr;
    template <class T>
    struct expr_ref;

    template <class T>
    [[nodiscard]] constexpr auto as_expr(T& x) -> expr_ref<T>;

    template <class T>
    [[nodiscard]] constexpr auto as_expr(T&& x) -> expr<std::decay_t<T>>;

    // -----------------------------
    // 5b) Free operators for plain terminals
    // -----------------------------
    template <class L, class R>
    concept has_member_plus = requires(L&& l, R&& r) {
        std::forward<L>(l).operator+(std::forward<R>(r));
    };

    template <class L, class R>
    concept has_member_mul = requires(L&& l, R&& r) {
        std::forward<L>(l).operator*(std::forward<R>(r));
    };

    // Free operator+ / operator* overloads.
    template <class L, class R>
        requires Expression<std::remove_cvref_t<L>> && Operand<R> && (!has_member_plus<L, R>)
    constexpr auto operator+(L&& l, R&& r) {
        return make_node<add_tag>(std::forward<L>(l), std::forward<R>(r));
    }

    template <class L, class R>
        requires Expression<std::remove_cvref_t<L>> && Operand<R> && (!has_member_mul<L, R>)
    constexpr auto operator*(L&& l, R&& r) {
        return make_node<mul_tag>(std::forward<L>(l), std::forward<R>(r));
    }

    template <class L, class R>
        requires (!Expression<std::remove_cvref_t<L>>) && Terminal<std::remove_cvref_t<L>> && Operand<R> && (!
            has_member_plus<L, R>)
    constexpr auto operator+(L&& l, R&& r) {
        return make_node<add_tag>(as_expr(std::forward<L>(l)), std::forward<R>(r));
    }

    template <class L, class R>
        requires (!Expression<std::remove_cvref_t<L>>) && Terminal<std::remove_cvref_t<L>> && Operand<R> && (!
            has_member_mul<L, R>)
    constexpr auto operator*(L&& l, R&& r) {
        return make_node<mul_tag>(as_expr(std::forward<L>(l)), std::forward<R>(r));
    }

    // -----------------------------
    // 5c) Expression wrapper helpers
    // -----------------------------
    template <class T>
    struct expr : interface<expr<T>> {
        T value;

        constexpr explicit expr(T v) : value(std::move(v)) {}

        constexpr operator const T&() const noexcept { return value; }

        template <auto Alias>
        [[nodiscard]] constexpr auto as() const noexcept {
            return *this;
        }

        template <alias_string Alias>
        [[nodiscard]] constexpr auto as() const noexcept {
            return *this;
        }
    };

    template <class T>
    struct expr_ref : interface<expr_ref<T>> {
        T* p;

        constexpr explicit expr_ref(T* pp) : p(pp) {}

        constexpr operator T*() const noexcept { return p; }
    };

    // mark wrappers as terminals
    template <class T>
    struct is_terminal<expr<T>> : std::true_type {};

    template <class T>
    struct is_terminal<expr_ref<T>> : std::true_type {};

    // Type-identity traits for structural_equal dispatch.
    template <class T>
    struct is_expr_wrapper : std::false_type {};

    template <class T>
    struct is_expr_wrapper<expr<T>> : std::true_type {};

    template <class T>
    inline constexpr bool is_expr_wrapper_v = is_expr_wrapper<std::decay_t<T>>::value;

    template <class T>
    struct is_expr_ref_wrapper : std::false_type {};

    template <class T>
    struct is_expr_ref_wrapper<expr_ref<T>> : std::true_type {};

    template <class T>
    inline constexpr bool is_expr_ref_wrapper_v = is_expr_ref_wrapper<std::decay_t<T>>::value;

    // as_expr helpers: lvalues -> expr_ref, rvalues -> expr (by value)
    template <class T>
    [[nodiscard]] constexpr auto as_expr(T& x) -> expr_ref<T> { return expr_ref<T>{&x}; }

    template <class T>
    [[nodiscard]] constexpr auto as_expr(T&& x) -> expr<std::decay_t<T>> {
        return expr<std::decay_t<T>>{std::forward<T>(x)};
    }

    // -----------------------------
    // 5d) structural_unwrap — ADL customization point.
    //   Vākya's structural hashing/equality call structural_unwrap(x) before
    //   inspecting a value. Default: identity (returns the value unchanged).
    //   Lithe overloads this for its phase wrappers (surface/canonical/…)
    //   so those unwrap to .value transparently — without Vākya naming them.
    // -----------------------------
    template <class T>
    [[nodiscard]] constexpr decltype(auto) structural_unwrap(T&& x) noexcept {
        return std::forward<T>(x);
    }

    // -----------------------------
    // 6) Node (flattened AST)
    // -----------------------------
    template <class Tag, class... Children>
    struct node : interface<node<Tag, Children...>> {
        using is_lithe_node = void; // marker to identify real AST node types
        using tag_type = Tag; // expose tag type

        std::tuple<Children...> children;

        constexpr node(const node&) = default;

        constexpr node(node&&) = default;

        node& operator=(const node&) = default;

        node& operator=(node&&) = default;

        template <class... Args>
            requires (sizeof...(Args) == sizeof...(Children) &&
                (sizeof...(Args) == 0 || !std::conjunction_v < std::is_same<std::decay_t<Args>, node>



        ...
        >
        )
        )
        constexpr explicit node(Args&&... args)
            : children(std::forward<Args>(args)...) {}
    };

    // Factory
    template <class Tag, class... Args>
    [[nodiscard]] constexpr auto make_node(Args&&... args) {
        return node<Tag, Args...>(std::forward<Args>(args)...);
    }

    // Lightweight explicit AST builder (no state, zero-overhead wrapper).
    struct IRBuilder {
        template <class A, class B>
        [[nodiscard]] constexpr auto CreateAdd(A&& a, B&& b) const {
            return make_node<add_tag>(std::forward<A>(a), std::forward<B>(b));
        }

        template <class A, class B>
        [[nodiscard]] constexpr auto CreateSub(A&& a, B&& b) const {
            return make_node<sub_tag>(std::forward<A>(a), std::forward<B>(b));
        }

        template <class A, class B>
        [[nodiscard]] constexpr auto CreateMul(A&& a, B&& b) const {
            return make_node<mul_tag>(std::forward<A>(a), std::forward<B>(b));
        }

        template <class A, class B>
        [[nodiscard]] constexpr auto CreateDiv(A&& a, B&& b) const {
            return make_node<div_tag>(std::forward<A>(a), std::forward<B>(b));
        }

        template <class Base, class... I>
        [[nodiscard]] constexpr auto CreateSubscript(Base&& base, I&&... idx) const {
            return make_node<subscript_tag>(std::forward<Base>(base), std::forward<I>(idx)...);
        }

        template <class Cond, class Then, class Else>
        [[nodiscard]] constexpr auto CreateIf(Cond&& c, Then&& t, Else&& e) const {
            return make_node<if_tag>(std::forward<Cond>(c), std::forward<Then>(t), std::forward<Else>(e));
        }

        template <class... Args>
        [[nodiscard]] constexpr auto CreateSeq(Args&&... args) const {
            return make_node<seq_tag>(std::forward<Args>(args)...);
        }

        template <class Fn, class... Args>
        [[nodiscard]] constexpr auto CreateCall(Fn&& fn, Args&&... args) const {
            return make_node<call_tag>(std::forward<Fn>(fn), std::forward<Args>(args)...);
        }
    };

    // -----------------------------
    // 7) Basic evaluate (single-view)
    // -----------------------------
    template <class Expr, class Transform>
    constexpr decltype(auto) evaluate(Expr&& expr, Transform&& t) {
        if constexpr (VariantExpr<Expr>) {
            return std::visit(
                [&]<typename T0>(T0&& alt) -> decltype(auto) {
                    return evaluate(std::forward<T0>(alt), std::forward<Transform>(t));
                },
                std::forward<Expr>(expr)
            );
        }
        else if constexpr (Expression<Expr>) {
            using E = std::decay_t<Expr>;
            using children_t = std::decay_t<decltype(std::declval<E>().children)>;
            if constexpr (std::tuple_size_v<children_t> == 0) {
                return t.on_terminal(std::forward<Expr>(expr));
            }
            else {
                return std::apply(
                    [&]<typename... T0>(T0&&... ch) -> decltype(auto) {
                        return t.on_node(
                            typename E::tag_type{},
                            evaluate(std::forward<T0>(ch), t)...
                        );
                    },
                    std::forward<Expr>(expr).children
                );
            }
        }
        else {
            return t.on_terminal(std::forward<Expr>(expr));
        }
    }

    // -----------------------------
    // 7b) visit: lightweight traversal/inspection (no dual-view)
    // -----------------------------
    template <class Expr, class Visitor>
    constexpr decltype(auto) visit(Expr&& expr, Visitor&& v) {
        if constexpr (VariantExpr<Expr>) {
            return std::visit(
                [&]<typename T0>(T0&& alt) -> decltype(auto) {
                    return visit(std::forward<T0>(alt), std::forward<Visitor>(v));
                },
                std::forward<Expr>(expr)
            );
        }
        else if constexpr (Expression<Expr>) {
            using E = std::decay_t<Expr>;
            using children_t = std::decay_t<decltype(std::declval<E>().children)>;
            if constexpr (std::tuple_size_v<children_t> == 0) {
                return v.on_terminal(std::forward<Expr>(expr));
            }
            else {
                return std::apply(
                    [&]<typename... T0>(T0&&... ch) -> decltype(auto) {
                        return v.on_node(
                            typename E::tag_type{},
                            visit(std::forward<T0>(ch), std::forward<Visitor>(v))...
                        );
                    },
                    std::forward<Expr>(expr).children
                );
            }
        }
        else {
            return v.on_terminal(std::forward<Expr>(expr));
        }
    }

    // transform: provide both original children and transformed children
    template <class Expr, class Transform>
    constexpr decltype(auto) transform(Expr&& expr, Transform&& t) {
        if constexpr (VariantExpr<Expr>) {
            return std::visit(
                [&]<typename T0>(T0&& alt) -> decltype(auto) {
                    return transform(std::forward<T0>(alt), std::forward<Transform>(t));
                },
                std::forward<Expr>(expr)
            );
        }
        else if constexpr (Expression<Expr>) {
            using E = std::decay_t<Expr>;
            using children_t = std::decay_t<decltype(std::declval<E>().children)>;
            if constexpr (std::tuple_size_v<children_t> == 0) {
                return t.on_terminal(std::forward<Expr>(expr));
            }
            else {
                return std::apply(
                    [&]<typename... T0>(T0&&... ch) -> decltype(auto) {
                        auto children_tup = std::tuple < T0
                        ...
                        >
                        (std::forward<T0>(ch)
                        ...
                        )
                        ;
                        return [&]<std::size_t... I>(std::index_sequence<I...>) -> decltype(auto) {
                            return t.on_node(
                                typename E::tag_type{},
                                std::get < I > (children_tup)...,
                                transform(std::get < I > (children_tup), t)...
                            );
                        }(std::index_sequence_for < T0...>{});
                    },
                    std::forward<Expr>(expr).children
                );
            }
        }
        else {
            return t.on_terminal(std::forward<Expr>(expr));
        }
    }

    // Helper: rebuild a node<Tag> from given children (useful for rewrite rules)
    // Always stores children by value (std::decay_t) — rebuilt nodes own their data.
    template <class Tag, class... Children>
    constexpr auto rebuild(Children&&... children) {
        return node<Tag, std::decay_t<Children>...>(std::forward<Children>(children)...);
    }

    // Relay helper for rewrite_once.
    template <class Stored>
    struct rewrite_relay {
        Stored rule;

        template <class R>
        constexpr explicit rewrite_relay(R&& r) : rule(std::forward<R>(r)) {}

        template <class T>
        constexpr decltype(auto) on_terminal(T&& x) const {
            return std::forward<T>(x);
        }

        template <class Tag, class... Args>
        constexpr decltype(auto) on_node(Tag tag, Args&&... args) const {
            return rule.on_node(tag, std::forward<Args>(args)...);
        }
    };

    // rewrite_once: single-pass rewrite using a rule 'r'.
    template <class Expr, class Rule>
    constexpr decltype(auto) rewrite_once(Expr&& expr, Rule&& r) {
        if constexpr (Expression<Expr>) {
            using stored_t = std::conditional_t<
                std::is_lvalue_reference_v<Rule>,
                std::remove_reference_t<Rule>&,
                std::remove_reference_t<Rule>
            >;
            return transform(
                std::forward<Expr>(expr),
                rewrite_relay<stored_t>{std::forward<Rule>(r)}
            );
        }
        else {
            return std::forward<Expr>(expr);
        }
    }

    // -----------------------------------------------------------------------
    // Emit-phase helpers: structural equality and an emit::evaluate wrapper.
    // -----------------------------------------------------------------------
    namespace emit { namespace constants {
            inline constexpr std::size_t kGoldenHashMix = 0x9e3779b97f4a7c15ULL;
            inline constexpr auto kUnknownTagText = "<tag>";
            inline constexpr auto kUnknownTerminalText = "<term>";
            inline constexpr auto kDagTerminalPrefix = "term(";
            inline constexpr auto kDagNodeFallbackText = "node";
            inline constexpr auto kDagEmptyNodeText = "<empty>";
        } // namespace constants

        // ADL barrier: calls structural_unwrap unqualified so overloads defined in
        // the argument's namespace (e.g. Lithe's phase-wrapper overloads) are
        // found, while the vakya default identity overload is also visible.
        namespace unwrap_detail {
            using vakya::structural_unwrap;

            template <class T>
            [[nodiscard]] constexpr decltype(auto) call(T&& x)
                noexcept(noexcept(structural_unwrap(std::forward<T>(x)))) {
                return structural_unwrap(std::forward<T>(x));
            }
        } // namespace unwrap_detail

        // Detect whether structural_unwrap(x) is a non-identity unwrap (i.e. a
        // registered wrapper like Lithe's phase wrappers). True when the unwrapped
        // decayed type differs from the input decayed type.
        template <class T>
        inline constexpr bool is_unwrappable_v =
            !std::is_same_v<std::decay_t<T>,
                            std::decay_t<decltype(unwrap_detail::call(std::declval<T>()))>>;

        template <class A, class B>
        constexpr bool structural_equal(const A& a, const B& b) {
            // Unwrap registered wrappers (e.g. Lithe phase wrappers) transparently
            // via the structural_unwrap ADL customization point.
            if constexpr (is_unwrappable_v<const A&> && is_unwrappable_v<const B&>) {
                return emit::structural_equal(unwrap_detail::call(a), unwrap_detail::call(b));
            }
            else if constexpr (is_unwrappable_v<const A&>) {
                return emit::structural_equal(unwrap_detail::call(a), b);
            }
            else if constexpr (is_unwrappable_v<const B&>) {
                return emit::structural_equal(a, unwrap_detail::call(b));
            }

            // Handle std::variant-like carriers first.
            else if constexpr (VariantExpr<A> && VariantExpr<B>) {
                return std::visit([](auto const& aa, auto const& bb) -> bool {
                    return emit::structural_equal(aa, bb);
                }, a, b);
            }
            else if constexpr (VariantExpr<A>) {
                return std::visit([&](auto const& aa) -> bool { return emit::structural_equal(aa, b); }, a);
            }
            else if constexpr (VariantExpr<B>) {
                return std::visit([&](auto const& bb) -> bool { return emit::structural_equal(a, bb); }, b);
            }

            // Both are AST nodes: compare tag types and recurse on children.
            else if constexpr (Expression<A> && Expression<B>) {
                using DA = std::decay_t<A>;
                using DB = std::decay_t<B>;
                if constexpr (!std::is_same_v<typename DA::tag_type, typename DB::tag_type>) {
                    return false;
                }
                else {
                    using ca_t = decltype(std::declval<DA&>().children);
                    using cb_t = decltype(std::declval<DB&>().children);
                    constexpr std::size_t NA = std::tuple_size_v<std::decay_t<ca_t>>;
                    constexpr std::size_t NB = std::tuple_size_v<std::decay_t<cb_t>>;
                    if constexpr (NA != NB) {
                        return false;
                    }
                    else {
                        const auto& ca = a.children;
                        const auto& cb = b.children;
                        return [&]<std::size_t... I>(std::index_sequence<I...>) -> bool {
                            return (... && emit::structural_equal(std::get < I > (ca), std::get < I > (cb)));
                        }(std::make_index_sequence < NA >
                        {}
                        )
                        ;
                    }
                }
            }

            // Terminals and wrappers:
            else {
                using DA = std::decay_t<A>;
                using DB = std::decay_t<B>;

                if constexpr (is_expr_ref_wrapper_v<A> && is_expr_ref_wrapper_v<B>) {
                    if constexpr (std::is_same_v<DA, DB>) {
                        if (a.p == b.p) return true;
                        if (a.p && b.p) return emit::structural_equal(*a.p, *b.p);
                        return false;
                    }
                    else {
                        if (a.p && b.p) return emit::structural_equal(*a.p, *b.p);
                        return false;
                    }
                }

                else if constexpr (is_expr_wrapper_v<A> && is_expr_wrapper_v<B>) {
                    return emit::structural_equal(a.value, b.value);
                }

                else if constexpr (is_expr_wrapper_v<A>)
                    return emit::structural_equal(a.value, b);
                else if constexpr (is_expr_wrapper_v<B>) {
                    return emit::structural_equal(a, b.value);
                }

                else if constexpr (is_expr_ref_wrapper_v<A> && is_expr_wrapper_v<B>) {
                    if (a.p) return emit::structural_equal(*a.p, b.value);
                    return false;
                }
                else if constexpr (is_expr_wrapper_v<A> && is_expr_ref_wrapper_v<B>) {
                    if (b.p) return emit::structural_equal(a.value, *b.p);
                    return false;
                }

                else if constexpr (is_expr_ref_wrapper_v<A> && std::is_arithmetic_v<DB>) {
                    if (a.p) return emit::structural_equal(*a.p, b);
                    return false;
                }
                else if constexpr (std::is_arithmetic_v<DA>&& is_expr_ref_wrapper_v<B>) {
                    if (b.p) return emit::structural_equal(a, *b.p);
                    return false;
                }

                else if constexpr (is_expr_wrapper_v<A> && std::is_arithmetic_v<DB>) {
                    return emit::structural_equal(a.value, b);
                }
                else if constexpr (std::is_arithmetic_v<DA>&& is_expr_wrapper_v<B>) {
                    return emit::structural_equal(a, b.value);
                }

                else if constexpr (std::is_arithmetic_v<DA>&& std::is_arithmetic_v<DB>) {
                    return a == b;
                }
                // Same-type terminals that define operator== (e.g. column_expr, literal_expr).
                // structural_payload_hash equality is a necessary condition; operator== is
                // the authoritative check. This enables user-defined terminal types without
                // requiring changes to Vākya per-type.
                else if constexpr (std::is_same_v<DA, DB>&& std::equality_comparable<DA>) {
                    return a == b;
                }
                else {
                    return false;
                }
            }
        }

        // Thin forwarder so tests can call vakya::emit::evaluate(...)
        template <class Expr, class Transform>
        constexpr decltype(auto) evaluate(Expr&& expr, Transform&& t) {
            return vakya::evaluate(std::forward<Expr>(expr), std::forward<Transform>(t));
        }

        // Thin forwarder so passes can invoke visit via the emit phase.
        template <class Expr, class Visitor>
        constexpr decltype(auto) visit(Expr&& expr, Visitor&& v) {
            return vakya::visit(std::forward<Expr>(expr), std::forward<Visitor>(v));
        }

        // ------------------------------------------------------------------
        // Tag metadata: single source of truth (openly extensible)
        // ------------------------------------------------------------------
        inline constexpr std::uint8_t kVariadicArity = 0xFF;

        // Reserved band: built-in tags occupy [0, kExtensionIdBase); downstream
        // tags MUST return stable_id >= kExtensionIdBase from their own
        // specialisation so ids never collide with built-ins.
        inline constexpr std::size_t kExtensionIdBase = 1000u;

        // Single source of truth for per-tag metadata. Downstream EDSLs
        // specialise THIS (in their own header) to register custom tags.
        template <class Tag>
        struct tag_descriptor {
            static constexpr std::string_view symbol = constants::kUnknownTagText;
            static constexpr std::size_t stable_id = 0x9u; // legacy tag_id fallback
            static constexpr std::uint8_t arity = kVariadicArity;
            // Commutative tags: the pattern matcher will automatically try both
            // argument orderings when the canonical order fails to match.
            static constexpr bool is_commutative = false;
        };

        template <>
        struct tag_descriptor<add_tag> {
            static constexpr std::string_view symbol = "+";
            static constexpr std::size_t stable_id = 1u;
            static constexpr std::uint8_t arity = 2;
            static constexpr bool is_commutative = true;
        };

        template <>
        struct tag_descriptor<sub_tag> {
            static constexpr std::string_view symbol = "-";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<mul_tag> {
            static constexpr std::string_view symbol = "*";
            static constexpr std::size_t stable_id = 2u;
            static constexpr std::uint8_t arity = 2;
            static constexpr bool is_commutative = true;
        };

        template <>
        struct tag_descriptor<div_tag> {
            static constexpr std::string_view symbol = "/";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<mod_tag> {
            static constexpr std::string_view symbol = "%";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<neg_tag> {
            static constexpr std::string_view symbol = "neg";
            static constexpr std::size_t stable_id = 3u;
            static constexpr std::uint8_t arity = 1;
        };

        template <>
        struct tag_descriptor<eq_tag> {
            static constexpr std::string_view symbol = "==";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<ne_tag> {
            static constexpr std::string_view symbol = "!=";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<lt_tag> {
            static constexpr std::string_view symbol = "<";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<le_tag> {
            static constexpr std::string_view symbol = "<=";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<gt_tag> {
            static constexpr std::string_view symbol = ">";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<ge_tag> {
            static constexpr std::string_view symbol = ">=";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<and_tag> {
            static constexpr std::string_view symbol = "&&";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<or_tag> {
            static constexpr std::string_view symbol = "||";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<not_tag> {
            static constexpr std::string_view symbol = "!";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 1;
        };

        template <>
        struct tag_descriptor<bit_and_tag> {
            static constexpr std::string_view symbol = "&";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<bit_or_tag> {
            static constexpr std::string_view symbol = "|";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<bit_xor_tag> {
            static constexpr std::string_view symbol = "^";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<bit_not_tag> {
            static constexpr std::string_view symbol = "~";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 1;
        };

        template <>
        struct tag_descriptor<shl_tag> {
            static constexpr std::string_view symbol = "<<";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<shr_tag> {
            static constexpr std::string_view symbol = ">>";
            static constexpr std::size_t stable_id = 0x9u;
            static constexpr std::uint8_t arity = 2;
        };

        template <>
        struct tag_descriptor<subscript_tag> {
            static constexpr std::string_view symbol = "idx";
            static constexpr std::size_t stable_id = 4u;
            static constexpr std::uint8_t arity = kVariadicArity;
        };

        template <>
        struct tag_descriptor<get_element_ptr_tag> {
            static constexpr std::string_view symbol = "gep";
            static constexpr std::size_t stable_id = 5u;
            static constexpr std::uint8_t arity = kVariadicArity;
        };

        template <>
        struct tag_descriptor<extract_value_tag> {
            static constexpr std::string_view symbol = "extractvalue";
            static constexpr std::size_t stable_id = 6u;
            static constexpr std::uint8_t arity = kVariadicArity;
        };

        template <>
        struct tag_descriptor<insert_value_tag> {
            static constexpr std::string_view symbol = "insertvalue";
            static constexpr std::size_t stable_id = 7u;
            static constexpr std::uint8_t arity = kVariadicArity;
        };

        template <>
        struct tag_descriptor<indirect_call_tag> {
            static constexpr std::string_view symbol = "indirectcall";
            static constexpr std::size_t stable_id = 8u;
            static constexpr std::uint8_t arity = kVariadicArity;
        };

        // Comprehensive tag name mapping for debug dump.
        template <class Tag>
        struct tag_name {
            static constexpr auto value = tag_descriptor<Tag>::symbol.data();
        };

        // ------------------------------------------------------------------
        // Hash utilities and structural hash for IR (variant/node/terminals)
        // ------------------------------------------------------------------
        [[nodiscard]] constexpr std::size_t hash_combine(const std::size_t h, const std::size_t x) {
            return h ^ (x + constants::kGoldenHashMix + (h << 6) + (h >> 2));
        }

        template <class Tag>
        struct tag_id {
            static constexpr std::size_t value = tag_descriptor<Tag>::stable_id;
        };

        // Opt-in leaf-payload hook.
        struct no_payload_t {};

        constexpr no_payload_t structural_payload_hash(...) noexcept { return {}; }

        template <class E>
        concept HasPayloadHash = requires(const E& e) {
            { structural_payload_hash(e) } -> std::convertible_to<std::size_t>;
        };

        // structural_hash: produce a stable-ish size_t for an IR value.
        template <class E>
        constexpr std::size_t structural_hash(const E& e) {
            if constexpr (VariantExpr<E>) {
                return std::visit([](auto const& alt) -> std::size_t { return emit::structural_hash(alt); }, e);
            }
            else if constexpr (Expression<E>) {
                using Dec = std::decay_t<E>;
                std::size_t h = tag_id<typename Dec::tag_type>::value;
                if constexpr (HasPayloadHash<Dec>)
                    h = hash_combine(h, structural_payload_hash(e));
                const auto& children = e.children;
                std::apply([&](auto const&... ch) {
                    ((h = hash_combine(h, emit::structural_hash(ch))), ...);
                }, children);
                return h;
            }
            else {
                using D = std::decay_t<E>;
                if constexpr (is_expr_ref_wrapper_v<E>) {
                    return emit::structural_hash(*e.p);
                }
                else if constexpr (is_expr_wrapper_v<E>) {
                    return emit::structural_hash(e.value);
                }
                else if constexpr (is_unwrappable_v<const E&>) {
                    return emit::structural_hash(unwrap_detail::call(e));
                }
                else if constexpr (std::is_arithmetic_v<D>) {
                    return std::hash<D>{}(e);
                }
                else if constexpr (requires { std::hash<D>{}(e); }) {
                    return std::hash<D>{}(e);
                }
                else {
                    return constants::kGoldenHashMix;
                }
            }
        }

        // Debug dump: produce a simple string representation.
        template <class Expr>
            requires (!requires(Expr e) { e.dag; e.dag.nodes; e.dag.root; })
        std::string dump(Expr&& expr) {
            if constexpr (VariantExpr<Expr>) {
                return std::visit([](auto&& alt) -> std::string { return dump(std::forward<decltype(alt)>(alt)); },
                                  std::forward<Expr>(expr));
            }
            else if constexpr (Expression<Expr>) {
                using E = std::decay_t<Expr>;
                const char* tag = tag_name<typename E::tag_type>::value;
                std::ostringstream out;
                out << '(' << tag;
                std::apply([&](auto&&... ch) {
                    ((out << ' ' << dump(std::forward<decltype(ch)>(ch))), ...);
                }, std::forward<Expr>(expr).children);
                out << ')';
                return out.str();
            }
            else {
                using D = std::decay_t<Expr>;
                if constexpr (std::is_arithmetic_v<D>) {
                    return std::to_string(expr);
                }
                else if constexpr (is_expr_wrapper_v<Expr>) {
                    return dump(expr.value);
                }
                else if constexpr (is_expr_ref_wrapper_v<Expr>) {
                    return dump(*expr.p);
                }
                else {
                    return {constants::kUnknownTerminalText};
                }
            }
        }
    } // namespace emit

    // Stable key type for side-car systems keyed by expression structure.
    using structural_hash_t = std::size_t;

    template <class Expr>
    [[nodiscard]] constexpr structural_hash_t structural_hash(const Expr& expr) {
        return emit::structural_hash(expr);
    }

    template <class A, class B>
    [[nodiscard]] constexpr bool structural_equal(const A& a, const B& b) {
        return emit::structural_equal(a, b);
    }

    template <class Expr>
    [[nodiscard]] constexpr structural_hash_t structural_key(const Expr& expr) {
        return emit::structural_hash(expr);
    }

    // Small lang facade to preserve older call sites: vakya::lang::as_expr(...)
    namespace lang {
        template <class T>
        constexpr auto as_expr(T& x) -> expr_ref<T> { return vakya::as_expr(x); }

        template <class T>
        constexpr auto as_expr(T&& x) -> expr<std::decay_t<T>> { return vakya::as_expr(std::forward<T>(x)); }
    } // namespace lang

    // -----------------------------
    // Minimal Shared DAG Carrier
    // -----------------------------
    namespace graph {
        using node_id = std::size_t;

        struct dag_node {
            node_id id{};
            std::size_t hash{};
            std::any expr; // type-erased expression storage
            std::type_index type_tag; // O(1) type check without exceptions
            std::vector<node_id> children;
            std::size_t use_count = 0;
            std::string_view operation_sv; // points to constexpr storage for built-in ops
            std::string operation_dynamic; // heap storage only for user-defined op names

            dag_node() : type_tag(typeid(void)) {}

            template <class Expr>
            dag_node(const node_id nid, const std::size_t h, Expr e, std::vector<node_id> ch = {},
                     const std::size_t uses = 0,
                     std::string_view op_sv = {}, std::string op_dyn = "")
                : id(nid), hash(h), expr(std::move(e)), type_tag(typeid(Expr)),
                  children(std::move(ch)), use_count(uses),
                  operation_sv(op_sv), operation_dynamic(std::move(op_dyn)) {}

            [[nodiscard]] std::string_view operation_name() const noexcept {
                return operation_sv.empty() ? std::string_view{operation_dynamic} : operation_sv;
            }
        };

        template <class Expr>
        struct dag_view {
            std::unordered_map<node_id, dag_node> nodes;
            node_id root{};

            constexpr dag_view() = default;

            constexpr dag_view(std::unordered_map<node_id, dag_node> n, const node_id r)
                : nodes(std::move(n)), root(r) {}

            [[nodiscard]] const dag_node* get_node(const node_id id) const {
                auto it = nodes.find(id);
                return it != nodes.end() ? &it->second : nullptr;
            }

            [[nodiscard]] const dag_node* get_root_node() const {
                return get_node(root);
            }

            [[nodiscard]] std::size_t node_count() const {
                return nodes.size();
            }

            [[nodiscard]] std::size_t shared_node_count() const {
                std::size_t count = 0;
                for (const auto& node : nodes | std::views::values) {
                    if (node.use_count > 1) ++count;
                }
                return count;
            }
        };

        template <class Expr>
        struct shared_expr {
            dag_view<Expr> dag;

            constexpr shared_expr() = default;

            constexpr explicit shared_expr(dag_view<Expr> d)
                : dag(std::move(d)) {}

            [[nodiscard]] const dag_node* root_node() const {
                return dag.get_root_node();
            }

            [[nodiscard]] std::size_t size() const {
                return dag.node_count();
            }

            [[nodiscard]] std::size_t sharing_count() const {
                return dag.shared_node_count();
            }
        };

        template <class Expr>
        struct dag_builder {
            dag_view<Expr> dag;
            std::unordered_map<std::size_t, std::vector<node_id>> buckets;
            node_id next_id = 1;

            template <class E>
            node_id intern(const E& expr) {
                std::size_t h = emit::structural_hash(expr);

                auto& bucket = buckets[h];

                const std::type_index target_tag{typeid(E)};
                for (node_id candidate_id : bucket) {
                    auto it = dag.nodes.find(candidate_id);
                    if (it != dag.nodes.end() && it->second.expr.has_value()) {
                        if (it->second.type_tag == target_tag) {
                            const auto& stored_expr = *std::any_cast<const E>(&it->second.expr);
                            if (emit::structural_equal(stored_expr, expr)) {
                                return candidate_id;
                            }
                        }
                    }
                }

                std::vector<node_id> child_ids;
                std::string_view op_sv;
                std::string op_dyn;

                if constexpr (vakya::Expression<E>) {
                    using tag_t = std::decay_t<E>::tag_type;
                    op_sv = emit::tag_name<tag_t>::value;

                    std::apply([this, &child_ids]<typename... T0>(T0 const&... children) {
                        ((child_ids.push_back(this->template intern<std::decay_t<T0>>(children))), ...);
                    }, expr.children);
                }
                else {
                    op_dyn = emit::dump(expr);
                }

                node_id new_id = next_id++;
                dag_node new_node{new_id, h, expr, child_ids, 0, op_sv, std::move(op_dyn)};
                dag.nodes[new_id] = std::move(new_node);
                bucket.push_back(new_id);

                for (node_id child_id : child_ids) {
                    auto child_it = dag.nodes.find(child_id);
                    if (child_it != dag.nodes.end()) {
                        ++child_it->second.use_count;
                    }
                }

                return new_id;
            }
        };

        template <class Expr>
        [[nodiscard]] constexpr auto build_dag(Expr const& expr) {
            using expr_t = std::decay_t<Expr>;

            dag_builder<expr_t> builder;
            builder.dag.root = builder.intern(expr);

            return shared_expr<expr_t>{std::move(builder.dag)};
        }

        template <class Expr>
        std::vector<node_id> topo_order(dag_view<Expr> const& dag) {
            std::vector<node_id> result;
            std::unordered_set<node_id> visited;

            auto dfs = [&](auto& dfs_ref, node_id id) -> void {
                if (visited.contains(id)) return;

                visited.insert(id);

                const dag_node* node = dag.get_node(id);
                if (!node) return;

                for (node_id child_id : node->children) {
                    dfs_ref(dfs_ref, child_id);
                }

                result.push_back(id);
            };

            dfs(dfs, dag.root);

            return result;
        }
    } // namespace graph

    // dump overload for shared_expr after graph namespace is defined
    namespace emit {
        template <class E>
        std::string dump_dag_node_expr(E const& expr) {
            if constexpr (vakya::Expression<E>) {
                using tag_t = std::decay_t<E>::tag_type;
                return std::string(tag_name<tag_t>::value);
            }
            else if constexpr (vakya::VariantExpr<E>) {
                return std::visit([](auto const& alt) -> std::string {
                    return dump_dag_node_expr(alt);
                }, expr);
            }
            else {
                return dump(expr);
            }
        }

        template <class Expr>
        std::string dump(graph::shared_expr<Expr> const& g) {
            std::string result;

            auto terminal_text = [](graph::dag_node const& node) -> std::string {
                const auto op = node.operation_name();
                if (!op.empty() && op != constants::kUnknownTerminalText) {
                    return std::string{op};
                }
                std::ostringstream oss;
                oss << constants::kDagTerminalPrefix << node.hash << ')';
                return oss.str();
            };

            auto op_text = [](graph::dag_node const& node) -> std::string {
                const auto op = node.operation_name();
                if (!op.empty()) {
                    return std::string{op};
                }
                return constants::kDagNodeFallbackText;
            };

            std::vector<graph::node_id> node_ids;
            node_ids.reserve(g.dag.nodes.size());
            for (const auto& [id, node] : g.dag.nodes) {
                node_ids.push_back(id);
            }
            std::ranges::sort(node_ids);

            for (auto id : node_ids) {
                auto it = g.dag.nodes.find(id);
                if (it == g.dag.nodes.end()) continue;

                const auto& node = it->second;

                std::ostringstream line;
                line << '%' << id << " = ";

                if (!node.expr.has_value()) {
                    line << constants::kDagEmptyNodeText;
                }
                else {
                    if (node.children.empty()) {
                        line << terminal_text(node);
                    }
                    else {
                        line << op_text(node) << '(';
                        for (std::size_t i = 0; i < node.children.size(); ++i) {
                            if (i > 0) line << ", ";
                            line << '%' << node.children[i];
                        }
                        line << ')';
                    }
                }

                line << " [uses=" << node.use_count << "]\n";
                result += line.str();
            }

            std::ostringstream footer;
            footer << "root = %" << g.dag.root << '\n';
            result += footer.str();

            return result;
        }

        template <class Expr, class Transform>
        auto evaluate(graph::shared_expr<Expr> const& g, Transform&& t) {
            const auto* root_node = g.dag.get_root_node();
            if (root_node && root_node->expr.has_value()) {
                if (const auto* typed = std::any_cast<const Expr>(&root_node->expr)) {
                    return vakya::evaluate(*typed, std::forward<Transform>(t));
                }
            }
            return decltype(vakya::evaluate(std::declval<const Expr&>(), t)){};
        }
    } // namespace emit (extension for graph types)


    // Small, flexible IR holder.
    namespace ir {
        template <class... Ts>
        using any_expr = std::variant<Ts...>;

        template <class... Ts, class E>
        constexpr auto to_ir(E&& e) -> any_expr<Ts...> {
            return any_expr<Ts...>{std::forward<E>(e)};
        }
    } // namespace ir

    // -----------------------------
    // Tree Utility Layer
    // -----------------------------
    namespace tree { namespace detail {
            template <class T>
            using direct_children_tuple_t = std::conditional_t<
                Expression<T>,
                decltype(std::declval<std::remove_reference_t<T>&>().children),
                std::tuple<>
            >;

            template <class V>
            struct children_variant;

            template <class... Ts>
            struct children_variant<std::variant<Ts...>> {
                using type = std::variant<std::decay_t<direct_children_tuple_t<Ts>>...>;
            };

            template <class V>
            using children_variant_t = children_variant<std::decay_t<V>>::type;
        } // namespace detail

        template <class E, class... NewChildren>
        constexpr auto rebuild_with(E&& e, NewChildren&&... children) {
            using ET = std::remove_reference_t<E>;

            if constexpr (Expression<ET>) {
                using tag_t = ET::tag_type;
                constexpr std::size_t N = std::tuple_size_v<decltype(std::declval<ET&>().children)>;
                static_assert(sizeof...(NewChildren) == N,
                              "vakya::tree::rebuild_with: child count must match node arity");
                return make_node<tag_t>(std::forward<NewChildren>(children)...);
            }
            else {
                static_assert(sizeof...(NewChildren) == 0,
                              "vakya::tree::rebuild_with: terminals do not accept replacement children");
                return std::forward<E>(e);
            }
        }

        template <std::size_t I, class E, class NewChild>
        constexpr auto replace_child(E&& e, NewChild&& child) {
            using ET = std::remove_reference_t<E>;

            if constexpr (Expression<ET>) {
                using tag_t = ET::tag_type;
                constexpr std::size_t N = std::tuple_size_v<decltype(std::declval<ET&>().children)>;
                static_assert(I < N, "vakya::tree::replace_child: index out of range");

                auto&& tup = std::forward<E>(e).children;
                return [&]<std::size_t... J>(std::index_sequence<J...>) {
                    auto pick = [&]<std::size_t Jx>() -> decltype(auto) {
                        if constexpr (Jx == I) {
                            return std::forward<NewChild>(child);
                        }
                        else {
                            return std::get < Jx > (std::forward<decltype(tup)>(tup));
                        }
                    };
                    return make_node<tag_t>(pick.template operator()<J>()...);
                }(std::make_index_sequence < N >
                {}
                )
                ;
            }
            else {
                return std::forward<E>(e);
            }
        }

        template <class E, class Fn>
        constexpr auto map_children(E&& e, Fn&& fn) {
            using ET = std::remove_reference_t<E>;

            if constexpr (VariantExpr<ET>) {
                return std::visit(
                    [&]<typename T0>(T0&& alt) constexpr {
                        return map_children(std::forward<T0>(alt), std::forward<Fn>(fn));
                    },
                    std::forward<E>(e)
                );
            }
            else if constexpr (Expression<ET>) {
                using children_t = std::decay_t<decltype(std::declval<ET&>().children)>;
                if constexpr (std::tuple_size_v<children_t> == 0) {
                    return std::forward<E>(e);
                }
                else {
                    return std::apply(
                        [&]<typename... T0>(T0&&... ch) constexpr {
                            return rebuild_with(
                                std::forward<E>(e),
                                fn(std::forward<T0>(ch))...
                            );
                        },
                        std::forward<E>(e).children
                    );
                }
            }
            else {
                return std::forward<E>(e);
            }
        }

        template <class E>
        constexpr std::size_t arity(E const& e) {
            if constexpr (VariantExpr<E>) {
                return std::visit([](auto const& alt) constexpr { return arity(alt); }, e);
            }
            else if constexpr (Expression<E>) {
                using children_t = decltype(std::declval<std::remove_reference_t<E>&>().children);
                return std::tuple_size_v<std::remove_reference_t<children_t>>;
            }
            else {
                return 0;
            }
        }

        template <class E>
        constexpr std::size_t size(E const& e) {
            if constexpr (VariantExpr<E>) {
                return std::visit([](auto const& alt) constexpr { return size(alt); }, e);
            }
            else if constexpr (Expression<E>) {
                std::size_t count = 1;
                std::apply([&](auto const&... ch) constexpr {
                    ((count += size(ch)), ...);
                }, e.children);
                return count;
            }
            else {
                return 1;
            }
        }

        template <class E>
        constexpr std::size_t depth(E const& e) {
            if constexpr (VariantExpr<E>) {
                return std::visit([](auto const& alt) constexpr { return depth(alt); }, e);
            }
            else if constexpr (Expression<E>) {
                std::size_t max_child_depth = 0;
                std::apply([&](auto const&... ch) constexpr {
                    ((max_child_depth = std::max(max_child_depth, depth(ch))), ...);
                }, e.children);
                return 1 + max_child_depth;
            }
            else {
                return 1;
            }
        }

        template <class E, class Fn>
        constexpr void for_each_child(E&& e, Fn&& fn) {
            if constexpr (VariantExpr<std::remove_reference_t<E>>) {
                std::visit([&]<typename T0>(T0&& alt) constexpr {
                    for_each_child(std::forward<T0>(alt), std::forward<Fn>(fn));
                }, std::forward<E>(e));
            }
            else if constexpr (Expression<std::remove_reference_t<E>>) {
                std::apply([&]<typename... T0>(T0&&... ch) constexpr {
                    ((fn(std::forward<T0>(ch))), ...);
                }, std::forward<E>(e).children);
            }
        }

        template <class E>
        constexpr decltype(auto) children_tuple(E&& e) {
            if constexpr (VariantExpr<std::remove_reference_t<E>>) {
                return std::visit([]<typename T0>(T0&& alt) constexpr -> detail::children_variant_t<E> {
                    return detail::children_variant_t<E>{children_tuple(std::forward<T0>(alt))};
                }, std::forward<E>(e));
            }
            else if constexpr (Expression<std::remove_reference_t<E>>) {
                return (std::forward<E>(e).children);
            }
            else {
                return std::tuple<>{};
            }
        }

        template <class Expr>
        constexpr bool is_leaf(const Expr& expr) {
            return arity(expr) == 0;
        }

        template <class Expr>
        constexpr std::size_t internal_nodes(const Expr& expr) {
            if constexpr (Expression<Expr>) {
                std::size_t count = 1;
                std::apply([&count](auto&&... ch) {
                    ((count += internal_nodes(ch)), ...);
                }, expr.children);
                return count;
            }
            else {
                return 0;
            }
        }

        template <class Expr>
        constexpr std::size_t leaf_nodes(const Expr& expr) {
            if constexpr (Expression<Expr>) {
                std::size_t count = 0;
                std::apply([&count](auto&&... ch) {
                    ((count += leaf_nodes(ch)), ...);
                }, expr.children);
                return count;
            }
            else {
                return 1;
            }
        }

        template <class E, template <class> class Pred>
        consteval bool all_tags_satisfy() {
            using Dec = std::decay_t<E>;
            if constexpr (!Expression<Dec>) {
                return true;
            }
            else if constexpr (!Pred<Dec>::value) {
                return false;
            }
            else {
                using children_t = std::remove_reference_t<decltype(std::declval<Dec&>().children)>;
                constexpr std::size_t N = std::tuple_size_v<children_t>;
                return []<std::size_t... I>(std::index_sequence<I...>) consteval {
                    return (all_tags_satisfy<std::tuple_element_t < I, children_t>, Pred > () &&
                    ...
                    )
                    ;
                }(std::make_index_sequence < N >
                {}
                )
                ;
            }
        }

        template <class E, template <class> class Pred>
        consteval bool any_tag_satisfies() {
            using Dec = std::decay_t<E>;
            if constexpr (!Expression<Dec>) {
                return false;
            }
            else if constexpr (Pred<Dec>::value) {
                return true;
            }
            else {
                using children_t = std::remove_reference_t<decltype(std::declval<Dec&>().children)>;
                constexpr std::size_t N = std::tuple_size_v<children_t>;
                return []<std::size_t... I>(std::index_sequence<I...>) consteval {
                    return (any_tag_satisfies<std::tuple_element_t < I, children_t>, Pred > () ||
                    ...
                    )
                    ;
                }(std::make_index_sequence < N >
                {}
                )
                ;
            }
        }

        template <class E, class Contrib, class Combine, class Acc>
        consteval Acc fold(Contrib contrib, Combine combine, Acc init) {
            using Dec = std::decay_t<E>;
            if constexpr (!Expression<Dec>) {
                return init;
            }
            else {
                Acc acc = combine(init, static_cast<Acc>(contrib.template operator()<Dec>()));
                using children_t = std::remove_reference_t<decltype(std::declval<Dec&>().children)>;
                constexpr std::size_t N = std::tuple_size_v<children_t>;
                [&]<std::size_t... I>(std::index_sequence<I...>) consteval {
                    ((acc = combine(acc,
                                    fold<std::tuple_element_t<I, children_t>>(contrib, combine, init))), ...);
                }(std::make_index_sequence < N >
                {}
                )
                ;
                return acc;
            }
        }
    } // namespace tree
} // namespace vakya


// Free operator+/operator* for plain terminals in namespace vakya.
namespace vakya {
    template <class L, class R>
        requires (!vakya::Expression<std::remove_cvref_t<L>>)
        && vakya::is_terminal<std::remove_cvref_t<L>>::value
        && (!std::is_arithmetic_v<std::remove_cvref_t<L>>)
        && vakya::Operand<R>
        && (!vakya::has_member_plus<L, R>)
    constexpr auto operator+(L&& l, R&& r) {
        return vakya::make_node<vakya::add_tag>(std::forward<L>(l), std::forward<R>(r));
    }

    template <class L, class R>
        requires (!vakya::Expression<std::remove_cvref_t<L>>)
        && vakya::is_terminal<std::remove_cvref_t<L>>::value
        && (!std::is_arithmetic_v<std::remove_cvref_t<L>>)
        && vakya::Operand<R>
        && (!vakya::has_member_mul<L, R>)
    constexpr auto operator*(L&& l, R&& r) {
        return vakya::make_node<vakya::mul_tag>(std::forward<L>(l), std::forward<R>(r));
    }

    // =========================================================================
    // Recursive Expression Inspection & Traversal
    // =========================================================================

    template <class T>
    struct terminal_payload_type {
        using type = std::decay_t<decltype(structural_unwrap(std::declval<T>()))>;
    };

    template <class T>
    struct terminal_payload_type<expr<T>> {
        using type = std::decay_t<T>;
    };

    template <class T>
    struct terminal_payload_type<expr_ref<T>> {
        using type = std::remove_cvref_t<T>;
    };

    template <class T>
    using terminal_payload_type_t = typename terminal_payload_type<std::decay_t<T>>::type;

    template <class Expr, class Visitor>
    consteval bool all_terminals_satisfy(Visitor&& visitor) {
        using Dec = std::decay_t<Expr>;
        if constexpr (!Expression<Dec>) {
            using Terminal = terminal_payload_type_t<Dec>;
            if constexpr (requires(Visitor v) { v.template operator()<Terminal>(); }) {
                return visitor.template operator()<Terminal>();
            } else if constexpr (requires(Visitor v) { v(std::declval<Terminal>()); }) {
                return visitor(Terminal{});
            } else {
                return false;
            }
        } else {
            using children_t = std::remove_reference_t<decltype(std::declval<Dec&>().children)>;
            constexpr std::size_t N = std::tuple_size_v<children_t>;
            return []<std::size_t... I>(std::index_sequence<I...>, auto&& vis) consteval {
                return (all_terminals_satisfy<std::tuple_element_t<I, children_t>>(vis) && ...);
            }(std::make_index_sequence<N>{}, visitor);
        }
    }

    template <class Expr, class Visitor>
    constexpr void visit_expression(const Expr& expression, Visitor&& visitor) {
        using Dec = std::decay_t<Expr>;
        if constexpr (!Expression<Dec>) {
            visitor(expression);
        } else {
            visitor(expression);
            std::apply([&](const auto&... child) {
                (visit_expression(child, visitor), ...);
            }, expression.children);
        }
    }

    // =========================================================================
    // Boolean Logic Policies
    // =========================================================================

    enum class tribool_value : std::uint8_t {
        false_value = 0,
        true_value = 1,
        unknown_value = 2
    };

    struct two_valued_logic {
        static constexpr bool is_three_valued = false;

        [[nodiscard]] static constexpr bool logical_and(bool a, bool b) noexcept { return a && b; }
        [[nodiscard]] static constexpr bool logical_or(bool a, bool b) noexcept { return a || b; }
        [[nodiscard]] static constexpr bool logical_not(bool a) noexcept { return !a; }
    };

    struct sql_three_valued_logic {
        static constexpr bool is_three_valued = true;

        [[nodiscard]] static constexpr tribool_value logical_and(tribool_value a, tribool_value b) noexcept {
            if (a == tribool_value::false_value || b == tribool_value::false_value) return tribool_value::false_value;
            if (a == tribool_value::true_value && b == tribool_value::true_value) return tribool_value::true_value;
            return tribool_value::unknown_value;
        }

        [[nodiscard]] static constexpr tribool_value logical_or(tribool_value a, tribool_value b) noexcept {
            if (a == tribool_value::true_value || b == tribool_value::true_value) return tribool_value::true_value;
            if (a == tribool_value::false_value && b == tribool_value::false_value) return tribool_value::false_value;
            return tribool_value::unknown_value;
        }

        [[nodiscard]] static constexpr tribool_value logical_not(tribool_value a) noexcept {
            if (a == tribool_value::true_value) return tribool_value::false_value;
            if (a == tribool_value::false_value) return tribool_value::true_value;
            return tribool_value::unknown_value;
        }
    };

    struct optional_propagating_logic {
        static constexpr bool is_three_valued = false;

        template <class T>
        [[nodiscard]] static constexpr std::optional<bool> logical_and(const std::optional<T>& a, const std::optional<T>& b) noexcept {
            if (!a || !b) return std::nullopt;
            return static_cast<bool>(*a && *b);
        }
    };

    // =========================================================================
    // Expression Analysis Properties
    // =========================================================================

    enum class nullability_kind : std::uint8_t {
        non_nullable,
        nullable,
        unknown
    };

    struct expression_properties {
        std::size_t result_type_id{0};
        nullability_kind nullability{nullability_kind::non_nullable};
        std::uint32_t effects{0};

        bool pure{true};
        bool deterministic{true};
        bool total{true};
        bool foldable{false};
        bool may_overflow{false};
        bool may_produce_nan{false};
    };

    // =========================================================================
    // Generic Operation Capability Descriptors
    // =========================================================================

    template <class OpTag>
    struct operation_descriptor {
        using tag = OpTag;
        static constexpr std::string_view name = "operation";
    };

    struct starts_with_operation {
        static constexpr std::uint32_t id = 0x1001;
    };

    struct ends_with_operation {
        static constexpr std::uint32_t id = 0x1002;
    };

    struct contains_operation {
        static constexpr std::uint32_t id = 0x1003;
    };

    struct regex_match_operation {
        static constexpr std::uint32_t id = 0x1004;
    };

} // namespace vakya

