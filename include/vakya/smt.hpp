#pragma once

// vakya/smt.hpp — SMT solver interface stub + optional Tarka bridge adapter.
//
// C++23, header-only, no virtual, no macros. Opt-in.
// Namespace: vakya::types
//
// smt_backend<B> concept: assert_formula / check_sat / get_model.
// no_smt_backend: zero-cost default returning deferred.
// tarka_smt_backend<TarkaBackend>: opt-in Tarka bridge (behind
//   __has_include("tarka/tarka.hpp")), living on the Vakya side.
//   Dependency direction: Vakya consumes Tarka, never the reverse.

#include "vakya/constraints.hpp"

#include <concepts>   // std::movable

namespace vakya::types {
    // Minimal formula type (opaque to the stub)
    struct smt_formula {
        std::uint64_t id = 0;
    };

    template <class B>
    concept smt_backend =
        requires(B& b, const smt_formula& f) {
            { b.assert_formula(f) } -> std::same_as<void>;
            { b.check_sat() } -> std::same_as<solve_status>;
        };

    // Zero-cost default stub: all constraints deferred
    struct no_smt_backend {
        void assert_formula(const smt_formula&) noexcept {}
        [[nodiscard]] solve_status check_sat() noexcept { return solve_status::deferred; }
    };

    static_assert(smt_backend<no_smt_backend>);

    // smt_constraint_solver: wraps an smt_backend, handles arithmetic refinement
    // constraints.  With no_smt_backend, all such constraints are deferred (zero
    // penalty). With tarka_smt_backend, the real backend is invoked.
    template <smt_backend Backend = no_smt_backend>
    class smt_constraint_solver {
    public:
        smt_constraint_solver() = default;

        explicit smt_constraint_solver(Backend b)
            requires std::movable<Backend>
            : backend_(std::move(b)) {}

        [[nodiscard]] bool handles(constraint_kind k) const noexcept {
            (void)k;
            return false;
        }

        [[nodiscard]] solve_result solve(std::span<const constraint> batch,
                                         solve_context /*ctx*/) {
            for (const constraint& c : batch) {
                smt_formula f;
                f.id = c.payload;
                backend_.assert_formula(f);
            }
            solve_result r;
            r.status = backend_.check_sat();
            return r;
        }

    private:
        [[no_unique_address]] Backend backend_;
    };

    static_assert(constraint_solver<smt_constraint_solver<no_smt_backend>>);
} // namespace vakya::types

// =============================================================================
// Optional Tarka bridge — pulled only when tarka/tarka.hpp is available.
//
// Dependency direction: Vakya →(consumes)→ Tarka. Tarka has zero Vakya include.
// Guard: __has_include(<tarka/tarka.hpp>)
// =============================================================================

#if __has_include("tarka/tarka.hpp")
#include "tarka/tarka.hpp"

namespace vakya::types {
    // -----------------------------------------------------------------------------
    // tarka_smt_backend<TarkaBackend>
    //
    // Bridges Vakya's smt_backend concept to any tarka::SmtSolverBackend.
    // The opaque smt_formula path is a no-op (satisfies the concept); callers use
    // assert_tarka(Term) to feed real Tarka terms into the underlying solver.
    //
    // Usage:
    //   #include <tarka/backends/z3_backend.hpp>
    //   tarka_smt_backend<tarka::backend::z3_backend> tarka_b;
    //   tarka_b.assert_tarka(my_term);
    //   auto status = tarka_b.check_sat();
    // -----------------------------------------------------------------------------

    template <tarka::SmtSolverBackend TarkaBackend>
    class tarka_smt_backend {
    public:
        tarka_smt_backend() = default;

        explicit tarka_smt_backend(TarkaBackend b)
            requires std::movable < TarkaBackend >
            : backend_(std::move(b)) {}

        // ---- smt_backend concept satisfaction (opaque path — no-op) -------------
        void assert_formula(const smt_formula&) noexcept {}

