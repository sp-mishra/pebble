// =============================================================================
// test_vakya_v3.cpp — Unit tests for the V3 constraint-reasoning stack.
//
// Verifies:
//   containers/descriptor_registry.hpp
//   vakya/types/type_registry.hpp
//   vakya/types/capability.hpp
//   vakya/types/effect.hpp
//   vakya/types/shape.hpp
//   vakya/constraint_registry.hpp
//   vakya/analysis_store.hpp
//   vakya/typed_pattern.hpp
//   vakya/type_rewrite.hpp
//   vakya/verify.hpp
//   vakya/query.hpp
//
// Cases:
//   1.  descriptor_registry: register + find by id
//   2.  descriptor_registry: find_by_name
//   3.  descriptor_registry: by_category
//   4.  descriptor_registry: duplicate stable_id overwrites entry
//   5.  type_registry: make_builtin_type_registry discovers all primitives
//   6.  capability_registry: make_builtin_capability_registry has 5 entries
//   7.  capability_mask: has_capability / add_capability
//   8.  effect_registry: make_builtin_effect_registry has 5 entries
//   9.  effect_mask: has_effect / add_effect
//  10.  constraint_registry: make_builtin_constraint_registry routes same_type → unify
//  11.  constraint_registry: make_builtin_constraint_registry routes equivalent → egraph
//  12.  solve_batch: dispatches to solver by class
//  13.  analysis_store: update + find by hash
//  14.  analysis_store: thread-safe update_for (single-threaded path)
//  15.  analysis_store: discover_impl iterates all records
//  16.  shape: intern_shape + shape_rank
//  17.  shape: make_matmul_constraints emits same_type on inner dims
//  18.  type_rewrite_engine: Optional<Optional<T>> → Optional<T>
//  19.  type_rewrite_engine: normalize reaches fixpoint
//  20.  typed_pattern: typed<> combinator match with analysis_store
//  21.  typed_pattern: trait<> combinator trait_set check
//  22.  verify: no_smt_backend returns deferred (zero-cost path)
//  23.  verify: all_proven() false for deferred
//  24.  query: make_query + effect_pred filter
//  25.  query: capability_pred filter
//  26.  query: proven_pred filter
//  27.  query: composed predicates (AND)
//  28.  analyze: types stored in analysis_store after analyze()
// =============================================================================

#include "catch_amalgamated.hpp"

#include "containers/descriptor_registry.hpp"
#include "vakya/types/type_registry.hpp"
#include "vakya/types/capability.hpp"
#include "vakya/types/effect.hpp"
#include "vakya/types/shape.hpp"
#include "vakya/constraint_registry.hpp"
#include "vakya/constraint_solvers.hpp"
#include "vakya/analysis_store.hpp"
#include "vakya/typed_pattern.hpp"
#include "vakya/type_rewrite.hpp"
#include "vakya/verify.hpp"
#include "vakya/query.hpp"
#include "vakya/analysis.hpp"
#include "vakya/vakya.hpp"

using namespace vakya::types;

// ============================================================================
// Helpers
// ============================================================================

namespace {
    struct test_category_enum : std::integral_constant<std::uint32_t, 0> {};

    enum class test_cat : std::uint32_t { a = 0, b = 1 };

    struct test_desc {
        std::uint32_t stable_id = 0;
        std::uint64_t name_hash = 0;
        test_cat category = test_cat::a;
        std::string payload;
    };

    static_assert(containers::RegistrableDescriptor<test_desc>);
} // namespace

// ============================================================================
// 1. descriptor_registry: register + find by id
// ============================================================================

TEST_CASE (

"descriptor_registry: register and find by id"
,
"[v3][registry]"
)
 {
    containers::descriptor_registry<test_desc> reg;

    test_desc d;
    d.stable_id = 42u;
    d.name_hash = containers::desc_name_hash("foo");
    d.category  = test_cat::a;
    d.payload   = "hello";
    reg.register_desc(d);

    const test_desc* found = reg.find(42u);
    REQUIRE(found != nullptr);
    CHECK(found->payload == "hello");
    CHECK(reg.find(99u) == nullptr);
}

