#pragma once
// =============================================================================
// tarka/egraph_opt.hpp — Equality Saturation Canonicalization (opt-in)
//
// Namespace:  tarka
// Provides:
//   TarkaOpTraits           — provides stable op ids for generic rule packs
//   intern_into_egraph      — bridge: Tarka Term → egraph::e_graph node
//                             (overloads: 2-arg, 3-arg, 4-arg with sort_map)
//   reconstruct_from_egraph — extraction_result → canonical Tarka Term
//   egraph_optimize         — intern → saturate → extract → reconstruct
//
// Design:
//   - Bridges into egraph::e_graph<size_t,size_t,...> without leaking Tarka
//     types into egraph (egraph is domain-agnostic).
//   - Reuses generic commutativity / associativity / identity_zero rule packs
//     parametrized on TarkaOpTraits.
//   - is_commutative sourced from op_descriptor (no second table).
//   - sort_map (e_class_id → Sort) populated during interning; required by
//     reconstruct_from_egraph to re-intern typed Term nodes.
//   - Returns original term if saturation produces no improvement or if
//     reconstruction fails (missing sort info for an extracted class).
// =============================================================================

#include "tarka/context.hpp"
#include "containers/graph/egraph.hpp"

#include "containers/dynamic/SmallVector.hpp"

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tarka {
    // =========================================================================
    // TarkaOpTraits — feeds generic egraph rule packs
    // =========================================================================

    struct TarkaOpTraits {
        static constexpr std::size_t commutative_op = static_cast<std::size_t>(Op::And);
        static constexpr std::size_t associative_op = static_cast<std::size_t>(Op::Add);
        static constexpr std::size_t add_op = static_cast<std::size_t>(Op::Add);
        static constexpr std::size_t mul_op = static_cast<std::size_t>(Op::Mul);
        static constexpr std::size_t zero_op = static_cast<std::size_t>(Op::Lit);
        static constexpr std::size_t one_op = static_cast<std::size_t>(Op::Lit);
        static constexpr std::size_t zero_payload = 0;
        static constexpr std::size_t one_payload = 1;
        // Convenience aliases for test access
        static constexpr std::size_t And = static_cast<std::size_t>(Op::And);
        static constexpr std::size_t Or = static_cast<std::size_t>(Op::Or);
    };

    using TarkaEGraph = egraph::e_graph<std::size_t, std::size_t>;

    // =========================================================================
    // intern_into_egraph — post-order bridge
    //
    // 4-arg form: also populates sort_map with {canonical_class_id → Sort}
    // for every interned node, enabling sort-typed reconstruction.
    // Pass sort_map=nullptr to skip sort tracking.
    // =========================================================================

    inline egraph::e_class_id intern_into_egraph(
        TarkaEGraph& g,
        Term t,
        std::unordered_map<const TermImpl*, egraph::e_class_id>& visited,
        std::unordered_map<egraph::e_class_id, Sort>* sort_map) {
        auto it = visited.find(t.ptr());
        if (it != visited.end()) return it->second;

        egraph::e_node<std::size_t, std::size_t> node;
        node.op = static_cast<std::size_t>(t.op());
        node.payload = static_cast<std::size_t>(t.ptr()->payload_hash);

        for (const Term& c : t.children()) {
            egraph::e_class_id cid = intern_into_egraph(g, c, visited, sort_map);
            node.children.push_back(cid);
        }

        const egraph::e_class_id id = g.add(std::move(node));
        visited.emplace(t.ptr(), id);

        if (sort_map)
            sort_map->emplace(g.find(id), t.sort());

        return id;
    }

    // 3-arg overload — no sort_map tracking
    inline egraph::e_class_id intern_into_egraph(
        TarkaEGraph& g,
        Term t,
        std::unordered_map<const TermImpl*, egraph::e_class_id>& visited) {
        return intern_into_egraph(g, t, visited, nullptr);
    }

    // 2-arg convenience overload — no sort_map, local visited map
    inline egraph::e_class_id intern_into_egraph(TarkaEGraph& g, Term t) {
        std::unordered_map<const TermImpl*, egraph::e_class_id> visited;
        return intern_into_egraph(g, t, visited, nullptr);
    }

    // =========================================================================
    // reconstruct_from_egraph — extraction_result → canonical Tarka Term
    //
    // Walks best_nodes bottom-up via iterative post-order, rebuilding typed
    // Term nodes via ctx.make_term.
    // Requires sort_map populated by intern_into_egraph (same graph, same run).
    // Returns std::nullopt if any class lacks a best node or sort mapping.
    // =========================================================================

    template <class ExtractionResult>
    [[nodiscard]] std::optional<Term> reconstruct_from_egraph(
        const TarkaEGraph& g,
        egraph::e_class_id root_id,
        const ExtractionResult& result,
        const std::unordered_map<egraph::e_class_id, Sort>& sort_map,
        Context& ctx) {
        std::unordered_map<egraph::e_class_id, Term> memo;

        struct Frame {
            egraph::e_class_id id;
            bool ready;
        };
        std::vector<Frame> stack;
        stack.reserve(64);
        stack.push_back({g.find(root_id), false});

        while (!stack.empty()) {
            auto& [id, ready] = stack.back();
            const egraph::e_class_id cid = g.find(id);

            if (memo.contains(cid)) {
                stack.pop_back();
                continue;
            }

            if (cid >= result.best_nodes.size() || !result.best_nodes[cid].has_value()) {
                return std::nullopt;
            }

            const auto& node = *result.best_nodes[cid];

            if (!ready) {
                ready = true;
                for (auto ch : node.children) {
                    const egraph::e_class_id ch_root = g.find(ch);
                    if (!memo.contains(ch_root))
                        stack.push_back({ch_root, false});
                }
                continue;
            }

            auto sort_it = sort_map.find(cid);
            if (sort_it == sort_map.end()) return std::nullopt;

            const Op op = static_cast<Op>(node.op);
            const Sort sort = sort_it->second;

            std::vector<Term> child_terms;
            child_terms.reserve(node.children.size());
            for (auto ch : node.children) {
                auto mit = memo.find(g.find(ch));
                if (mit == memo.end()) return std::nullopt;
                child_terms.push_back(mit->second);
            }

            Term rebuilt = ctx.make_term(
                op, sort,
                std::span<const Term>(child_terms.data(), child_terms.size()),
                static_cast<std::uint64_t>(node.payload));

            memo.emplace(cid, rebuilt);
            stack.pop_back();
        }

        auto it = memo.find(g.find(root_id));
        if (it == memo.end()) return std::nullopt;
        return it->second;
    }

    // =========================================================================
    // saturation_config
    // =========================================================================

    struct saturation_config {
        std::size_t max_iters = 10;
        std::size_t max_enodes = 100'000;
    };

    // =========================================================================
    // egraph_node_count_dag — DAG-aware unique-node count
    //
    // Counts *distinct* TermImpl nodes reachable from t. Hash-consing means a
    // shared sub-term has one address, so this is the true DAG size. The old
    // tree-count inflated shared formulae (1000 refs to 5 nodes → 1000), which
    // made after_count >= before_count almost always true, so egraph_optimize
    // never fired. Deduping fixes the comparison.
    // =========================================================================

    [[nodiscard]] inline std::size_t egraph_node_count_dag(Term t) {
        std::unordered_set<const TermImpl*> seen;
        seen.reserve(64);
        containers::dynamic::SmallVector<const TermImpl*, 512 * sizeof(const TermImpl*)> stack;
        if (t.valid()) stack.push_back(t.ptr());
        while (!stack.empty()) {
            const TermImpl* cur = stack.back();
            stack.pop_back();
            if (!seen.insert(cur).second) continue;
            const Term* ch = reinterpret_cast<const Term*>(cur + 1);
            for (std::uint16_t i = 0; i < cur->child_count; ++i) stack.push_back(ch[i].ptr());
        }
        return seen.size();
    }

    // =========================================================================
    // egraph_optimize — intern → saturate → extract → reconstruct
    //
    // Returns the canonical (minimal node-count) equivalent Term.
    // Falls back to the original term when:
    //   - saturation yields no improvement, or
    //   - reconstruction fails (missing sort info for an extracted class).
    // =========================================================================

    [[nodiscard]] inline Term egraph_optimize(Term t, saturation_config cfg = {}) {
        TarkaEGraph g;
        g.reserve(256);

        std::unordered_map<const TermImpl*, egraph::e_class_id> visited;
        std::unordered_map<egraph::e_class_id, Sort> sort_map;

        egraph::e_class_id root = intern_into_egraph(g, t, visited, &sort_map);

        // Compress sort_map keys after initial union-find path compression
        auto compress_sort_map = [&] {
            std::unordered_map<egraph::e_class_id, Sort> compressed;
            compressed.reserve(sort_map.size());
            for (auto& [id, s] : sort_map)
                compressed.emplace(g.find(id), s);
            sort_map = std::move(compressed);
        };
        compress_sort_map();

        egraph::saturation_limits limits;
        limits.max_iters = cfg.max_iters;
        limits.max_enodes = cfg.max_enodes;

        auto rules = std::tuple{
            egraph::commutativity<TarkaOpTraits>{},
            egraph::associativity<TarkaOpTraits>{},
            egraph::identity_zero<TarkaOpTraits>{}
        };
        [[maybe_unused]] auto report = egraph::saturate(g, rules, limits);

        // Re-compress after saturation merges classes
        compress_sort_map();

        auto extraction = egraph::extract_best(g, root);

        // Compare node counts
        const std::size_t before_count = egraph_node_count_dag(t);
        const std::size_t after_count = [&] {
            std::size_t c = 0;
            for (const auto& n : extraction.best_nodes)
                if (n.has_value()) ++c;
            return c;
        }();

        if (after_count >= before_count) return t;

        Context& ctx = t.ctx();
        auto rebuilt = reconstruct_from_egraph(g, root, extraction, sort_map, ctx);
        return rebuilt.value_or(t);
    }
} // namespace tarka
