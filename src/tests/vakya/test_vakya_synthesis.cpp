// =============================================================================
// test_vakya_synthesis.cpp — synthesis cluster (affinity / cost lattice / proof-carrying).
//
// Verifies:
//   vakya/exec_affinity.hpp    — synthesize_affinity fold
//   vakya/cost.hpp             — cost_join lattice + synthesize_cost bands
//   vakya/proof_carrying.hpp   — certificates + verified_rewrite_engine
//   vakya/analysis_store.hpp   — widened record: trivially-copyable + zero defaults
//
// Cases:
//   1.  analysis_record: trivially-copyable (static_assert holds at compile)
//   2.  analysis_record: optimization fields default to null/unknown/0
//   3.  cost_join: max monoid; unknown is bottom
//   4.  synthesize_cost: band thresholds
//   5.  synthesize_cost: 0 work → unknown
//   6.  synthesize_shape_cost: static shape product → band
//   7.  affinity: pure (no effects, no rw)
//   8.  affinity: io_bound on IO effect
//   9.  affinity: sequential on Exception / open tail
//  10.  affinity: cpu_bound on heavy pure compute
//  11.  affinity: reads effect row when present
//  12.  certificate: certify_rewrite proven on identical hash
//  13.  certificate: egraph checker deferred when not same class
//  14.  certificate_arena: add + by_id round-trip
//  15.  verified_rewrite_engine: proven applies
//  16.  verified_rewrite_engine: deferred blocked under proven_only
//  17.  verified_rewrite_engine: deferred applies (flagged) under allow_deferred
//  18.  verified_rewrite_engine: refuted never applies
// =============================================================================

#include "catch_amalgamated.hpp"

#include "vakya/exec_affinity.hpp"
#include "vakya/cost.hpp"
#include "vakya/proof_carrying.hpp"
#include "vakya/analysis_store.hpp"
#include "vakya/types/effect.hpp"
#include "vakya/types/effect_row.hpp"
#include "vakya/types/shape.hpp"
#include "vakya/types/value_param.hpp"

#include <type_traits>

using namespace vakya::types;

// ============================================================================
// 1-2. widened analysis_record
// ============================================================================

TEST_CASE("opt record: trivially copyable", "[opt][record]") {
    static_assert(std::is_trivially_copyable_v<analysis_record>);
    CHECK(std::is_trivially_copyable_v<analysis_record>);
}

TEST_CASE("opt record: zero defaults", "[opt][record]") {
    analysis_record rec;
    CHECK(rec.region.is_null());
    CHECK(rec.effect_row.is_null());
    CHECK(rec.rw.is_null());
    CHECK(rec.state == kNoTypestate);
    CHECK(rec.simd_width == 0);
    CHECK(rec.tile_hint == 0);
    CHECK(rec.affinity == execution_affinity::unknown);
    CHECK(rec.cost == cost_class::unknown);
    CHECK(rec.cert_id == 0u);
}

// ============================================================================
// 3-6. cost lattice
// ============================================================================

TEST_CASE("opt cost: join max monoid", "[opt][cost]") {
    CHECK(cost_join(cost_class::tiny, cost_class::heavy) == cost_class::heavy);
    CHECK(cost_join(cost_class::unknown, cost_class::small) == cost_class::small);
    CHECK(cost_join(cost_class::moderate, cost_class::unknown) == cost_class::moderate);
    CHECK(cost_join(cost_class::small, cost_class::small) == cost_class::small);
}

TEST_CASE("opt cost: synthesize bands", "[opt][cost]") {
    CHECK(synthesize_cost(4u) == cost_class::tiny);
    CHECK(synthesize_cost(64u) == cost_class::small);
    CHECK(synthesize_cost(1024u) == cost_class::moderate);
    CHECK(synthesize_cost(1u << 20) == cost_class::heavy);
}

TEST_CASE("opt cost: zero work unknown", "[opt][cost]") {
    CHECK(synthesize_cost(0u) == cost_class::unknown);
}

TEST_CASE("opt cost: shape product band", "[opt][cost]") {
    type_arena arena;
    // Build a shape<4,4> where dims carry literal extents in payload_hash.
    type_node d;
    d.kind = type_kind::constructor;
    d.descriptor_stable_id = type_descriptor<value_param_type_tag>::stable_id;
    d.payload_hash = 4u;
    const type_ref dim = arena.intern(std::move(d));
    const type_ref dims[2] = {dim, dim};
    const shape_ref s = intern_shape(arena, std::span<const type_ref>(dims, 2));
    // 4 * 4 = 16 elements → small band (>= tiny_below 8, < 256).
    CHECK(synthesize_shape_cost(arena, s) == cost_class::small);
}

// ============================================================================
// 7-11. execution affinity
// ============================================================================

TEST_CASE("opt affinity: pure", "[opt][affinity]") {
    analysis_record rec; // no effects, null rw
    CHECK(synthesize_affinity(rec) == execution_affinity::pure);
}

TEST_CASE("opt affinity: io_bound", "[opt][affinity]") {
    analysis_record rec;
    rec.effects = kEffectMaskIO;
    CHECK(synthesize_affinity(rec) == execution_affinity::io_bound);
}

