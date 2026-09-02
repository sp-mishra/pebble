#pragma once

// =============================================================================
// vakya/types/value_param.hpp — const-generic value params + SIMD/tile synthesis
//                               (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// A value_param is a type-level term standing for a compile-time VALUE (a lane
// count, a tile size, a static extent) rather than a type. It is interned as a
// constructor node tagged value_param_type_tag; the literal value rides in
// type_node::payload_hash, and a symbolic value_param carries a var_id instead.
// No type_kind enum edit — the ext-band stable_id distinguishes it (users should
// not pay for a wider enum they don't use).
//
// unify_value(a, b): two value_params unify iff both literal-equal, or one is a
// var (binds), else a kValueEqKind SMT obligation is emitted for the symbolic case.
//
// synthesize_simd_width / synthesize_tile: derive a SIMD lane count and loop-tile
// size from a static extent under a width_policy. Nothing is hardcoded to an ISA —
// the policy carries the max width + tile bytes, overridable via analyze_options.
//
// Dependencies: vakya/types.hpp, vakya/constraints.hpp
// =============================================================================

#include "vakya/types.hpp"
#include "vakya/constraints.hpp"

#include <cstdint>

namespace vakya::types {
    // ============================================================================
    // value_param_type_tag — ext-band constructor tag for value-level params.
    // ============================================================================

    struct value_param_type_tag {};

    template <>
    struct type_descriptor<value_param_type_tag> {
        static constexpr std::uint32_t stable_id = kTypeKindExtensionBase + 50u;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "ValueParam";
    };

    // ============================================================================
    // intern_value_param — literal compile-time value.
    // ============================================================================

    [[nodiscard]] inline type_ref intern_value_param(type_arena& arena, std::uint64_t value) {
        type_node n;
        n.kind = type_kind::constructor;
        n.descriptor_stable_id = type_descriptor<value_param_type_tag>::stable_id;
        n.payload_hash = value;
        n.var_id = kInvalidTypeVarId; // literal — not symbolic
        return arena.intern(std::move(n));
    }

    // Symbolic value_param (a value variable, resolved later / by SMT).
    [[nodiscard]] inline type_ref intern_value_var(type_arena& arena, type_var_id vid) {
        type_node n;
        n.kind = type_kind::constructor;
        n.descriptor_stable_id = type_descriptor<value_param_type_tag>::stable_id;
        n.payload_hash = 0;
        n.var_id = vid;
        return arena.intern(std::move(n));
    }

    [[nodiscard]] inline bool is_value_param(const type_arena& arena, type_ref t) noexcept {
        const type_node* n = arena.get(t);
        return n && n->descriptor_stable_id == type_descriptor<value_param_type_tag>::stable_id;
    }

    [[nodiscard]] inline bool value_param_is_literal(const type_arena& arena, type_ref t) noexcept {
        const type_node* n = arena.get(t);
        return n && n->var_id == kInvalidTypeVarId;
    }

    // Literal value (0 if not a literal value_param).
    [[nodiscard]] inline std::uint64_t value_param_literal(const type_arena& arena,
                                                           type_ref t) noexcept {
        const type_node* n = arena.get(t);
        if (!n || !is_value_param(arena, t) || n->var_id != kInvalidTypeVarId) return 0;
        return n->payload_hash;
    }

    // ============================================================================
    // kValueEqKind — ext-band constraint "value_param A == value_param B" (routes
    // to unify class; symbolic residual falls to SMT band). extension band +22.
    // ============================================================================

