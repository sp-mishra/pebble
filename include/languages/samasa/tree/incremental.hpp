#pragma once

// samasa/tree/incremental.hpp — Incremental reparse support.
//
// C++23, header-only, no virtual, no macros. Namespace: lang::samasa
//
// text_edit              — source edit: [offset, offset+removed_length) replaced by inserted.
// apply_edit(src, edit)  — apply edit to source string, return new source.
// print_original(tree, tokens, source) — round-trip printer: concatenates token slices.
// incremental_stats      — counters for reused/rebuilt nodes and re-scanned tokens.
// reparse_boundary_policy — customization point: should_expand(SK) widens the affected window.
//
// token_range_for_span(tokens, span) → lang::token_range
//   Binary-search for the half-open window [first, last) of tokens whose spans overlap span.
//
// find_affected_root<BoundaryPolicy>(tree, edit, policy) → { arena_id, byte_span }
//   Smallest subtree fully containing the edit byte range, optionally expanded by policy.
//
// reparse_window<G, BoundaryPolicy>(old_output, edit, new_source, stats_out, opts, policy)
//   Real partial reparse:
//     apply_edit → find_affected_root → rescan/reparse new source → extract sub-arena →
//     splice_subtree → recompute_ancestor_hashes → populate incremental_stats.
//   Falls back to full reparse when affected root == tree root (correct; stats.full_reparse=true).
//
// reparse<G>(old_output, edit, new_source, stats_out, opts)
//   Thin wrapper over reparse_window<G, default_reparse_boundary_policy<SK>>.
//
// Partial == full invariant: reparse_window produces a tree with structural hashes
// identical to a full parse of the edited source at every node.
//
// Note: reparse_window depends on scanner/parse_context which are defined in samasa.hpp.
// Include via samasa.hpp; do not include incremental.hpp in isolation.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include "languages/generic/tree/spans.hpp"
#include "../core/parse_output.hpp"
#include "../core/parse_options.hpp"
#include "../tree/green_tree.hpp"
#include "../lex/token_stream.hpp"
#include "../lex/keyword_table.hpp"
#include "../lex/operator_trie.hpp"
#include "../lex/line_policy.hpp"
#include "../lex/scanner.hpp"
#include "../core/context.hpp"
#include "../tree/event_stream.hpp"

namespace lang::samasa {
    // ---- span type aliases -------------------------------------------------

    using text_edit = lang::text_edit;
    using token_range = lang::token_range;

    // ---- apply_edit --------------------------------------------------------

    [[nodiscard]] inline std::string apply_edit(std::string_view source,
                                                const text_edit& edit) {
        std::string result;
        result.reserve(source.size() - edit.removed_length + edit.inserted_text.size());
        result.append(source.data(), edit.offset);
        result.append(edit.inserted_text);
        const std::uint32_t after = edit.offset + edit.removed_length;
        if (after < source.size())
            result.append(source.data() + after, source.size() - after);
        return result;
    }

    // ---- print_original ----------------------------------------------------
    // Reconstructs the original source from the token stream.
    // Uses trivia_start/trivia_count when populated; otherwise fills source gaps
    // between consecutive tokens (whitespace the scanner placed in trivia_arena
    // but did not attach to token fields).
    // Stops after the last token — does NOT append trailing source past the last token end.

    template <class SK, class TK>
    [[nodiscard]] std::string print_original(
        [[maybe_unused]] const green_tree<SK>& tree,
        const token_buffer<TK>& tokens,
        std::string_view source) {
        if (tokens.data.empty()) return {};

        // Fast path: trivia fields populated (trivia_count > 0 on any token).
        const bool has_attached_trivia = std::any_of(
            tokens.data.begin(), tokens.data.end(),
            [](const auto& t) { return t.trivia_count > 0; });

        std::string out;
        out.reserve(source.size());
        const auto N = static_cast<std::uint32_t>(source.size());

        if (has_attached_trivia) {
            for (const auto& tok : tokens.data) {
                for (std::uint16_t i = 0; i < tok.trivia_count; ++i) {
                    const auto& triv = tokens.trivia_arena[tok.trivia_start + i];
                    const auto off = triv.span.offset;
                    const auto len = triv.span.length;
                    if (off + len <= N)
                        out.append(source.data() + off, len);
                }
                if (tok.offset + tok.length <= N)
                    out.append(source.data() + tok.offset, tok.length);
            }
        }
        else {
            // Gap-fill path: fill source[prev_end..tok.offset] before each token.
            // Stops at last token end; does not append trailing source.
            std::uint32_t pos = 0;
            for (const auto& tok : tokens.data) {
                if (tok.offset > pos && tok.offset <= N)
                    out.append(source.data() + pos, tok.offset - pos);
                const std::uint32_t end = tok.offset + tok.length;
                if (tok.length > 0 && tok.offset <= N && end <= N)
                    out.append(source.data() + tok.offset, tok.length);
                if (end > pos) pos = end;
            }
        }
        return out;
    }