TEST_CASE("opt affinity: sequential on exception", "[opt][affinity]") {
    analysis_record rec;
    rec.effects = kEffectMaskException;
    CHECK(synthesize_affinity(rec) == execution_affinity::sequential);
}

TEST_CASE("opt affinity: cpu_bound heavy pure", "[opt][affinity]") {
    analysis_record rec;
    rec.effects = 0;
    rec.cost = cost_class::heavy;
    CHECK(synthesize_affinity(rec) == execution_affinity::cpu_bound);
}

TEST_CASE("opt affinity: reads effect row", "[opt][affinity]") {
    effect_row_arena rows;
    const effect_row_var tail = rows.fresh_tail();
    const effect_row_ref open = rows.intern_open_row(0, tail); // polymorphic tail

    analysis_record rec;
    rec.effect_row = open;
    // Open tail is conservatively sequential (caller effects unknown).
    CHECK(synthesize_affinity(rec, &rows) == execution_affinity::sequential);
}

// ============================================================================
// 12-14. certificates
// ============================================================================

namespace {
    // A trivial same-class predicate for the egraph checker.
    struct never_same {
        bool operator()(std::uint64_t, std::uint64_t) const noexcept { return false; }
    };
    struct always_same {
        bool operator()(std::uint64_t, std::uint64_t) const noexcept { return true; }
    };
} // namespace

TEST_CASE("opt cert: proven on identical hash", "[opt][cert]") {
    egraph_equivalence_checker<never_same> checker{never_same{}};
    const rewrite_certificate cert = certify_rewrite(0x99, 0x99, 0, checker);
    CHECK(cert.status == proof_status::proven); // identical → proven
}

TEST_CASE("opt cert: deferred when not same class", "[opt][cert]") {
    egraph_equivalence_checker<never_same> checker{never_same{}};
    const rewrite_certificate cert = certify_rewrite(0x1, 0x2, 0, checker);
    CHECK(cert.status == proof_status::deferred);
}

TEST_CASE("opt cert: arena round-trip", "[opt][cert]") {
    certificate_arena arena;
    rewrite_certificate cert;
    cert.lhs_hash = 0xAA;
    cert.rhs_hash = 0xBB;
    cert.status = proof_status::proven;
    const certificate_ref ref = arena.add(cert);
    REQUIRE(arena.get(ref) != nullptr);
    CHECK(arena.get(ref)->lhs_hash == 0xAA);
    // by_id uses the bare 1-based index stored in analysis_record::cert_id.
    CHECK(arena.by_id(ref.index) != nullptr);
    CHECK(arena.by_id(0) == nullptr); // 0 = no certificate
}

// ============================================================================
// 15-18. verified_rewrite_engine
// ============================================================================

TEST_CASE("opt engine: proven applies", "[opt][cert]") {
    certificate_arena arena;
    verified_rewrite_engine engine(arena, rewrite_policy::proven_only);
    egraph_equivalence_checker<always_same> checker{always_same{}};

    bool applied = false;
    const rewrite_decision d =
        engine.certify_and_apply(0x1, 0x2, 0, checker, [&] { applied = true; });
    CHECK(d.applied);
    CHECK(applied);
    CHECK(d.status == proof_status::proven);
}

TEST_CASE("opt engine: deferred blocked under proven_only", "[opt][cert]") {
    certificate_arena arena;
    verified_rewrite_engine engine(arena, rewrite_policy::proven_only);
    egraph_equivalence_checker<never_same> checker{never_same{}};

    bool applied = false;
    const rewrite_decision d =
        engine.certify_and_apply(0x1, 0x2, 0, checker, [&] { applied = true; });
    CHECK_FALSE(d.applied);
    CHECK_FALSE(applied);
    CHECK(d.status == proof_status::deferred);
    // Certificate is still recorded even when the rewrite is blocked.
    REQUIRE(arena.get(d.cert) != nullptr);
}

TEST_CASE("opt engine: deferred applies under allow_deferred", "[opt][cert]") {
    certificate_arena arena;
    verified_rewrite_engine engine(arena, rewrite_policy::allow_deferred);
    egraph_equivalence_checker<never_same> checker{never_same{}};

    bool applied = false;
    const rewrite_decision d =
        engine.certify_and_apply(0x1, 0x2, 0, checker, [&] { applied = true; });
    CHECK(d.applied); // applied but flagged deferred
    CHECK(d.status == proof_status::deferred);
}

TEST_CASE("opt engine: refuted never applies", "[opt][cert]") {
    certificate_arena arena;
    verified_rewrite_engine engine(arena, rewrite_policy::allow_deferred);

    // A checker that refutes everything.
    struct refuter {
        proof_status check(std::uint64_t, std::uint64_t, std::uint64_t) const {
            return proof_status::refuted;
        }
    } checker;

    bool applied = false;
    const rewrite_decision d =
        engine.certify_and_apply(0x1, 0x2, 0, checker, [&] { applied = true; });
    CHECK_FALSE(d.applied);
    CHECK(d.status == proof_status::refuted);
}
