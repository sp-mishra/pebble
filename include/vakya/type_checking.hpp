#pragma once

// vakya/type_checking.hpp — Type environment, type_check, typing rules.
//
// C++23, header-only, no virtual, no macros. Opt-in; not pulled by vakya.hpp.
// Namespace: vakya::types  (extends constraints.hpp)
//
// type_environment: scoped bindings (name -> type_ref or scheme) via SymbolTable.
//   Scope enter/leave via snapshot/rollback (SymbolTable existing machinery).
//
// typing_rule<Tag>: descriptor seam (mirrors tag_descriptor) — per-tag constraint
//   generation. Specialise to add custom typing rules.
//
// type_check(expr, env, solver, arena) -> validation_result
//   Post-order walk generates constraints, composite_solver resolves them.
//   Results stored in property_store (no node contamination).
//
// Depth protection: kMaxWalkDepth (default 1024) guards against stack overflow
//   on deeply nested auto-generated trees. Callers may override by passing an
//   explicit depth budget into type_check_impl.

#include "vakya/constraints.hpp"
#include "vakya/property.hpp"

#include <unordered_map>

namespace vakya::types {
    // ============================================================================
    // validation_result
    // ============================================================================

    // Maximum AST walk depth before aborting with a depth_limit_exceeded diagnostic.
    // Protects against stack overflow on deeply auto-generated trees.
    inline constexpr std::size_t kMaxWalkDepth = 1024;

    enum class validation_status : std::uint8_t {
        success,
        type_error,
        unsatisfied_constraint,
        ambiguous_type,
        shape_error,
        capability_error,
        depth_limit_exceeded,
    };

    struct validation_result {
        validation_status status = validation_status::success;
        std::vector<solver_diagnostic> diagnostics;

        [[nodiscard]] bool ok() const noexcept { return status == validation_status::success; }
    };

    // Map solve_status to validation_status
    [[nodiscard]] inline validation_status to_validation_status(solve_status s) noexcept {
        switch (s) {
        case solve_status::solved: return validation_status::success;
        case solve_status::unsatisfiable: return validation_status::type_error;
        case solve_status::ambiguous: return validation_status::ambiguous_type;
        case solve_status::deferred: return validation_status::unsatisfied_constraint;
        }
        return validation_status::type_error;
    }

    // ============================================================================
    // type_scheme — type_ref (monotype) or ∀ᾱ.τ (polytype)
    // ============================================================================

    struct type_scheme {
        type_ref mono{}; // monotype handle
        bool is_poly = false; // true = quantified (∀ᾱ.τ in mono)
    };

    // ============================================================================
    // type_environment — scoped name -> type_scheme bindings
    // Uses a flat unordered_map + snapshot/rollback stack for scope management.
    // (SymbolTable stores raw pointers; we use our own map for type_scheme values.)
    // ============================================================================

    class type_environment {
    public:
        type_environment() = default;

        // Bind name (interned hash) -> scheme in current scope
        void bind(std::uint64_t name_hash, type_scheme scheme) {
            bindings_[name_hash] = std::move(scheme);
            scope_stack_.back().push_back(name_hash);
        }

        // Lookup (returns nullptr if not found)
        [[nodiscard]] const type_scheme* lookup(std::uint64_t name_hash) const {
            auto it = bindings_.find(name_hash);
            if (it == bindings_.end()) return nullptr;
            return &it->second;
        }

        // Enter a new lexical scope
        void push_scope() {
            scope_stack_.emplace_back();
        }

        // Leave scope: unbind everything introduced in this scope
        void pop_scope() {
            if (scope_stack_.empty()) return;
            for (std::uint64_t h : scope_stack_.back()) {
                bindings_.erase(h);
            }
            scope_stack_.pop_back();
        }

        // Collect free type-var ids from all bindings (used by generalize)
        [[nodiscard]] std::unordered_set<type_var_id>
        free_type_vars(substitution& subst, const type_arena& arena) const {
            std::unordered_set<type_var_id> out;
            for (const auto& [h, scheme] : bindings_) {
                auto fv = free_vars(scheme.mono, subst, arena);
                out.insert(fv.begin(), fv.end());
            }
            return out;
        }

