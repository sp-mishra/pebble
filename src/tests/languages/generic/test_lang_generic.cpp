// =============================================================================
// test_lang_generic.cpp — Unit tests for include/languages/generic/ layer.
//
// Verifies: generic/identity.hpp
//           generic/diagnostics.hpp
//           generic/reflection.hpp
//           generic/descriptors.hpp
//           generic/effects.hpp
//           generic/symbol_table.hpp
//           generic/module_system.hpp
//           generic/proof.hpp
//           generic/rules.hpp
//           generic/import_resolver.hpp
//           generic/registry.hpp
//
//  1.  stable_entity_id derivation + cross-compile stability.
//  2.  stable_entity_id kind fields correct.
//  3.  fp_combine is non-symmetric (order matters).
//  4.  module_resolver with pluggable extension (custom ".mylang").
//  5.  circular import detection — LANG-IMP-003.
//  6.  topo-sort ordering (A→B→C produces C, B, A order).
//  7.  version constraint check — LANG-IMP-004.
//  8.  capability-gated import — LANG-IMP-005.
//  9.  symbol flow: exported symbols from importee flow to resolve_result.
// 10.  rule_engine requires_ rule check.
// 11.  rule_engine conflicts rule — mutual exclusion.
// 12.  rule_engine generates_obligation → obligation emitted.
// 13.  symbol_table<uppercase_export_policy> — uppercase → exported.
// 14.  collecting_sink errors/warnings separation.
// 15.  discharge_driver three-way: assume→proven, check→unknown+guard.
// 16.  registry_builder collision detection — LANG-REG-001.
// 17.  registry_builder build() — global fingerprint non-zero.
// 18.  dependency_graph cycle_nodes() — returns cycle participants.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "languages/generic/core/identity.hpp"
#include "languages/generic/core/diagnostics.hpp"
#include "languages/generic/host/descriptors.hpp"
#include "languages/generic/semantic/symbol_table.hpp"
#include "languages/generic/module/module_system.hpp"
#include "languages/generic/semantic/proof.hpp"
#include "languages/generic/semantic/rules.hpp"
#include "languages/generic/module/import_resolver.hpp"
#include "languages/generic/host/registry.hpp"
#include "vakya/smt.hpp"

#include <string>
#include <string_view>

// ============================================================================
// Test 1 — stable_entity_id derivation stability
// ============================================================================

TEST_CASE("lang: stable_entity_id derivation is stable", "[lang][identity]") {
    constexpr auto id1 = lang::detail::make_id("math.dot", lang::kKindFunction);
    constexpr auto id2 = lang::detail::make_id("math.dot", lang::kKindFunction);

    CHECK(id1 == id2);
    CHECK(id1.valid());
    CHECK(id1.kind == lang::kKindFunction);
}

// ============================================================================
// Test 2 — stable_entity_id kind fields
// ============================================================================

TEST_CASE("lang: stable_entity_id kind constants correct", "[lang][identity]") {
    constexpr auto fn  = lang::detail::make_id("a.b", lang::kKindFunction);
    constexpr auto ty  = lang::detail::make_id("a.b", lang::kKindType);
    constexpr auto mod = lang::detail::make_id("a.b", lang::kKindModule);

    CHECK(fn.kind  == lang::kKindFunction);
    CHECK(ty.kind  == lang::kKindType);
    CHECK(mod.kind == lang::kKindModule);
    // Different kinds → different ids
    CHECK(fn != ty);
    CHECK(fn != mod);
}

// ============================================================================
// Test 3 — fp_combine is non-symmetric
// ============================================================================

TEST_CASE("lang: fp_combine order matters", "[lang][identity]") {
    constexpr lang::descriptor_fingerprint a = 0xDEADBEEFULL;
    constexpr lang::descriptor_fingerprint b = 0xCAFEBABEULL;
    constexpr auto ab = lang::detail::fp_combine(a, b);
    constexpr auto ba = lang::detail::fp_combine(b, a);
    CHECK(ab != ba);
}

// ============================================================================
// Test 4 — module_resolver with pluggable extension
// ============================================================================

TEST_CASE("lang: module_resolver pluggable source_extension", "[lang][module_system]") {
    lang::resolver_config cfg;
    cfg.source_extension = ".mylang";
    lang::module_resolver resolver{cfg};

    // Native modules resolve regardless of extension.
    lang::module_descriptor d;
    d.name = "core";
    d.kind = lang::module_kind::native;
    resolver.add_native(d);

    auto result = resolver.resolve("core");
    REQUIRE(result.has_value());
    CHECK(result->kind == lang::module_kind::native);
    CHECK(result->name == "core");

    // Unregistered module returns nullopt.
    CHECK_FALSE(resolver.resolve("unknown").has_value());

    CHECK(resolver.config().source_extension == ".mylang");
}

