#pragma once
// =============================================================================
// medha/adapters/tarka.hpp — Tarka SMT discharge adapter
//
// C++23, header-only, no virtual, no macros.
//
// Uses medha::proof_status (owned by medha/commit.hpp, no Vakya dependency).
// Wraps vakya::proof_obligation + tarka_smt_backend<z3_backend> when present.
// Builds proof_result in the Medha adapter, not in Tarka.
//
// Hard rules:
//   unknown / unsupported / timeout / deferred are NEVER treated as proven.
//   refuted → diagnostic or hard error per policy.
//   no_smt_backend → all obligations deferred; runtime validation stands.
//
// Discharge path:
//   obligations: medha_proof_obligation (Medha-owned)
//   solver: vakya::types::tarka_smt_backend<tarka::backend::z3_backend> (optional)
// =============================================================================

#include "medha/commit.hpp"

#if __has_include("vakya/verify.hpp")
#  include "vakya/verify.hpp"
#  define MEDHA_HAS_VAKYA_VERIFY 1
#endif

#if __has_include("vakya/smt.hpp")
#  include "vakya/smt.hpp"
#  define MEDHA_HAS_VAKYA_SMT 1
#endif

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace medha::adapters::tarka {
    // ============================================================================
    // assumption_set — logical assumptions for a proof
    // ============================================================================

    struct assumption_set {
        std::vector<std::uint64_t> term_hashes; // structural hashes of assumption terms
    };

    // ============================================================================
    // counterexample — populated on refuted (§23)
    // ============================================================================

    struct counterexample {
        std::string description;
        std::uint64_t term_hash = 0;
    };

    // ============================================================================
    // artifact_compatibility — AOT compatibility metadata (§24)
    // ============================================================================

    struct artifact_compatibility {
        std::uint32_t dialect_version = 1;
        std::uint64_t resource_schema_hash = 0;
    };

    // ============================================================================
    // proof_result — rich result built in the Medha adapter (§23)
    // ============================================================================

    struct proof_result {
        proof_status status = proof_status::deferred;
        std::string_view solver_name{}; // "z3"
        std::string_view solver_version{};
        std::uint64_t constraint_hash = 0;
        std::chrono::nanoseconds timeout{};
        assumption_set assumptions{};
        std::optional<counterexample> model{}; // populated on refuted
        artifact_compatibility compatibility{};
    };

    // ============================================================================
    // obligation_kind — what Medha asks Tarka to prove (§23)
    // ============================================================================

    enum class obligation_kind : std::uint8_t {
        invariant_preservation = 0, // post-state satisfies resource invariant
        key_disjointness = 1, // two write sets provably disjoint
        commutativity = 2, // two transactions commute
    };

    // ============================================================================
    // medha_proof_obligation — a Medha-level obligation over resource state
    // ============================================================================

    struct medha_proof_obligation {
        obligation_kind kind{};
        std::uint64_t constraint_hash = 0; // structural hash
        std::string description;
    };

    // ============================================================================
    // tarka_discharge — discharge Medha obligations through vakya/tarka
    // ============================================================================

#ifdef MEDHA_HAS_VAKYA_VERIFY

    template <class SmtBackend = vakya::types::no_smt_backend>
    [[nodiscard]] std::vector<proof_result>
    discharge(const std::vector<medha_proof_obligation>& obligations,
              SmtBackend& backend = {}) {
        std::vector<proof_result> results;
        results.reserve(obligations.size());

        for (const auto& ob : obligations) {
            proof_result r{};
            r.constraint_hash = ob.constraint_hash;
            r.solver_name = "z3";

            if constexpr (std::is_same_v<SmtBackend, vakya::types::no_smt_backend>) {
                r.status = proof_status::deferred;
            }
            else {
                // With a real backend: map solve_status to proof_status.
                vakya::types::smt_formula formula{ob.constraint_hash};
                backend.assert_formula(formula);
                auto sat = backend.check_sat();
                using ss = vakya::types::solve_status;
                if (sat == ss::solved) r.status = proof_status::proven;
                else if (sat == ss::unsatisfiable) r.status = proof_status::refuted;
                else if (sat == ss::deferred) r.status = proof_status::deferred;
                else r.status = proof_status::unknown;
            }
            results.push_back(std::move(r));
        }
        return results;
    }

#else

    // No vakya/verify.hpp: all obligations deferred
    [[nodiscard]] inline std::vector<proof_result>
    discharge(const std::vector<medha_proof_obligation>& obligations) {
        std::vector<proof_result> results;
        for (const auto& ob : obligations) {
            proof_result r{};
            r.constraint_hash = ob.constraint_hash;
            r.status = proof_status::deferred;
            results.push_back(r);
        }
        return results;
    }

#endif  // MEDHA_HAS_VAKYA_VERIFY
} // namespace medha::adapters::tarka