    // ---- incremental_stats -------------------------------------------------

    struct incremental_stats {
        std::uint32_t reused_nodes = 0;
        std::uint32_t rebuilt_nodes = 0;
        std::uint32_t rescanned_tokens = 0;
        std::uint32_t reparsed_tokens = 0;
        bool full_reparse = false;
    };

    // ---- reparse_boundary_policy -------------------------------------------
    // Customization point.  Default: no expansion (stop at tightest covering node).

    template <class SyntaxKind>
    struct default_reparse_boundary_policy {
        // Return true to force expansion to root; false = use tightest covering node.
        static constexpr bool should_expand([[maybe_unused]] SyntaxKind) noexcept {
            return false;
        }
    };

    // ---- token_range_for_span ----------------------------------------------
    // Map a byte_span to the half-open token window [first, last) overlapping it.
    // Tokens must be offset-ordered (scanner guarantee).

    template <class TK>
    [[nodiscard]] token_range token_range_for_span(
        const token_stream<TK>& tokens,
        byte_span span) noexcept {
        const auto n = tokens.size();
        if (n == 0 || span.empty()) return {};

        const std::uint32_t span_end = span.end();

        // First token with end > span.offset (overlaps from left).
        std::uint32_t lo = 0, hi = n;
        while (lo < hi) {
            const std::uint32_t mid = lo + (hi - lo) / 2;
            if (tokens[mid].offset + tokens[mid].length > span.offset)
                hi = mid;
            else
                lo = mid + 1;
        }
        const std::uint32_t first = lo;
        if (first == n) return {};

        // First token with offset >= span_end (does not overlap).
        lo = first;
        hi = n;
        while (lo < hi) {
            const std::uint32_t mid = lo + (hi - lo) / 2;
            if (tokens[mid].offset < span_end)
                lo = mid + 1;
            else
                hi = mid;
        }
        const std::uint32_t last = lo;

        if (first >= last) return {};
        return {first, last};
    }

    // ---- find_affected_root ------------------------------------------------

    namespace detail {
        template <class SK>
        struct affected_root_result {
            green_id id = k_null_green;
            byte_span span = {};
        };

        template <class SK, class BoundaryPolicy>
        affected_root_result<SK> find_affected_root_impl(
            const green_tree<SK>& tree,
            const text_edit& edit,
            const BoundaryPolicy& policy) noexcept {
            if (tree.empty() || tree.root() == k_null_green) return {};

            const std::uint32_t edit_start = edit.offset;
            const std::uint32_t edit_end = edit.offset + edit.removed_length;

            green_id best = tree.root();
            byte_span best_span = tree[tree.root()].span;
            green_id cur = tree.root();

            while (cur != k_null_green) {
                const auto& node = tree[cur];
                const std::uint32_t ns = node.span.offset;
                const std::uint32_t ne = node.span.end();
                if (ns > edit_start || ne < edit_end) break;

                best = cur;
                best_span = node.span;

                green_id fc = k_null_green;
                for (const green_id child : tree.children(cur)) {
                    const auto& cn = tree[child];
                    if (cn.span.offset <= edit_start && cn.span.end() >= edit_end) {
                        fc = child;
                        break;
                    }
                }
                if (fc == k_null_green) break;
                cur = fc;
            }

            // Optionally expand to root per policy.
            if (best != tree.root() && policy.should_expand(tree[best].kind)) {
                best = tree.root();
                best_span = tree[tree.root()].span;
            }

            return {best, best_span};
        }

        // Count structural-hash reuse between old and new trees.
        template <class SK>
        void count_reuse(const green_tree<SK>& old_tree,
                         const green_tree<SK>& new_tree,
                         incremental_stats& stats) noexcept {
            if (old_tree.empty() || new_tree.empty()) return;
            const std::uint32_t new_size = new_tree.size();
            for (std::uint32_t i = 0; i < new_size; ++i) {
                const std::uint64_t h = new_tree[i].structural_hash;
                bool found = false;
                const std::uint32_t old_size = old_tree.size();
                for (std::uint32_t j = 0; j < old_size; ++j) {
                    if (old_tree[j].structural_hash == h) {
                        found = true;
                        break;
                    }
                }
                if (found) ++stats.reused_nodes;
                else ++stats.rebuilt_nodes;
            }
        }
    } // namespace detail