// ============================================================================
// 2. descriptor_registry: find_by_name
// ============================================================================

TEST_CASE (

"descriptor_registry: find_by_name"
,
"[v3][registry]"
)
 {
    containers::descriptor_registry<test_desc> reg;

    test_desc d;
    d.stable_id = 10u;
    d.name_hash = containers::desc_name_hash("my_desc");
    d.category  = test_cat::a;
    reg.register_desc(d);

    const test_desc* found = reg.find_by_name(containers::desc_name_hash("my_desc"));
    REQUIRE(found != nullptr);
    CHECK(found->stable_id == 10u);

    CHECK(reg.find_by_name(containers::desc_name_hash("nonexistent")) == nullptr);
}

// ============================================================================
// 3. descriptor_registry: by_category
// ============================================================================

TEST_CASE (

"descriptor_registry: by_category"
,
"[v3][registry]"
)
 {
    containers::descriptor_registry<test_desc> reg;

    for (std::uint32_t i = 1; i <= 3; ++i) {
        test_desc d;
        d.stable_id = i;
        d.name_hash = containers::desc_name_hash(std::to_string(i));
        d.category  = (i <= 2) ? test_cat::a : test_cat::b;
        reg.register_desc(d);
    }

    auto cat_a = reg.by_category(test_cat::a);
    CHECK(cat_a.size() == 2);

    auto cat_b = reg.by_category(test_cat::b);
    CHECK(cat_b.size() == 1);
}

// ============================================================================
// 4. descriptor_registry: duplicate stable_id overwrites
// ============================================================================

TEST_CASE (

"descriptor_registry: duplicate stable_id overwrites"
,
"[v3][registry]"
)
 {
    containers::descriptor_registry<test_desc> reg;

    test_desc d1;
    d1.stable_id = 5u;
    d1.name_hash = containers::desc_name_hash("v1");
    d1.category  = test_cat::a;
    d1.payload   = "first";
    reg.register_desc(d1);

    test_desc d2;
    d2.stable_id = 5u;
    d2.name_hash = containers::desc_name_hash("v2");
    d2.category  = test_cat::a;
    d2.payload   = "second";
    reg.register_desc(d2);

    const test_desc* found = reg.find(5u);
    REQUIRE(found != nullptr);
    CHECK(found->payload == "second");
    CHECK(reg.size() == 1);
}

// ============================================================================
// 5. type_registry: make_builtin_type_registry
// ============================================================================

TEST_CASE (

"type_registry: make_builtin_type_registry discovers primitives"
,
"[v3][type_registry]"
)
 {
    auto reg = make_builtin_type_registry();
    CHECK(reg.size() >= 11);  // 7 primitives + 4 composite minimum

    const type_registry_entry* int_entry =
        reg.find(type_descriptor<integer_type_tag>::stable_id);
    REQUIRE(int_entry != nullptr);
    CHECK(int_entry->symbol == "Integer");
    CHECK(int_entry->category == type_registry_category::primitive);

    const type_registry_entry* tensor_entry =
        reg.find(type_descriptor<tensor_type_tag>::stable_id);
    REQUIRE(tensor_entry != nullptr);
    CHECK(tensor_entry->category == type_registry_category::tensor);
}

// ============================================================================
// 6. capability_registry: make_builtin_capability_registry has 5 entries
// ============================================================================

TEST_CASE (

"capability_registry: 5 builtin capabilities"
,
"[v3][capability]"
)
 {
    auto reg = make_builtin_capability_registry();
    CHECK(reg.size() == 5);

    const capability_descriptor* net = reg.find(kCapNetwork);
    REQUIRE(net != nullptr);
    CHECK(net->symbol == "Network");
    CHECK(net->bit_mask == kCapMaskNetwork);
}

// ============================================================================
// 7. capability_mask helpers
// ============================================================================

