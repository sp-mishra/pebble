#pragma once

// vakya/pattern.hpp — Compile-time structural pattern matching DSL over Vākya
// AST nodes. Standalone: depends only on vakya/vakya.hpp (no compiler layers).
//
// Opt-in via:  #include "vakya/pattern.hpp"
// Namespace:   vakya::pattern
//
// Design constraints:
//   - NO virtual, NO macros
//   - C++23: explicit object parameter, concepts, [[no_unique_address]], consteval
//   - Header-only

#include "vakya.hpp"

#include <any>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace vakya::pattern {
    // ============================================================================
    // 1.  ID hashing helper
    // ============================================================================

    namespace detail {
        // id_hash is NOT constexpr because std::hash<T>::operator() is not constexpr in
        // most standard libraries.  The key stored in pattern_var is therefore computed
        // at static initialisation time (zero overhead for inline constexpr objects).
        template <class T>
        [[nodiscard]] std::size_t id_hash(const T& id) noexcept {
            if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
                return std::hash<bool>{}(id);
            }
            else if constexpr (std::is_integral_v<std::decay_t<T>>) {
                return std::hash<std::decay_t<T>>{}(id);
            }
            else {
                // fixed_string<N> and anything else with .view()
                return std::hash<std::string_view>{}(id.view());
            }
        }
    } // namespace detail

    // ============================================================================
    // 2.  match_result — runtime bindings from pattern_var IDs to matched values.
    // ============================================================================

    class match_result {
    public:
        using binding_map = std::unordered_map<std::size_t, std::any>;

        match_result() = default;

        void bind(std::size_t key, std::any value) {
            bindings_[key] = std::move(value);
        }

        [[nodiscard]] const binding_map& bindings() const noexcept { return bindings_; }

        template <class T>
        [[nodiscard]] std::optional<T> get(std::size_t key) const {
            if (auto it = bindings_.find(key); it != bindings_.end()) {
                if constexpr (std::is_same_v<std::decay_t<T>, std::any>) {
                    return it->second; // return the std::any directly
                }
                else {
                    if (auto* p = std::any_cast<std::decay_t<T>>(&it->second)) {
                        return *p;
                    }
                }
            }
            return std::nullopt;
        }

        template <class T, class ID>
        [[nodiscard]] std::optional<T> get(const ID& id) const {
            return get<T>(detail::id_hash(id));
        }

        [[nodiscard]] bool has(std::size_t key) const noexcept {
            return bindings_.find(key) != bindings_.end();
        }

        template <class ID>
        [[nodiscard]] bool has(const ID& id) const noexcept {
            return has(detail::id_hash(id));
        }

        void merge(const match_result& other) {
            for (const auto& [k, v] : other.bindings_) {
                bindings_.emplace(k, v);
            }
        }

    private:
        binding_map bindings_;
    };

    // ============================================================================
    // 3.  pattern_var<ID> — wildcard placeholder
    // ============================================================================

    template <auto ID>
    struct pattern_var {
        using is_pattern_var = void;

        static constexpr auto id = ID;

        // key is not constexpr because std::hash is not constexpr in most implementations.
        // It is computed once at programme start (inline static ensures a single ODR copy).
        static const std::size_t key;
    };

    template <auto ID>
    const std::size_t pattern_var<ID>::key = detail::id_hash(ID);

    // ============================================================================
    // 4.  literal_pattern<V> — exact terminal value match
    // ============================================================================

    template <auto V>
    struct literal_pattern {
        using is_literal_pattern = void;

        static constexpr auto value = V;
    };

    // ============================================================================
    // 5.  Pattern concept
    // ============================================================================

    namespace detail {
        template <class P>
        struct is_pattern_impl : std::false_type {};

        template <auto ID>
        struct is_pattern_impl<pattern_var<ID>> : std::true_type {};

        template <auto V>
        struct is_pattern_impl<literal_pattern<V>> : std::true_type {};

        template <class T>
        inline constexpr bool is_pattern_v = is_pattern_impl<std::decay_t<T>>::value;

        // Specialisation for vakya::node<Tag, Ps...>: all children must also satisfy Pattern.
        template <class Tag, class... Ps>
        struct is_pattern_impl<vakya::node<Tag, Ps...>>
            : std::bool_constant<(is_pattern_v<Ps> && ...)> {};
    } // namespace detail

    template <class P>
    concept Pattern =
        requires { typename std::decay_t<P>::is_pattern_var; } ||
        requires { typename std::decay_t<P>::is_literal_pattern; } ||
        detail::is_pattern_v<P>;

    // ============================================================================
    // 6.  match_impl — tag-dispatched recursive matcher
    //
    //     Uses a tag-dispatch struct so each overload specialisation is a
    //     distinct function template at namespace scope — no ambiguous recursion.
    //
    //     Non-linear patterns: when pv<ID> appears more than once in a pattern,
    //     the second+ occurrence verifies structural equality against the first
    //     bound value rather than unconditionally overwriting it.
    // ============================================================================

    namespace detail {
        // Safe commutative query: returns tag_descriptor<T>::is_commutative when
        // the member exists; false otherwise (older or undecorated specializations).
        template <class Tag>
        concept has_is_commutative = requires {
            { vakya::emit::tag_descriptor<Tag>::is_commutative } -> std::convertible_to<bool>;
        };

        template <class Tag>
        inline constexpr bool tag_is_commutative = false;

        template <class Tag>
            requires has_is_commutative<Tag>
        inline constexpr bool tag_is_commutative<Tag> =
            vakya::emit::tag_descriptor<Tag>::is_commutative;

        // Tag structs for dispatch.
        struct match_pvar_tag {};

        struct match_litpat_tag {};

        struct match_node_tag {};

        // Dispatch selector: which branch to take for a given pattern type.
        template <class P>
        using match_dispatch_tag =
        std::conditional_t<
            requires { typename std::decay_t<P>::is_pattern_var; },
            match_pvar_tag,
            std::conditional_t<
                requires { typename std::decay_t<P>::is_literal_pattern; },
                match_litpat_tag,
                match_node_tag
            >
        >;

        // Forward declaration of the unified entry point.
        // accumulated carries bindings from sibling slots seen so far (for
        // non-linear pattern variable consistency checking).
        template <class Pat, class Expr>
        [[nodiscard]] std::optional<match_result>
        do_match(const Pat& pat, const Expr& expr,
                 const match_result& accumulated = {});

        // --- (a) pattern_var: binds or verifies (non-linear matching) ---
        template <class Pat, class Expr>
        [[nodiscard]] std::optional<match_result>
        do_match_impl(match_pvar_tag, const Pat& /*pat*/, const Expr& expr,
                      const match_result& accumulated) {
            if (accumulated.has(Pat::key)) {
                // Non-linear: same variable seen before — check structural equality.
                const auto& existing = accumulated.bindings().at(Pat::key);
                // structural_equal is type-erased via std::any: use a type check
                // then delegate to vakya::structural_equal.
                if constexpr (requires { vakya::structural_equal(expr, expr); }) {
                    if (const auto* prev = std::any_cast<std::decay_t<Expr>>(&existing)) {
                        if (!vakya::structural_equal(*prev, expr)) return std::nullopt;
                        return match_result{}; // consistent — no new binding
                    }
                }
                return std::nullopt; // type mismatch between occurrences
            }
            match_result res;
            res.bind(Pat::key, std::any{expr});
            return res;
        }

        // --- (b) literal_pattern: equality check on terminal ---
        template <class Pat, class Expr>
        [[nodiscard]] std::optional<match_result>
        do_match_impl(match_litpat_tag, const Pat& /*pat*/, const Expr& expr,
                      const match_result& /*accumulated*/) {
            constexpr auto V = Pat::value;

            // Vākya's expr<T> and expr_ref<T> wrappers have implicit conversions to T / T*
            // but their operator== is overloaded to build an eq_tag node (not a bool).
            // We must unwrap them before comparing.
            if constexpr (vakya::is_expr_wrapper_v<Expr>) {
                // expr<T>: compare .value directly.
                if constexpr (requires { { expr.value == V } -> std::convertible_to<bool>; }) {
                    if (static_cast<bool>(expr.value == V)) return match_result{};
                }
            }
            else if constexpr (vakya::is_expr_ref_wrapper_v<Expr>) {
                // expr_ref<T>: dereference and compare.
                if (expr.p) {
                    if constexpr (requires { { *expr.p == V } -> std::convertible_to<bool>; }) {
                        if (static_cast<bool>(*expr.p == V)) return match_result{};
                    }
                }
            }
            else if constexpr (!vakya::Expression<Expr>) {
                // Plain terminal (arithmetic or user-defined non-Expression).
                if constexpr (requires { { expr == V } -> std::convertible_to<bool>; }) {
                    if (static_cast<bool>(expr == V)) return match_result{};
                }
            }
            // If Expr is an Expression node, literal_pattern never matches.
            return std::nullopt;
        }

        // Helper: try to match all children of a binary node with given child order.
        // Returns accumulated match_result or nullopt. Used by commutative retry.
        template <class DP, class DE, std::size_t I0, std::size_t I1>
        [[nodiscard]] std::optional<match_result>
        try_match_children_2(const DP& pat, const DE& expr,
                             std::index_sequence<I0, I1>,
                             const match_result& prior) {
            // child 0 of pat vs expr-child[I0], child 1 of pat vs expr-child[I1]
            match_result acc = prior;
            auto r0 = do_match(std::get < 0 > (pat.children),
                               std::get < I0 > (expr.children), acc);
            if (!r0) return std::nullopt;
            acc.merge(*r0);
            auto r1 = do_match(std::get < 1 > (pat.children),
                               std::get < I1 > (expr.children), acc);
            if (!r1) return std::nullopt;
            acc.merge(*r1);
            // return only the new bindings (excluding prior)
            match_result result;
            for (const auto& [k, v] : acc.bindings()) {
                if (!prior.has(k)) result.bind(k, v);
                else {
                    // keep updated value if present
                    const auto& pv = prior.bindings().at(k);
                    // pv unchanged — don't re-bind
                    (void)pv;
                }
            }
            return result;
        }

        // --- (c) node<Tag, Ps...> pattern: structural recursive match ---
        template <class Pat, class Expr>
        [[nodiscard]] std::optional<match_result>
        do_match_impl(match_node_tag, const Pat& pat, const Expr& expr,
                      const match_result& prior) {
            if constexpr (!vakya::Expression<Expr>) {
                return std::nullopt; // pattern needs a node, got a terminal
            }
            else {
                using DE = std::decay_t<Expr>;
                using DP = std::decay_t<Pat>;
                if constexpr (!std::is_same_v<typename DE::tag_type, typename DP::tag_type>) {
                    return std::nullopt; // tag mismatch
                }
                else {
                    using pat_children_t = std::decay_t<decltype(std::declval<DP&>().children)>;
                    using expr_children_t = std::decay_t<decltype(std::declval<DE&>().children)>;
                    constexpr std::size_t NP = std::tuple_size_v<pat_children_t>;
                    constexpr std::size_t NC = std::tuple_size_v<expr_children_t>;
                    if constexpr (NP != NC) {
                        return std::nullopt; // arity mismatch
                    }
                    else {
                        match_result accumulated = prior;
                        match_result new_bindings;
                        bool ok = [&]<std::size_t... I>(std::index_sequence<I...>) -> bool {
                            return (... && ([&]() -> bool {
                                auto child_res = do_match(
                                    std::get < I > (pat.children),
                                    std::get < I > (expr.children),
                                    accumulated);
                                if (!child_res.has_value()) return false;
                                accumulated.merge(*child_res);
                                new_bindings.merge(*child_res);
                                return true;
                            }()));
                        }(std::make_index_sequence < NP >
                        {}
                        )
                        ;

                        if (ok) return new_bindings;

                        // Commutative retry for binary nodes whose tag declares is_commutative.
                        if constexpr (NP == 2) {
                            using Tag = typename DP::tag_type;
                            if constexpr (tag_is_commutative<Tag>) {
                                match_result acc2 = prior;
                                match_result nb2;
                                bool ok2 = ([&]() -> bool {
                                    auto r0 = do_match(std::get < 0 > (pat.children),
                                                       std::get < 1 > (expr.children), acc2);
                                    if (!r0) return false;
                                    acc2.merge(*r0);
                                    nb2.merge(*r0);
                                    auto r1 = do_match(std::get < 1 > (pat.children),
                                                       std::get < 0 > (expr.children), acc2);
                                    if (!r1) return false;
                                    acc2.merge(*r1);
                                    nb2.merge(*r1);
                                    return true;
                                })();
                                if (ok2) return nb2;
                            }
                        }
                        return std::nullopt;
                    }
                }
            }
        }

        // Unified entry point — dispatches to the right impl.
        template <class Pat, class Expr>
        [[nodiscard]] std::optional<match_result>
        do_match(const Pat& pat, const Expr& expr, const match_result& accumulated) {
            return do_match_impl(match_dispatch_tag<Pat>{}, pat, expr, accumulated);
        }
    } // namespace detail

    // ============================================================================
    // 7.  match_pattern — public API
    // ============================================================================

    /// Match pattern object `pat` against `expr`.  Returns bindings on success.
    template <Pattern Pat, class Expr>
    [[nodiscard]] std::optional<match_result>
    match_pattern(const Pat& pat, const Expr& expr) {
        return detail::do_match(pat, expr);
    }

    /// Match pattern type `Pat` (default-constructed) against `expr`.
    template <class Pat, class Expr>
        requires Pattern<Pat> && std::default_initializable<Pat>
    [[nodiscard]] std::optional<match_result>
    match_pattern(const Expr& expr) {
        return detail::do_match(Pat{}, expr);
    }

    // ============================================================================
    // 8.  rewrite_rule<LHS, RHSBuilder>
    // ============================================================================

    template <Pattern LHS, class RHSBuilder>
    struct rewrite_rule {
        [[no_unique_address]] LHS lhs;
        [[no_unique_address]] RHSBuilder rhs_builder;
        std::string_view name{}; // optional human-readable label

        constexpr rewrite_rule(LHS l, RHSBuilder r, std::string_view nm = {})
            : lhs(std::move(l)), rhs_builder(std::move(r)), name(nm) {}

        /// Try to apply this rule.  Returns the builder's result or nullopt on miss.
        template <class Expr>
        [[nodiscard]] auto try_apply(const Expr& expr) const
            -> std::optional<std::invoke_result_t<const RHSBuilder&, match_result>> {
            auto m = match_pattern(lhs, expr);
            if (!m.has_value()) return std::nullopt;
            return rhs_builder(std::move(*m));
        }
    };

    /// Factory: deduce LHS and RHSBuilder.
    template <Pattern LHS, class RHSBuilder>
    [[nodiscard]] constexpr auto rule(LHS lhs, RHSBuilder rhs) {
        return rewrite_rule<LHS, RHSBuilder>{std::move(lhs), std::move(rhs)};
    }

    /// Named factory: rule("add_zero", lhs, rhs) — attaches a label for
