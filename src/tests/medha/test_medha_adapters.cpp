// =============================================================================
// test_medha_adapters.cpp — Unit tests for smriti/pravaha/tarka adapters
//
// Verifies:
//   medha/adapters/smriti.hpp   (arena_scope checkpoint/rollback, OOM)
//   medha/adapters/pravaha.hpp  (replay_policy, scheduled_transaction)
//   medha/adapters/tarka.hpp    (proof_result, no_smt_backend → deferred,
//                                discharge returns correct status)
//
// Cases:
//   1.  smriti: arena_scope commit marks as committed (no rollback)
//   2.  smriti: arena_scope destructor without commit triggers rollback path
//   3.  smriti: arena_scope allocate succeeds for small sizes
//   4.  smriti: OOM when arena is exhausted
//   5.  pravaha: replay_policy::can_retry all-false = false
//   6.  pravaha: replay_policy::can_retry all-true = true
//   7.  pravaha: scheduled_transaction with replay-safe body executes
//   8.  pravaha: refuses retry when not replay-safe (single attempt)
//   9.  tarka: no SMT backend → all obligations deferred
//  10.  tarka: discharge with empty obligations returns empty
//  11.  tarka: proof_result default status = deferred
//  12.  tarka: obligation_kind values are distinct
// =============================================================================

#include "catch_amalgamated.hpp"

#include "medha/adapters/smriti.hpp"
#include "medha/adapters/pravaha.hpp"
#include "medha/adapters/tarka.hpp"

using namespace medha;

// =============================================================================
// Smriti adapter tests (guarded by MEDHA_HAS_SMRITI)
// =============================================================================

#ifdef MEDHA_HAS_SMRITI

TEST_CASE ("smriti: arena_scope commit marks committed", "[medha][adapters][smriti]") {
    smriti::pools::LinearArena arena{1024};
    {
        adapters::arena_scope scope{arena};
        scope.commit();
    }
    REQUIRE(arena.used_bytes() == 0);
}

TEST_CASE ("smriti: arena_scope allocate succeeds", "[medha][adapters][smriti]") {
    smriti::pools::LinearArena arena{1024};
    adapters::arena_scope scope{arena};
    auto r = scope.allocate(64);
    REQUIRE(r.has_value());
    REQUIRE(*r != nullptr);
}

TEST_CASE ("smriti: arena_scope OOM when arena exhausted", "[medha][adapters][smriti]") {
    smriti::pools::LinearArena arena{32};
    adapters::arena_scope scope{arena};
    auto r = scope.allocate(64);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().status == tx_status::out_of_memory);
}

TEST_CASE ("smriti: arena_scope rollback on scope exit without commit",
          "[medha][adapters][smriti]") {
    smriti::pools::LinearArena arena{1024};
    std::size_t bytes_before = arena.used_bytes();
    {
        adapters::arena_scope scope{arena};
        auto r = scope.allocate(128);
        REQUIRE(r.has_value());
        REQUIRE(arena.used_bytes() > bytes_before);
    }
    REQUIRE(arena.used_bytes() == bytes_before);
}

#endif  // MEDHA_HAS_SMRITI

// =============================================================================
// Pravaha adapter tests
// =============================================================================

TEST_CASE (


"pravaha: replay_policy all-false → cannot retry"
,
"[medha][adapters][pravaha]"
)
 {
    adapters::pravaha::replay_policy rp{false, false, false};
    REQUIRE_FALSE(rp.can_retry());
}

TEST_CASE (


"pravaha: replay_policy all-true → can retry"
,
"[medha][adapters][pravaha]"
)
 {
    adapters::pravaha::replay_policy rp{true, true, true};
    REQUIRE(rp.can_retry());
}

TEST_CASE (


"pravaha: scheduled_transaction with replay-safe body executes and commits"
,
"[medha][adapters][pravaha]"
)
 {
    adapters::pravaha::replay_policy rp{true, true, true};
    options opts{.retry = retry::bounded{3}};

    auto st = adapters::pravaha::make_scheduled(
        [](transaction_context&) -> std::expected<void, tx_error> { return {}; },
        opts,
        rp);

    auto r = st.execute();
    REQUIRE(r.has_value());
    REQUIRE(r->status == tx_status::committed);
}

TEST_CASE (


"pravaha: scheduled_transaction without replay safety runs once"
,
"[medha][adapters][pravaha]"
)
 {
    adapters::pravaha::replay_policy rp{false, false, false};
    options opts{.retry = retry::bounded{3}};

    int call_count = 0;
    auto st = adapters::pravaha::make_scheduled(
        [&](transaction_context&) -> std::expected<void, tx_error> {
            ++call_count;
            return {};
        },
        opts,
        rp);

    auto r = st.execute();
    REQUIRE(r.has_value());
    REQUIRE(call_count == 1);  // single attempt only
}

// =============================================================================
// Tarka adapter tests
// =============================================================================

TEST_CASE (


"tarka: no SMT backend → all obligations deferred"
,
"[medha][adapters][tarka]"
)
 {
    using namespace medha::adapters::tarka;
    std::vector<medha_proof_obligation> obs = {
        {obligation_kind::invariant_preservation, 0x12345678ULL, "invariant"},
        {obligation_kind::key_disjointness,       0xABCDEF00ULL, "disjoint"},
    };

#ifdef MEDHA_HAS_VAKYA_VERIFY
    vakya::types::no_smt_backend backend{};
    auto results = discharge(obs, backend);
#else
    auto results = discharge(obs);
#endif

    REQUIRE(results.size() == 2);
    for (const auto& r : results) {
        REQUIRE(r.status == proof_status::deferred);
    }
}

TEST_CASE (


"tarka: discharge with empty obligations returns empty"
,
"[medha][adapters][tarka]"
)
 {
    using namespace medha::adapters::tarka;
    std::vector<medha_proof_obligation> obs{};

#ifdef MEDHA_HAS_VAKYA_VERIFY
    vakya::types::no_smt_backend backend{};
    auto results = discharge(obs, backend);
#else
    auto results = discharge(obs);
#endif

    REQUIRE(results.empty());
}

TEST_CASE (


"tarka: proof_result default status is deferred"
,
"[medha][adapters][tarka]"
)
 {
    adapters::tarka::proof_result r{};
    REQUIRE(r.status == proof_status::deferred);
}

TEST_CASE (


"tarka: obligation_kind values are distinct"
,
"[medha][adapters][tarka]"
)
 {
    using namespace medha::adapters::tarka;
    REQUIRE(obligation_kind::invariant_preservation != obligation_kind::key_disjointness);
    REQUIRE(obligation_kind::key_disjointness       != obligation_kind::commutativity);
    REQUIRE(obligation_kind::invariant_preservation != obligation_kind::commutativity);
}