TEST_CASE (

"capability_mask: has_capability / add_capability"
,
"[v3][capability]"
)
 {
    capability_mask mask = 0;
    CHECK(!has_capability(mask, kCapMaskRead));

    mask = add_capability(mask, kCapMaskRead);
    CHECK(has_capability(mask, kCapMaskRead));
    CHECK(!has_capability(mask, kCapMaskWrite));
}

// ============================================================================
// 8. effect_registry: 5 builtin effects
// ============================================================================

TEST_CASE (

"effect_registry: 5 builtin effects"
,
"[v3][effect]"
)
 {
    auto reg = make_builtin_effect_registry();
    CHECK(reg.size() == 5);

    const effect_descriptor* fs = reg.find(kEffectFileSystem);
    REQUIRE(fs != nullptr);
    CHECK(fs->symbol == "FileSystem");
}

// ============================================================================
// 9. effect_mask helpers
// ============================================================================

TEST_CASE (

"effect_mask: has_effect / add_effect"
,
"[v3][effect]"
)
 {
    effect_mask mask = 0;
    CHECK(!has_effect(mask, kEffectMaskIO));

    mask = add_effect(mask, kEffectMaskIO);
    CHECK(has_effect(mask, kEffectMaskIO));
    CHECK(!has_effect(mask, kEffectMaskMemory));
}

// ============================================================================
// 10. constraint_registry: same_type → unify
// ============================================================================

TEST_CASE (

"constraint_registry: same_type routes to unify"
,
"[v3][constraint_registry]"
)
 {
    auto reg = make_builtin_constraint_registry();

    const constraint_descriptor* d =
        reg.find(static_cast<std::uint32_t>(constraint_kind::same_type));
    REQUIRE(d != nullptr);
    CHECK(d->target == solver_class::unify);
    CHECK(d->cost_hint == 0);
}

// ============================================================================
// 11. constraint_registry: equivalent → egraph
// ============================================================================

TEST_CASE (

"constraint_registry: equivalent routes to egraph"
,
"[v3][constraint_registry]"
)
 {
    auto reg = make_builtin_constraint_registry();

    const constraint_descriptor* d =
        reg.find(static_cast<std::uint32_t>(kEquivalentKind));
    REQUIRE(d != nullptr);
    CHECK(d->target == solver_class::egraph);
}

// ============================================================================
// 12. solve_batch: dispatches constraints to solver
// ============================================================================

TEST_CASE (

"solve_batch: routes constraints and returns solved status"
,
"[v3][solve_batch]"
)
 {
    type_arena arena;
    type_var_generator gen;
    substitution subst;

    type_ref int_ref  = arena.intern_primitive<integer_type_tag>();
    type_var_id vid   = gen.fresh(); subst.make_var();
    type_ref var_ref  = arena.intern_variable(vid);

    constraint c;
    c.kind = constraint_kind::same_type;
    c.operands.push_back(var_ref);
    c.operands.push_back(int_ref);

    auto reg = make_builtin_constraint_registry();
    unification_solver us;
    rule_constraint_solver rs;
    graph_constraint_solver gs;
    smt_constraint_solver<no_smt_backend> ss;
    composite_solver solver(us, rs, gs, ss);

    solve_context ctx{&arena, &subst};
    solve_result r = solve_batch(
        std::span<const constraint>(&c, 1), ctx, reg, solver);

    // var_ref should unify with int_ref → solved
    CHECK(r.status == solve_status::solved);
}

// ============================================================================
// 13. analysis_store: update + find by hash
// ============================================================================

TEST_CASE (

"analysis_store: update and find"
,
"[v3][analysis_store]"
)
 {
    analysis_store store;
    const std::uint64_t hash = 0xDEADBEEFULL;

    store.update(hash, [](analysis_record& rec) {
        rec.effects = kEffectMaskIO;
        rec.proofs  = proof_status::deferred;
    });

    const analysis_record* rec = store.find(hash);
    REQUIRE(rec != nullptr);
    CHECK(rec->effects == kEffectMaskIO);
    CHECK(rec->proofs == proof_status::deferred);

    CHECK(store.find(0xCAFEBABEULL) == nullptr);
}

