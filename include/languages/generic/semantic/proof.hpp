#pragma once

// generic/proof.hpp — Generic proof/discharge engine (delegates to vakya/verify.hpp).
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// Wraps the vakya::types verification stack so the generic layer and all language
// frontends share one real SMT-backed proof engine rather than a stub.
//
// vakya/verify.hpp provides (namespace vakya::types):
//   proof_status {unknown, proven, refuted, deferred}
//   proof_obligation, obligation_result, verification_report
//   collect_obligations(analysis_record&, expr_hash) → vector<proof_obligation>
//   verify<SmtBackend>(expr_hash, analysis_store, solver) → verification_report
//   Zero-cost path: no_smt_backend → all obligations resolve to deferred.
//   Real path: tarka_smt_backend<TarkaBackend> → Z3 discharge via Tarka.
//
// lang:: adds:
//   verify_policy   — off | assume | check | paranoid (frontend policy knob)
//   proof_construct_kind — proof | assert_ (whether runtime guard is allowed)
//   obligation_record   — lang-level obligation with policy + outcome; bridges
//                         into vakya::types::proof_obligation for actual discharge.
//   assumption_context  — in-scope assumptions + active verify_policy.
//   discharge_driver    — builds vakya obligations, calls vakya::types::verify(),
//                         interprets three-way outcome against verify_policy.
//
// Usage (no SMT — deferred path):
//   lang::discharge_driver<vakya::types::no_smt_backend> driver;
//   // outcomes will be proof_status::deferred / unknown.
//
// Usage (with Tarka/Z3):
//   #if __has_include(<tarka/tarka.hpp>)
//   using backend = vakya::types::tarka_smt_backend<tarka::RouterEngine<tarka::backend::z3>>;
//   lang::discharge_driver<backend> driver;
//   #endif

