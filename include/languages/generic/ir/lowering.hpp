#pragma once

// languages/generic/ir/lowering.hpp — Single funnel: event_log → ir_module.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// lower_events<KE, EP>(log, leaf_fns) → ir_module<KE,EP>
//   Builds an ir_module from an event_log using the same two-pass stack builder
//   as green_arena::build. Leaf materialization injected via LeafFns callbacks.
//
// LeafFns concept:
//   leaf_fns.span(token_index)  → byte_span
//   leaf_fns.hash(token_index)  → std::uint64_t
//
// Mapping table (realized in later stages, documented now):
//   samasa green_tree<SK>    ≡  ir_module<SK, monostate>
//   samasa event_stream<SK>  ≡  event_log<SK, samasa_diag_code>
//   crank  ast_arena         →  ir_module<CrankKind, type_ref> (Stage 8)
//   vakya  type trees        →  ir_module<VakyaKind, type_ref> with handle_store (Stage 9)

#include "ir_module.hpp"
#include "../tree/event_log.hpp"
#include "../tree/green_arena.hpp"

namespace lang {

    template <class KindEnum,
              class ExtPayload = std::monostate,
              class DiagCode   = std::uint16_t,
              class LeafFns>
    [[nodiscard]] ir_module<KindEnum, ExtPayload> lower_events(
        const event_log<KindEnum, DiagCode>& log,
        LeafFns&&                             leaf_fns)
    {
        // Delegate to green_arena::build for the two-pass stack algorithm, then
        // project into ir_module (same layout, same hashes).
        auto arena = green_arena<KindEnum>::build(
            log,
            [&](std::uint32_t idx) { return leaf_fns.span(idx); },
            [&](std::uint32_t idx) { return leaf_fns.hash(idx); });

        ir_module<KindEnum, ExtPayload> mod;

        // Transfer nodes from arena into mod, preserving ids and child linkage.
        const std::uint32_t n = arena.size();
        mod = ir_module<KindEnum, ExtPayload>{};  // reset

        // We rebuild using the arena's structure rather than storing two copies.
        // For Stage 0 correctness: walk arena nodes in id order, push into mod.
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto& an = arena[static_cast<arena_id>(i)];
            ir_node<KindEnum, ExtPayload> nd{};
            nd.kind            = an.kind;
            nd.span            = an.span;
            nd.structural_hash = an.structural_hash;
            nd.first_child     = an.first_child;  // same offset semantics
            nd.child_count     = an.child_count;
            static_cast<void>(mod.push(nd));
        }
        mod.set_root(static_cast<ir_node_id>(arena.root()));
        return mod;
    }

} // namespace lang
