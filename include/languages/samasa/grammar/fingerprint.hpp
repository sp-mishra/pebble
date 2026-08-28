#pragma once

// samasa/grammar/fingerprint.hpp — Grammar + tree fingerprints.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// grammar_fingerprint<G>()  — consteval descriptor_fingerprint; folds rule names +
//   shapes + terminal spellings through akshara::fnv1a64 + lang::detail::fp_combine.
//   Used to invalidate caches when grammar changes between builds.
//
// green_fingerprint         — runtime identity triple for a parsed tree.
//   grammar_hash  — grammar_fingerprint<G>() as uint64.
//   source_hash   — fnv1a64 of source text.
//   tree_hash     — structural_hash of the green root node.
//   Used for incremental caches, LSP document caches, test snapshots.
//
// fingerprint(tree, grammar_fp) — build a green_fingerprint from a parse result.

#include "grammar.hpp"
#include "meta/akshara.hpp"
#include "languages/generic/core/identity.hpp"
#include "meta/meta.hpp"
#include "../core/parse_output.hpp"
#include <cstdint>
#include <string_view>

namespace lang::samasa {

    // ---- grammar_fingerprint<G>() ------------------------------------------

    template <class G>
    consteval lang::descriptor_fingerprint grammar_fingerprint() {
        lang::descriptor_fingerprint fp = lang::detail::fp_from_string("samasa.grammar");

        meta::for_each<typename G::rules>([&fp](auto rule_instance) {
            using Rule = std::remove_cvref_t<decltype(rule_instance)>;
            fp = lang::detail::fp_combine(
                fp, akshara::fnv1a64(static_cast<std::string_view>(Rule::name)));
        });

        fp = lang::detail::fp_with_scalar(fp, G::rule_count);
        return fp;
    }

    // ---- green_fingerprint -------------------------------------------------
    // Runtime identity triple for a fully parsed tree.

    struct green_fingerprint {
        std::uint64_t grammar_hash = 0; // grammar_fingerprint<G>()
        std::uint64_t source_hash  = 0; // fnv1a64 of source text
        std::uint64_t tree_hash    = 0; // structural_hash of root green node

        [[nodiscard]] bool operator==(const green_fingerprint&) const noexcept = default;
        [[nodiscard]] bool valid() const noexcept {
            return grammar_hash != 0 && source_hash != 0;
        }
    };

    // ---- fingerprint() -----------------------------------------------------
    // Build a green_fingerprint from a grammar fingerprint + source text + tree root hash.

    namespace detail {
        [[nodiscard]] inline std::uint64_t fnv1a64_rt(std::string_view s) noexcept {
            std::uint64_t h = 14695981039346656037ULL;
            for (unsigned char c : s) {
                h ^= static_cast<std::uint64_t>(c);
                h *= 1099511628211ULL;
            }
            return h;
        }
    }

    template <class SK, class TK>
    [[nodiscard]] green_fingerprint fingerprint(
        const parse_output<SK,TK>& output,
        lang::descriptor_fingerprint grammar_fp,
        std::string_view source) noexcept
    {
        const std::uint64_t src_hash  = detail::fnv1a64_rt(source);
        const std::uint64_t tree_hash =
            output.tree.empty() ? 0
            : output.tree[output.tree.root()].structural_hash;
        return {grammar_fp, src_hash, tree_hash};
    }

} // namespace lang::samasa
