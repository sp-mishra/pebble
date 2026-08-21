#pragma once

// languages/generic/tree/green_arena.hpp — Generic flat CST/IR arena.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// green_node<KE> — flat record: kind, span, first_child (index into child_ids),
//                  child_count, structural_hash.
// green_arena<KE> — flat arena built from event_log via two-pass stack builder.
//                   Layout identical to ir_module<KE,monostate> — Stage 3 adoption is zero-copy.
//
// Build API:
//   green_arena::build(log, leaf_span_fn, leaf_hash_fn)
//     leaf_span_fn : std::uint32_t token_index → byte_span
//     leaf_hash_fn : std::uint32_t token_index → std::uint64_t structural hash
//
// Hashing: fp_from_string / fp_combine from core/identity.hpp — identical to
//          samasa green_tree so hashes match by construction.
//
// splice_subtree / recompute_ancestor_hashes — partial-reparse primitives (Stage 6).

#include <cstdint>
#include <limits>
#include <span>
#include <vector>
#include "spans.hpp"
#include "event_log.hpp"
#include "../core/identity.hpp"

namespace lang {

    using arena_id = std::uint32_t;
    inline constexpr arena_id k_null_arena = std::numeric_limits<std::uint32_t>::max();

    // ---- green_node --------------------------------------------------------

    template <class KindEnum>
    struct green_node {
        KindEnum      kind            = {};
        byte_span     span            = {};
        arena_id      first_child     = k_null_arena; // index into child_ids_
        std::uint32_t child_count     = 0;
        std::uint64_t structural_hash = 0;
    };

    // ---- green_arena -------------------------------------------------------

    template <class KindEnum>
    class green_arena {
    public:
        using node_type = green_node<KindEnum>;

        [[nodiscard]] arena_id      root()  const noexcept { return root_id_; }
        [[nodiscard]] std::uint32_t size()  const noexcept {
            return static_cast<std::uint32_t>(nodes_.size());
        }
        [[nodiscard]] bool          empty() const noexcept { return nodes_.empty(); }

        [[nodiscard]] const node_type& operator[](arena_id id) const { return nodes_[id]; }

        [[nodiscard]] std::span<const arena_id> children(arena_id id) const noexcept {
            const auto& n = nodes_[id];
            if (n.child_count == 0 || n.first_child == k_null_arena) return {};
            return std::span<const arena_id>(child_ids_.data() + n.first_child, n.child_count);
        }

        // ---- build ---------------------------------------------------------
        // LeafSpanFn : (std::uint32_t token_index) -> byte_span
        // LeafHashFn : (std::uint32_t token_index) -> std::uint64_t
        template <class DiagCode, class LeafSpanFn, class LeafHashFn>
        [[nodiscard]] static green_arena build(
            const event_log<KindEnum, DiagCode>& log,
            LeafSpanFn&&  leaf_span,
            LeafHashFn&&  leaf_hash)
        {
            green_arena arena;

            struct frame {
                KindEnum      kind;
                std::uint32_t staging_start;
                byte_span     span;
            };

            std::vector<frame>    stack;
            std::vector<arena_id> staging;
            stack.reserve(64);
            staging.reserve(256);

            for (const auto& ev : log.all()) {
                switch (ev.kind) {
                case event_kind::begin_node:
                case event_kind::tombstone:
                    stack.push_back({ev.node_kind,
                                     static_cast<std::uint32_t>(staging.size()), {}});
                    break;

                case event_kind::token: {
                    node_type leaf{};
                    leaf.span            = leaf_span(ev.token_index);
                    leaf.first_child     = k_null_arena;
                    leaf.child_count     = 0;
                    leaf.structural_hash = leaf_hash(ev.token_index);
                    const arena_id id    = static_cast<arena_id>(arena.nodes_.size());
                    arena.nodes_.push_back(leaf);
                    staging.push_back(id);
                    if (!stack.empty())
                        stack.back().span = byte_span::hull(stack.back().span, leaf.span);
                    break;
                }

                case event_kind::error: {
                    node_type err{};
                    err.span            = ev.span;
                    err.first_child     = k_null_arena;
                    err.child_count     = 0;
                    err.structural_hash = ::lang::detail::fp_with_scalar(
                        ::lang::detail::fp_from_string("error"),
                        static_cast<std::uint64_t>(ev.diag_code));
                    const arena_id id   = static_cast<arena_id>(arena.nodes_.size());
                    arena.nodes_.push_back(err);
                    staging.push_back(id);
                    if (!stack.empty())
                        stack.back().span = byte_span::hull(stack.back().span, err.span);
                    break;
                }

                case event_kind::end_node: {
                    if (stack.empty()) break;
                    const frame f = stack.back(); stack.pop_back();

                    const std::uint32_t staging_base = f.staging_start;
                    const std::uint32_t child_count  =
                        static_cast<std::uint32_t>(staging.size()) - staging_base;

                    node_type node{};
                    node.kind        = f.kind;
                    node.span        = !ev.span.empty() ? ev.span : f.span;
                    node.child_count = child_count;

                    const arena_id node_id = static_cast<arena_id>(arena.nodes_.size());
                    arena.nodes_.push_back(node);

                    const arena_id child_ids_offset =
                        static_cast<arena_id>(arena.child_ids_.size());
                    for (std::uint32_t i = 0; i < child_count; ++i)
                        arena.child_ids_.push_back(staging[staging_base + i]);
                    staging.resize(staging_base);

                    arena.nodes_[node_id].first_child = child_ids_offset;
                    arena.nodes_[node_id].child_count = child_count;

                    // Structural hash: FNV-1a seed over kind + child hashes + span.length
                    const auto kind_val = static_cast<std::uint64_t>(
                        static_cast<std::underlying_type_t<KindEnum>>(f.kind));
                    std::uint64_t h = 14695981039346656037ULL ^ kind_val;
                    h *= 1099511628211ULL;
                    for (std::uint32_t i = 0; i < child_count; ++i) {
                        h = ::lang::detail::fp_combine(
                            h, arena.nodes_[arena.child_ids_[child_ids_offset + i]].structural_hash);
                    }
                    h = ::lang::detail::fp_combine(
                        h, static_cast<std::uint64_t>(arena.nodes_[node_id].span.length));
                    arena.nodes_[node_id].structural_hash = h;

                    staging.push_back(node_id);
                    if (!stack.empty())
                        stack.back().span = byte_span::hull(stack.back().span,
                                                             arena.nodes_[node_id].span);
                    arena.root_id_ = node_id;
                    break;
                }
                }
            }

            return arena;
        }

