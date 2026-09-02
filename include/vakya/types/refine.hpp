#pragma once

// =============================================================================
// vakya/types/refine.hpp — refinement subtyping + bounds-check elision (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// A refinement type is a base type τ narrowed by a predicate P over its value:
//   { v : τ | P(v) }          e.g.  { n : int | 0 <= n && n < len }
// Subtyping is implication: { v:τ | P } <: { v:τ | Q }  iff  P ⇒ Q (same base τ).
// That implication is an SMT obligation (kRefineSubKind, ext band → Tarka bridge);
// with no_smt_backend it is `deferred`, never a spurious failure.
//
// The predicate rides as a term payload (tarka::Term* cast to uint64_t, matching
// verify.hpp's proof_obligation convention) so this header pulls in no Tarka type.
// The base type is a plain type_ref; a refinement is interned as a constructor node
// tagged refinement_type_tag with the base as child[0] and the predicate in
// payload_hash — no type_kind enum edit (users don't pay for a wider enum).
//
// Bounds-check elision: once P ⇒ (0 <= i < len) is proven, the fact is recorded as
// a bit in analysis_record so Crank can DROP the runtime check. elision_bit
// constants define that convention over analysis_record::features (a free bit
// vector already in the record) — Vākya sets the bit; the consumer reads it.
//
// Dependencies: vakya/types.hpp, vakya/constraints.hpp, vakya/analysis_store.hpp
// =============================================================================

#include "vakya/types.hpp"
#include "vakya/constraints.hpp"
#include "vakya/analysis_store.hpp"

#include <cstdint>

namespace vakya::types {
    // ============================================================================
    // refinement_type_tag — ext-band constructor tag for { v : τ | P }.
    // ============================================================================

    struct refinement_type_tag {};

    template <>
    struct type_descriptor<refinement_type_tag> {
        static constexpr std::uint32_t stable_id = kTypeKindExtensionBase + 51u;
        static constexpr std::uint8_t arity = 1; // child[0] = base type τ
        static constexpr std::string_view symbol = "Refine";
    };

    // ============================================================================
    // intern_refinement — { base | predicate_term }.
    // predicate_term is a tarka::Term* cast to uint64_t (0 = trivially-true refine).
    // ============================================================================

    [[nodiscard]] inline type_ref
    intern_refinement(type_arena& arena, type_ref base, std::uint64_t predicate_term) {
        type_node n;
        n.kind = type_kind::constructor;
        n.descriptor_stable_id = type_descriptor<refinement_type_tag>::stable_id;
        n.children.push_back(base);
        n.payload_hash = predicate_term;
        return arena.intern(std::move(n));
    }

    [[nodiscard]] inline bool is_refinement(const type_arena& arena, type_ref t) noexcept {
        const type_node* n = arena.get(t);
        return n && n->descriptor_stable_id == type_descriptor<refinement_type_tag>::stable_id;
    }

    // Base type τ of a refinement (null if `t` is not a refinement).
    [[nodiscard]] inline type_ref refinement_base(const type_arena& arena, type_ref t) noexcept {
        const type_node* n = arena.get(t);
        if (!n || !is_refinement(arena, t) || n->children.empty()) return type_ref{};
        return n->children[0];
    }

    // Predicate term payload (0 if none / not a refinement).
    [[nodiscard]] inline std::uint64_t
    refinement_predicate(const type_arena& arena, type_ref t) noexcept {
        const type_node* n = arena.get(t);
        return (n && is_refinement(arena, t)) ? n->payload_hash : 0;
    }

    // ============================================================================
    // kRefineSubKind — ext-band constraint "refinement A <: refinement B" i.e.
    // P_A ⇒ P_B over a shared base. Routes to the SMT band (implication). extension band +25.
    // ============================================================================

    inline constexpr constraint_kind kRefineSubKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 25);

    // The two refinement type_refs travel as operands; the solver reads their
    // predicate payloads and emits the implication P_sub ⇒ P_sup to Tarka.
    [[nodiscard]] inline constraint
    refine_subtype_obligation(type_ref sub, type_ref sup) {
        constraint c;
        c.kind = kRefineSubKind;
        c.operands.push_back(sub);
        c.operands.push_back(sup);
        return c;
    }

    // ============================================================================
    // syntactic_subtype — the cheap fast path decidable WITHOUT SMT:
    //   - identical refinements (same base, same predicate term) → true
    //   - sup has the trivially-true predicate (0)                → true (anything ⇒ ⊤)
    // Everything else returns false: NOT "not a subtype", just "not decided here" —
    // the caller then emits refine_subtype_obligation for the SMT band.
    // ============================================================================

    [[nodiscard]] inline bool
    syntactic_subtype(const type_arena& arena, type_ref sub, type_ref sup) noexcept {
        if (!is_refinement(arena, sub) || !is_refinement(arena, sup)) return false;
        if (refinement_base(arena, sub) != refinement_base(arena, sup)) return false;
        if (refinement_predicate(arena, sup) == 0) return true; // ⇒ ⊤
        return refinement_predicate(arena, sub) == refinement_predicate(arena, sup);
    }

    // ============================================================================
    // elision_bit — convention over analysis_record::features. When Vākya proves the
    // refinement discharges a runtime guard, it sets the matching bit; the consumer
    // reads it to DROP the check. These are the only feature bits this refinement layer owns; other
    // bits stay free for observability.
    // ============================================================================

    inline constexpr std::uint64_t kElisionBoundsCheck = 1ULL << 0; // 0<=i<len proven
    inline constexpr std::uint64_t kElisionNullCheck = 1ULL << 1; // non-null proven
    inline constexpr std::uint64_t kElisionOverflow = 1ULL << 2; // no-overflow proven

    // Set / query an elision bit on a record's feature vector.
    inline void mark_elision(analysis_record& rec, std::uint64_t bit) noexcept {
        rec.features |= bit;
    }

    [[nodiscard]] inline bool
    has_elision(const analysis_record& rec, std::uint64_t bit) noexcept {
        return (rec.features & bit) == bit;
    }
} // namespace vakya::types
