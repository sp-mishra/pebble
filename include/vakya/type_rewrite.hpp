#pragma once

// =============================================================================
// vakya/type_rewrite.hpp — type-level rewriting via egraph (V3, opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// Rewrites over TYPE TERMS (not value expressions), proven convergent by egraph.
// Uses the exact egraph round-trip Tarka uses for terms:
//   intern_type_into_egraph → rebuild → reconstruct_from_egraph
//
// Built-in rewrite rules:
//   Optional idempotence:  Optional<Optional<T>> ↝ Optional<T>
//
// Alias expansion (String ↝ Array<Character>) uses type_arena::canonicalize.
// New rules registered via type_rewrite_engine::add_rule().
//
// kEquivalentKind (first ext-band constraint kind) = constraint for egraph solver.
//
// Guard: __has_include("containers/graph/egraph.hpp")
// When egraph unavailable: egraph_type_canonicalize not compiled in.
//
// Dependencies: vakya/types.hpp, vakya/constraints.hpp
// =============================================================================

#include "vakya/types.hpp"
#include "vakya/constraints.hpp"

#if __has_include("containers/graph/egraph.hpp")
#include "containers/graph/egraph.hpp"
#define VAKYA_TYPE_REWRITE_EGRAPH 1
#else
#define VAKYA_TYPE_REWRITE_EGRAPH 0
#endif

#include <cstdint>
#include <string_view>
#include <vector>

namespace vakya::types {
    // ============================================================================
    // kEquivalentKind — egraph constraint kind (first ext-band slot)
    // ============================================================================

    inline constexpr constraint_kind kEquivalentKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 0);

    // ============================================================================
    // type_rewrite_rule — a rewrite rule over type_ref pairs
    //
    // lhs_stable_id: source type descriptor's stable_id
    // rewrite:       stateless function pointer; returns new type_ref or null handle
    // ============================================================================

    struct type_rewrite_rule {
        std::uint32_t lhs_stable_id = 0;
        std::string_view name{};
        type_ref (*rewrite)(const type_node&, type_arena&) = nullptr;
    };

    // ============================================================================
    // type_rewrite_engine — applies rewrite rules to type terms
    // ============================================================================

    class type_rewrite_engine {
    public:
        explicit type_rewrite_engine() {
            install_default_rules();
        }

        void add_rule(type_rewrite_rule rule) {
            rules_.push_back(rule);
        }

        // Apply rules once (no recursion into children)
        [[nodiscard]] type_ref rewrite_once(type_ref t, type_arena& arena) const {
            const type_node* n = arena.get(t);
            if (!n) return t;
            for (const type_rewrite_rule& r : rules_) {
                if (n->descriptor_stable_id != r.lhs_stable_id) continue;
                if (!r.rewrite) continue;
                type_ref result = r.rewrite(*n, arena);
                if (!result.is_null() && result != t) return result;
            }
            return t;
        }

        // Recursively normalize children then apply rules
        [[nodiscard]] type_ref rewrite_recursive(type_ref t, type_arena& arena,
                                                 std::size_t depth = 0) const {
            constexpr std::size_t kMaxDepth = 128;
            if (depth > kMaxDepth) return t;

            const type_node* n = arena.get(t);
            if (!n) return t;

            type_node rebuilt = *n;
            bool any_changed = false;
            for (std::size_t i = 0; i < rebuilt.children.size(); ++i) {
                type_ref nc = rewrite_recursive(rebuilt.children[i], arena, depth + 1);
                if (nc != rebuilt.children[i]) {
                    rebuilt.children[i] = nc;
                    any_changed = true;
                }
            }

            type_ref base = any_changed ? arena.intern(std::move(rebuilt)) : t;
            return rewrite_once(base, arena);
        }

        // Normalize to fixpoint
        [[nodiscard]] type_ref normalize(type_ref t, type_arena& arena) const {
            type_ref current = t;
            for (int iter = 0; iter < 32; ++iter) {
                type_ref next = rewrite_recursive(current, arena);
                if (next == current) break;
                current = next;
            }
            return current;
        }

    private:
        // Optional<Optional<T>> → Optional<T>
        static type_ref rewrite_optional_idempotent(const type_node& n, type_arena& arena) {
            if (n.children.size() != 1) return type_ref{};
            const type_node* inner = arena.get(n.children[0]);
            if (!inner) return type_ref{};
            if (inner->descriptor_stable_id != type_descriptor<optional_type_tag>::stable_id)
                return type_ref{};
            return n.children[0];
        }

        void install_default_rules() {
            type_rewrite_rule opt_idemp;
            opt_idemp.lhs_stable_id = type_descriptor<optional_type_tag>::stable_id;
            opt_idemp.name = "optional_idempotent";
            opt_idemp.rewrite = &rewrite_optional_idempotent;
            rules_.push_back(opt_idemp);
        }

        std::vector<type_rewrite_rule> rules_;
    };

    // ============================================================================
    // make_type_alias_rule — helper to build an alias expansion rule
    // ============================================================================

    template <class LhsCtor>
    [[nodiscard]] type_rewrite_rule make_type_alias_rule(
        std::string_view name,
        type_ref (*rhs_factory)(const type_node&, type_arena&)) {
        type_rewrite_rule rule;
        rule.lhs_stable_id = type_descriptor<LhsCtor>::stable_id;
        rule.name = name;
        rule.rewrite = rhs_factory;
        return rule;
    }

    // ============================================================================
    // egraph_type_canonicalize — structural canonicalization via egraph round-trip
    // Available only when containers/graph/egraph.hpp is present.
    // ============================================================================

