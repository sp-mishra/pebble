// =============================================================================
// test_vakya_rule_registry.cpp — tests for the Vākya Rule Registry.
//
// Verifies: include/vakya/rule_registry.hpp
//
// The Rule Registry layers metadata + discovery on top of pattern::rule_set
// without changing pattern.hpp. These tests exercise the pure vakya:: surface.
//
// Cases:
//   1. rule_descriptor / rule_category to_string.
//   2. rule_pack: descriptor carried; apply_first fires the wrapped rule_set.
//   3. registry: register_pack + size + find(id) + find(name).
//   4. registry: by_category filters correctly.
//   5. registry: discover lists all entries.
//   6. registry: apply<RuleSet>(name, expr) drives a registered pack.
//   7. make_arithmetic_registry pre-loads the four built-in packs.
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/rule_registry.hpp"
#include "vakya/vakya.hpp"

using namespace vakya;

// ============================================================================
// Test 1 — descriptor + category names.
// ============================================================================

TEST_CASE (


"rule_registry: category to_string"
,
"[vakya][rule_registry]"
)
{
    REQUIRE(to_string(rule_category::arithmetic) == "arithmetic");
    REQUIRE(to_string(rule_category::tensor)     == "tensor");
    REQUIRE(to_string(rule_category::custom)     == "custom");
}

// ============================================================================
// Test 2 — rule_pack carries descriptor and applies its rule_set.
// ============================================================================

TEST_CASE (


"rule_registry: rule_pack apply_first fires wrapped set"
,
"[vakya][rule_registry]"
)
{
    const auto& pack = rule_packs::arithmetic_add_zero;
    REQUIRE(pack.descriptor.category == rule_category::arithmetic);
    REQUIRE(pack.descriptor.name == "arith.add_zero");

    int x = 5;
    auto e = vakya::as_expr(x) + 0; // add(x, 0)
    auto out = pack.apply_first(e);
    REQUIRE(out.has_value());
}

// ============================================================================
// Test 3 — register + find.
// ============================================================================

TEST_CASE (


"rule_registry: register_pack + find"
,
"[vakya][rule_registry]"
)
{
    rule_registry r;
    r.register_pack(rule_packs::arithmetic_add_zero);
    r.register_pack(rule_packs::arithmetic_mul_one);

    REQUIRE(r.size() == 2);
    REQUIRE(r.find(std::size_t{0}) != nullptr);
    REQUIRE(r.find("arith.mul_one") != nullptr);
    REQUIRE(r.find("nonexistent") == nullptr);
    REQUIRE(r.find("arith.add_zero")->descriptor.version == rule_version{1, 0, 0});
}

// ============================================================================
// Test 4 — by_category.
// ============================================================================

TEST_CASE (


"rule_registry: by_category filters"
,
"[vakya][rule_registry]"
)
{
    auto r = make_arithmetic_registry();
    auto arith = r.by_category(rule_category::arithmetic);
    REQUIRE(arith.size() == 4);
    auto tensors = r.by_category(rule_category::tensor);
    REQUIRE(tensors.empty());
}

// ============================================================================
// Test 5 — discover.
// ============================================================================

TEST_CASE (


"rule_registry: discover lists entries"
,
"[vakya][rule_registry]"
)
{
    auto r = make_arithmetic_registry();
    REQUIRE(r.discover().size() == 4);
    for (const auto& e : r.discover()) {
        REQUIRE(e.rules_ptr != nullptr);
    }
}

// ============================================================================
// Test 6 — apply by name.
// ============================================================================

TEST_CASE (


"rule_registry: apply by name drives registered pack"
,
"[vakya][rule_registry]"
)
{
    auto r = make_arithmetic_registry();

    int x = 9;
    auto e = vakya::as_expr(x) + 0; // add(x, 0)

    using set_t = std::decay_t<decltype(pattern::rules::arithmetic::add_zero)>;
    auto out = r.apply<set_t>("arith.add_zero", e);
    REQUIRE(out.has_value());
}

// ============================================================================
// Test 7 — selective load vs full load.
// ============================================================================

TEST_CASE (


"rule_registry: selective load registers only chosen packs"
,
"[vakya][rule_registry]"
)
{
    rule_registry r;
    r.register_pack(rule_packs::arithmetic_mul_zero); // load one only
    REQUIRE(r.size() == 1);
    REQUIRE(r.find("arith.mul_zero") != nullptr);
    REQUIRE(r.find("arith.add_zero") == nullptr);
}
