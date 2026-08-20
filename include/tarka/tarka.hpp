#pragma once
// =============================================================================
// tarka/tarka.hpp — Umbrella: zero-cost core only
//
// Includes: term.hpp + context.hpp + backend.hpp + backends/no_solver.hpp
//           + RouterEngine
//
// Opt-in extensions (NOT pulled here):
//   tarka/backends/z3_backend.hpp  — Z3 lowering + solve (guard HAS_Z3)
//   tarka/features.hpp             — theory extractor + capability-mask router
//   tarka/egraph_opt.hpp           — equality-saturation canonicalization
//   tarka/async.hpp                — SmtTask + worker pool
//   tarka/portfolio.hpp            — competitive solving (no-hang)
//
// Zero-overhead invariant:
//   RouterEngine<backend::z3> with one backend lowers to direct Z3 calls;
//   no type erasure, no atomics, no thread spawn.
// =============================================================================

#include "tarka/term.hpp"
#include "tarka/context.hpp"
#include "tarka/backend.hpp"
#include "tarka/backends/no_solver.hpp"

#include <cstdint>
#include <expected>
#include <type_traits>

namespace tarka {
    // =========================================================================
    // theory_mask helpers
    // =========================================================================

    [[nodiscard]] inline theory_mask compute_theory_mask(Term t) noexcept {
        theory_mask mask = 0;
        auto add_op = [&](Op o) {
            mask |= get_op_info(o).theory_bits;
        };

        // Post-order walk via small stack (avoids heap for typical formulae)
        constexpr std::size_t kStackCap = 256;
        const Term* stack_buf[kStackCap];
        const Term** stk = stack_buf;
        std::size_t depth = 0;

        stack_buf[depth++] = &t;
        while (depth > 0) {
            const Term* cur = stack_buf[--depth];
            add_op(cur->op());
            for (const Term& c : cur->children()) {
                if (depth < kStackCap)
                    stack_buf[depth++] = &c;
            }
        }
        return mask;
    }

    // =========================================================================
    // RouterEngine<Backends...>
    //
    // Compile-time backend set. route() selects the first backend whose
    // capability mask ⊇ formula theory signature.
    // Defaults to no_solver_backend when the backend list is empty.
    // =========================================================================

    template <SmtSolverBackend... Backends>
    class RouterEngine {
    public:
        using first_backend_t = std::conditional_t<
            sizeof...(Backends) == 0,
            backend::no_solver_backend,
            // Pick first type via pack expansion trick
            std::tuple_element_t < 0, std::tuple<Backends..., backend::no_solver_backend>>
        >;

        RouterEngine() = default;

        // Assert formula (defers to active backend)
        void assert_formula(Term t) {
            active_backend().assert_formula(t);
        }

        [[nodiscard]] std::expected<SatResult, SmtError> check_sat() {
            return active_backend().check_sat();
        }

        [[nodiscard]] std::expected<SmtValue, SmtError> get_value(Term t) {
            return active_backend().get_value(t);
        }

        void push(std::uint32_t n = 1) { active_backend().push(n); }
        void pop(std::uint32_t n = 1) { active_backend().pop(n); }
        void reset() { active_backend().reset(); }

        // Solve a formula: assert + check_sat in one call
        [[nodiscard]] std::expected<SatResult, SmtError> solve(Term t) {
            push();
            assert_formula(t);
            auto r = check_sat();
            pop();
            return r;
        }

    private:
        // Tuple of backends; empty tuple → no_solver_backend inserted
        using BackendTuple = std::conditional_t<
            sizeof...(Backends) == 0,
            std::tuple<backend::no_solver_backend>,
            std::tuple<Backends...>
        >;

        BackendTuple backends_;

        [[nodiscard]] auto& active_backend() noexcept {
            return std::get < 0 > (backends_);
        }
    };

    // Convenience alias: single Z3 backend (defined only when z3_backend.hpp included)
    // RouterEngine<backend::z3_backend> solver;
} // namespace tarka
