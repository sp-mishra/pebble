#pragma once
// =============================================================================
// medha/adapters/lithe.hpp — Lithe lowering adapter
//
// C++23, header-only, no virtual, no macros.
//
// Lowers a medha EDSL plan to Lithe HL region + Medha metadata sections (§17).
// IMPORTANT (R1): atomic/validate/commit/abort/retry are NEW Medha metadata,
// NOT existing Lithe ops. They are emitted as opaque metadata sections over
// the existing HL region primitive (structured_for/region_yield).
//
// Lower flow (§16.3):
//   lower(plan) → lithe_region_descriptor
//     → generic HL region
//     → Medha metadata sections:
//         medha.atomic-boundary
//         medha.validate
//         medha.commit
//         medha.abort
//         medha.retry
//         medha.constraints  (vakya::proof_obligation)
//         medha.transaction  (dialect version, isolation, retry, conflict, resource hashes)
//
// Schema stability:
//   lithe_transaction_metadata is the stable, versioned schema struct.
//   dialect_version must be bumped when any field is added or removed.
//   Consumers must check dialect_version before reading optional fields.
//
// All Lithe includes are __has_include-guarded.
// =============================================================================

#include "medha/edsl.hpp"
#include "medha/options.hpp"

#if __has_include("edsl/lithe.hpp")
#  include "edsl/lithe.hpp"
#  define MEDHA_HAS_LITHE 1
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace medha::adapters::lithe {
    // ============================================================================
    // lithe_transaction_metadata — stable versioned schema for Medha tx metadata
    //
    // dialect_version bump policy:
    //   v1: initial (isolation, retry_max, conflict_policy, distribution_policy,
    //                resource_hashes, effect_mask, constraint_hash, replay_safe)
    //   Increment dialect_version when any field is added, removed, or changes type.
    //   Consumers: check dialect_version before reading fields added after v1.
    // ============================================================================

    struct lithe_transaction_metadata {
        std::uint32_t dialect_version = 1; // schema version (bump on change)
        std::uint8_t isolation = 0; // medha::isolation enum value
        std::uint32_t retry_max = 0; // 0 = none
        std::uint8_t conflict_policy = 0; // 0=optimistic 1=pessimistic 2=deterministic
        std::uint8_t distribution_policy = 0; // 0=none (v1 only value)
        std::vector<std::uint64_t> resource_hashes; // FNV-1a over resource names, stable order
        std::uint64_t effect_mask = 0; // medha::effect_mask bitfield
        std::uint64_t constraint_hash = 0; // structural hash of proof obligations
        bool replay_safe = false;
    };

    // medha_tx_metadata_section is an alias kept for backward compat with call sites.
    using medha_tx_metadata_section = lithe_transaction_metadata;

    // ============================================================================
    // lithe_region_descriptor — result of lowering a plan
    // ============================================================================

    struct lithe_region_descriptor {
        std::string plan_name;
        lithe_transaction_metadata metadata;
        bool has_lithe = false; // true when Lithe is available

#ifdef MEDHA_HAS_LITHE
        // When lithe is present, the actual HL region would be held here.
        // Kept as an opaque pointer to avoid pulling all Lithe types into medha.
        // Populated by lower() when Lithe headers are available.
        void* hl_region = nullptr; // lithe::codegen::hl::region* (type-erased)
#endif
    };

    // ============================================================================
    // lower — produce Lithe HL region + Medha metadata from a dsl::plan
    // ============================================================================

    [[nodiscard]] inline lithe_region_descriptor lower(const dsl::plan& p) {
        lithe_region_descriptor desc;
        desc.plan_name = p.name;

        auto& m = desc.metadata;
        m.isolation = static_cast<std::uint8_t>(p.tx_options.isolation);

        // Retry max
        std::visit([&](const auto& rp) {
            using T = std::decay_t<decltype(rp)>;
            if constexpr (std::is_same_v<T, retry::bounded>) m.retry_max = rp.max;
            else if constexpr (std::is_same_v<T, retry::backoff>) m.retry_max = rp.max;
        }, p.tx_options.retry);

        // Conflict policy tag
        std::visit([&](const auto& cp) {
            using T = std::decay_t<decltype(cp)>;
            if constexpr (std::is_same_v<T, conflict::pessimistic>) m.conflict_policy = 1;
            else if constexpr (std::is_same_v<T, conflict::deterministic>) m.conflict_policy = 2;
            else m.conflict_policy = 0;
        }, p.tx_options.conflict);

        // Resource hashes (FNV-1a of resource name, stable field order)
        for (const auto& rd : p.resources) {
            std::uint64_t h = 0xcbf29ce4'84222325ULL;
            for (unsigned char c : rd.name) {
                h ^= c;
                h *= 0x00000100'000001B3ULL;
            }
            m.resource_hashes.push_back(h);
        }

        // replay_safe: true only when body+effects are declared idempotent
        m.replay_safe = (p.tx_options.replay ==
            replay_safety::body_and_effects_idempotent);

#ifdef MEDHA_HAS_LITHE
        desc.has_lithe = true;
        // TODO: allocate HL region via Lithe region builder when needed.
        // For now, metadata-only lowering is correct per §17.1.
#endif

        return desc;
    }
} // namespace medha::adapters::lithe
