#pragma once
// =============================================================================
// tarka/native/model.hpp — Model extraction (SAT + theory -> SmtValue)
//
// Namespace:  tarka::native
// Provides:   model_builder — reads a satisfying assignment out of the solved
//             cdcl_solver / atom_registry / theory combination and converts a
//             queried Term into a tarka::SmtValue.
//
// Supported Sorts:
//   - Bool     => bool (from SAT variable assignment)
//   - BitVec   => bv_value (from theory_bv bit-blasted model)
//   - Int      => std::int64_t (from theory_lra / theory_dl potentials)
//   - Real     => rational (from theory_lra / theory_dl exact potentials)
//   - Function / UF => string or representative ID
//
// Design:
//   - No virtual, no macros. Header-only, C++23.
// =============================================================================

#include "containers/numeric/exact_rational.hpp"
#include "tarka/native/atom_registry.hpp"
#include "tarka/native/cdcl_solver.hpp"
#include "tarka/native/ids.hpp"
#include "tarka/term.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <variant>

namespace tarka::native {
    // Forward declarations of theory engines in tarka::native
    class theory_bv;
    class theory_lra;
    class theory_dl;
    class theory_uf;
    class theory_array;

    class model_builder {
    public:
        model_builder(const cdcl_solver& sat, const atom_registry& reg) noexcept
            : sat_(sat), reg_(reg) {}

        // Boolean value of a formula-level term
        [[nodiscard]] std::optional<bool> bool_value(Term t) const {
            if (t.op() == Op::True) return true;
            if (t.op() == Op::False) return false;

            // Direct check in atom registry
            for (std::size_t i = 0; i < reg_.num_atoms(); ++i) {
                const AtomId a{static_cast<std::uint32_t>(i)};
                const AtomInfo& info = reg_.atom(a);
                if (info.term.ptr() == t.ptr()) {
                    const LBool v = sat_.value(info.var);
                    if (v == LBool::Undef) return std::nullopt;
                    return v == LBool::True;
                }
            }
            return std::nullopt;
        }

        // Generic value evaluation over the theory combination
        template <class Combination>
        [[nodiscard]] std::expected<SmtValue, SmtError> eval(Term t, const Combination& theories) const {
            if (!t.valid()) {
                return std::unexpected(SmtError{SmtError::Kind::Internal, "invalid term queried for model value"});
            }

            const Sort s = t.sort();
            if (!s.valid()) {
                return std::unexpected(SmtError{SmtError::Kind::Internal, "term has invalid sort"});
            }

            switch (s.kind()) {
            case SortKind::Bool: {
                if (auto b = bool_value(t)) {
                    return SmtValue{*b};
                }
                return std::unexpected(SmtError{SmtError::Kind::Unsupported, "no boolean value found in model"});
            }
            case SortKind::BitVec: {
                if constexpr (Combination::template has_theory<theory_bv>) {
                    const auto& bv_th = theories.template get<theory_bv>();
                    if (auto val = bv_th.get_value(t)) {
                        return SmtValue{*val};
                    }
                }
                return std::unexpected(SmtError{SmtError::Kind::Unsupported, "no bitvector value found in model"});
            }
            case SortKind::Int: {
                // Try LRA first
                if constexpr (Combination::template has_theory<theory_lra>) {
                    const auto& lra_th = theories.template get<theory_lra>();
                    if (auto val = lra_th.get_value(t)) {
                        if (auto iv = val->floor().to_int64()) {
                            return SmtValue{*iv};
                        }
                    }
                }
                // Try DL
                if constexpr (Combination::template has_theory<theory_dl>) {
                    const auto& dl_th = theories.template get<theory_dl>();
                    if (auto val = dl_th.get_value(t)) {
                        if (auto iv = val->floor().to_int64()) {
                            return SmtValue{*iv};
                        }
                    }
                }
                return std::unexpected(SmtError{SmtError::Kind::Unsupported, "no integer value found in model"});
            }
            case SortKind::Real: {
                // Try LRA first
                if constexpr (Combination::template has_theory<theory_lra>) {
                    const auto& lra_th = theories.template get<theory_lra>();
                    if (auto val = lra_th.get_value(t)) {
                        if (auto pair = val->to_int64_pair()) {
                            return SmtValue{rational{pair->first, pair->second}};
                        }
                    }
                }
                // Try DL
                if constexpr (Combination::template has_theory<theory_dl>) {
                    const auto& dl_th = theories.template get<theory_dl>();
                    if (auto val = dl_th.get_value(t)) {
                        if (auto pair = val->to_int64_pair()) {
                            return SmtValue{rational{pair->first, pair->second}};
                        }
                    }
                }
                return std::unexpected(SmtError{SmtError::Kind::Unsupported, "no real value found in model"});
            }
            default: {
                return std::unexpected(SmtError{SmtError::Kind::Unsupported, "unsupported sort for model extraction"});
            }
            }
        }

    private:
        const cdcl_solver& sat_;
        const atom_registry& reg_;
    };
} // namespace tarka::native
