#pragma once

// =============================================================================
// vakya/proof_carrying.hpp — proof-carrying optimization (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// A rewrite_certificate is evidence that a rewrite lhs → rhs preserves semantics:
//   { lhs_hash, rhs_hash, equivalence_term, status }
// where equivalence_term is a tarka::Term* (cast to uint64_t, matching verify.hpp)
// asserting lhs ≡ rhs, and status records how that was discharged:
//   proven   — witnessed by construction (e-graph congruence) or Tarka-proven
//   deferred — no SMT backend / not yet checked (rewrite may still be applied,
//              flagged so a consumer can re-verify or fall back)
//   refuted  — the equivalence was falsified (rewrite MUST NOT be applied)
//
// certify_rewrite builds a certificate from an equivalence-checking policy.
// verified_rewrite_engine gates application on the policy: an e-graph witness is
// proven-by-construction (congruence closure already merged the classes); otherwise
// the equivalence goes to SMT; a `deferred` verdict still applies the rewrite but
// records cert_id so downstream can see it is unverified.
//
// The certificate is interned in a certificate_arena (slot_map); its 1-based index
// is stored in analysis_record::cert_id (0 = no certificate).
//
// Dependencies: vakya/analysis_store.hpp, vakya/constraints.hpp,
//               containers/associative/slot_map.hpp
// =============================================================================

#include "vakya/analysis_store.hpp"
#include "vakya/constraints.hpp"
#include "containers/associative/slot_map.hpp"
#include "containers/handle/generational_handle.hpp"

#include <concepts>
#include <cstdint>

namespace vakya::types {
    // ============================================================================
    // rewrite_certificate — evidence lhs ≡ rhs.
    // ============================================================================

    struct rewrite_certificate {
        std::uint64_t lhs_hash = 0;
        std::uint64_t rhs_hash = 0;
        std::uint64_t equivalence_term = 0; // tarka::Term* as uint64 (0 = none)
        proof_status status = proof_status::unknown;
    };

    // Certificate handle (own phantom tag; 1-based like every optimization-layer arena).
    struct certificate_tag {};
    using certificate_ref = containers::generational_handle<certificate_tag, std::uint32_t>;

    // ============================================================================
    // certificate_arena — stores certificates; cert_id in the record is the index.
    // ============================================================================

    class certificate_arena {
    public:
        certificate_arena() = default;

        [[nodiscard]] certificate_ref add(const rewrite_certificate& cert) {
            return store_.insert(cert);
        }

        [[nodiscard]] const rewrite_certificate* get(certificate_ref r) const noexcept {
            return store_.find(r);
        }

        // Resolve the bare cert_id stored in analysis_record (1-based; 0 = none).
        [[nodiscard]] const rewrite_certificate* by_id(std::uint32_t cert_id) const noexcept {
            if (cert_id == 0) return nullptr;
            return store_.find(certificate_ref{cert_id, 1});
        }

        [[nodiscard]] std::size_t size() const noexcept { return store_.size(); }

    private:
        containers::slot_map<rewrite_certificate, certificate_ref> store_;
    };

    // ============================================================================
    // kEquivCertKind — ext-band constraint "lhs ≡ rhs" (routes to egraph class:
    // congruence closure; residual falls to SMT band). extension band +26.
    // ============================================================================

    inline constexpr constraint_kind kEquivCertKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 26);

    // ============================================================================
    // EquivalenceChecker — a policy deciding lhs ≡ rhs. Returns a proof_status:
    //   proven   → apply freely
    //   deferred → apply but flag (no backend / unchecked)
    //   refuted  → do not apply
    // An e-graph engine models this by returning `proven` when both hashes already
    // sit in the same e-class (proven-by-construction); an SMT-backed checker returns
    // proven/refuted/deferred from Tarka. no_smt_backend → deferred.
    // ============================================================================

    template <class C>
    concept EquivalenceChecker = requires(C c, std::uint64_t lhs, std::uint64_t rhs,
                                          std::uint64_t term) {
        { c.check(lhs, rhs, term) } -> std::same_as<proof_status>;
    };

    // Built-in checker: e-class-witnessed equivalence. Two expressions are proven
    // equivalent iff they hash-collide into the same class per a caller-supplied
    // `same_class(lhs, rhs)` predicate; otherwise deferred (never spuriously refuted).
    template <class SameClass>
    struct egraph_equivalence_checker {
        SameClass same_class; // bool(uint64 lhs, uint64 rhs)

        [[nodiscard]] proof_status
        check(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t /*term*/) const {
            if (lhs == rhs) return proof_status::proven; // identical AST
            return same_class(lhs, rhs) ? proof_status::proven : proof_status::deferred;
        }
    };

    // ============================================================================
    // certify_rewrite — build a certificate via an EquivalenceChecker.
    // ============================================================================

    template <EquivalenceChecker C>
    [[nodiscard]] rewrite_certificate
    certify_rewrite(std::uint64_t lhs_hash, std::uint64_t rhs_hash,
                    std::uint64_t equivalence_term, const C& checker) {
        rewrite_certificate cert;
        cert.lhs_hash = lhs_hash;
        cert.rhs_hash = rhs_hash;
        cert.equivalence_term = equivalence_term;
        cert.status = checker.check(lhs_hash, rhs_hash, equivalence_term);
        return cert;
    }

    // ============================================================================
    // rewrite_policy — how strict the engine is about applying an unproven rewrite.
    // ============================================================================

    enum class rewrite_policy : std::uint8_t {
        proven_only = 0,   // apply iff status == proven (safest)
        allow_deferred = 1, // apply proven + deferred (flagged); reject only refuted
    };

    struct rewrite_decision {
        bool applied = false;
        certificate_ref cert{}; // certificate recorded for this attempt
        proof_status status = proof_status::unknown;
    };

    // ============================================================================
    // verified_rewrite_engine — gates a rewrite on its certificate.
    //
    // certify_and_apply builds the certificate, stores it, and decides application
    // per the policy. `apply` is a caller-supplied side effect (mutate the IR / arena)
    // invoked ONLY when the decision permits it. The cert_id is returned so the caller
    // can stamp it into the affected analysis_record(s). Refuted rewrites are never
    // applied; deferred rewrites are applied only under allow_deferred, always flagged.
    // ============================================================================

    class verified_rewrite_engine {
    public:
        explicit verified_rewrite_engine(certificate_arena& arena,
                                         rewrite_policy policy = rewrite_policy::proven_only) noexcept
            : arena_(&arena), policy_(policy) {}

        template <EquivalenceChecker C, class Apply>
            requires std::invocable<Apply>
        rewrite_decision
        certify_and_apply(std::uint64_t lhs_hash, std::uint64_t rhs_hash,
                          std::uint64_t equivalence_term, const C& checker, Apply&& apply) {
            const rewrite_certificate cert =
                certify_rewrite(lhs_hash, rhs_hash, equivalence_term, checker);
            rewrite_decision d;
            d.status = cert.status;
            d.cert = arena_->add(cert);

            const bool permitted =
                cert.status == proof_status::proven ||
                (cert.status == proof_status::deferred &&
                 policy_ == rewrite_policy::allow_deferred);

            if (permitted) {
                std::forward<Apply>(apply)();
                d.applied = true;
            }
            return d;
        }

    private:
        certificate_arena* arena_ = nullptr;
        rewrite_policy policy_ = rewrite_policy::proven_only;
    };
} // namespace vakya::types
