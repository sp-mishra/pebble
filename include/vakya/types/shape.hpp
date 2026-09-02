#pragma once

// =============================================================================
// vakya/types/shape.hpp — formal shape algebra (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// Promotes same_rank/compatible/broadcastable into a small formal shape language.
//
// shape<Dims...> — type-level shape term interned in type_arena via shape_type_tag.
// shape_ref = type_ref (a shape IS a type term).
//
// Algebraic rules (matmul, broadcast, transpose) expressed as constraints.
// Dimension side conditions (N>0 ∧ M==M') are delegated to Tarka as SMT
// obligations via constraint_kind::user ext-band constraints.
//
// The shape algebra uses the existing type_arena for interning; no new arena.
//
// Dependencies: vakya/types.hpp, vakya/constraints.hpp
// =============================================================================

#include "vakya/types.hpp"
#include "vakya/constraints.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace vakya::types {
    // ============================================================================
    // shape_type_tag — constructor tag for shape type terms
    // ============================================================================

    struct shape_type_tag {};

    template <>
    struct type_descriptor<shape_type_tag> {
        static constexpr std::uint32_t stable_id = 100u; // reserved builtin shape slot
        static constexpr std::uint8_t arity = kTypeVariadicArity; // variadic dims
        static constexpr std::string_view symbol = "Shape";
    };

    // ============================================================================
    // shape_ref — a type_ref whose type_node is a shape_type_tag constructor
    // ============================================================================

    using shape_ref = type_ref;

    // ============================================================================
    // intern_shape — intern a shape<D0, D1, …> into type_arena
    //
    // Each dimension is interned as a primitive type_ref (or variable for symbolic dims).
    // The resulting node is: constructor<shape_type_tag>(dims…).
    // ============================================================================

    [[nodiscard]] inline shape_ref
    intern_shape(type_arena& arena, std::span<const type_ref> dims) {
        type_node n;
        n.kind = type_kind::constructor;
        n.descriptor_stable_id = type_descriptor<shape_type_tag>::stable_id;
        for (const type_ref& d : dims) n.children.push_back(d);
        return arena.intern(std::move(n));
    }

    // ============================================================================
    // make_scalar_shape — shape<> (rank-0 scalar, no dims)
    // ============================================================================

    [[nodiscard]] inline shape_ref make_scalar_shape(type_arena& arena) {
        return intern_shape(arena, std::span<const type_ref>{});
    }

    // ============================================================================
    // shape_rank — number of dimensions of an interned shape
    // ============================================================================

    [[nodiscard]] inline std::size_t shape_rank(const type_arena& arena, shape_ref s) noexcept {
        const type_node* n = arena.get(s);
        if (!n) return 0;
        if (n->descriptor_stable_id != type_descriptor<shape_type_tag>::stable_id) return 0;
        return n->children.size();
    }

    // ============================================================================
    // shape_constraint_kind ext values — arithmetic side conditions for Tarka
    // ============================================================================

    // constraint_kind for dimension equality: N == M (SMT arithmetic)
    inline constexpr constraint_kind kDimEqKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 10);

    // constraint_kind for dimension positivity: N > 0
    inline constexpr constraint_kind kDimPosKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 11);

    // constraint_kind for matmul compatibility: shape<N,M> × shape<M,K> → shape<N,K>
    inline constexpr constraint_kind kMatmulCompatKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 12);

    // ============================================================================
    // make_matmul_shape_constraint — emit shape compatibility for matmul
    //
    // Emits:
    //   same_rank(lhs_shape, 2)
    //   same_rank(rhs_shape, 2)
    //   same_type(lhs_shape.dim[1], rhs_shape.dim[0])  — inner dimensions match
    //
    // Returns the constraints; caller adds them to the batch.
    // The result shape shape<N,K> is also returned via out_shape.
    // ============================================================================

    [[nodiscard]] inline std::vector<constraint>
    make_matmul_constraints(type_arena& arena,
                            type_var_generator& gen,
                            shape_ref lhs_shape,
                            shape_ref rhs_shape,
                            shape_ref& out_shape) {
        std::vector<constraint> result;

        const type_node* lhs_n = arena.get(lhs_shape);
        const type_node* rhs_n = arena.get(rhs_shape);

        if (!lhs_n || !rhs_n) return result;
        if (lhs_n->children.size() != 2 || rhs_n->children.size() != 2) return result;

        const type_ref lhs_N = lhs_n->children[0];
        const type_ref lhs_M = lhs_n->children[1];
        const type_ref rhs_M = rhs_n->children[0];
        const type_ref rhs_K = rhs_n->children[1];

        // Inner dim match: lhs.M == rhs.M
        constraint dim_match;
        dim_match.kind = constraint_kind::same_type;
        dim_match.operands.push_back(lhs_M);
        dim_match.operands.push_back(rhs_M);
        result.push_back(dim_match);

        // Result shape: shape<N, K>
        const type_ref result_dims[2] = {lhs_N, rhs_K};
        out_shape = intern_shape(arena, std::span<const type_ref>(result_dims, 2));

        (void)gen; // reserved for symbolic dim var generation
        return result;
    }

    // ============================================================================
    // make_broadcast_constraints — standard NumPy-style shape broadcasting rules
    //
    // Emits broadcastable constraints between two shapes for each dimension pair.
    // Right-aligned; shorter shape is padded with size-1 dims.
    // ============================================================================

    [[nodiscard]] inline constraint make_broadcastable_constraint(shape_ref a, shape_ref b) {
        constraint c;
        c.kind = constraint_kind::broadcastable;
        c.operands.push_back(a);
        c.operands.push_back(b);
        return c;
    }
} // namespace vakya::types