#if VAKYA_TYPE_REWRITE_EGRAPH

    using type_egraph_t = egraph::e_graph<std::uint32_t, std::uint64_t>;
    using type_enode_t = egraph::e_node<std::uint32_t, std::uint64_t>;

    [[nodiscard]] inline egraph::e_class_id
    intern_type_into_egraph(type_ref t, const type_arena& arena, type_egraph_t& eg) {
        const type_node* n = arena.get(t);
        if (!n) return egraph::kInvalidClassId;

        type_enode_t enode;
        enode.op = n->descriptor_stable_id;
        enode.payload = (n->var_id != kInvalidTypeVarId)
                            ? static_cast<std::uint64_t>(n->var_id)
                            : n->alias_name_hash;

        for (const type_ref& c : n->children) {
            egraph::e_class_id child_id = intern_type_into_egraph(c, arena, eg);
            if (child_id == egraph::kInvalidClassId) return egraph::kInvalidClassId;
            enode.children.push_back(child_id);
        }
        return eg.add(std::move(enode));
    }

    // Forward declaration for mutual recursion
    [[nodiscard]] inline type_ref
    reconstruct_from_egraph(egraph::e_class_id root,
                            const type_egraph_t& eg,
                            const decltype(egraph::extract_best<egraph::node_count_cost>(
                                std::declval<const type_egraph_t&>(),
                                std::declval<egraph::e_class_id>()))& extraction,
                            type_arena& arena);

    [[nodiscard]] inline type_ref
    egraph_type_canonicalize(type_ref t, const type_arena& src_arena,
                             type_arena& dst_arena, type_var_generator& gen) {
        (void)gen;
        type_egraph_t eg;
        eg.reserve(64);

        egraph::e_class_id root = intern_type_into_egraph(t, src_arena, eg);
        if (root == egraph::kInvalidClassId) return t;

        eg.rebuild();

        auto extraction = egraph::extract_best<egraph::node_count_cost>(eg, root);
        egraph::e_class_id canon_root = eg.find(root);

        if (canon_root >= extraction.best_nodes.size()) return t;
        if (!extraction.best_nodes[canon_root]) return t;

        return reconstruct_from_egraph(canon_root, eg, extraction, dst_arena);
    }

    [[nodiscard]] inline type_ref
    reconstruct_from_egraph(egraph::e_class_id root,
                            const type_egraph_t& eg,
                            const decltype(egraph::extract_best<egraph::node_count_cost>(
                                std::declval<const type_egraph_t&>(),
                                std::declval<egraph::e_class_id>()))& extraction,
                            type_arena& arena) {
        egraph::e_class_id canon_root = eg.find(root);
        if (canon_root >= extraction.best_nodes.size()) return type_ref{};
        if (!extraction.best_nodes[canon_root]) return type_ref{};

        const auto& best_node = *extraction.best_nodes[canon_root];

        type_node n;
        n.descriptor_stable_id = best_node.op;
        n.payload_hash = best_node.payload;

        if (best_node.op == 0 && best_node.children.empty()) {
            n.kind = type_kind::variable;
            n.var_id = static_cast<type_var_id>(best_node.payload);
        }
        else {
            n.kind = best_node.children.empty() ? type_kind::primitive : type_kind::constructor;
        }

        for (egraph::e_class_id child_id : best_node.children) {
            type_ref child_ref = reconstruct_from_egraph(child_id, eg, extraction, arena);
            if (child_ref.is_null()) return type_ref{};
            n.children.push_back(child_ref);
        }

        return arena.intern(std::move(n));
    }

#endif // VAKYA_TYPE_REWRITE_EGRAPH
} // namespace vakya::types
