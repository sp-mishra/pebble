#pragma once

// vakya/type_inference.hpp — Algorithm-W HM type inference.
//
// C++23, header-only, no virtual, no macros. Opt-in; not pulled by vakya.hpp.
// Namespace: vakya::types  (extends type_checking.hpp)
//
// infer(expr, env, subst, arena, gen) -> expected<type_ref, infer_error>
//   Bottom-up Hindley-Milner:
//     - fresh var at each leaf
//     - incremental unify per node via typing_rule<Tag>
//     - generalize at let-boundaries
//     - instantiate at use sites
//   kosha-cached on structural_hash (type.infer).
//   NADI-traced (guarded by __has_include).
//
// Depth protection: infer/infer_impl respect kMaxWalkDepth (from type_checking.hpp).
//   Callers may pass a custom max_depth to infer().

#include "vakya/type_checking.hpp"
#include "containers/cache/kosha.hpp"

namespace vakya::types {
    // ============================================================================
    // infer_error
    // ============================================================================

    struct infer_error {
        unify_error_kind kind;
        type_ref a{};
        type_ref b{};
        std::string message;
    };

    // ============================================================================
    // infer_cache — kosha LRU keyed by structural_hash uint64_t -> type_ref
    // ============================================================================

    using infer_cache_t = kosha::core::Cache<
        std::uint64_t,
        type_ref,
        kosha::core::LRUPolicy<std::uint64_t>
    >;

    // ============================================================================
    // infer — Algorithm-W, incremental bottom-up
    // ============================================================================

    // Forward declare for recursion
    template <class Expr>
    std::expected<type_ref, infer_error>
    infer_impl(const Expr& expr,
               type_environment& env,
               substitution& subst,
               type_arena& arena,
               type_var_generator& gen,
               infer_cache_t& cache,
               std::size_t depth);

    template <class Expr>
    [[nodiscard]] std::expected<type_ref, infer_error>
    infer(const Expr& expr,
          type_environment& env,
          substitution& subst,
          type_arena& arena,
          type_var_generator& gen,
          infer_cache_t& cache,
          std::size_t max_depth = kMaxWalkDepth) {
        const std::uint64_t hash = vakya::structural_hash(expr);

        // Cache hit: recurrent subtree skip
        if (auto cached = cache.get(hash)) {
            return *cached;
        }

        auto result = infer_impl(expr, env, subst, arena, gen, cache, max_depth);
        if (result) {
            (void)cache.put(hash, *result);
        }
        return result;
    }

    template <class Expr>
    std::expected<type_ref, infer_error>
    infer_impl(const Expr& expr,
               type_environment& env,
               substitution& subst,
               type_arena& arena,
               type_var_generator& gen,
               infer_cache_t& cache,
               std::size_t depth) {
        // Terminals (expr<T>, expr_ref<T>, arithmetic leaves) have no tag_type —
        // assign a fresh variable to be resolved by parent node unification.
        if constexpr (vakya::is_terminal_v<Expr>) {
            type_var_id vid = subst.make_var();
            return arena.intern_variable(vid);
        }
        else {
            using Tag = typename Expr::tag_type;

            if (depth == 0) {
                type_var_id vid = subst.make_var();
                return arena.intern_variable(vid);
            }

            // Infer children bottom-up
            std::vector<type_ref> child_types;
            bool child_error = false;
            infer_error first_child_error;

            vakya::tree::for_each_child(expr, [&](const auto& child) {
                if (child_error) return;
                auto r = infer(child, env, subst, arena, gen, cache, depth - 1);
                if (!r) {
                    child_error = true;
                    first_child_error = r.error();
                }
                else {
                    child_types.push_back(*r);
                }
            });

            if (child_error) return std::unexpected(first_child_error);

            // Emit constraints for this node via typing_rule<Tag>
            auto [result_type, constraints] = detail::invoke_emit<Tag>(child_types, env, arena, gen, subst);

            // Incremental unify each same_type constraint (Algorithm-W: solve as we go)
            for (const constraint& c : constraints) {
                if (c.kind != constraint_kind::same_type) continue;
                if (c.operands.size() < 2) continue;

                auto ur = unify(c.operands[0], c.operands[1], subst, arena);
                if (!ur) {
                    const unify_error& ue = ur.error();
                    return std::unexpected(infer_error{
                        ue.kind, ue.a, ue.b,
                        "type mismatch during inference"
                    });
                }
            }

            // Apply accumulated substitution to result type
            type_ref applied = apply(result_type, subst, arena);
            return applied;
        }
    }
} // namespace vakya::types