// ============================================================================
// Test 5 — circular import detection LANG-IMP-003
// ============================================================================

TEST_CASE("lang: import_graph circular import LANG-IMP-003", "[lang][import_resolver]") {
    lang::import_graph graph;
    // A → B → C → A (cycle)
    graph.declare_imports("A", {{"B"}});
    graph.declare_imports("B", {{"C"}});
    graph.declare_imports("C", {{"A"}});

    lang::module_resolver resolver;
    auto result = graph.resolve(resolver);

    REQUIRE_FALSE(result.ok());
    bool found_circular = false;
    for (const auto& e : result.errors)
        if (e.kind == lang::import_error_kind{lang::import_error_kind::kind::circular}) { found_circular = true; break; }
    CHECK(found_circular);
}

// ============================================================================
// Test 6 — topo sort ordering: C before B before A
// ============================================================================

TEST_CASE("lang: import_graph topo sort importees first", "[lang][import_resolver]") {
    lang::module_resolver resolver;
    // Register native modules so they resolve.
    for (const auto& name : {"A", "B", "C"}) {
        lang::module_descriptor d;
        d.name = name;
        d.kind = lang::module_kind::native;
        resolver.add_native(d);
    }

    lang::import_graph graph;
    graph.declare_imports("A", {{"B"}});
    graph.declare_imports("B", {{"C"}});
    // C has no imports.

    auto result = graph.resolve(resolver);
    REQUIRE(result.ok());
    REQUIRE(result.compile_order.size() >= 3u);

    // C must appear before B, B must appear before A.
    auto pos = [&](std::string_view name) -> std::size_t {
        for (std::size_t i = 0; i < result.compile_order.size(); ++i)
            if (result.compile_order[i] == name) return i;
        return std::string::npos;
    };
    CHECK(pos("C") < pos("B"));
    CHECK(pos("B") < pos("A"));
}

// ============================================================================
// Test 7 — version constraint LANG-IMP-004
// ============================================================================

TEST_CASE("lang: import_graph version constraint LANG-IMP-004", "[lang][import_resolver]") {
    lang::module_resolver resolver;
    lang::module_descriptor d;
    d.name    = "util";
    d.kind    = lang::module_kind::native;
    d.version = {0, 5, 0}; // version 0.5.0
    resolver.add_native(d);

    lang::import_graph graph;
    // Require version >= 1.0.0 — should fail.
    graph.declare_imports("app", {
        lang::import_spec{.module_name = "util",
                          .min_version = {1, 0, 0},
                          .max_version = {255, 255, 255}}
    });

    auto result = graph.resolve(resolver);
    REQUIRE_FALSE(result.ok());
    CHECK(result.errors[0].kind == lang::import_error_kind{lang::import_error_kind::kind::version_mismatch});
    CHECK(result.errors[0].code == "LANG-IMP-004");
}

// ============================================================================
// Test 8 — capability gate LANG-IMP-005
// ============================================================================

TEST_CASE("lang: import_graph capability gate LANG-IMP-005", "[lang][import_resolver]") {
    lang::module_resolver resolver;
    lang::module_descriptor d;
    d.name = "gpu_lib";
    d.kind = lang::module_kind::native;
    resolver.add_native(d);

    lang::import_graph graph;
    graph.declare_imports("app", {
        lang::import_spec{.module_name         = "gpu_lib",
                          .required_capabilities = 0xFF /* some capability bit */}
    });

    // No capabilities registered for gpu_lib.
    lang::module_capabilities_map caps;
    auto result = graph.resolve(resolver, caps);

    REQUIRE_FALSE(result.ok());
    CHECK(result.errors[0].kind == lang::import_error_kind{lang::import_error_kind::kind::capability_mismatch});
    CHECK(result.errors[0].code == "LANG-IMP-005");
}

// ============================================================================
// Test 9 — symbol flow from resolved importee
// ============================================================================

TEST_CASE("lang: import_graph symbol flow from importee", "[lang][import_resolver]") {
    lang::module_resolver resolver;
    lang::module_descriptor d;
    d.name = "math";
    d.kind = lang::module_kind::native;
    resolver.add_native(d);

    lang::import_graph graph;
    graph.declare_imports("app", {{"math"}});

    lang::symbol_provider provider = [](std::string_view mod) -> std::vector<lang::symbol_entry> {
        if (mod == "math") {
            lang::symbol_entry e;
            e.name       = "Dot";
            e.kind       = lang::sym_kind::function;
            e.visibility = lang::sym_visibility::exported;
            return {e};
        }
        return {};
    };

    auto result = graph.resolve(resolver, {}, provider);
    REQUIRE(result.ok());
    REQUIRE_FALSE(result.resolved.empty());
    CHECK(result.resolved[0].desc.name == "math");
    CHECK(result.resolved[0].exported_symbols.size() == 1u);
    CHECK(result.resolved[0].exported_symbols[0].name == "Dot");
}

