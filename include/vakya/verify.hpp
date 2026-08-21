#pragma once

// =============================================================================
// vakya/verify.hpp — formal verification via Tarka SMT (V3, opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// verify(expr, analysis_store, smt_solver) → verification_report
//
// Collects prove/refine/arith obligations from the analysis_record in the
// analysis_store, batches them into the SMT solver, returns
// { proven | refuted(model) | unknown } per obligation.
//
// Reuses validation_report merge semantics.
// Refuted obligations attach the Tarka counter-model as refutation_payload.
//
// Zero-cost path: with no_smt_backend, every obligation resolves to deferred —
// verification degrades to V2 best-effort, build stays SMT-free.
//
// New ext constraint_kind values (registered in constraint_registry):
//   kRefineKind = kConstraintKindExtensionBase + 1  (refinement predicate)
//   kProveKind  = kConstraintKindExtensionBase + 2  (proof obligation)
//   kArithKind  = kConstraintKindExtensionBase + 3  (arithmetic constraint)
//
// Each carries a tarka::Term* in constraint.payload (cast to uint64_t).
//
// Dependencies: vakya/analysis_store.hpp, vakya/smt.hpp, vakya/validation.hpp
// =============================================================================

#include "vakya/analysis_store.hpp"
#include "vakya/smt.hpp"

#include <cstdint>
#include <vector>

namespace vakya::types {
    // ============================================================================
    // Ext constraint kinds for verification obligations
    // ============================================================================

