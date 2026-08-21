#pragma once

// vakya/rewrite.hpp — Guarded rewrite rules over Vākya pattern::rewrite_rule.
//
// C++23, header-only, no virtual, no macros. Opt-in; not pulled by vakya.hpp.
// Namespace: vakya::types  (extends type_checking.hpp)
//
// guarded_rule<Pattern, Rewrite, Guard>: superset of pattern::rewrite_rule.
//   Structural match first (reuse pattern::match_pattern),
//   then evaluate Guard(match_result, type_env, solver) -> bool,
//   only then apply the rewrite.
//
//   Guard = always_true_guard: plain structural rule — backward-compatible.
//
// Domain packs via existing rule_registry (categories algebra/tensor/statistics/physics).
// Zero change to pattern.hpp.

#include "vakya/type_checking.hpp"
#include "vakya/pattern.hpp"
#include "vakya/rule_registry.hpp"

#include <optional>

namespace vakya::types {
    // ============================================================================
    // Guard concept: Guard(match_result, type_environment&, Solver&) -> bool
    // ============================================================================

    template <class G, class Solver>
    concept GuardFn =
        requires(const G& guard,
                 const vakya::pattern::match_result& m,
                 type_environment& env,
                 Solver& solver) {
            { guard(m, env, solver) } -> std::convertible_to<bool>;
        };

    // Default guard: always fires (makes guarded_rule backward-compatible with plain rule)
    struct always_true_guard {
        template <class Solver>
        [[nodiscard]] constexpr bool
        operator()(const vakya::pattern::match_result& /*m*/,
                   type_environment& /*env*/,
                   Solver& /*solver*/) const noexcept {
            return true;
        }
    };

    // ============================================================================
    // guarded_rule<Pattern, Rewrite, Guard>
    // ============================================================================

    template <vakya::pattern::Pattern PatternT,
              class RewriteBuilder,
              class Guard = always_true_guard>
    class guarded_rule {
    public:
        [[no_unique_address]] PatternT pattern;
        [[no_unique_address]] RewriteBuilder rhs_builder;
        [[no_unique_address]] Guard guard;
        std::string_view name{};

        constexpr guarded_rule(PatternT p, RewriteBuilder r, Guard g = Guard{},
                               std::string_view nm = {})
            : pattern(std::move(p)),
              rhs_builder(std::move(r)),
              guard(std::move(g)),
              name(nm) {}

        // Try to apply: structural match, then guard, then rewrite.
        template <class Expr, class Solver>
        [[nodiscard]] auto try_apply(const Expr& expr,
                                     type_environment& env,
                                     Solver& solver) const
            -> std::optional<std::invoke_result_t<const RewriteBuilder&,
                                                  vakya::pattern::match_result>> {
            static_assert(GuardFn<Guard, Solver>, "Guard must satisfy GuardFn<G, Solver>");

            auto m = vakya::pattern::match_pattern(pattern, expr);
            if (!m.has_value()) return std::nullopt;

            if (!guard(*m, env, solver)) return std::nullopt;

            return rhs_builder(*m);
        }

        // Fallback: try_apply without env/solver (uses always_true_guard semantics)
        template <class Expr>
        [[nodiscard]] auto try_apply(const Expr& expr) const
            -> std::optional<std::invoke_result_t<const RewriteBuilder&,
                                                  vakya::pattern::match_result>> {
            auto m = vakya::pattern::match_pattern(pattern, expr);
            if (!m.has_value()) return std::nullopt;
            // No guard check in the non-env overload — behaves like plain rewrite_rule
            return rhs_builder(*m);
        }
    };

    // ============================================================================
    // Factories
    // ============================================================================

    template <vakya::pattern::Pattern P, class R>
    [[nodiscard]] constexpr auto guarded(P pattern, R rhs) {
        return guarded_rule<P, R, always_true_guard>{
            std::move(pattern), std::move(rhs), always_true_guard{}
        };
    }

    template <vakya::pattern::Pattern P, class R, class G>
    [[nodiscard]] constexpr auto guarded(P pattern, R rhs, G guard) {
        return guarded_rule<P, R, G>{
            std::move(pattern), std::move(rhs), std::move(guard)
        };
    }

    template <vakya::pattern::Pattern P, class R, class G>
    [[nodiscard]] constexpr auto guarded(std::string_view name, P pattern, R rhs, G guard) {
        return guarded_rule<P, R, G>{
            std::move(pattern), std::move(rhs), std::move(guard), name
        };
    }
} // namespace vakya::types