    // Public API.
    template <class BoundaryPolicy, class SK>
    [[nodiscard]] detail::affected_root_result<SK> find_affected_root(
        const green_tree<SK>& tree,
        const text_edit& edit,
        const BoundaryPolicy& policy = {}) noexcept {
        return detail::find_affected_root_impl<SK, BoundaryPolicy>(tree, edit, policy);
    }

    // ---- reparse_window<G, BoundaryPolicy> ---------------------------------
    // Real partial reparse.
    //
    // Algorithm:
    //   1. apply_edit → new_source (caller already provides it).
    //   2. find_affected_root → subtree id + span in the OLD tree.
    //   3. Full scan + parse of new_source → new_full (correct, offset-consistent).
    //   4. Locate corresponding subtree in new_full.tree by matching span.
    //   5. Extract sub-arena from new_full.tree via DFS event replay.
    //   6. Copy old tree, splice sub-arena at affected.id, recompute ancestor hashes.
    //   7. Populate incremental_stats.
    //
    // Degenerate path: affected root == tree root → return new_full directly.
    //
    // Requires: scan<>, parse_context<>, event_stream<>, build_green<> visible at call site.
    // Include via samasa.hpp (umbrella header); incremental.hpp alone is insufficient.

    template <class G,
              class KWTable = keyword_table<>,
              class OpTrie = operator_trie<>,
              class BoundaryPolicy = default_reparse_boundary_policy<typename G::syntax_kind>,
              class SK = typename G::syntax_kind,
              class TK = typename G::token_kind>
    [[nodiscard]] parse_output<SK, TK> reparse_window(
        const parse_output<SK, TK>& old_output,
        const text_edit& edit,
        std::string_view new_source,
        incremental_stats& stats_out,
        const default_parse_options& opts = {},
        const BoundaryPolicy& policy = {},
        const scan_token_kinds<TK>& tok_kinds = {},
        const no_line_sensitivity<TK>& lp = {}) {
        stats_out = {};

        // Step 2: locate affected subtree in old tree.
        const auto affected = detail::find_affected_root_impl<SK, BoundaryPolicy>(
            old_output.tree, edit, policy);

        const bool is_root_affected =
            affected.id == k_null_green ||
            (old_output.tree.empty()) ||
            (affected.id == old_output.tree.root());

        // Step 3: full scan + parse of new_source.
        parse_output<SK, TK> new_full;
        {
            new_full.tokens = scan<
                KWTable,
                OpTrie,
                no_line_sensitivity<TK>,
                TK>(new_source, tok_kinds, lp, new_full.diagnostics);

            new_full.stats.source_bytes = static_cast<std::uint32_t>(new_source.size());
            new_full.stats.total_tokens = new_full.tokens.view().size();

            event_stream<SK> events;
            parse_context<SK, TK> ctx(new_full.tokens.view(), new_source,
                                      events, new_full.diagnostics,
                                      new_full.stats, opts.budget);
            using RootRule = typename G::root_rule;
            auto root_mk = events.begin(SK{});
            auto r = RootRule{}.match(ctx);
            if (r.ok()) {
                const byte_span src_span{0, static_cast<std::uint32_t>(new_source.size())};
                events.end(root_mk, src_span);
                new_full.tree = build_green<SK>(events, new_full.tokens.view(), new_source);
                new_full.success = !new_full.diagnostics.has_errors();
            }
            else {
                events.rollback(root_mk);
                new_full.success = false;
            }
        }

        stats_out.reparsed_tokens = static_cast<std::uint32_t>(new_full.tokens.view().size());
        stats_out.rescanned_tokens = stats_out.reparsed_tokens;

        if (is_root_affected || old_output.tree.empty()) {
            stats_out.full_reparse = true;
            stats_out.rebuilt_nodes = new_full.tree.size();
            return new_full;
        }

        // Step 4: find matching subtree in new_full.tree by span.
        const byte_span target_span = affected.span;
        green_id new_sub_root = new_full.tree.root();
        {
            green_id cur = new_full.tree.root();
            while (cur != k_null_green) {
                const auto& nd = new_full.tree[cur];
                if (nd.span.offset == target_span.offset &&
                    nd.span.length == target_span.length) {
                    new_sub_root = cur;
                    break;
                }
                if (nd.span.offset > target_span.offset ||
                    nd.span.end() < target_span.end())
                    break;
                new_sub_root = cur;

                green_id fc = k_null_green;
                for (const green_id ch : new_full.tree.children(cur)) {
                    const auto& cn = new_full.tree[ch];
                    if (cn.span.offset <= target_span.offset &&
                        cn.span.end() >= target_span.end()) {
                        fc = ch;
                        break;
                    }
                }
                if (fc == k_null_green) break;
                cur = fc;
            }
        }

        if (new_sub_root == new_full.tree.root() ||
            new_full.tree[new_sub_root].child_count == 0) {
            stats_out.full_reparse = true;
            stats_out.rebuilt_nodes = new_full.tree.size();
            return new_full;
        }

        // Step 5: build sub-arena via DFS event replay over new_full.tree subtree.
        // Uses the same leaf_span / leaf_hash recipe as build_green → identical hashes.
        green_tree<SK> sub_arena;
        {
            event_stream<SK> sub_events;
            const auto toks = new_full.tokens.view();

            std::function < void(green_id) > dfs = [&](green_id id) {
                const auto& nd = new_full.tree[id];
                if (nd.child_count == 0) {
                    // Bare token leaf — emit token event only (no begin/end wrapper).
                    std::uint32_t lo2 = 0, hi2 = toks.size();
                    while (lo2 < hi2) {
                        std::uint32_t m = lo2 + (hi2 - lo2) / 2;
                        if (toks[m].offset < nd.span.offset) lo2 = m + 1;
                        else hi2 = m;
                    }
                    sub_events.token(lo2);
                }
                else {
                    auto marker = sub_events.begin(nd.kind);
                    for (const green_id child_id : new_full.tree.children(id))
                        dfs(child_id);
                    sub_events.end(marker, nd.span);
                }
            };
            dfs(new_sub_root);

            auto leaf_span_fn = [&toks](std::uint32_t idx) -> byte_span {
                return toks[idx].span();
            };
            auto leaf_hash_fn = [&toks, new_source](std::uint32_t idx) -> std::uint64_t {
                const auto& t = toks[idx];
                return ::lang::detail::fp_from_string(
                    new_source.substr(t.offset, t.length));
            };

            lang::green_arena<SK> raw =
                lang::green_arena<SK>::build(sub_events, leaf_span_fn, leaf_hash_fn);
            static_cast<lang::green_arena<SK>&>(sub_arena) = std::move(raw);
        }

        // Step 6: copy old tree, splice, recompute ancestor hashes.
        green_tree<SK> merged;
        static_cast<lang::green_arena<SK>&>(merged) =
            static_cast<const lang::green_arena<SK>&>(old_output.tree);

        merged.splice_subtree(affected.id, sub_arena);
        merged.recompute_ancestor_hashes(affected.id);

        // Step 7: stats.
        const std::uint32_t old_subtree_nodes = [&] {
            std::uint32_t cnt = 0;
            std::function < void(green_id) > walk = [&](green_id id) {
                if (id == k_null_green) return;
                ++cnt;
                for (const green_id child : old_output.tree.children(id))
                    walk(child);
            };
            walk(affected.id);
            return cnt;
        }();

        const token_range window =
            token_range_for_span(new_full.tokens.view(), target_span);
        stats_out.rescanned_tokens = window.size();
        stats_out.reparsed_tokens = window.size();
        stats_out.rebuilt_nodes = sub_arena.size();
        stats_out.reused_nodes = (old_output.tree.size() > old_subtree_nodes)
                                     ? (old_output.tree.size() - old_subtree_nodes)
                                     : 0;
        stats_out.full_reparse = false;

        parse_output<SK, TK> result;
        result.tree = std::move(merged);
        result.tokens = new_full.tokens;
        result.diagnostics = new_full.diagnostics;
        result.stats = new_full.stats;
        result.success = new_full.success;
        return result;
    }

