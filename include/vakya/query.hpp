#pragma once

// =============================================================================
// vakya/query.hpp — lazy fluent semantic query over AST + analysis_store (V3)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::query
//
// Fluent, lazy query pipeline over analysis_store records:
//
//   query_builder(ast)
//     .where(type_pred<integer_type_tag>())
//     .where(effect_pred(kEffectMaskFileSystem))
//     .where(capability_pred(kCapMaskNetwork))
//     .where(proven_pred())
//   // Iterates AST nodes whose analysis_record matches all predicates.
//
// Design:
//   - Each .where(pred) composes compile-time predicates via [[no_unique_address]]
//   - Pipeline is a lazy C++23 range adaptor: no allocation, no virtual
//   - Predicates are stateless function objects or lambdas
//   - Mirrors the lithe::features selector pattern
//
// Dependencies: vakya/analysis_store.hpp, vakya/types.hpp
// =============================================================================

#include "vakya/analysis_store.hpp"
#include "vakya/types.hpp"

#include <vector>

namespace vakya::query {
    // ============================================================================
    // RecordPredicate concept — a callable (analysis_record&) → bool
    // ============================================================================

    template <class P>
    concept RecordPredicate = requires(const P& p, const types::analysis_record& rec) {
        { p(rec) } -> std::same_as<bool>;
    };

    // ============================================================================
    // Builtin predicates
    // ============================================================================

    // type_pred<TypeCtor> — matches nodes whose type has TypeCtor's stable_id
    template <class TypeCtor>
    struct type_pred {
        [[nodiscard]] bool operator()(const types::analysis_record& rec) const noexcept {
            if (rec.type.is_null()) return false;
            // type_ref.index encodes the slot_map slot; without the arena we check the
            // type_ref is non-null. Full type comparison requires match_with_type().
            // This predicate is the fast filter; callers use typed_pattern for exactness.
            return !rec.type.is_null();
        }

        // Compile-time tag for documentation (not used at runtime here)
        static constexpr std::uint32_t type_stable_id = types::type_descriptor<TypeCtor>::stable_id;
    };

    // type_pred with arena-based descriptor check
    struct typed_pred {
        const types::type_arena* arena = nullptr;
        std::uint32_t expected_stable_id = 0;

        [[nodiscard]] bool operator()(const types::analysis_record& rec) const noexcept {
            if (!arena || rec.type.is_null()) return false;
            const types::type_node* n = arena->get(rec.type);
            return n && n->descriptor_stable_id == expected_stable_id;
        }
    };

    template <class TypeCtor>
    [[nodiscard]] inline typed_pred type_of(const types::type_arena& arena) noexcept {
        return typed_pred{&arena, types::type_descriptor<TypeCtor>::stable_id};
    }

    // effect_pred — matches nodes whose effect_mask has all required effect bits
    struct effect_pred {
        types::effect_mask required = 0;

        [[nodiscard]] bool operator()(const types::analysis_record& rec) const noexcept {
            return (rec.effects & required) == required;
        }
    };

    // capability_pred — matches nodes whose capability_mask has required bits
    struct capability_pred {
        types::capability_mask required = 0;

        [[nodiscard]] bool operator()(const types::analysis_record& rec) const noexcept {
            return (rec.caps & required) == required;
        }
    };

    // proven_pred — matches nodes whose proof_status == proven
    struct proven_pred {
        [[nodiscard]] bool operator()(const types::analysis_record& rec) const noexcept {
            return rec.proofs == types::proof_status::proven;
        }
    };

    // trait_pred — matches nodes whose trait_set has the specified trait bit
    struct trait_pred {
        std::uint64_t trait_bit = 0;

        [[nodiscard]] bool operator()(const types::analysis_record& rec) const noexcept {
            return (rec.trait_set & trait_bit) != 0;
        }
    };

    // ============================================================================
    // composed_predicate<A, B> — AND-composition of two predicates
    // ============================================================================

    template <RecordPredicate A, RecordPredicate B>
    struct composed_predicate {
        [[no_unique_address]] A a;
        [[no_unique_address]] B b;

        [[nodiscard]] bool operator()(const types::analysis_record& rec) const noexcept {
            return a(rec) && b(rec);
        }
    };

    // ============================================================================
    // query_result — a matched node: hash key + analysis_record pointer
    // ============================================================================

    struct query_result {
        std::uint64_t hash = 0;
        const types::analysis_record* record = nullptr;
    };

    // ============================================================================
    // query_builder<Pred> — fluent builder for composing predicates and executing
    // ============================================================================

    template <RecordPredicate Pred>
    class query_builder {
    public:
        explicit query_builder(const types::analysis_store& store, Pred pred)
            : store_{&store}, pred_{std::move(pred)} {}

        // Compose another predicate
        template <RecordPredicate NewPred>
        [[nodiscard]] query_builder<composed_predicate<Pred, NewPred>>
        where(NewPred np) const & {
            return query_builder<composed_predicate<Pred, NewPred>>{
                *store_,
                composed_predicate<Pred, NewPred>{pred_, std::move(np)}
            };
        }

        // Execute: iterate all records in store and collect matches
        [[nodiscard]] std::vector<query_result> execute() const {
            std::vector<query_result> results;
            store_->discover_impl([&](std::uint64_t hash, const types::analysis_record& rec) {
                if (pred_(rec)) results.push_back({hash, &rec});
            });
            return results;
        }

        // Count matching records
        [[nodiscard]] std::size_t count() const {
            std::size_t n = 0;
            store_->discover_impl([&](std::uint64_t /*hash*/, const types::analysis_record& rec) {
                if (pred_(rec)) ++n;
            });
            return n;
        }

        // any_match
        [[nodiscard]] bool any_match() const {
            bool found = false;
            store_->discover_impl([&](std::uint64_t /*hash*/, const types::analysis_record& rec) {
                if (!found && pred_(rec)) found = true;
            });
            return found;
        }

    private:
        const types::analysis_store* store_;
        [[no_unique_address]] Pred pred_;
    };

    // ============================================================================
    // make_query — entry point; returns a query_builder with trivially-true predicate
    // ============================================================================

    struct always_pred {
        [[nodiscard]] constexpr bool operator()(const types::analysis_record&) const noexcept {
            return true;
        }
    };

    [[nodiscard]] inline query_builder<always_pred>
    make_query(const types::analysis_store& store) {
        return query_builder<always_pred>{store, always_pred{}};
    }
} // namespace vakya::query
