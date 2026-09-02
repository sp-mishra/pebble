#pragma once

// samasa/tree/green_tree.hpp — samasa adapters over the generic green_arena substrate.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// Stage 3: green_node<SK>, green_tree<SK>, green_id, k_null_green are aliases/wrappers
//          over the generic lang::green_arena substrate.
//
// green_tree<SK> inherits lang::green_arena<SK> and adds a samasa-flavored
//   build(events, tokens, source) static entry — same signature as before,
//   forwarding internally to build_green(). All call sites unchanged.
//
// build_green(events, tokens, source) — samasa-flavored builder (also callable directly).
//   Leaf span  : tokens[token_index].span()
//   Leaf hash  : fp_from_string(source.substr(span.offset, span.length))
//   Hashes are byte-identical to the pre-Stage-3 implementation by construction.

#include <string_view>
#include "languages/generic/tree/green_arena.hpp"
#include "event_stream.hpp"
#include "languages/generic/core/identity.hpp"
#include "../lex/token_stream.hpp"
#include "languages/generic/tree/green_arena.hpp"

namespace lang::samasa {
    // Type/constant aliases.
    using lang::green_node;
    using green_id = lang::arena_id;
    inline constexpr green_id k_null_green = lang::k_null_arena;

    // -------------------------------------------------------------------------
    // green_tree<SK> — thin wrapper over green_arena<SK>.
    // Adds the samasa-flavored build(events, tokens, source) static entry so
    // existing call sites (tests + samasa.hpp) compile unchanged.
    // -------------------------------------------------------------------------

    template <class SyntaxKind>
    struct green_tree : lang::green_arena<SyntaxKind> {
        // Forward all arena constructors.
        using lang::green_arena<SyntaxKind>::green_arena;

        // samasa-flavored build: token-span + source-substr FNV leaf hashing.
        // Signature preserved from the pre-Stage-3 implementation.
        template <class TokenKind>
        [[nodiscard]] static green_tree build(
            const event_stream<SyntaxKind>& events,
            const token_stream<TokenKind>& tokens,
            std::string_view source = {}) {
            auto leaf_span = [&tokens](std::uint32_t idx) -> byte_span {
                return tokens[idx].span();
            };
            auto leaf_hash = [&tokens, source](std::uint32_t idx) -> std::uint64_t {
                const auto s = tokens[idx].span();
                return ::lang::detail::fp_from_string(source.substr(s.offset, s.length));
            };
            // Build via generic arena; move-construct into green_tree wrapper.
            lang::green_arena<SyntaxKind> arena =
                lang::green_arena<SyntaxKind>::build(events, leaf_span, leaf_hash);
            green_tree result;
            static_cast<lang::green_arena<SyntaxKind>&>(result) = std::move(arena);
            return result;
        }
    };

    // build_green — free-function entry point (used internally by samasa.hpp).
    template <class SyntaxKind, class TokenKind>
    [[nodiscard]] green_tree<SyntaxKind> build_green(
        const event_stream<SyntaxKind>& events,
        const token_stream<TokenKind>& tokens,
        std::string_view source = {}) {
        return green_tree<SyntaxKind>::build(events, tokens, source);
    }
} // namespace lang::samasa