// ============================================================================
// Test 10 — rule_engine requires_ violation
// ============================================================================

TEST_CASE("lang: rule_engine requires_ violation", "[lang][rules]") {
    lang::rule_engine engine;
    lang::rule_descriptor r;
    r.id      = lang::detail::make_id("tx.requires_io", lang::kKindRule);
    r.kind    = lang::rule_kind::requires_;
    r.subject = "tx";
    r.object  = "io";
    r.diagnostic_code = "LANG-RULE-001";
    engine.add_rule(r);

    // Symbol table with "tx" but not "io".
    lang::symbol_table<> tbl;
    lang::symbol_entry e;
    e.name = "tx"; e.kind = lang::sym_kind::module;
    tbl.define(e);

    lang::symbol_table_view_adapter adapter{tbl};
    lang::dependency_graph graph;
    lang::module_capabilities_map caps;

    auto result = engine.check(adapter, graph, caps);
    REQUIRE_FALSE(result.ok());
    CHECK(result.violated[0].subject == "tx");
}

// ============================================================================
// Test 11 — rule_engine conflicts violation
// ============================================================================

TEST_CASE("lang: rule_engine conflicts violation", "[lang][rules]") {
    lang::rule_engine engine;
    lang::rule_descriptor r;
    r.id      = lang::detail::make_id("pure.conflicts_io", lang::kKindRule);
    r.kind    = lang::rule_kind::conflicts;
    r.subject = "pure";
    r.object  = "io";
    engine.add_rule(r);

    // Both "pure" and "io" present — conflict.
    lang::symbol_table<> tbl;
    for (const auto* name : {"pure", "io"}) {
        lang::symbol_entry e;
        e.name = name; e.kind = lang::sym_kind::constant;
        tbl.define(e);
    }

    lang::symbol_table_view_adapter adapter{tbl};
    lang::dependency_graph graph;
    lang::module_capabilities_map caps;

    auto result = engine.check(adapter, graph, caps);
    REQUIRE_FALSE(result.ok());
    CHECK(result.violated[0].kind == lang::rule_kind::conflicts);
}

// ============================================================================
// Test 12 — rule_engine generates_obligation
// ============================================================================

TEST_CASE("lang: rule_engine generates_obligation on violation", "[lang][rules]") {
    lang::rule_engine engine;
    lang::rule_descriptor r;
    r.id                      = lang::detail::make_id("div.safe", lang::kKindRule);
    r.kind                    = lang::rule_kind::requires_;
    r.subject                 = "safe_div";
    r.object                  = "nonzero_divisor"; // not present
    r.generates_obligation    = true;
    r.obligation_description  = "divisor must be non-zero";
    engine.add_rule(r);

    lang::symbol_table<> tbl;
    lang::symbol_entry e;
    e.name = "safe_div"; e.kind = lang::sym_kind::function;
    tbl.define(e);

    lang::symbol_table_view_adapter adapter{tbl};
    lang::dependency_graph graph;
    lang::module_capabilities_map caps;

    auto result = engine.check(adapter, graph, caps);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.obligations.size() == 1u);
    CHECK(result.obligations[0].description == "divisor must be non-zero");
}

// ============================================================================
// Test 13 — symbol_table<uppercase_export_policy> uppercase → exported
// ============================================================================

TEST_CASE("lang: symbol_table uppercase_export_policy", "[lang][symbol_table]") {
    lang::symbol_table<lang::uppercase_export_policy> tbl;

    lang::symbol_entry upper; upper.name = "Dot";    upper.kind = lang::sym_kind::function;
    lang::symbol_entry lower; lower.name = "helper"; lower.kind = lang::sym_kind::function;
    tbl.define(upper);
    tbl.define(lower);

    auto* d = tbl.lookup("Dot");
    REQUIRE(d != nullptr);
    CHECK(d->visibility == lang::sym_visibility::exported);

    auto* h = tbl.lookup("helper");
    REQUIRE(h != nullptr);
    CHECK(h->visibility == lang::sym_visibility::module_local);

    auto exps = tbl.exported_symbols();
    CHECK(exps.size() == 1u);
    CHECK(exps[0]->name == "Dot");
}

// ============================================================================
// Test 14 — collecting_sink errors/warnings separation
// ============================================================================

