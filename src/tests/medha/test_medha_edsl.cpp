// =============================================================================
// test_medha_edsl.cpp — Unit tests for medha EDSL build/compile/bind/lower
//
// Verifies:
//   medha/edsl.hpp          (plan_builder, compile, validate_plan, bindings)
//   medha/adapters/lithe.hpp (lower, lithe_region_descriptor, metadata)
//
// Cases:
//   1.  plan_builder: build produces correct plan name
//   2.  plan_builder: isolation and retry stored in options
//   3.  plan_builder: resources and keys registered
//   4.  plan_builder: let_load and store_stmt in body
//   5.  validate_plan: valid plan returns ok
//   6.  validate_plan: missing resource name → error
//   7.  validate_plan: unbound resource reference → error
//   8.  compile: valid plan → executable_plan::valid() true
//   9.  compile: invalid plan → valid() false
//  10.  executable_plan::run: valid plan returns committed
//  11.  executable_plan::run: invalid plan returns rejected error
//  12.  bindings: bind_string + find_string
//  13.  lithe lower: plan_name propagated to descriptor
//  14.  lithe lower: isolation stored in metadata
//  15.  lithe lower: retry max stored in metadata
//  16.  lithe lower: resource hashes non-zero for non-empty names
//  17.  lithe lower: conflict policy tag stored
//  18.  lithe lower: two plans with different resources have different hashes
// =============================================================================

#include "catch_amalgamated.hpp"

#include "medha/edsl.hpp"
#include "medha/adapters/lithe.hpp"

using namespace medha;
using namespace medha::dsl;

// =============================================================================
// Cases 1-4: plan_builder
// =============================================================================

TEST_CASE (


"edsl: plan_builder produces correct plan name"
,
"[medha][edsl]"
)
 {
    auto p = transaction("transfer").build();
    REQUIRE(p.name == "transfer");
}

TEST_CASE (


"edsl: plan_builder isolation and retry stored"
,
"[medha][edsl]"
)
 {
    auto p = transaction("t")
                 .isolation(isolation::serializable)
                 .retry(5)
                 .build();
    REQUIRE(p.tx_options.isolation == isolation::serializable);
    auto* br = std::get_if<retry::bounded>(&p.tx_options.retry);
    REQUIRE(br != nullptr);
    REQUIRE(br->max == 5);
}

TEST_CASE (


"edsl: plan_builder resources and keys registered"
,
"[medha][edsl]"
)
 {
    auto p = transaction("t")
                 .with_resource(resource("accounts"))
                 .with_key(key("account_a"))
                 .with_input(input("amount"))
                 .build();
    REQUIRE(p.resources.size() == 1);
    REQUIRE(p.resources[0].name == "accounts");
    REQUIRE(p.keys.size() == 1);
    REQUIRE(p.keys[0].name == "account_a");
    REQUIRE(p.inputs.size() == 1);
    REQUIRE(p.inputs[0].name == "amount");
}

TEST_CASE (


"edsl: plan_builder let_load and store_stmt in body"
,
"[medha][edsl]"
)
 {
    auto p = transaction("t")
                 .with_resource(resource("accounts"))
                 .let_load("a", "accounts", "account_a")
                 .store_stmt("accounts", "account_a", "a - amount")
                 .build();
    REQUIRE(p.body.size() == 2);
    REQUIRE(p.body[0].kind == plan_statement_kind::let_load);
    REQUIRE(p.body[0].lhs_var == "a");
    REQUIRE(p.body[1].kind == plan_statement_kind::store);
    REQUIRE(p.body[1].value_expr == "a - amount");
}

// =============================================================================
// Cases 5-7: validate_plan
// =============================================================================

TEST_CASE (


"edsl: validate_plan: valid plan returns ok"
,
"[medha][edsl]"
)
 {
    auto p = transaction("t")
                 .with_resource(resource("accounts"))
                 .let_load("a", "accounts", "key")
                 .build();
    auto vr = validate_plan(p);
    REQUIRE(vr.ok);
    REQUIRE(vr.errors.empty());
}

TEST_CASE (


"edsl: validate_plan: missing resource_name → error"
,
"[medha][edsl]"
)
 {
    plan p;
    p.name = "bad";
    p.body.push_back(plan_statement{plan_statement_kind::store, {}, "", "k", "v"});
    auto vr = validate_plan(p);
    REQUIRE_FALSE(vr.ok);
}

TEST_CASE (


"edsl: validate_plan: unbound resource reference → error"
,
"[medha][edsl]"
)
 {
    plan p;
    p.name = "bad";
    p.body.push_back(plan_statement{
        plan_statement_kind::let_load, "x", "nonexistent_resource", "k", {}});
    auto vr = validate_plan(p);
    REQUIRE_FALSE(vr.ok);
    REQUIRE(!vr.errors.empty());
}