/// diagnostics/tracing. Behaviour is identical to the unnamed overload.
    template <Pattern LHS, class RHSBuilder>
    [[nodiscard]] constexpr auto rule(std::string_view name, LHS lhs, RHSBuilder rhs) {
        return rewrite_rule<LHS, RHSBuilder>{std::move(lhs), std::move(rhs), name};
    }

    // ============================================================================
    // 9.  rule_set<Rules...>
    // ============================================================================

    template <class... Rules>
    struct rule_set {
        std::tuple<Rules...> rules_;

        constexpr explicit rule_set(Rules... rs) : rules_(std::move(rs)...) {}

        /// Try rules in order; return std::any of the first match.
        template <class Expr>
        [[nodiscard]] std::optional<std::any> apply_first(const Expr& expr) const {
            std::optional<std::any> out;
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                (([&]() {
                    if (out.has_value()) return;
                    auto res = std::get < I > (rules_).try_apply(expr);
                    if (res.has_value()) {
                        out = std::move(*res);
                    }
                }()), ...);
            }(std::index_sequence_for < Rules...>{});
            return out;
        }

        /// Apply all matching rules left-to-right, threading the result.
        template <class Expr>
        [[nodiscard]] Expr apply_all(Expr expr) const {
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                (([&]() {
                    auto res = std::get < I > (rules_).try_apply(expr);
                    if (res.has_value()) {
                        using result_t = std::decay_t<decltype(*res)>;
                        if constexpr (std::is_same_v<result_t, std::decay_t<Expr>>) {
                            expr = std::move(*res);
                        }
                    }
                }()), ...);
            }(std::index_sequence_for < Rules...>{});
            return expr;
        }
    };

    /// Factory: make_rule_set(rule1, rule2, ...).
    template <class... Rules>
    [[nodiscard]] constexpr auto make_rule_set(Rules... rs) {
        return rule_set<Rules...>{std::move(rs)...};
    }

    // ============================================================================
    // 10. Pattern DSL builder helpers
    // ============================================================================

    /// Shorthand for a pattern wildcard variable with the given ID.
    template <auto ID>
    inline constexpr pattern_var<ID> pv{};

    /// Shorthand for a literal-value pattern.
    template <auto V>
    inline constexpr literal_pattern<V> lit{};

    template <Pattern L, Pattern R>
    [[nodiscard]] constexpr auto add(L l, R r) {
        return vakya::make_node<vakya::add_tag>(std::move(l), std::move(r));
    }

    template <Pattern L, Pattern R>
    [[nodiscard]] constexpr auto sub(L l, R r) {
        return vakya::make_node<vakya::sub_tag>(std::move(l), std::move(r));
    }

    template <Pattern L, Pattern R>
    [[nodiscard]] constexpr auto mul(L l, R r) {
        return vakya::make_node<vakya::mul_tag>(std::move(l), std::move(r));
    }

    // Named div_ to avoid clash with integer division operator in some contexts.
    template <Pattern L, Pattern R>
    [[nodiscard]] constexpr auto div_(L l, R r) {
        return vakya::make_node<vakya::div_tag>(std::move(l), std::move(r));
    }

    template <Pattern X>
    [[nodiscard]] constexpr auto neg(X x) {
        return vakya::make_node<vakya::neg_tag>(std::move(x));
    }

    // ============================================================================
    // 11. Built-in arithmetic rules
    // ============================================================================

    namespace rules::arithmetic { namespace detail {
            // Patterns are constructed once at program start via inline const.

            // x + 0
            inline const auto _x_add_zero_pat = add(pv<0>, lit<0>);
            // 0 + x
            inline const auto _zero_add_x_pat = add(lit<0>, pv<1>);
            // x * 1
            inline const auto _x_mul_one_pat = mul(pv<0>, lit<1>);
            // 1 * x
            inline const auto _one_mul_x_pat = mul(lit<1>, pv<1>);
            // x * 0
            inline const auto _x_mul_zero_pat = mul(pv<0>, lit<0>);
            // 0 * x
            inline const auto _zero_mul_x_pat = mul(lit<0>, pv<1>);
            // -(-x)
            inline const auto _double_neg_pat = neg(neg(pv<0>));
        } // namespace detail

        /// x + 0 → x   /   0 + x → x
        inline const auto add_zero = make_rule_set(
            rule(detail::_x_add_zero_pat,
                 [](const match_result& m) -> std::optional<std::any> {
                     return m.get<std::any>(std::size_t{0});
                 }),
            rule(detail::_zero_add_x_pat,
                 [](const match_result& m) -> std::optional<std::any> {
                     return m.get<std::any>(std::size_t{1});
                 })
        );

        /// x * 1 → x   /   1 * x → x
        inline const auto mul_one = make_rule_set(
            rule(detail::_x_mul_one_pat,
                 [](const match_result& m) -> std::optional<std::any> {
                     return m.get<std::any>(std::size_t{0});
                 }),
            rule(detail::_one_mul_x_pat,
                 [](const match_result& m) -> std::optional<std::any> {
                     return m.get<std::any>(std::size_t{1});
                 })
        );

        /// x * 0 → 0   /   0 * x → 0
        inline const auto mul_zero = make_rule_set(
            rule(detail::_x_mul_zero_pat,
                 [](const match_result& /*m*/) -> std::optional<std::any> {
                     return std::any{0};
                 }),
            rule(detail::_zero_mul_x_pat,
                 [](const match_result& /*m*/) -> std::optional<std::any> {
                     return std::any{0};
                 })
        );

        /// -(-x) → x
        inline const auto double_neg = make_rule_set(
            rule(detail::_double_neg_pat,
                 [](const match_result& m) -> std::optional<std::any> {
                     return m.get<std::any>(std::size_t{0});
                 })
        );
    } // namespace rules::arithmetic
} // namespace vakya::pattern