TEST_CASE("lang: collecting_sink errors and warnings", "[lang][diagnostics]") {
    enum class my_kind { bad, note };
    struct my_helper {
        static constexpr std::string_view to_code(my_kind k) noexcept {
            return k == my_kind::bad ? "ERR-001" : "NOTE-001";
        }
    };
    using my_diag = lang::lang_diagnostic<my_kind>;
    lang::collecting_sink<my_diag> sink;

    my_diag err; err.kind = my_kind::bad;  err.level = lang::severity::error;
    my_diag note; note.kind = my_kind::note; note.level = lang::severity::warning;
    sink.on_diagnostic(err);
    sink.on_diagnostic(note);

    CHECK(sink.has_errors());
    CHECK(sink.has_warnings());
    CHECK(sink.size() == 2u);
    CHECK(sink.max_severity() == lang::severity::error);
}

// ============================================================================
// Test 15 — discharge_driver: assume→proven, check→unknown+guard
// ============================================================================

TEST_CASE("lang: discharge_driver policy outcomes", "[lang][proof]") {
    lang::discharge_driver<> driver;
    lang::assumption_context actx;
    lang::analysis_store astore;
    vakya::types::smt_constraint_solver<vakya::types::no_smt_backend> solver{{}};

    // assume policy → proven
    {
        std::vector<lang::obligation_record> obs;
        lang::obligation_record ob;
        ob.description = "x > 0";
        ob.policy      = lang::verify_policy::assume;
        ob.expr_hash   = 0; // not used in assume path
        obs.push_back(ob);

        auto r = driver.discharge("f", obs, actx, astore, solver);
        REQUIRE(r.outcomes.size() == 1u);
        CHECK(r.outcomes[0].status == lang::proof_status::proven);
        CHECK_FALSE(r.outcomes[0].guard_inserted);
    }

    // check policy → unknown (no SMT backend) + guard inserted
    {
        std::vector<lang::obligation_record> obs;
        lang::obligation_record ob;
        ob.description = "divisor != 0";
        ob.policy      = lang::verify_policy::check;
        ob.expr_hash   = 42; // not in astore → proof_status::unknown
        obs.push_back(ob);

        auto r = driver.discharge("safe_div", obs, actx, astore, solver);
        REQUIRE(r.outcomes.size() == 1u);
        // no_smt_backend returns deferred/unknown → guard inserted
        CHECK(r.outcomes[0].guard_inserted);
    }
}

// ============================================================================
// Test 16 — registry_builder collision detection LANG-REG-001
// ============================================================================

TEST_CASE("lang: registry_builder duplicate function LANG-REG-001", "[lang][registry]") {
    lang::registry_builder<> builder;

    lang::function_descriptor_base f1;
    f1.name = "math.dot";
    f1.arity = 2;

    lang::function_descriptor_base f2;
    f2.name = "math.dot"; // duplicate
    f2.arity = 2;

    builder.add_function(f1);
    builder.add_function(f2);

    auto result = builder.build();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().size() == 1u);
    CHECK(result.error()[0].kind == lang::build_diag_kind{lang::build_diag_kind::kind::duplicate_id});
    CHECK(result.error()[0].code() == "LANG-REG-001");
}

// ============================================================================
// Test 17 — registry_builder global fingerprint non-zero
// ============================================================================

TEST_CASE("lang: registry_builder global fingerprint non-zero", "[lang][registry]") {
    lang::registry_builder<> builder;
    lang::function_descriptor_base f;
    f.name        = "util.scale";
    f.arity       = 1;
    f.fingerprint = lang::detail::fp_from_string("util.scale");
    builder.add_function(f);

    auto result = builder.build();
    REQUIRE(result.has_value());
    CHECK(result->global_fingerprint() != 0u);
    CHECK(result->function_count() == 1u);
    CHECK(result->find_function("util.scale") != nullptr);
}

// ============================================================================
// Test 18 — dependency_graph cycle_nodes returns cycle participants
// ============================================================================

TEST_CASE("lang: dependency_graph cycle_nodes", "[lang][module_system]") {
    lang::dependency_graph g;
    // A → B → C → A (cycle), D is separate
    g.add_import("A", "B");
    g.add_import("B", "C");
    g.add_import("C", "A");
    g.add_module([]{ lang::module_descriptor d; d.name = "D"; return d; }());

    auto cycle = g.cycle_nodes();
    CHECK_FALSE(cycle.empty());

    // All cycle nodes must be A, B, or C.
    for (const auto& n : cycle) {
        CHECK((n == "A" || n == "B" || n == "C"));
    }

    // Topo order returns empty on cycle.
    CHECK(g.topo_order().empty());
}
