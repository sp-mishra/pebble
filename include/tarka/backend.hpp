#pragma once
// =============================================================================
// tarka/backend.hpp — Solver Facet Concepts
//
// Namespace:  tarka
// Provides:
//   SmtSolverBackend<B>  — concept for a static zero-erasure SMT backend
//   CancelableBackend<B> — refinement: adds check_sat_cancelable
//
// Design:
//   Static path: fully typed, no virtual, no type erasure. RouterEngine
//   requires SmtSolverBackend. PortfolioEngine requires CancelableBackend.
//
//   The dynamic C-ABI boundary (slot_map + generational_handle thunks) is a
//   separate opt-in header; a c_abi_backend adapter lifts the C ABI back into
//   the SmtSolverBackend concept — same static/dynamic split Lithe uses.
// =============================================================================

#include "tarka/term.hpp"

#include <concepts>
#include <cstdint>
#include <expected>
#include <stop_token>

namespace tarka {
    // =========================================================================
    // SmtSolverBackend<B>
    // =========================================================================

    template <typename B>
    concept SmtSolverBackend = requires(B& b, const B& cb, Sort s, Term t, std::uint32_t n) {
        // Associated types
        typename B::native_term_t;
        typename B::native_sort_t;

        // Theory capability mask — static constexpr
        { B::capabilities() } -> std::convertible_to<theory_mask>;

        // Sort lowering
        { b.lower_sort(s) } -> std::same_as<typename B::native_sort_t>;

        // Term lowering
        { b.lower_term(t) } -> std::same_as<typename B::native_term_t>;

        // Assertion / solve
        { b.assert_formula(t) } -> std::same_as<void>;
        { b.check_sat() } -> std::same_as<std::expected<SatResult, SmtError>>;

        // Model extraction
        { b.get_value(t) } -> std::same_as<std::expected<SmtValue, SmtError>>;

        // Scope management
        { b.push(n) } -> std::same_as<void>;
        { b.pop(n) } -> std::same_as<void>;
        { b.reset() } -> std::same_as<void>;
    };

    // =========================================================================
    // CancelableBackend<B>
    // =========================================================================

    template <typename B>
    concept CancelableBackend =
        SmtSolverBackend<B> &&
        requires(B& b, Term t, std::stop_token tok) {
            {
                b.check_sat_cancelable(t, tok)
            } ->
            std::same_as<std::expected<SatResult, SmtError>>;
        };
} // namespace tarka