#include "vakya/verify.hpp"
#include "languages/generic/core/diagnostics.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lang {
    // =========================================================================
    // Re-export vakya proof_status so lang:: callers don't need to spell
    // vakya::types:: everywhere.
    // =========================================================================

    using proof_status = vakya::types::proof_status;
    using proof_obligation = vakya::types::proof_obligation;
    using obligation_result = vakya::types::obligation_result;
    using verification_report = vakya::types::verification_report;
    using analysis_store = vakya::types::analysis_store;
    using analysis_record = vakya::types::analysis_record;

    // =========================================================================
    // verify_policy — frontend policy knob for how aggressively to discharge
    // =========================================================================

    enum class verify_policy : std::uint8_t {
        off, // skip all proof attempts; treat all as deferred
        assume, // trust annotations; mark all proven without SMT call
        check, // attempt SMT proof; deferred/unknown → insert runtime guard
        paranoid, // attempt SMT proof; deferred/unknown → compile error
    };

    [[nodiscard]] constexpr std::string_view to_string(verify_policy p) noexcept {
        switch (p) {
        case verify_policy::off: return "off";
        case verify_policy::assume: return "assume";
        case verify_policy::check: return "check";
        case verify_policy::paranoid: return "paranoid";
        }
        return "unknown";
    }

    // =========================================================================
    // proof_construct_kind — assert_ always inserts runtime guard when unknown
    // =========================================================================

    enum class proof_construct_kind : std::uint8_t {
        proof, // derivable property — can be statically discharged
        assert_, // runtime assertion — always inserts guard when not proven
    };

    // =========================================================================
    // discharge_outcome — lang-level result of discharging one obligation
    // =========================================================================

    struct discharge_outcome {
        proof_status status = proof_status::unknown;
        bool guard_inserted = false; // runtime guard was emitted
        std::string description;
        std::string diagnostic_code;
    };

    // =========================================================================
    // obligation_record — lang-level obligation with policy + vakya bridge
    // =========================================================================

    struct obligation_record {
        std::string description;
        proof_construct_kind kind = proof_construct_kind::proof;
        verify_policy policy = verify_policy::check;
        std::string proof_term; // symbolic hint for the SMT encoder
        std::uint64_t expr_hash = 0; // vakya analysis_store key
        discharge_outcome outcome; // filled by discharge_driver::discharge()
    };

    // =========================================================================
    // assumption_context — in-scope assumptions + active policy
    // =========================================================================

    struct assumption_context {
        std::vector<std::string> assumptions;
        verify_policy policy = verify_policy::check;
    };

    // =========================================================================
    // proof_diagnostic_kind
    // =========================================================================

    struct proof_diag_kind {
        enum class kind : std::uint8_t {
            refuted, // LANG-PROOF-001: obligation statically false
            guard_inserted, // LANG-PROOF-002: runtime guard inserted
            deferred, // LANG-PROOF-003: no SMT backend; proof deferred
            paranoid_fail, // LANG-PROOF-004: paranoid policy + not proven = error
        };

        kind value = kind::refuted;

        constexpr proof_diag_kind() = default;
        constexpr proof_diag_kind(kind k) noexcept : value(k) {}

        [[nodiscard]] static constexpr std::string_view to_code(proof_diag_kind k) noexcept {
            switch (k.value) {
            case kind::refuted: return "LANG-PROOF-001";
            case kind::guard_inserted: return "LANG-PROOF-002";
            case kind::deferred: return "LANG-PROOF-003";
            case kind::paranoid_fail: return "LANG-PROOF-004";
            }
            return "LANG-PROOF-000";
        }

        [[nodiscard]] constexpr bool operator==(const proof_diag_kind&) const noexcept = default;
    };

    using proof_diagnostic = lang_diagnostic<proof_diag_kind>;

    // =========================================================================
    // discharge_driver<SmtBackend>
    //
    // Drives obligation discharge using vakya::types::verify() under the hood.
    //   SmtBackend = vakya::types::no_smt_backend   → deferred (zero-cost)
    //   SmtBackend = tarka_smt_backend<...>         → real Z3 discharge
    //
    // Interprets three-way proof_status against verify_policy:
    //   proven   → drop runtime guard
    //   refuted  → compile error always (regardless of policy)
    //   deferred/unknown + check    → insert runtime guard
    //   deferred/unknown + paranoid → compile error
    //   deferred/unknown + assume   → treat as proven
    //   deferred/unknown + off      → skip, no guard
    // =========================================================================

    template <vakya::types::smt_backend SmtBackend = vakya::types::no_smt_backend>
    class discharge_driver {
    public:
        struct discharge_result {
            std::vector<discharge_outcome> outcomes;
            std::vector<proof_diagnostic> diagnostics;

            [[nodiscard]] bool ok() const noexcept {
                for (const auto& d : diagnostics)
                    if (d.level >= severity::error) return false;
                return true;
            }
        };

        [[nodiscard]] discharge_result
        discharge(std::string_view fn_name,
                  std::vector<obligation_record>& obligations,
                  const assumption_context& actx,
                  const analysis_store& astore,
                  vakya::types::smt_constraint_solver<SmtBackend>& solver) const {
            discharge_result result;

            for (auto& ob : obligations) {
                // Call vakya::types::verify() for real SMT discharge.
                vakya::types::verification_report vr =
                    vakya::types::verify(ob.expr_hash, astore, solver);

                const verify_policy effective = (ob.policy == verify_policy::check &&
                                                    actx.policy == verify_policy::paranoid)
                                                    ? verify_policy::paranoid
                                                    : ob.policy;

                discharge_outcome out;
                out.description = ob.description;

                switch (effective) {
                case verify_policy::off:
                    out.status = proof_status::unknown;
                    out.guard_inserted = false;
                    break;

                case verify_policy::assume:
                    out.status = proof_status::proven;
                    out.guard_inserted = false;
                    break;

                case verify_policy::check:
                case verify_policy::paranoid: {
                    if (vr.overall == proof_status::refuted) {
                        out.status = proof_status::refuted;
                        out.guard_inserted = false;
                        out.diagnostic_code = "LANG-PROOF-001";
                        proof_diagnostic d;
                        d.kind = proof_diag_kind{proof_diag_kind::kind::refuted};
                        d.symbol = std::string(fn_name);
                        d.message = "obligation refuted: '" + ob.description + "'";
                        d.level = severity::error;
                        result.diagnostics.push_back(std::move(d));
                    }
                    else if (vr.overall == proof_status::proven) {
                        out.status = proof_status::proven;
                        out.guard_inserted = false;
                    }
                    else {
                        // deferred or unknown
                        out.status = proof_status::unknown;
                        if (effective == verify_policy::paranoid) {
                            out.guard_inserted = false;
                            out.diagnostic_code = "LANG-PROOF-004";
                            proof_diagnostic d;
                            d.kind = proof_diag_kind{proof_diag_kind::kind::paranoid_fail};
                            d.symbol = std::string(fn_name);
                            d.message = "paranoid: could not prove '" + ob.description + "'";
                            d.level = severity::error;
                            result.diagnostics.push_back(std::move(d));
                        }
                        else {
                            out.guard_inserted = true;
                            out.diagnostic_code = "LANG-PROOF-002";
                            proof_diagnostic d;
                            d.kind = proof_diag_kind{proof_diag_kind::kind::guard_inserted};
                            d.symbol = std::string(fn_name);
                            d.message = "runtime guard inserted for '" + ob.description + "'";
                            d.level = severity::note;
                            result.diagnostics.push_back(std::move(d));
                        }
                    }
                    break;
                }
                }

                ob.outcome = out;
                result.outcomes.push_back(std::move(out));
            }

            return result;
        }
    };
} // namespace lang
