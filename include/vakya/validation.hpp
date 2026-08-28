#pragma once

// vakya/validation.hpp — Composable expression validator.
//
// C++23, header-only, no virtual, no macros. Opt-in; not pulled by vakya.hpp.
// Namespace: vakya::types  (extends type_checking.hpp)
//
// validator<Checks...>: variadic [[no_unique_address]] fold of independent check
//   functors each returning validation_result. Aggregate to validation_report.
//   Pay only for composed checks. Zero erasure.

#include "vakya/type_checking.hpp"

#include <vector>
#include <tuple>

namespace vakya::types {
    // ============================================================================
    // validation_report — aggregated results across all checks
    // ============================================================================

    struct validation_report {
        validation_status overall_status = validation_status::success;
        std::vector<solver_diagnostic> diagnostics;

        void merge(const validation_result& r) {
            if (static_cast<std::uint8_t>(r.status) >
                static_cast<std::uint8_t>(overall_status)) {
                overall_status = r.status;
            }
            diagnostics.insert(diagnostics.end(),
                               r.diagnostics.begin(),
                               r.diagnostics.end());
        }

        [[nodiscard]] bool ok() const noexcept {
            return overall_status == validation_status::success;
        }
    };

    // ============================================================================
    // check_fn<C> concept — a check functor callable with (expr_args...) -> validation_result
    // Each check is independent; receives the expression by const-ref.
    // ============================================================================

    template <class C, class Expr>
    concept ValidationCheck =
        requires(const C& c, const Expr& expr) {
            { c(expr) } -> std::same_as<validation_result>;
        };

    // ============================================================================
    // validator<Checks...>
    // ============================================================================

    template <class... Checks>
    class validator {
    public:
        explicit validator(Checks... cs) : checks_(std::move(cs)...) {}

        template <class Expr>
        [[nodiscard]] validation_report validate(const Expr& expr) const {
            validation_report report;
            apply_checks(expr, report, std::index_sequence_for < Checks...>{});
            return report;
        }

    private:
        template <class Expr, std::size_t... Is>
        void apply_checks(const Expr& expr,
                          validation_report& report,
                          std::index_sequence<Is...>) const {
            (report.merge(std::get < Is > (checks_)(expr)), ...);
        }

        std::tuple<Checks...> checks_;
    };

    // Factory — deduces Checks...
    template <class... Checks>
    [[nodiscard]] auto make_validator(Checks... cs) {
        return validator<Checks...>{std::move(cs)...};
    }
} // namespace vakya::types