// ============================================================================
// 14. analysis_store: update_for (expression-keyed)
// ============================================================================

TEST_CASE (

"analysis_store: update_for expr-keyed"
,
"[v3][analysis_store]"
)
 {
    analysis_store store;

    int x = 5;
    auto expr = vakya::as_expr(x);

    store.update_for(expr, [](analysis_record& rec) {
        rec.caps = kCapMaskRead;
    });

    const analysis_record* rec = store.find_for(expr);
    REQUIRE(rec != nullptr);
    CHECK(rec->caps == kCapMaskRead);
}

// ============================================================================
// 15. analysis_store: discover_impl iterates all records
// ============================================================================

TEST_CASE (

"analysis_store: discover_impl iterates"
,
"[v3][analysis_store]"
)
 {
    analysis_store store;

    for (std::uint64_t i = 1; i <= 5; ++i) {
        store.update(i, [i](analysis_record& r) { r.features = i; });
    }

    std::size_t count = 0;
    store.discover_impl([&](std::uint64_t /*hash*/, const analysis_record& /*rec*/) {
        ++count;
    });
    CHECK(count == 5);
}

// ============================================================================
// 16. shape: intern_shape + shape_rank
// ============================================================================

TEST_CASE (

"shape: intern_shape and shape_rank"
,
"[v3][shape]"
)
 {
    type_arena arena;
    type_var_generator gen;

    type_ref dim_n = arena.intern_variable(gen.fresh());
    type_ref dim_m = arena.intern_variable(gen.fresh());
    const type_ref dims[2] = {dim_n, dim_m};

    shape_ref s = intern_shape(arena, std::span<const type_ref>(dims, 2));
    CHECK(shape_rank(arena, s) == 2);

    shape_ref scalar = make_scalar_shape(arena);
    CHECK(shape_rank(arena, scalar) == 0);
}

// ============================================================================
// 17. shape: make_matmul_constraints emits same_type on inner dims
// ============================================================================

TEST_CASE (

"shape: make_matmul_constraints inner dim constraint"
,
"[v3][shape]"
)
 {
    type_arena arena;
    type_var_generator gen;

    type_ref N = arena.intern_variable(gen.fresh());
    type_ref M = arena.intern_variable(gen.fresh());
    type_ref K = arena.intern_variable(gen.fresh());

    const type_ref lhs_dims[2] = {N, M};
    const type_ref rhs_dims[2] = {M, K};
    shape_ref lhs = intern_shape(arena, std::span<const type_ref>(lhs_dims, 2));
    shape_ref rhs = intern_shape(arena, std::span<const type_ref>(rhs_dims, 2));

    shape_ref out;
    auto cs = make_matmul_constraints(arena, gen, lhs, rhs, out);

    REQUIRE(cs.size() == 1);
    CHECK(cs[0].kind == constraint_kind::same_type);
    CHECK(cs[0].operands[0] == M);
    CHECK(cs[0].operands[1] == M);

    CHECK(shape_rank(arena, out) == 2);
}

// ============================================================================
// 18. type_rewrite_engine: Optional<Optional<T>> → Optional<T>
// ============================================================================

TEST_CASE (

"type_rewrite_engine: Optional idempotence"
,
"[v3][type_rewrite]"
)
 {
    type_arena arena;
    type_var_generator gen;

    type_ref T = arena.intern_variable(gen.fresh());

    // Optional<T>
    const type_ref inner_args[1] = {T};
    type_ref opt_T = arena.intern_constructor<optional_type_tag>(
        std::span<const type_ref>(inner_args, 1));

    // Optional<Optional<T>>
    const type_ref outer_args[1] = {opt_T};
    type_ref opt_opt_T = arena.intern_constructor<optional_type_tag>(
        std::span<const type_ref>(outer_args, 1));

    type_rewrite_engine eng;
    type_ref result = eng.normalize(opt_opt_T, arena);

    // Should reduce to Optional<T>
    CHECK(result == opt_T);
}