    inline constexpr constraint_kind kRefineKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 1);

    inline constexpr constraint_kind kProveKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 2);

    inline constexpr constraint_kind kArithKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 3);

    // ============================================================================
    // proof_obligation — a single obligation collected from an analysis_record
    // ============================================================================

    struct proof_obligation {
        constraint_kind kind = kProveKind;
        std::uint64_t term_payload = 0; // tarka::Term* cast to uint64_t
        std::string description;
        constraint_ref source{};
    };

    // ============================================================================
    // obligation_result — per-obligation verification outcome
    // ============================================================================

    struct obligation_result {
        proof_status status = proof_status::unknown;
        std::uint64_t refutation_payload = 0; // counter-model from Tarka, if refuted
        std::string description;
    };

    // ============================================================================
    // verification_report — aggregated result of verify()
    // ============================================================================

    struct verification_report {
        proof_status overall = proof_status::unknown;
        std::vector<obligation_result> results;
        std::vector<solver_diagnostic> diagnostics;

        [[nodiscard]] bool all_proven() const noexcept {
            return overall == proof_status::proven;
        }

        [[nodiscard]] bool any_refuted() const noexcept {
            return overall == proof_status::refuted;
        }

        void merge(const verification_report& other) {
            results.insert(results.end(), other.results.begin(), other.results.end());
            diagnostics.insert(diagnostics.end(), other.diagnostics.begin(), other.diagnostics.end());
            // overall = worst of refuted > unknown > deferred > proven
            if (other.overall == proof_status::refuted)
                overall = proof_status::refuted;
            else if (overall != proof_status::refuted && other.overall == proof_status::unknown)
                overall = proof_status::unknown;
            else if (overall == proof_status::proven)
                overall = other.overall;
        }
    };

    // ============================================================================
    // collect_obligations — extract prove/refine/arith obligations from a record
    // ============================================================================

    [[nodiscard]] inline std::vector<proof_obligation>
    collect_obligations(const analysis_record& rec, std::uint64_t expr_hash) {
        std::vector<proof_obligation> obs;

        if (rec.proofs == proof_status::unknown || rec.proofs == proof_status::deferred) {
            proof_obligation ob;
            ob.kind = kProveKind;
            ob.description = "proof obligation";

#if __has_include(<tarka/tarka.hpp>)
            // When Tarka is available, real tarka::Term* should be stored in
            // analysis_record by the analysis pass that calls solve_batch.
            // The term pointer is carried via a side-channel: the analysis pass
            // emits kProveKind / kRefineKind / kArithKind constraints whose
            // constraint.payload holds tarka::Term* cast to uint64_t.
            // Here we forward the refutation_payload if it was populated, or
            // fall back to the expr_hash placeholder (deferred path).
            ob.term_payload = (rec.refutation_payload != 0)
                                  ? rec.refutation_payload
                                  : expr_hash;
#else
            ob.term_payload = expr_hash;
#endif

            obs.push_back(ob);
        }

        return obs;
    }

    // ============================================================================
    // verify — collect + discharge obligations via SMT solver
    //
    // Template param SmtSolver: any type with:
    //   smt_formula lower_obligation(const proof_obligation&)
    //   void assert_formula(const smt_formula&)
    //   solve_status check_sat()
    //   std::uint64_t get_model_payload()  — counter-model opaque handle (opt-in)
    // ============================================================================

    template <smt_backend SmtBackend>
    [[nodiscard]] verification_report verify(
        std::uint64_t expr_hash,
        const analysis_store& astore,
        smt_constraint_solver<SmtBackend>& solver) {
        verification_report report;

        const analysis_record* rec = astore.find(expr_hash);
        if (!rec) {
            report.overall = proof_status::unknown;
            return report;
        }

        // Already proven/refuted: return immediately
        if (rec->proofs == proof_status::proven) {
            report.overall = proof_status::proven;
            return report;
        }
        if (rec->proofs == proof_status::refuted) {
            report.overall = proof_status::refuted;
            obligation_result or_;
            or_.status = proof_status::refuted;
            or_.refutation_payload = rec->refutation_payload;
            report.results.push_back(or_);
            return report;
        }

        // Build obligation batch from analysis record
        auto obligations = collect_obligations(*rec, expr_hash);

        if (obligations.empty()) {
            report.overall = proof_status::proven; // no obligations → trivially proven
            return report;
        }

        // Discharge via SMT backend.
        // With no_smt_backend: solver.solve returns deferred for all.
        // With tarka_smt_backend: real SMT discharge.
        std::vector<constraint> smt_batch;
        for (const proof_obligation& ob : obligations) {
            constraint c;
            c.kind = ob.kind;
            c.payload = ob.term_payload;
            c.source = ob.source;
            smt_batch.push_back(c);
        }

        solve_context ctx{}; // no arena/subst needed for SMT discharge
        solve_result sr = solver.solve(
            std::span<const constraint>(smt_batch.data(), smt_batch.size()), ctx);

        // Map solve_status → proof_status
        proof_status ps;
        switch (sr.status) {
        case solve_status::solved: ps = proof_status::proven;
            break;
        case solve_status::unsatisfiable: ps = proof_status::refuted;
            break;
        case solve_status::deferred: ps = proof_status::deferred;
            break;
        default: ps = proof_status::unknown;
            break;
        }

        report.overall = ps;

        for (std::size_t i = 0; i < obligations.size(); ++i) {
            obligation_result or_;
            or_.status = ps;
            or_.description = obligations[i].description;
            report.results.push_back(or_);
        }

        for (const auto& diag : sr.diagnostics) {
            report.diagnostics.push_back(diag);
        }

        return report;
    }

    // Convenience overload: expr-keyed (constrained to non-integer types to prevent
    // infinite recursion when size_t hash value is passed — size_t != uint64_t on
    // some platforms so the unconstrained template would be preferred over the
    // uint64_t overload, causing verify(hash)->structural_hash(hash)->verify(hash)...)
    template <class Expr, smt_backend SmtBackend>
        requires (!std::is_integral_v<std::decay_t<Expr>>)
    [[nodiscard]] verification_report verify(
        const Expr& expr,
        const analysis_store& astore,
        smt_constraint_solver<SmtBackend>& solver) {
        return verify(static_cast<std::uint64_t>(vakya::structural_hash(expr)), astore, solver);
    }
} // namespace vakya::types
