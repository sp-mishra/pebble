#pragma once
// =============================================================================
// tarka/native/ids.hpp — Strong index types for the native solver
//
// Namespace:  tarka::native
// Provides:   Var, Lit, ClauseRef, TheoryVar, AtomId — strongly-typed indices.
//
// Design:
//   - No virtual, no macros. Trivially copyable 32-bit handles.
//   - Var: propositional variable index (0-based).
//   - Lit: literal = (var << 1) | sign; sign bit 1 == negated.
//   - ClauseRef: index into the clause database.
//   - TheoryVar: index into a theory solver's variable space.
//   - AtomId: index into the atom registry (theory/Boolean atom).
// =============================================================================

#include <cstdint>
#include <functional>
#include <limits>

namespace tarka::native {
    // -------------------------------------------------------------------------
    // Var
    // -------------------------------------------------------------------------

    enum class Var : std::uint32_t {};

    inline constexpr Var kNullVar{std::numeric_limits<std::uint32_t>::max()};

    [[nodiscard]] constexpr std::uint32_t var_index(Var v) noexcept {
        return static_cast<std::uint32_t>(v);
    }

    [[nodiscard]] constexpr Var make_var(std::uint32_t i) noexcept { return Var{i}; }

    // -------------------------------------------------------------------------
    // Lit — literal packed as (var << 1) | sign
    // -------------------------------------------------------------------------

    enum class Lit : std::uint32_t {};

    inline constexpr Lit kNullLit{std::numeric_limits<std::uint32_t>::max()};

    [[nodiscard]] constexpr Lit make_lit(Var v, bool negated) noexcept {
        return Lit{(var_index(v) << 1u) | (negated ? 1u : 0u)};
    }

    [[nodiscard]] constexpr Var lit_var(Lit l) noexcept {
        return Var{static_cast<std::uint32_t>(l) >> 1u};
    }

    [[nodiscard]] constexpr bool lit_sign(Lit l) noexcept {
        return (static_cast<std::uint32_t>(l) & 1u) != 0u;
    }

    [[nodiscard]] constexpr Lit lit_neg(Lit l) noexcept {
        return Lit{static_cast<std::uint32_t>(l) ^ 1u};
    }

    [[nodiscard]] constexpr std::uint32_t lit_index(Lit l) noexcept {
        return static_cast<std::uint32_t>(l);
    }

    // -------------------------------------------------------------------------
    // ClauseRef / TheoryVar / AtomId
    // -------------------------------------------------------------------------

    enum class ClauseRef : std::uint32_t {};

    inline constexpr ClauseRef kNullClause{std::numeric_limits<std::uint32_t>::max()};

    [[nodiscard]] constexpr std::uint32_t clause_index(ClauseRef c) noexcept {
        return static_cast<std::uint32_t>(c);
    }

    enum class TheoryVar : std::uint32_t {};

    [[nodiscard]] constexpr std::uint32_t tvar_index(TheoryVar v) noexcept {
        return static_cast<std::uint32_t>(v);
    }

    enum class AtomId : std::uint32_t {};

    inline constexpr AtomId kNullAtom{std::numeric_limits<std::uint32_t>::max()};

    [[nodiscard]] constexpr std::uint32_t atom_index(AtomId a) noexcept {
        return static_cast<std::uint32_t>(a);
    }
} // namespace tarka::native

// std::hash for use as keys
namespace std {
    template <>
    struct hash<tarka::native::Var> {
        [[nodiscard]] std::size_t operator()(tarka::native::Var v) const noexcept {
            return tarka::native::var_index(v);
        }
    };

    template <>
    struct hash<tarka::native::Lit> {
        [[nodiscard]] std::size_t operator()(tarka::native::Lit l) const noexcept {
            return tarka::native::lit_index(l);
        }
    };
} // namespace std