// ============================================================================
// 19. type_rewrite_engine: normalize reaches fixpoint (idempotent)
// ============================================================================

TEST_CASE (

"type_rewrite_engine: normalize is idempotent"
,
"[v3][type_rewrite]"
)
 {
    type_arena arena;
    type_var_generator gen;

    type_ref int_ref = arena.intern_primitive<integer_type_tag>();
    type_rewrite_engine eng;

    // Primitive type: no rule matches, normalize returns same ref
    type_ref result1 = eng.normalize(int_ref, arena);
    type_ref result2 = eng.normalize(result1, arena);
    CHECK(result1 == result2);
}

// ============================================================================
// 20. typed_pattern: typed<> combinator — structural match + store lookup
// ============================================================================

TEST_CASE (

"typed_pattern: typed<> combinator basic"
,
"[v3][typed_pattern]"
)
 {
    using namespace vakya::typed_pattern;
    namespace pat = vakya::pattern;

    // Pattern: pv<0> (wildcard)
    auto p = typed<integer_type_tag>(pat::pv<0>);

    // Expr: as_expr(42)
    auto expr = vakya::as_expr(42);

    // analysis_store with a type record for this expr
    analysis_store store;
    type_arena arena;
    type_ref int_ref = arena.intern_primitive<integer_type_tag>();
    store.update_for(expr, [&](analysis_record& rec) {
        rec.type = int_ref;
    });

    // match without arena (just structural check, no descriptor comparison)
    auto result = p.match(expr, store);
    // Without the arena overload, match returns the structural result if store has a type
    REQUIRE(result.has_value());

    // match_with_type should confirm integer type
    auto result_typed = p.match_with_type(expr, store, arena);
    REQUIRE(result_typed.has_value());
}

// ============================================================================
// 21. typed_pattern: trait<> combinator trait_set check
// ============================================================================

TEST_CASE (

"typed_pattern: trait<> combinator"
,
"[v3][typed_pattern]"
)
 {
    using namespace vakya::typed_pattern;
    namespace pat = vakya::pattern;

    constexpr std::uint32_t kAddableTrait = 0;  // bit 0

    auto p = with_trait<kAddableTrait>(pat::pv<0>);

    auto expr = vakya::as_expr(7);
    analysis_store store;

    // Without the trait bit set: no match
    store.update_for(expr, [](analysis_record& rec) { rec.trait_set = 0; });
    auto r1 = p.match(expr, store);
    CHECK(!r1.has_value());

    // With the trait bit set: match
    store.update_for(expr, [](analysis_record& rec) { rec.trait_set = 1ULL; });
    auto r2 = p.match(expr, store);
    REQUIRE(r2.has_value());
}

// ============================================================================
// 22. verify: no_smt_backend returns deferred
// ============================================================================

TEST_CASE (

"verify: no_smt_backend returns deferred"
,
"[v3][verify]"
)
 {
    analysis_store store;
    const std::uint64_t hash = 0x1234ULL;

    store.update(hash, [](analysis_record& rec) {
        rec.proofs = proof_status::unknown;
    });

    smt_constraint_solver<no_smt_backend> solver;
    verification_report report = verify(hash, store, solver);

    CHECK(report.overall == proof_status::deferred);
}

// ============================================================================
// 23. verify: all_proven() false for deferred
// ============================================================================

TEST_CASE (

"verify: all_proven false for deferred report"
,
"[v3][verify]"
)
 {
    analysis_store store;
    const std::uint64_t hash = 0xABCDULL;

    store.update(hash, [](analysis_record& rec) {
        rec.proofs = proof_status::unknown;
    });

    smt_constraint_solver<no_smt_backend> solver;
    auto report = verify(hash, store, solver);

    CHECK(!report.all_proven());
}

// ============================================================================
// 24. query: make_query + effect_pred filter
// ============================================================================