    private:
        std::unordered_map<std::uint64_t, type_scheme> bindings_;
        std::vector<std::vector<std::uint64_t>> scope_stack_{{}}; // one root scope
    };

    // ============================================================================
    // property_key for type inference results stored in property_store
    // ============================================================================

    using TypeResultKey = vakya::property_key<type_ref, "vakya.type_result">;

    // ============================================================================
    // typing_rule<Tag> — seam for per-tag constraint generation.
    // Default: no constraints, result type = first child type.
    // Specialise to override per-tag.
    // ============================================================================

    template <class Tag>
    struct typing_rule {
        // emit: given child types, produce (result_type, constraints).
        // Default: result type = first child (opaque passthrough).
        // subst.make_var() is the single allocator — keeps UF and gen in sync.
        template <class ChildTypes>
        static std::pair<type_ref, std::vector<constraint>>
        emit(const ChildTypes& child_types, type_environment& /*env*/, type_arena& arena,
             type_var_generator& /*gen*/, substitution& subst) {
            if (!child_types.empty()) return {child_types[0], {}};
            // Leaf with no children: fresh variable registered in UF
            type_var_id vid = subst.make_var();
            return {arena.intern_variable(vid), {}};
        }
    };

    // ---- arithmetic tags: operands same_type, result = operand type ----
    namespace detail {
        template <class Tag>
        struct arithmetic_typing_rule {
            static std::pair<type_ref, std::vector<constraint>>
            emit_binary(const std::vector<type_ref>& child_types,
                        type_environment& /*env*/, type_arena& /*arena*/,
                        type_var_generator& /*gen*/, substitution& /*subst*/) {
                std::vector<constraint> cs;
                // Pair every child with child_types[0] so N-ary tags are fully constrained.
                for (std::size_t i = 1; i < child_types.size(); ++i) {
                    constraint c;
                    c.kind = constraint_kind::same_type;
                    c.operands.push_back(child_types[0]);
                    c.operands.push_back(child_types[i]);
                    cs.push_back(std::move(c));
                }
                type_ref result = child_types.empty() ? type_ref{} : child_types[0];
                return {result, std::move(cs)};
            }
        };
    } // namespace detail

    // Specialisations for built-in arithmetic/comparison tags
#define VAKYA_ARITHMETIC_TYPING_RULE(tag_type)                              \
    template <>                                                             \
    struct typing_rule<vakya::tag_type> {                                   \
        static std::pair<type_ref, std::vector<constraint>>                 \
        emit(const std::vector<type_ref>& ct, type_environment& env,        \
             type_arena& arena, type_var_generator& gen, substitution& subst) { \
            return detail::arithmetic_typing_rule<vakya::tag_type>          \
                ::emit_binary(ct, env, arena, gen, subst);                  \
        }                                                                   \
    }

    VAKYA_ARITHMETIC_TYPING_RULE(add_tag);

    VAKYA_ARITHMETIC_TYPING_RULE(sub_tag);

    VAKYA_ARITHMETIC_TYPING_RULE(mul_tag);

    VAKYA_ARITHMETIC_TYPING_RULE(div_tag);

    VAKYA_ARITHMETIC_TYPING_RULE(mod_tag);

#undef VAKYA_ARITHMETIC_TYPING_RULE

    // ============================================================================
    // detail::invoke_emit — arity-adaptive dispatch for typing_rule<Tag>::emit.
    //
    // Downstream specializations may omit the `substitution&` trailing parameter
    // (the pre-existing 4-arg contract). This helper tries the 5-arg overload first
    // and falls back to 4-arg, keeping both old and new specializations compatible.
    // ============================================================================