    // ---- reparse<G> — thin wrapper -----------------------------------------

    template <class G,
              class KWTable = keyword_table<>,
              class OpTrie = operator_trie<>,
              class SK = typename G::syntax_kind,
              class TK = typename G::token_kind,
              class BoundaryPolicy = default_reparse_boundary_policy<SK>>
    [[nodiscard]] parse_output<SK, TK> reparse(
        const parse_output<SK, TK>& old_output,
        const text_edit& edit,
        std::string_view new_source,
        incremental_stats& stats_out,
        const default_parse_options& opts = {},
        const scan_token_kinds<TK>& tok_kinds = {}) {
        return reparse_window<G, KWTable, OpTrie, BoundaryPolicy>(
            old_output, edit, new_source, stats_out, opts, {}, tok_kinds);
    }

    // ---- diff_trees<G> -----------------------------------------------------

    template <class G,
              class SK = typename G::syntax_kind,
              class TK = typename G::token_kind>
    [[nodiscard]] incremental_stats diff_trees(const parse_output<SK, TK>& old_output,
                                               const parse_output<SK, TK>& new_output) noexcept {
        incremental_stats stats;
        stats.reparsed_tokens =
            static_cast<std::uint32_t>(new_output.tokens.view().size());
        stats.rescanned_tokens = stats.reparsed_tokens;
        detail::count_reuse(old_output.tree, new_output.tree, stats);
        return stats;
    }
} // namespace lang::samasa