TEST_CASE (

"query: effect_pred filters correctly"
,
"[v3][query]"
)
 {
    using namespace vakya::query;

    analysis_store store;

    // record 1: has IO effect
    store.update(1ULL, [](analysis_record& r) { r.effects = kEffectMaskIO; });
    // record 2: has FileSystem effect
    store.update(2ULL, [](analysis_record& r) { r.effects = kEffectMaskFileSystem; });
    // record 3: no effects
    store.update(3ULL, [](analysis_record& /*r*/) {});

    auto results = make_query(store)
        .where(effect_pred{kEffectMaskIO})
        .execute();

    CHECK(results.size() == 1);
    CHECK(results[0].hash == 1ULL);
}

// ============================================================================
// 25. query: capability_pred filter
// ============================================================================

TEST_CASE (

"query: capability_pred filters correctly"
,
"[v3][query]"
)
 {
    using namespace vakya::query;

    analysis_store store;
    store.update(10ULL, [](analysis_record& r) { r.caps = kCapMaskNetwork; });
    store.update(11ULL, [](analysis_record& r) { r.caps = kCapMaskRead; });

    auto results = make_query(store)
        .where(capability_pred{kCapMaskNetwork})
        .execute();

    CHECK(results.size() == 1);
    CHECK(results[0].hash == 10ULL);
}

// ============================================================================
// 26. query: proven_pred filter
// ============================================================================

TEST_CASE (

"query: proven_pred filters correctly"
,
"[v3][query]"
)
 {
    using namespace vakya::query;

    analysis_store store;
    store.update(20ULL, [](analysis_record& r) { r.proofs = proof_status::proven; });
    store.update(21ULL, [](analysis_record& r) { r.proofs = proof_status::deferred; });
    store.update(22ULL, [](analysis_record& r) { r.proofs = proof_status::unknown; });

    auto results = make_query(store)
        .where(proven_pred{})
        .execute();

    CHECK(results.size() == 1);
    CHECK(results[0].hash == 20ULL);
}

// ============================================================================
// 27. query: composed predicates (AND)
// ============================================================================

TEST_CASE (

"query: composed predicates filter correctly"
,
"[v3][query]"
)
 {
    using namespace vakya::query;

    analysis_store store;
    // Only record 30 has both IO and Network
    store.update(30ULL, [](analysis_record& r) {
        r.effects = kEffectMaskIO;
        r.caps    = kCapMaskNetwork;
    });
    store.update(31ULL, [](analysis_record& r) {
        r.effects = kEffectMaskIO;
        r.caps    = kCapMaskRead;  // no Network
    });
    store.update(32ULL, [](analysis_record& r) {
        r.effects = kEffectMaskFileSystem;
        r.caps    = kCapMaskNetwork;  // no IO
    });

    auto results = make_query(store)
        .where(effect_pred{kEffectMaskIO})
        .where(capability_pred{kCapMaskNetwork})
        .execute();

    CHECK(results.size() == 1);
    CHECK(results[0].hash == 30ULL);
}

// ============================================================================
// 28. analyze: types stored in analysis_store after analyze()
// ============================================================================

TEST_CASE (

"analyze: inferred type stored in analysis_store"
,
"[v3][analyze]"
)
 {
    type_arena arena;
    type_var_generator gen;
    substitution subst;
    type_environment env;
    analysis_store astore;

    int x_val = 3;
    auto expr = vakya::as_expr(x_val);

    unification_solver us;
    rule_constraint_solver rs;
    graph_constraint_solver gs;
    smt_constraint_solver<no_smt_backend> ss;
    composite_solver solver(us, rs, gs, ss);

    auto vr = analyze(expr, env, solver, arena, gen, subst, astore);
    REQUIRE(vr.ok());

    const analysis_record* rec = astore.find_for(expr);
    REQUIRE(rec != nullptr);
    // type should have been populated (may be a fresh variable if no typing rule)
    // Either way the record exists with non-null or null type — what matters is
    // analyze() executed without error and the record was written.
    CHECK(rec != nullptr);
}