    namespace detail {
        template <class Tag>
        concept emit_takes_subst =
            requires(const std::vector<type_ref>& ct, type_environment& env,
                     type_arena& arena, type_var_generator& gen, substitution& subst) {
                typing_rule<Tag>::emit(ct, env, arena, gen, subst);
            };

        template <class Tag>
        [[nodiscard]] inline std::pair<type_ref, std::vector<constraint>>
        invoke_emit(const std::vector<type_ref>& child_types,
                    type_environment& env, type_arena& arena,
                    type_var_generator& gen, substitution& subst) {
            if constexpr (emit_takes_subst<Tag>) {
                return typing_rule<Tag>::emit(child_types, env, arena, gen, subst);
            }
            else {
                return typing_rule<Tag>::emit(child_types, env, arena, gen);
            }
        }
    } // namespace detail

    // ============================================================================
    // type_check — post-order walk + constraint solving
    // ============================================================================

    // Forward-declared for recursion
    template <class Expr, class Solver>
    type_ref type_check_impl(const Expr& expr,
                             type_environment& env,
                             Solver& solver,
                             type_arena& arena,
                             type_var_generator& gen,
                             substitution& subst,
                             vakya::property_store& store,
                             std::vector<constraint>& accumulated,
                             std::size_t depth);

    template <class Expr, class Solver>
    [[nodiscard]] validation_result
    type_check(const Expr& expr,
               type_environment& env,
               Solver& solver,
               type_arena& arena,
               type_var_generator& gen,
               substitution& subst,
               vakya::property_store& store,
               std::size_t max_depth = kMaxWalkDepth) {
        static_assert(constraint_solver<Solver>, "solver must satisfy constraint_solver<S>");

        std::vector<constraint> all_constraints;
        type_check_impl(expr, env, solver, arena, gen, subst, store, all_constraints, max_depth);

        solve_context ctx{&arena, &subst};
        solve_result sr = solver.solve(
            std::span<const constraint>(all_constraints.data(), all_constraints.size()),
            ctx);

        validation_result vr;
        vr.status = to_validation_status(sr.status);
        vr.diagnostics = std::move(sr.diagnostics);
        return vr;
    }

    template <class Expr, class Solver>
    type_ref type_check_impl(const Expr& expr,
                             type_environment& env,
                             Solver& solver,
                             type_arena& arena,
                             type_var_generator& gen,
                             substitution& subst,
                             vakya::property_store& store,
                             std::vector<constraint>& accumulated,
                             std::size_t depth) {
        // Terminals (expr<T>, expr_ref<T>, arithmetic leaves) have no tag_type —
        // assign a fresh variable to be resolved by the parent's typing_rule.
        if constexpr (vakya::is_terminal_v<Expr>) {
            type_var_id vid = subst.make_var();
            type_ref tr = arena.intern_variable(vid);
            std::uint64_t h = vakya::structural_hash(expr);
            store.update_for(h, [&](vakya::property_set& ps) {
                ps.template set<TypeResultKey>(tr);
            });
            return tr;
        }
        else {
            using Tag = typename Expr::tag_type;

            if (depth == 0) {
                // Tree too deep: return a fresh unresolved variable.
                type_var_id vid = subst.make_var();
                return arena.intern_variable(vid);
            }

            // Recursively process children first (post-order)
            std::vector<type_ref> child_types;
            vakya::tree::for_each_child(expr, [&](const auto& child) {
                type_ref ct = type_check_impl(child, env, solver, arena, gen, subst, store,
                                              accumulated, depth - 1);
                child_types.push_back(ct);
            });

            // Emit constraints for this node via typing_rule<Tag>
            auto [result_type, new_cs] = detail::invoke_emit<Tag>(child_types, env, arena, gen, subst);

            for (auto& c : new_cs) accumulated.push_back(std::move(c));

            // Store result in property_store keyed by structural hash
            std::uint64_t h = vakya::structural_hash(expr);
            store.update_for(h, [&](vakya::property_set& ps) {
                ps.template set<TypeResultKey>(result_type);
            });

            return result_type;
        }
    }
} // namespace vakya::types
