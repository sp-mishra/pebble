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

#include "containers/dynamic/SmallVector.hpp"

#include <cstdint>
#include <expected>
#include <span>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tarka {
    // =========================================================================
    // theory_mask helpers
    // =========================================================================

    [[nodiscard]] inline theory_mask compute_theory_mask(Term t) noexcept {
        theory_mask mask = 0;

        // Iterative DAG walk. SmallVector keeps typical formulae stack-resident
        // (SBO) but grows to the heap for deep terms — no silent truncation. A
        // visited set dedups shared sub-DAGs so each node's op is folded once.
        containers::dynamic::SmallVector<const Term*, 512 * sizeof(const Term*)> stack;
        std::unordered_set<const TermImpl*> visited;
        visited.reserve(64);

        stack.push_back(&t);
        while (!stack.empty()) {
            const Term* cur = stack.back();
            stack.pop_back();
            if (!cur->valid()) continue;
            if (!visited.insert(cur->ptr()).second) continue;
            mask |= get_op_info(cur->op()).theory_bits;
            for (const Term& c : cur->children()) stack.push_back(&c);
        }
        return mask;
    }

    // =========================================================================
    // RouterEngine<Backends...>
    //
    // Compile-time backend set. Selects the first backend whose capability mask
    // ⊇ the asserted formula's theory signature; falls back to backend[0] when
    // no backend fully covers (best-effort) or when only one backend exists.
    // Defaults to no_solver_backend when the backend list is empty.
    // =========================================================================

    template <SmtSolverBackend... Backends>
    class RouterEngine {
    public:
        using first_backend_t = std::conditional_t<
            sizeof...(Backends) == 0,
            backend::no_solver_backend,
            std::tuple_element_t < 0, std::tuple<Backends..., backend::no_solver_backend>>
        >;

        RouterEngine() = default;

        // Assert formula. Refines backend selection by the formula's theory mask.
        void assert_formula(Term t) {
            select_for(compute_theory_mask(t));
            dispatch([&](auto& b) { b.assert_formula(t); });
        }

        [[nodiscard]] std::expected<SatResult, SmtError> check_sat() {
            return dispatch([&](auto& b) { return b.check_sat(); });
        }

        [[nodiscard]] std::expected<SmtValue, SmtError> get_value(Term t) {
            return dispatch([&](auto& b) { return b.get_value(t); });
        }

        void push(std::uint32_t n = 1) { dispatch([&](auto& b) { b.push(n); }); }
        void pop(std::uint32_t n = 1) { dispatch([&](auto& b) { b.pop(n); }); }

        void reset() {
            active_idx_ = 0;
            dispatch([&](auto& b) { b.reset(); });
        }

        [[nodiscard]] std::expected<SatResult, SmtError>
        check_sat_assuming(std::span<const Term> assumptions) {
            return dispatch([&](auto& b) { return b.check_sat_assuming(assumptions); });
        }

        [[nodiscard]] std::vector<Term> get_unsat_core() const {
            return dispatch([&](const auto& b) { return b.get_unsat_core(); });
        }

        // Solve a formula: assert + check_sat in one call.
        [[nodiscard]] std::expected<SatResult, SmtError> solve(Term t) {
            select_for(compute_theory_mask(t));
            push();
            dispatch([&](auto& b) { b.assert_formula(t); });
            auto r = check_sat();
            pop();
            return r;
        }

        // Which backend the last selection landed on (for tests / diagnostics).
        [[nodiscard]] std::size_t active_index() const noexcept { return active_idx_; }

        // Solve a batch of independent formulae, keeping the backend context warm
        // across queries (incremental caching lives in the backend, item 22).
        [[nodiscard]] std::vector<std::expected<SatResult, SmtError>>
        solve_batch(std::span<const Term> terms) {
            std::vector<std::expected<SatResult, SmtError>> results;
            results.reserve(terms.size());
            for (Term t : terms) results.push_back(solve(t));
            return results;
        }

    private:
        using BackendTuple = std::conditional_t<
            sizeof...(Backends) == 0,
            std::tuple<backend::no_solver_backend>,
            std::tuple<Backends...>
        >;

        static constexpr std::size_t kBackendCount = std::tuple_size_v<BackendTuple>;

        BackendTuple backends_;
        std::size_t active_idx_ = 0;

        // Pick the first backend covering `mask`; leave active_idx_ at 0 when none
        // fully covers (backend[0] is the conservative default / superset backend).
        void select_for(theory_mask mask) noexcept {
            active_idx_ = first_covering(mask, std::make_index_sequence < kBackendCount >
            {});
        }

        template <std::size_t... Is>
        [[nodiscard]] static std::size_t first_covering(theory_mask mask,
                                                        std::index_sequence<Is...>) noexcept {
            std::size_t chosen = 0;
            bool found = false;
            // Fold over backends in order; first whose caps ⊇ mask wins.
            (void)((!found &&
                    ((std::tuple_element_t < Is, BackendTuple > ::capabilities() & mask) == mask)
                        ? (chosen = Is, found = true)
                        : false) || ...);
            return chosen;
        }

        // Invoke fn on the tuple element at active_idx_ (runtime index → static call).
        template <typename Fn>
        decltype(auto) dispatch(Fn&& fn) {
            return dispatch_impl(std::forward<Fn>(fn), std::make_index_sequence < kBackendCount >
            {});
        }

        template <typename Fn>
        decltype(auto) dispatch(Fn&& fn) const {
            return dispatch_impl(std::forward<Fn>(fn), std::make_index_sequence < kBackendCount >
            {});
        }

        template <typename Fn, std::size_t... Is>
        decltype(auto) dispatch_impl(Fn&& fn, std::index_sequence<Is...>) {
            using R = std::invoke_result_t<Fn&, std::tuple_element_t < 0, BackendTuple>&>;
            if constexpr (std::is_void_v<R>) {
                // Branchless jump table: O(1) dispatch via constexpr vtable
                using Trampoline = void(*)(BackendTuple&, Fn&);
                static constexpr Trampoline vtable[] = {
                    +[](BackendTuple& t, Fn& f) { f(std::get < Is > (t)); }...
                };
                vtable[active_idx_](backends_, fn);
            }
            else {
                using Trampoline = R(*)(BackendTuple&, Fn&);
                static constexpr Trampoline vtable[] = {
                    +[](BackendTuple& t, Fn& f) -> R { return f(std::get < Is > (t)); }...
                };
                return vtable[active_idx_](backends_, fn);
            }
        }

        template <typename Fn, std::size_t... Is>
        decltype(auto) dispatch_impl(Fn&& fn, std::index_sequence<Is...>) const {
            using R = std::invoke_result_t<Fn&, const std::tuple_element_t<0, BackendTuple>&>;
            if constexpr (std::is_void_v<R>) {
                using Trampoline = void(*)(const BackendTuple&, Fn&);
                static constexpr Trampoline vtable[] = {
                    +[](const BackendTuple& t, Fn& f) { f(std::get < Is > (t)); }...
                };
                vtable[active_idx_](backends_, fn);
            }
            else {
                using Trampoline = R(*)(const BackendTuple&, Fn&);
                static constexpr Trampoline vtable[] = {
                    +[](const BackendTuple& t, Fn& f) -> R { return f(std::get < Is > (t)); }...
                };
                return vtable[active_idx_](backends_, fn);
            }
        }
    };

    // Convenience alias: single Z3 backend (defined only when z3_backend.hpp included)
    // RouterEngine<backend::z3_backend> solver;
} // namespace tarka
