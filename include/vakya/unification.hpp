#pragma once

// vakya/unification.hpp — Robinson mgu + substitution + generalize/instantiate.
//
// C++23, header-only, no virtual, no macros. Opt-in; not pulled by vakya.hpp.
// Namespace: vakya::types  (extends types.hpp)
//
// substitution: Union-Find over type_var_id (containers/union_find.hpp)
//   + binding map type_var_id -> type_ref for var-to-term bindings.
//   Thread isolation: substitution is NOT thread-safe. Each compilation thread
//   must own its own substitution instance.
//
// unify(a, b, subst) -> std::expected<subst_delta, unify_error>
//   mgu with occurs-check, constructor decomposition, callable congruence.
//   No exceptions. subst_delta uses SmallVector<binding_record, 8> to avoid
//   heap allocation for unary/binary type terms.
//
// apply(subst, t) -> type_ref
// generalize(env_free_vars, t) -> type_ref  (∀ᾱ.τ)
// instantiate(∀ᾱ.τ)           -> type_ref  (freshen quantified vars)

#include "vakya/types.hpp"
#include "containers/union_find.hpp"
#include "containers/dynamic/SmallVector.hpp"

#include <expected>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace vakya::types {
    // ============================================================================
    // unify_error
    // ============================================================================

    enum class unify_error_kind : std::uint8_t {
        constructor_clash,
        arity_mismatch,
        infinite_type, // occurs-check failure
        kind_mismatch,
    };

    struct unify_error {
        unify_error_kind kind;
        type_ref a{};
        type_ref b{};
    };

    // ============================================================================
    // substitution — Union-Find + variable-to-term binding map
    // ============================================================================

    struct binding_record {
        type_var_id var;
        type_ref bound_to;
    };

    // subst_delta: SmallVector to avoid heap allocation for common unary/binary terms.
    using subst_delta = containers::dynamic::SmallVector<binding_record, 8 * sizeof(binding_record)>;

    class substitution {
    public:
        substitution() = default;

        // Allocate a fresh variable slot in the UF.
        type_var_id make_var() { return uf_.make_set(); }

        // Union two vars
        bool unite_vars(type_var_id a, type_var_id b) noexcept {
            return uf_.unite(a, b);
        }

        // Bind var v to a type_ref term t.
        void bind(type_var_id v, type_ref t) {
            type_var_id root = uf_.find(v);
            bindings_[root] = t;
        }

        // Look up binding for var v (follows UF root).
        [[nodiscard]] std::optional<type_ref> lookup(type_var_id v) const {
            // uf_ is mutable: path-splitting is a transparent optimisation that
            // does not change observable set membership.
            type_var_id root = uf_.find(v);
            auto it = bindings_.find(root);
            if (it == bindings_.end()) return std::nullopt;
            return it->second;
        }

        [[nodiscard]] type_var_id find_root(type_var_id v) noexcept {
            return uf_.find(v);
        }

        [[nodiscard]] std::size_t var_count() const noexcept { return uf_.size(); }

    private:
        mutable containers::union_find<type_var_id, std::uint8_t> uf_;
        std::unordered_map<type_var_id, type_ref> bindings_;
    };

    // ============================================================================
    // prune — follow variable chains to a non-var or unbound var
    // ============================================================================

    [[nodiscard]] inline type_ref prune(type_ref t,
                                        substitution& subst,
                                        const type_arena& arena) noexcept {
        for (;;) {
            const type_node* n = arena.get(t);
            if (!n || n->kind != type_kind::variable) return t;
            auto bound = subst.lookup(n->var_id);
            if (!bound) return t;
            t = *bound;
        }
    }

    // ============================================================================
    // occurs — does α appear free in τ under σ?
    // ============================================================================

    [[nodiscard]] inline bool occurs(type_var_id alpha,
                                     type_ref t,
                                     substitution& subst,
                                     const type_arena& arena,
                                     std::unordered_set<std::uint32_t>& visited) {
        t = prune(t, subst, arena);
        const type_node* n = arena.get(t);
        if (!n) return false;

        // Use handle index as visit key (type_ref has no operator<; index is unique per node)
        if (visited.count(t.index)) return false;
        visited.insert(t.index);

        if (n->kind == type_kind::variable) {
            return subst.find_root(n->var_id) == subst.find_root(alpha);
        }

        for (const type_ref& c : n->children) {
            if (occurs(alpha, c, subst, arena, visited)) return true;
        }
        return false;
    }

    // ============================================================================
    // apply — walk t under subst, re-intern result
    // ============================================================================

    [[nodiscard]] inline type_ref apply(type_ref t,
                                        substitution& subst,
                                        type_arena& arena) {
        t = prune(t, subst, arena);
        const type_node* n = arena.get(t);
        if (!n) return t;
        if (n->kind == type_kind::variable) return t; // unbound

        type_node rebuilt;
        rebuilt.kind = n->kind;
        rebuilt.descriptor_stable_id = n->descriptor_stable_id;
        rebuilt.var_id = n->var_id;
        rebuilt.alias_name_hash = n->alias_name_hash;
        rebuilt.alias_def = n->alias_def;
        rebuilt.payload_hash = n->payload_hash;
        for (type_var_id v : n->quantified_vars) rebuilt.quantified_vars.push_back(v);

        for (const type_ref& c : n->children) {
            rebuilt.children.push_back(apply(c, subst, arena));
        }
        return arena.intern(std::move(rebuilt));
    }

    // ============================================================================
    // unify — Robinson mgu
    // ============================================================================

    // Forward declare for mutual recursion
    [[nodiscard]] std::expected<subst_delta, unify_error>
    unify(type_ref a, type_ref b, substitution& subst, type_arena& arena);

    namespace detail {
        [[nodiscard]] inline std::expected<subst_delta, unify_error>
        bind_var(type_var_id alpha, type_ref t,
                 substitution& subst, type_arena& arena) {
            std::unordered_set<std::uint32_t> visited;
            if (occurs(alpha, t, subst, arena, visited)) {
                return std::unexpected(unify_error{
                    unify_error_kind::infinite_type,
                    arena.intern_variable(alpha),
                    t
                });
            }
            subst.bind(alpha, t);
            subst_delta delta;
            delta.push_back(binding_record{alpha, t});
            return delta;
        }
    } // namespace detail

    [[nodiscard]] inline std::expected<subst_delta, unify_error>
    unify(type_ref a, type_ref b, substitution& subst, type_arena& arena) {
        a = prune(a, subst, arena);
        b = prune(b, subst, arena);

        if (arena.type_equal(a, b)) return subst_delta{};

        const type_node* na = arena.get(a);
        const type_node* nb = arena.get(b);

        if (!na || !nb) return subst_delta{};

        // Variable cases
        if (na->kind == type_kind::variable) {
            return detail::bind_var(na->var_id, b, subst, arena);
        }
        if (nb->kind == type_kind::variable) {
            return detail::bind_var(nb->var_id, a, subst, arena);
        }

        if (na->kind != nb->kind) {
            return std::unexpected(unify_error{unify_error_kind::kind_mismatch, a, b});
        }

        // Callable: all children including return type
        if (na->kind == type_kind::callable) {
            if (na->children.size() != nb->children.size()) {
                return std::unexpected(unify_error{unify_error_kind::arity_mismatch, a, b});
            }
            subst_delta total;
            total.reserve(na->children.size() * 2);
            for (std::size_t i = 0; i < na->children.size(); ++i) {
                auto r = unify(na->children[i], nb->children[i], subst, arena);
                if (!r) return r;
                for (auto& br : *r) total.push_back(br);
            }
            return total;
        }

        // Constructor: check stable_id + congruence
        if (na->descriptor_stable_id != nb->descriptor_stable_id) {
            return std::unexpected(unify_error{unify_error_kind::constructor_clash, a, b});
        }
        if (na->children.size() != nb->children.size()) {
            return std::unexpected(unify_error{unify_error_kind::arity_mismatch, a, b});
        }

        subst_delta total;
        total.reserve(na->children.size() * 2);
        for (std::size_t i = 0; i < na->children.size(); ++i) {
            auto r = unify(na->children[i], nb->children[i], subst, arena);
            if (!r) return r;
            for (auto& br : *r) total.push_back(br);
        }
        return total;
    }

    // ============================================================================
    // free_vars — collect unbound type vars in a type_ref under σ
    // ============================================================================

    inline void free_vars_impl(type_ref t,
                               substitution& subst,
                               const type_arena& arena,
                               std::unordered_set<type_var_id>& out,
                               std::unordered_set<std::uint32_t>& visited) {
        t = prune(t, subst, arena);
        if (visited.count(t.index)) return;
        visited.insert(t.index);

        const type_node* n = arena.get(t);
        if (!n) return;

        if (n->kind == type_kind::variable) {
            out.insert(subst.find_root(n->var_id));
            return;
        }
        for (const type_ref& c : n->children) {
            free_vars_impl(c, subst, arena, out, visited);
        }
    }

    [[nodiscard]] inline std::unordered_set<type_var_id>
    free_vars(type_ref t, substitution& subst, const type_arena& arena) {
        std::unordered_set<type_var_id> out;
        std::unordered_set<std::uint32_t> visited;
        free_vars_impl(t, subst, arena, out, visited);
        return out;
    }

    // ============================================================================
    // generalize — ∀ᾱ.τ
    // ============================================================================

    [[nodiscard]] inline type_ref
    generalize(type_ref t,
               const std::unordered_set<type_var_id>& env_free,
               substitution& subst,
               type_arena& arena) {
        type_ref applied = apply(t, subst, arena);
        auto fv = free_vars(applied, subst, arena);

        containers::dynamic::SmallVector<type_var_id, 16> to_quantify;
        for (type_var_id v : fv) {
            if (!env_free.count(v)) to_quantify.push_back(v);
        }
        if (to_quantify.empty()) return applied;

        return arena.intern_quantified(
            std::span<const type_var_id>(to_quantify.data(), to_quantify.size()),
            applied);
    }

    // ============================================================================
    // instantiate — ∀ᾱ.τ → τ[ᾱ→fresh vars]
    // ============================================================================

    [[nodiscard]] inline type_ref
    instantiate(type_ref quantified,
                substitution& subst,
                type_arena& arena,
                type_var_generator& gen) {
        const type_node* n = arena.get(quantified);
        if (!n || n->kind != type_kind::quantified) return quantified;

        std::unordered_map<type_var_id, type_var_id> rename;
        rename.reserve(n->quantified_vars.size());
        for (type_var_id qv : n->quantified_vars) {
            // subst.make_var() is the authoritative allocator — gen is only needed
            // to keep type_var_generator::count() in sync with external callers
            // that use gen.fresh() for tracking. Both must advance together.
            type_var_id fv = subst.make_var();
            gen.sync_to(fv + 1); // keep gen counter >= allocated ids
            rename[qv] = fv;
        }

        type_ref body = n->children.empty() ? type_ref{} : n->children[0];

        // Generic auto lambda eliminates std::function heap + virtual-call overhead.
        auto rename_vars = [&](this auto&& self, type_ref t) -> type_ref {
            const type_node* tn = arena.get(t);
            if (!tn) return t;

            if (tn->kind == type_kind::variable) {
                auto it = rename.find(subst.find_root(tn->var_id));
                if (it != rename.end()) {
                    return arena.intern_variable(it->second);
                }
                return t;
            }

            type_node rebuilt;
            rebuilt.kind = tn->kind;
            rebuilt.descriptor_stable_id = tn->descriptor_stable_id;
            rebuilt.var_id = tn->var_id;
            rebuilt.alias_name_hash = tn->alias_name_hash;
            rebuilt.alias_def = tn->alias_def;
            rebuilt.payload_hash = tn->payload_hash;
            for (type_var_id v : tn->quantified_vars) rebuilt.quantified_vars.push_back(v);
            for (const type_ref& c : tn->children) {
                rebuilt.children.push_back(self(c));
            }
            return arena.intern(std::move(rebuilt));
        };

        return rename_vars(body);
    }
} // namespace vakya::types
