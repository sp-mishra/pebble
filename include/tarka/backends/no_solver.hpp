#pragma once
// =============================================================================
// tarka/backends/no_solver.hpp — Zero-cost default backend
//
// Namespace:  tarka::backend
// Provides:   no_solver_backend — models SmtSolverBackend, all ops deferred
//
// Mirrors vakya::types::no_smt_backend. RouterEngine<> defaults to this so
// Tarka builds and non-solver tests pass even with BUILD_Z3=OFF.
// =============================================================================

#include "tarka/backend.hpp"

#include <expected>
#include <cstdint>

namespace tarka::backend {
    struct no_solver_backend {
        struct native_term_t {};

        struct native_sort_t {};

        [[nodiscard]] static constexpr theory_mask capabilities() noexcept {
            return theory_bit(theory_family::all);
        }

        [[nodiscard]] native_sort_t lower_sort(Sort) noexcept { return {}; }
        [[nodiscard]] native_term_t lower_term(Term) noexcept { return {}; }

        void assert_formula(Term) noexcept {}

        [[nodiscard]] std::expected<SatResult, SmtError> check_sat() noexcept {
            return SatResult::Deferred;
        }

        [[nodiscard]] std::expected<SmtValue, SmtError> get_value(Term) noexcept {
            return std::unexpected(SmtError{
                SmtError::Kind::Unsupported,
                "no_solver_backend: model extraction not available"
            });
        }

        void push(std::uint32_t) noexcept {}
        void pop(std::uint32_t) noexcept {}
        void reset() noexcept {}

        [[nodiscard]] std::expected<SatResult, SmtError>
        check_sat_cancelable(Term, std::stop_token) noexcept {
            return SatResult::Deferred;
        }
    };

    static_assert(SmtSolverBackend<no_solver_backend>);
    static_assert(CancelableBackend<no_solver_backend>);
} // namespace tarka::backend