        // ---- splice / recompute -----------------------------------------------

        // splice_subtree: replace the subtree rooted at `at` with the nodes from `sub`.
        //
        // Algorithm:
        //   1. Append sub's nodes to this arena, remapping sub's internal child_ids by
        //      the append offset so all cross-refs stay valid.
        //   2. Overwrite the node at `at` with sub's root (remapped), preserving at's
        //      position in nodes_ so parent references outside the subtree are unchanged.
        //   3. Update the spliced node's span to sub.root().span.
        //
        // Freed slots from the old subtree are left as holes (holes carry k_null_arena
        // kind-zero nodes; a future reset()/rebuild reclaims them — pay-for-use).
        void splice_subtree(arena_id at, const green_arena& sub)
        {
            if (at == k_null_arena || sub.empty()) return;

            const auto append_offset = static_cast<arena_id>(nodes_.size());
            const auto child_offset  = static_cast<arena_id>(child_ids_.size());

            // Append sub's child_ids, offsetting each id by append_offset.
            child_ids_.reserve(child_ids_.size() + sub.child_ids_.size());
            for (arena_id c : sub.child_ids_)
                child_ids_.push_back(c == k_null_arena ? k_null_arena : c + append_offset);

            // Append sub's nodes, remapping first_child by child_offset.
            nodes_.reserve(nodes_.size() + sub.nodes_.size());
            for (const node_type& n : sub.nodes_) {
                node_type mapped = n;
                if (mapped.first_child != k_null_arena)
                    mapped.first_child += child_offset;
                nodes_.push_back(mapped);
            }

            // Overwrite node at `at` with the remapped sub-root.
            const arena_id sub_root_mapped = sub.root_id_ + append_offset;
            nodes_[at] = nodes_[sub_root_mapped];
            // Keep the id stable: the overwritten slot IS the new subtree root.
            // (sub_root_mapped still exists as a duplicate; harmless — unreachable
            //  from `at` because first_child now points into the remapped entries.)
        }

        // recompute_ancestor_hashes: walk from `from` up to root, recomputing
        // structural_hash at each ancestor using the same FNV recipe as build().
        //
        // Requires a transient parent map (O(n) build, O(1) lookup), discarded after
        // the call — nodes do not store parent ids (pay-for-use).
        void recompute_ancestor_hashes(arena_id from)
        {
            if (from == k_null_arena || nodes_.empty()) return;

            // Build transient parent map: parent_of[child] = parent's arena_id.
            const auto n = static_cast<std::uint32_t>(nodes_.size());
            std::vector<arena_id> parent_of(n, k_null_arena);
            for (arena_id p = 0; p < n; ++p) {
                const node_type& nd = nodes_[p];
                if (nd.child_count == 0 || nd.first_child == k_null_arena) continue;
                for (std::uint32_t ci = 0; ci < nd.child_count; ++ci) {
                    const arena_id child = child_ids_[nd.first_child + ci];
                    if (child < n) parent_of[child] = p;
                }
            }

            // Walk from `from` toward root, recomputing hashes bottom-up.
            arena_id cur = from;
            while (cur != k_null_arena && cur < n) {
                node_type& nd = nodes_[cur];
                // Recompute hash — identical recipe to build().
                const auto kind_val = static_cast<std::uint64_t>(
                    static_cast<std::underlying_type_t<KindEnum>>(nd.kind));
                std::uint64_t h = 14695981039346656037ULL ^ kind_val;
                h *= 1099511628211ULL;
                for (std::uint32_t ci = 0; ci < nd.child_count; ++ci) {
                    const arena_id child = child_ids_[nd.first_child + ci];
                    if (child < n)
                        h = ::lang::detail::fp_combine(h, nodes_[child].structural_hash);
                }
                h = ::lang::detail::fp_combine(h, static_cast<std::uint64_t>(nd.span.length));
                nd.structural_hash = h;

                cur = parent_of[cur];
            }
        }

    private:
        std::vector<node_type>  nodes_;
        std::vector<arena_id>   child_ids_;
        arena_id                root_id_ = k_null_arena;
    };

} // namespace lang