// =============================================================================
// Cases 8-11: compile + run
// =============================================================================

TEST_CASE (


"edsl: compile valid plan → executable_plan::valid() true"
,
"[medha][edsl]"
)
 {
    auto p = transaction("t")
                 .with_resource(resource("accounts"))
                 .let_load("a", "accounts", "key")
                 .build();
    auto ep = compile(std::move(p));
    REQUIRE(ep.valid());
}

TEST_CASE (


"edsl: compile invalid plan → valid() false"
,
"[medha][edsl]"
)
 {
    plan p;
    p.name = "bad";
    p.body.push_back(plan_statement{
        plan_statement_kind::let_load, "x", "missing", "k", {}});
    auto ep = compile(std::move(p));
    REQUIRE_FALSE(ep.valid());
}

TEST_CASE (


"edsl: executable_plan::run valid plan → committed"
,
"[medha][edsl]"
)
 {
    auto p = transaction("t")
                 .with_resource(resource("accounts"))
                 .let_load("a", "accounts", "key")
                 .build();
    auto ep = compile(std::move(p));
    bindings b;
    auto r = ep.run(b);
    REQUIRE(r.has_value());
    REQUIRE(r->status == tx_status::committed);
}

TEST_CASE (


"edsl: executable_plan::run invalid plan → rejected error"
,
"[medha][edsl]"
)
 {
    plan p;
    p.name = "bad";
    p.body.push_back(plan_statement{
        plan_statement_kind::let_load, "x", "missing", "k", {}});
    auto ep = compile(std::move(p));
    bindings b;
    auto r = ep.run(b);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().status == tx_status::rejected);
}

// =============================================================================
// Case 12: bindings
// =============================================================================

TEST_CASE (


"edsl: bindings bind_string + find_string"
,
"[medha][edsl]"
)
 {
    bindings b;
    b.bind_string("amount", "100");
    b.bind_string("account_a", "A123");
    REQUIRE(b.find_string("amount") != nullptr);
    REQUIRE(*b.find_string("amount") == "100");
    REQUIRE(b.find_string("missing") == nullptr);
}

// =============================================================================
// Cases 13-18: Lithe adapter lower
// =============================================================================

TEST_CASE (


"edsl/lithe: lower propagates plan_name"
,
"[medha][edsl][lithe]"
)
 {
    auto p = transaction("xfer")
                 .with_resource(resource("accounts"))
                 .build();
    auto desc = adapters::lithe::lower(p);
    REQUIRE(desc.plan_name == "xfer");
}

TEST_CASE (


"edsl/lithe: lower stores isolation in metadata"
,
"[medha][edsl][lithe]"
)
 {
    auto p = transaction("t")
                 .with_resource(resource("r"))
                 .isolation(isolation::serializable)
                 .build();
    auto desc = adapters::lithe::lower(p);
    REQUIRE(desc.metadata.isolation == static_cast<std::uint8_t>(isolation::serializable));
}

TEST_CASE (


"edsl/lithe: lower stores retry max in metadata"
,
"[medha][edsl][lithe]"
)
 {
    auto p = transaction("t")
                 .with_resource(resource("r"))
                 .retry(7)
                 .build();
    auto desc = adapters::lithe::lower(p);
    REQUIRE(desc.metadata.retry_max == 7);
}

TEST_CASE (


"edsl/lithe: lower resource hashes non-zero for non-empty names"
,
"[medha][edsl][lithe]"
)
 {
    auto p = transaction("t")
                 .with_resource(resource("accounts"))
                 .with_resource(resource("ledger"))
                 .build();
    auto desc = adapters::lithe::lower(p);
    REQUIRE(desc.metadata.resource_hashes.size() == 2);
    REQUIRE(desc.metadata.resource_hashes[0] != 0);
    REQUIRE(desc.metadata.resource_hashes[1] != 0);
}

TEST_CASE (


"edsl/lithe: lower conflict policy stored"
,
"[medha][edsl][lithe]"
)
 {
    // Default conflict is optimistic → policy tag 0
    auto p = transaction("t").with_resource(resource("r")).build();
    auto desc = adapters::lithe::lower(p);
    REQUIRE(desc.metadata.conflict_policy == 0);
}

TEST_CASE (


"edsl/lithe: two plans with different resources have different hashes"
,
"[medha][edsl][lithe]"
)
 {
    auto p1 = transaction("t").with_resource(resource("accounts")).build();
    auto p2 = transaction("t").with_resource(resource("ledger")).build();
    auto d1 = adapters::lithe::lower(p1);
    auto d2 = adapters::lithe::lower(p2);
    REQUIRE(d1.metadata.resource_hashes[0] != d2.metadata.resource_hashes[0]);
}