        [[nodiscard]] solve_status check_sat() {
            auto r = backend_.check_sat();
            if (!r.has_value()) return solve_status::unsatisfiable; // internal error
            switch (r.value()) {
            case tarka::SatResult::Sat: return solve_status::solved;
            case tarka::SatResult::Unsat: return solve_status::unsatisfiable;
            case tarka::SatResult::Unknown: return solve_status::deferred;
            case tarka::SatResult::Deferred: return solve_status::deferred;
            }
            return solve_status::deferred;
        }

        // ---- Tarka native path --------------------------------------------------

        // Assert a Tarka term directly (the real SMT path).
        void assert_tarka(tarka::Term t) {
            backend_.assert_formula(t);
        }

        // Lower a Tarka term to the backend's native type (e.g. z3::expr).
        [[nodiscard]] auto lower_term(tarka::Term t) {
            return backend_.lower_term(t);
        }

        void push(std::uint32_t n = 1) { backend_.push(n); }
        void pop(std::uint32_t n = 1) { backend_.pop(n); }
        void reset() { backend_.reset(); }

        [[nodiscard]] std::expected<tarka::SmtValue, tarka::SmtError>
        get_value(tarka::Term t) {
            return backend_.get_value(t);
        }

        [[nodiscard]] TarkaBackend& backend() noexcept { return backend_; }
        [[nodiscard]] const TarkaBackend& backend() const noexcept { return backend_; }

    private:
        TarkaBackend backend_;
    };

    // Verify concept satisfaction
    static_assert(smt_backend<tarka_smt_backend<tarka::backend::no_solver_backend>>);

    // -----------------------------------------------------------------------------
    // tarka_smt_constraint_solver<TarkaBackend>
    //
    // Specialization of smt_constraint_solver that actually handles arithmetic
    // refinement constraints by delegating to a tarka_smt_backend.
    //
    // Handles constraint_kind::user (and any extension kind >= 1000) that carry a
    // Tarka Term in constraint::payload interpreted as a pointer to a tarka::Term.
    // The caller stores a tarka::Term* cast to uint64_t in constraint.payload.
    //
    // Zero overhead when no such constraints are submitted: handles() gates all
    // dispatch, so the composite_solver hot path skips this solver for non-SMT
    // constraint kinds.
    // -----------------------------------------------------------------------------

    template <tarka::SmtSolverBackend TarkaBackend>
    class tarka_smt_constraint_solver {
    public:
        using backend_t = tarka_smt_backend<TarkaBackend>;

        tarka_smt_constraint_solver() = default;

        explicit tarka_smt_constraint_solver(TarkaBackend b)
            requires std::movable < TarkaBackend >
            : bridge_(std::move(b)) {}

        [[nodiscard]] bool handles(constraint_kind k) const noexcept {
            // Handle user-defined arithmetic refinement constraints (>=kConstraintKindExtensionBase)
            // and the explicit user kind.
            return k == constraint_kind::user ||
                static_cast<std::uint32_t>(k) >= kConstraintKindExtensionBase;
        }

        [[nodiscard]] solve_result solve(std::span<const constraint> batch,
                                         solve_context /*ctx*/) {
            bridge_.push();
            for (const constraint& c : batch) {
                if (!handles(c.kind)) continue;
                // payload encodes a tarka::Term* (caller contract)
                if (c.payload == 0) continue;
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                const auto* tp = reinterpret_cast<const tarka::Term*>(c.payload);
                if (tp && tp->valid()) bridge_.assert_tarka(*tp);
            }

            solve_result r;
            r.status = bridge_.check_sat();
            bridge_.pop();
            return r;
        }

        [[nodiscard]] backend_t& bridge() noexcept { return bridge_; }

    private:
        backend_t bridge_;
    };

    static_assert(constraint_solver<
        tarka_smt_constraint_solver<tarka::backend::no_solver_backend>>);
} // namespace vakya::types

#endif // __has_include("tarka/tarka.hpp")