    inline constexpr constraint_kind kValueEqKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 22);

    enum class value_unify_result : std::uint8_t {
        equal = 0, // both literal and equal
        not_equal = 1, // both literal and distinct
        deferred = 2, // at least one symbolic — emit SMT obligation
    };

    // ============================================================================
    // unify_value — decide two value_params structurally where possible.
    // ============================================================================

    [[nodiscard]] inline value_unify_result
    unify_value(const type_arena& arena, type_ref a, type_ref b) noexcept {
        if (!is_value_param(arena, a) || !is_value_param(arena, b))
            return value_unify_result::deferred;
        const bool la = value_param_is_literal(arena, a);
        const bool lb = value_param_is_literal(arena, b);
        if (la && lb) {
            return value_param_literal(arena, a) == value_param_literal(arena, b)
                       ? value_unify_result::equal
                       : value_unify_result::not_equal;
        }
        return value_unify_result::deferred;
    }

    [[nodiscard]] inline constraint make_value_eq_constraint(type_ref a, type_ref b) {
        constraint c;
        c.kind = kValueEqKind;
        c.operands.push_back(a);
        c.operands.push_back(b);
        return c;
    }

    // ============================================================================
    // width_policy — target SIMD/tile parameters. NO hardcoded ISA width: callers
    // seed this from analyze_options / a target profile. Defaults are conservative
    // and portable (128-bit vector, 4 KiB tile) — a documented default, not an
    // assumed hardware fact.
    // ============================================================================

    struct width_policy {
        std::uint16_t max_lane_bits = 128; // widest usable vector register, in bits
        std::uint16_t elem_bits = 32; // element width, in bits
        std::uint32_t tile_bytes = 4096; // working-set tile budget, in bytes
        std::uint16_t max_width = 64; // hard cap on synthesized lane count

        // Lanes that fit in one vector register for this element width.
        [[nodiscard]] constexpr std::uint16_t lanes() const noexcept {
            if (elem_bits == 0) return 1;
            const std::uint16_t l = static_cast<std::uint16_t>(max_lane_bits / elem_bits);
            return l == 0 ? 1 : l;
        }
    };

    // ============================================================================
    // synthesize_simd_width — largest power-of-two lane count that (a) divides the
    // static extent and (b) fits the policy. Returns 0 when the extent is symbolic
    // or too small to vectorize (extent < 2).
    // ============================================================================

    [[nodiscard]] inline std::uint16_t
    synthesize_simd_width(std::uint64_t static_extent, const width_policy& policy = {}) noexcept {
        if (static_extent < 2) return 0;
        std::uint16_t w = policy.lanes();
        if (w > policy.max_width) w = policy.max_width;
        // Shrink to a power of two that divides the extent.
        while (w > 1) {
            if ((static_extent % w) == 0) return w;
            w = static_cast<std::uint16_t>(w >> 1);
        }
        return 0; // no lane count > 1 divides the extent
    }

    // Overload: pull the extent from a literal value_param.
    [[nodiscard]] inline std::uint16_t
    synthesize_simd_width(const type_arena& arena, type_ref extent,
                          const width_policy& policy = {}) noexcept {
        if (!value_param_is_literal(arena, extent)) return 0; // symbolic → no synthesis
        return synthesize_simd_width(value_param_literal(arena, extent), policy);
    }

    // ============================================================================
    // synthesize_tile — loop-tile size = tile_bytes / element bytes, clamped to the
    // extent. 0 when the extent is symbolic.
    // ============================================================================

    [[nodiscard]] inline std::uint16_t
    synthesize_tile(std::uint64_t static_extent, const width_policy& policy = {}) noexcept {
        if (static_extent == 0) return 0;
        const std::uint32_t elem_bytes = policy.elem_bits == 0 ? 1u : (policy.elem_bits / 8u);
        const std::uint32_t budget = elem_bytes == 0
                                         ? policy.tile_bytes
                                         : policy.tile_bytes / elem_bytes;
        std::uint64_t tile = budget == 0 ? 1u : budget;
        if (tile > static_extent) tile = static_extent;
        if (tile > 0xFFFFu) tile = 0xFFFFu;
        return static_cast<std::uint16_t>(tile);
    }

    [[nodiscard]] inline std::uint16_t
    synthesize_tile(const type_arena& arena, type_ref extent,
                    const width_policy& policy = {}) noexcept {
        if (!value_param_is_literal(arena, extent)) return 0;
        return synthesize_tile(value_param_literal(arena, extent), policy);
    }
} // namespace vakya::types
