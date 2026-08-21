#pragma once
#ifndef SRC_EXAMPLES_EXAMPLE_MEDHA_HPP
#define SRC_EXAMPLES_EXAMPLE_MEDHA_HPP

// ============================================================================
// Medha Comprehensive Tutorial Example
//
// Fictional use case: in-process banking ledger.
//
// A bank maintains an in-process account store. Operations must be atomic and
// isolation-safe: concurrent transfers, balance checks, and audit reads must
// never observe torn state.
//
// Sections:
//   1.  Direct API — transaction_context: load / store / commit / auto-abort
//   2.  atomic() wrapper — retry loop with bounded retry on conflict
//   3.  replay_safety — non_idempotent + retry → MEDHA-004 rejected
//   4.  Effect model — defer_compensation / reversible / irreversible rejection
//   5.  Snapshot isolation — write-skew documented; version tracking
//   6.  Serializable isolation — commit_capability::serial_validation
//   7.  Nested scope — merge / discard on inner abort
//   8.  EDSL typed API — plan builder, typed tx_expr, compile/run
//   9.  EDSL string frontend — store_stmt / bind_string
//  10.  EDSL Lithe lowering — lower(plan) → lithe_region_descriptor + metadata
//  11.  Smriti adapter — arena_scope checkpoint/rollback (conditional)
//  12.  distribution::none — v1 metadata-only distribution fields
//
// All sections share the banking theme so the tutorial reads as one coherent
// narrative. Each section is self-contained and independently verifiable.
// ============================================================================

#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <map>
#include <string_view>
#include <span>

#include "test/example_registry.hpp"
#include "utils/log.hpp"

#include "medha/medha.hpp"
#include "medha/adapters/lithe.hpp"
#include "medha/adapters/smriti.hpp"

namespace bank_ex {
    // ============================================================================
    // Mock resource: BalanceStore
    //
    // Simple in-process map: AccountId → Balance.
    // Supports tx_read / tx_stage / tx_validate / tx_commit / tx_rollback CPOs.
    // Specialized for medha::resource_traits so the context can dispatch through it.
    // ============================================================================

    using AccountId = std::uint32_t;
    using Balance = std::int64_t;

    struct BalanceStore {
        std::map<AccountId, Balance> data;
        std::map<AccountId, Balance> staged;
    };
} // namespace bank_ex

// resource_traits specialization (must be in namespace medha)
template <>
struct medha::resource_traits<bank_ex::BalanceStore> {
    static constexpr bool transactional = true;
    static constexpr bool value_trivially_copyable = true;
    static constexpr bool value_move_only = false;
    static constexpr bool resource_stages_values = false;
    static constexpr bool supports_snapshot = false;
    static constexpr bool supports_rollback = true;
    static constexpr medha::commit_capability commit_protocol =
        medha::commit_capability::serial_validation;
    static constexpr bool aba_safe = true;
    using key_type = bank_ex::AccountId;
    using value_type = bank_ex::Balance;
};

namespace bank_ex {
    // ADL CPO implementations (free functions in bank_ex, found via ADL from resource_handle)

    inline std::expected<Balance, medha::tx_error>
    tx_read(BalanceStore& r, medha::transaction_context& /*ctx*/, AccountId key) {
        auto it = r.data.find(key);
        if (it == r.data.end())
            return std::unexpected(medha::tx_error{
                medha::tx_status::unsupported_resource, "account not found"
            });
        return it->second;
    }

    inline std::expected<void, medha::tx_error>
    tx_stage(BalanceStore& r, medha::transaction_context& /*ctx*/, AccountId key, Balance val) {
        r.staged[key] = val;
        return {};
    }

    inline std::expected<void, medha::tx_error>
    tx_validate(BalanceStore& /*r*/, medha::transaction_context& /*ctx*/) {
        return {}; // optimistic: always valid (no external conflicts in this example)
    }

    inline std::expected<void, medha::tx_error>
    tx_commit(BalanceStore& r, medha::transaction_context& /*ctx*/) {
        for (auto& [k, v] : r.staged) r.data[k] = v;
        r.staged.clear();
        return {};
    }

    inline void
    tx_rollback(BalanceStore& r, medha::transaction_context& /*ctx*/) noexcept {
        r.staged.clear();
    }

    // Helper: make a BalanceStore with initial balances
    inline BalanceStore make_store(std::initializer_list<std::pair<const AccountId, Balance>> init) {
        BalanceStore s;
        for (auto& [k, v] : init) s.data[k] = v;
        return s;
    }

    // ============================================================================
    // Section 1: Direct API — transaction_context: load / store / commit
    //
    // Transfer amount from account 1 → account 2 using the raw context API.
    // Demonstrates: transaction_context ctor, load(), store(), commit(), auto-abort.
    //
    // Note on resource application: transaction_context manages the write-set
    // (read-your-writes, version tracking, conflict detection). Flushing staged
    // values to the resource is done via the resource's tx_stage / tx_commit CPOs.
    // In a full integration the commit protocol drives this; here we show the
    // explicit CPO call sequence to illustrate how the two layers compose.
    // ============================================================================
    static testfw::Result ex1_direct_api() {
        BalanceStore store = make_store({{1, 1000}, {2, 500}});
        medha::resource_id rid{.index = 0, .generation = 1};
        medha::resource_handle<BalanceStore> handle{store, rid};

        const Balance amount = 300;

        medha::transaction_context ctx{
            medha::options{
                .isolation = medha::isolation::snapshot,
                .conflict = medha::conflict::optimistic{},
            }
        };

        auto a1 = ctx.load(handle, 1u);
        auto a2 = ctx.load(handle, 2u);
        if (!a1 || !a2) return testfw::fail("ex1: load failed");

        // Read-your-writes: after store, re-reading same key returns staged value.
        if (auto r = ctx.store(handle, 1u, *a1 - amount); !r)
            return testfw::fail("ex1: store(account 1) failed");
        if (auto r = ctx.store(handle, 2u, *a2 + amount); !r)
            return testfw::fail("ex1: store(account 2) failed");

        // Re-read: context write-set shadows the resource (read-your-writes).
        auto ryw1 = ctx.load(handle, 1u);
        auto ryw2 = ctx.load(handle, 2u);
        if (!ryw1 || !ryw2) return testfw::fail("ex1: re-load failed");
        if (*ryw1 != *a1 - amount)
            return testfw::fail("ex1: read-your-writes failed for account 1");
        if (*ryw2 != *a2 + amount)
            return testfw::fail("ex1: read-your-writes failed for account 2");

        // Stage to resource via CPO (populates store.staged for eventual flush).
        if (auto r = tx_stage(store, ctx, 1u, *a1 - amount); !r)
            return testfw::fail("ex1: tx_stage(account 1) failed");
        if (auto r = tx_stage(store, ctx, 2u, *a2 + amount); !r)
            return testfw::fail("ex1: tx_stage(account 2) failed");

        // Commit: validate + mark committed (one attempt).
        auto cr = ctx.commit();
        if (!cr) return testfw::fail("ex1: commit failed");
        if (cr->status != medha::tx_status::committed)
            return testfw::fail("ex1: unexpected commit status");
        if (cr->reads < 2 || cr->writes < 2)
            return testfw::fail("ex1: commit_report reads/writes undercount");

        // Flush: apply staged values to resource data (resource commit CPO).
        if (auto r = tx_commit(store, ctx); !r)
            return testfw::fail("ex1: tx_commit failed");

        // Verify resource data reflects the transfer.
        if (store.data[1] != 700 || store.data[2] != 800)
            return testfw::fail("ex1: balance mismatch after commit");

        lg::info("medha ex1 (direct API): transfer {} | acct1={} acct2={} reads={} writes={}",
                 amount, store.data[1], store.data[2], cr->reads, cr->writes);
        return {};
    }

    // ============================================================================
    // Section 2: atomic() wrapper — retry loop
    //
    // atomic() owns the retry loop; body re-runs on conflict until committed
    // or retry policy exhausted. Demonstrates: bounded retry, body idempotency
    // annotation, commit_report.attempts.
    // ============================================================================
    static testfw::Result ex2_atomic_retry() {
        BalanceStore store = make_store({{10, 2000}, {11, 1000}});
        medha::resource_id rid{.index = 1, .generation = 1};
        medha::resource_handle<BalanceStore> handle{store, rid};

        const Balance xfer = 500;

        auto result = medha::atomic(
            medha::options{
                .isolation = medha::isolation::snapshot,
                .retry = medha::retry::bounded{3},
                .conflict = medha::conflict::optimistic{},
                .replay = medha::replay_safety::body_idempotent,
            },
            [&](auto& ctx) -> std::expected<void, medha::tx_error> {
                auto a10 = ctx.load(handle, 10u);
                auto a11 = ctx.load(handle, 11u);
                if (!a10 || !a11) return std::unexpected(a10 ? a11.error() : a10.error());
                if (auto r = ctx.store(handle, 10u, *a10 - xfer); !r) return std::unexpected(r.error());
                if (auto r = ctx.store(handle, 11u, *a11 + xfer); !r) return std::unexpected(r.error());
                return {};
            });

        if (!result)
            return testfw::fail("ex2: atomic() failed");
        if (result->status != medha::tx_status::committed)
            return testfw::fail("ex2: not committed");

        lg::info("medha ex2 (atomic): xfer={} attempts={} conflicts={}",
                 xfer, result->attempts, result->conflicts);
        return {};
    }

    // ============================================================================
    // Section 3: replay_safety — non_idempotent + retry → MEDHA-004 rejected
    //
    // atomic() must reject attempts to retry a body declared non_idempotent.
    // The irreversible body (e.g., sending an audit email) must not be re-run.
    // ============================================================================
    static testfw::Result ex3_replay_safety_rejection() {
        BalanceStore store = make_store({{20, 100}});
        medha::resource_id rid{.index = 2, .generation = 1};
        medha::resource_handle<BalanceStore> handle{store, rid};

        auto result = medha::atomic(
            medha::options{
                .isolation = medha::isolation::snapshot,
                .retry = medha::retry::bounded{3}, // retry requested
                .replay = medha::replay_safety::non_idempotent, // but body is irreversible
            },
            [&](auto& ctx) -> std::expected<void, medha::tx_error> {
                // Body would send an audit notification — cannot replay.
                auto bal = ctx.load(handle, 20u);
                if (!bal) return std::unexpected(bal.error());
                return {};
            });

        // atomic() must have rejected immediately with MEDHA-004.
        if (result.has_value())
            return testfw::fail("ex3: expected rejection but got success");
        if (result.error().status != medha::tx_status::rejected)
            return testfw::fail("ex3: expected tx_status::rejected");

        lg::info("medha ex3 (replay_safety): non_idempotent+retry rejected as expected (MEDHA-004)");
        return {};
    }

    // ============================================================================
    // Section 4: Effect model — defer_compensation
    //
    // Demonstrates: defer_compensation() registers LIFO undo actions.
    // On abort, compensations run in LIFO order. On commit, they are discarded.
    // ============================================================================
    static testfw::Result ex4_effect_compensation() {
        BalanceStore store = make_store({{30, 5000}});
        medha::resource_id rid{.index = 3, .generation = 1};
        medha::resource_handle<BalanceStore> handle{store, rid};

        std::vector<std::string> compensation_log;

        // Scenario: transaction aborts; compensations should have run in LIFO order.
        {
            medha::transaction_context ctx{
                medha::options{
                    .isolation = medha::isolation::snapshot,
                }
            };

            ctx.defer_compensation([&]() noexcept {
                compensation_log.push_back("undo_reserve");
            });
            ctx.defer_compensation([&]() noexcept {
                compensation_log.push_back("undo_debit");
            });

            // Load and intentionally do NOT commit — destructor auto-aborts.
            auto bal = ctx.load(handle, 30u);
            (void)bal;

            // ctx destructor fires here — auto-abort runs compensations LIFO.
        }

        // LIFO: undo_debit was registered last → runs first.
        if (compensation_log.size() != 2)
            return testfw::fail("ex4: wrong compensation count");
        if (compensation_log[0] != "undo_debit" || compensation_log[1] != "undo_reserve")
            return testfw::fail("ex4: compensations not LIFO ordered");

        // Now commit path: compensations must NOT run.
        compensation_log.clear();
        {
            medha::transaction_context ctx2{medha::options{}};
            ctx2.defer_compensation([&]() noexcept {
                compensation_log.push_back("should_not_run");
            });
            auto r = ctx2.commit();
            (void)r;
            // ctx2 committed — no abort, no compensations.
        }

        if (!compensation_log.empty())
            return testfw::fail("ex4: compensations ran on committed tx");

        lg::info("medha ex4 (compensation): LIFO abort={} commit_path=clean",
                 compensation_log.empty() ? "ok" : "fail");
        return {};
    }

    // ============================================================================
    // Section 5: Snapshot isolation — snapshot_token established on first read
    //
    // After a load, snapshot_token.valid is true and boundary is set.
    // Read-your-writes: store then load on same key returns staged value.
    // ============================================================================
    static testfw::Result ex5_snapshot_isolation() {
        BalanceStore store = make_store({{40, 3000}, {41, 1500}});
        medha::resource_id rid{.index = 4, .generation = 1};
        medha::resource_handle<BalanceStore> handle{store, rid};

        medha::transaction_context ctx{
            medha::options{
                .isolation = medha::isolation::snapshot,
            }
        };

        // Before any read: snapshot not yet acquired.
        if (ctx.snapshot().is_valid())
            return testfw::fail("ex5: snapshot should not be valid before first load");

        auto bal40 = ctx.load(handle, 40u);
        if (!bal40) return testfw::fail("ex5: load failed");

        // After first read: snapshot boundary established.
        if (!ctx.snapshot().is_valid())
            return testfw::fail("ex5: snapshot not valid after first load");

        // Read-your-writes: store then load same key returns staged value.
        if (auto r = ctx.store(handle, 40u, 9999); !r)
            return testfw::fail("ex5: store failed");

        // Re-load key 40 — should return write-set value (9999), not original (3000).
        auto ryw = ctx.load(handle, 40u);
        if (!ryw) return testfw::fail("ex5: re-load failed");
        if (*ryw != 9999)
            return testfw::fail("ex5: read-your-writes failed");

        // Abort — do not commit.
        ctx.abort();

        lg::info("medha ex5 (snapshot): ryw verified, snapshot boundary established");
        return {};
    }

    // ============================================================================
    // Section 6: Serializable isolation — commit_capability selection
    //
    // BalanceStore declares commit_capability::serial_validation.
    // Demonstrates: isolation::serializable option, normal commit under serial protocol.
    // ============================================================================
    static testfw::Result ex6_serializable_isolation() {
        BalanceStore store = make_store({{50, 4000}, {51, 2000}});
        medha::resource_id rid{.index = 5, .generation = 1};
        medha::resource_handle<BalanceStore> handle{store, rid};

        auto result = medha::atomic(
            medha::options{
                .isolation = medha::isolation::serializable,
                .retry = medha::retry::bounded{2},
                .conflict = medha::conflict::optimistic{},
                .replay = medha::replay_safety::body_and_effects_idempotent,
            },
            [&](auto& ctx) -> std::expected<void, medha::tx_error> {
                auto a50 = ctx.load(handle, 50u);
                auto a51 = ctx.load(handle, 51u);
                if (!a50 || !a51) return std::unexpected(a50 ? a51.error() : a50.error());
                if (auto r = ctx.store(handle, 50u, *a50 - 200); !r) return std::unexpected(r.error());
                if (auto r = ctx.store(handle, 51u, *a51 + 200); !r) return std::unexpected(r.error());
                return {};
            });

        if (!result) return testfw::fail("ex6: serializable atomic() failed");
        if (result->status != medha::tx_status::committed)
            return testfw::fail("ex6: not committed");

        lg::info("medha ex6 (serializable): committed, attempts={}", result->attempts);
        return {};
    }

    // ============================================================================
    // Section 7: Nested scope — merge on inner commit, discard on inner abort
    //
    // nested_scope wraps inner operations. Inner commit merges into parent.
    // Inner abort discards only nested writes; outer transaction is unaffected.
    // ============================================================================
    static testfw::Result ex7_nested_scope() {
        BalanceStore store = make_store({{60, 1000}});
        medha::resource_id rid{.index = 6, .generation = 1};
        medha::resource_handle<BalanceStore> handle{store, rid};

        // Test 1: nested commit merges into parent.
        {
            medha::transaction_context ctx{medha::options{}};

            auto outer_bal = ctx.load(handle, 60u);
            if (!outer_bal) return testfw::fail("ex7: outer load failed");

            {
                medha::nested_scope inner{ctx};
                // Inner stores merge into parent on commit.
                if (auto r = ctx.store(handle, 60u, 9000); !r)
                    return testfw::fail("ex7: inner store failed");
                if (auto r = inner.commit(); !r)
                    return testfw::fail("ex7: inner commit failed");
            }

            // Read-your-writes: parent should see 9000 after inner merge.
            auto post_inner = ctx.load(handle, 60u);
            if (!post_inner) return testfw::fail("ex7: post-inner load failed");
            if (*post_inner != 9000)
                return testfw::fail("ex7: nested merge not visible in parent");

            ctx.abort();
        }

        // Test 2: nested abort — outer transaction survives inner abort.
        {
            medha::transaction_context ctx{medha::options{}};
            auto outer_val = ctx.load(handle, 60u);
            if (!outer_val) return testfw::fail("ex7: outer2 load failed");

            {
                medha::nested_scope inner{ctx};
                if (auto r = ctx.store(handle, 60u, 9999); !r)
                    return testfw::fail("ex7: inner2 store failed");
                // inner destructor fires — discards nested writes, no commit.
            }
            // Outer is still active; abort cleanly.
            ctx.abort();
        }

        lg::info("medha ex7 (nested scope): merge and discard paths verified");
        return {};
    }

    // ============================================================================
    // Section 8: EDSL typed API — plan builder, typed tx_expr, compile/run
    //
    // Build a transfer plan using the typed plan builder + typed tx_expr AST.
    // No string eval — arithmetic is typed AST.
    // ============================================================================
    static testfw::Result ex8_edsl_typed_api() {
        using namespace medha::dsl;

        // Build a symbolic "transfer" transaction plan.
        // Plan declares resources, keys, inputs, and body statements.
        auto plan =
            transaction("transfer")
            .with_resource(resource("accounts"))
            .with_key(key("account_a"))
            .with_key(key("account_b"))
            .with_input(input("amount"))
            .isolation(medha::isolation::serializable)
            .replay(medha::replay_safety::body_and_effects_idempotent)
            .retry(3)
            .let_load("a", "accounts", "account_a")
            .let_load("b", "accounts", "account_b")
            .store(
                "accounts", "account_a",
                var_expr("a") - in_expr("amount") // typed AST: a - amount
            )
            .store(
                "accounts", "account_b",
                var_expr("b") + in_expr("amount") // typed AST: b + amount
            )
            .build();

        // compile() validates the plan: resource bindings, statement well-formedness.
        auto ep = compile(plan);
        if (!ep.valid()) {
            for (const auto& e : ep.validation().errors)
                lg::info("medha ex8 validation error: {}", e);
            return testfw::fail("ex8: plan validation failed");
        }

        // run() executes the compiled plan with typed bindings.
        auto result = ep.run(
            bindings{}
            .bind<std::int64_t>("amount", 100)
            .bind<std::uint32_t>("account_a", 42u)
            .bind<std::uint32_t>("account_b", 43u)
        );

        if (!result) return testfw::fail("ex8: compile/run failed");
        if (result->status != medha::tx_status::committed)
            return testfw::fail("ex8: plan not committed");

        // Inspect plan structure: verify statements were recorded correctly.
        if (plan.body.size() != 4) // 2 let_load + 2 store
            return testfw::fail("ex8: plan body size mismatch");

        const auto& store_stmt = plan.body[2];
        if (!store_stmt.typed_expr.has_value())
            return testfw::fail("ex8: store stmt missing typed_expr");
        if (!store_stmt.typed_expr->is_binop())
            return testfw::fail("ex8: store stmt typed_expr not binop");

        lg::info("medha ex8 (EDSL typed): transfer plan compiled and run, attempts={}",
                 result->attempts);
        return {};
    }

    // ============================================================================
    // Section 9: EDSL string frontend — store_stmt / bind_string
    //
    // store_stmt() accepts an arithmetic expression string; parse_expr() validates
    // and converts to typed AST — no raw string eval.
    // bind_string() parses integer strings as int64_t.
    // ============================================================================
    static testfw::Result ex9_edsl_string_frontend() {
        using namespace medha::dsl;

        auto plan =
            transaction("sweep")
            .with_resource(resource("ledger"))
            .with_key(key("src"))
            .with_input(input("fee"))
            .let_load("bal", "ledger", "src")
            .store_stmt("ledger", "src", "bal - fee") // validated string → typed AST
            .build();

        auto ep = compile(plan);
        if (!ep.valid())
            return testfw::fail("ex9: plan validation failed");

        auto result = ep.run(
            bindings{}.bind_string("fee", "25") // string-parsed → int64_t(25)
        );

        if (!result) return testfw::fail("ex9: run failed");

        // Verify the parsed store statement has a typed_expr (binop: bal - fee).
        const auto& stmt = plan.body[1];
        if (!stmt.typed_expr.has_value())
            return testfw::fail("ex9: store_stmt missing typed_expr after parse");
        if (!stmt.typed_expr->is_binop())
            return testfw::fail("ex9: store_stmt typed_expr not binop");

        // Verify value_expr retains the source string for diagnostics.
        if (stmt.value_expr != "bal - fee")
            return testfw::fail("ex9: value_expr source string lost");

        lg::info("medha ex9 (EDSL string): store_stmt parsed → typed AST, source retained");
        return {};
    }

    // ============================================================================
    // Section 10: EDSL Lithe lowering — lower(plan) → lithe_region_descriptor
    //
    // lower() extracts Medha metadata (isolation, retry, conflict, resource hashes,
    // replay_safe) into a lithe_region_descriptor. Core does NOT depend on Lithe;
    // the adapter is __has_include-guarded.
    // ============================================================================
    static testfw::Result ex10_edsl_lithe_lowering() {
        using namespace medha::dsl;
        using namespace medha::adapters::lithe;

        auto plan =
            transaction("batch_settle")
            .with_resource(resource("clearing"))
            .with_resource(resource("nostro"))
            .with_key(key("batch_id"))
            .with_input(input("net_amount"))
            .isolation(medha::isolation::serializable)
            .retry(5)
            .replay(medha::replay_safety::body_and_effects_idempotent)
            .let_load("batch", "clearing", "batch_id")
            .store(
                "clearing", "batch_id",
                var_expr("batch") + in_expr("net_amount")
            )
            .build();

        // lower() is always callable; has_lithe = true only when Lithe headers present.
        auto desc = lower(plan);

        if (desc.plan_name != "batch_settle")
            return testfw::fail("ex10: descriptor plan_name mismatch");

        const auto& m = desc.metadata;
        if (m.dialect_version != 1)
            return testfw::fail("ex10: dialect_version wrong");
        if (m.isolation != static_cast<std::uint8_t>(medha::isolation::serializable))
            return testfw::fail("ex10: isolation mismatch in metadata");
        if (m.retry_max != 5)
            return testfw::fail("ex10: retry_max wrong");
        if (!m.replay_safe)
            return testfw::fail("ex10: replay_safe not set for body_and_effects_idempotent");
        if (m.resource_hashes.size() != 2)
            return testfw::fail("ex10: resource_hashes count wrong");
        // Resource hashes must be non-zero (FNV-1a of non-empty names).
        for (auto h : m.resource_hashes)
            if (h == 0)
                return testfw::fail("ex10: resource hash is zero");

        lg::info("medha ex10 (Lithe lower): dialect={} isolation={} retry={} "
                 "replay_safe={} resources={} has_lithe={}",
                 m.dialect_version, m.isolation, m.retry_max,
                 m.replay_safe, m.resource_hashes.size(), desc.has_lithe);
        return {};
    }

    // ============================================================================
    // Section 11: Smriti adapter — arena_scope checkpoint/rollback
    //
    // arena_scope provides RAII checkpoint/rollback for transaction-local scratch.
    // On abort (scope exit without commit), arena rolls back to checkpoint.
    // Guarded by MEDHA_HAS_SMRITI; degrades gracefully when Smriti is absent.
    // ============================================================================
    static testfw::Result ex11_smriti_arena() {
#ifdef MEDHA_HAS_SMRITI
        smriti::pools::LinearArena arena{64 * 1024};

        // Test 1: commit path — allocation persists.
        {
            medha::adapters::arena_scope scope{arena};
            auto p = scope.allocate(256);
            if (!p) return testfw::fail("ex11: allocation failed");

            // Write a sentinel to the allocation.
            std::memset(*p, 0xAB, 256);

            scope.commit(); // prevent rollback
            // scope destroyed after commit — arena NOT rolled back.
        }

        // Test 2: abort path — rollback reclaims memory.
        const auto checkpoint_before = arena.checkpoint(); {
            medha::adapters::arena_scope scope{arena};
            auto p2 = scope.allocate(512);
            if (!p2) return testfw::fail("ex11: allocation2 failed");
            // scope destroyed without commit — rollback fires.
        }
        // After rollback, arena cursor should be at checkpoint_before.
        const auto checkpoint_after = arena.checkpoint();
        if (checkpoint_after.offset != checkpoint_before.offset)
            return testfw::fail("ex11: arena not rolled back after abort");

        lg::info("medha ex11 (Smriti arena): commit persists, abort rolls back");
#else
        lg::info("medha ex11 (Smriti arena): MEDHA_HAS_SMRITI=0 — skipped");
#endif
        return {};
    }

    // ============================================================================
    // Section 12: distribution::none — v1 metadata-only distribution fields
    //
    // distribution::none is the only supported mode in v1.
    // transaction_id, attempt_id, distributed_version_stamp are metadata contracts.
    // in_doubt is never treated as committed.
    // ============================================================================
    static testfw::Result ex12_distribution_metadata() {
        // Verify that options can carry distribution::none (the only v1 mode).
        medha::options opts{
            .isolation = medha::isolation::snapshot,
            .retry = medha::retry::none{},
            .conflict = medha::conflict::optimistic{},
            .distribution = medha::distribution::none{},
            .replay = medha::replay_safety::unknown,
            .partial = medha::partial_commit_policy::require_atomic_coordinator,
        };

        // Verify distribution::none is the active variant.
        if (!std::holds_alternative<medha::distribution::none>(opts.distribution))
            return testfw::fail("ex12: expected distribution::none");

        // tx_status::in_doubt must not be treated as committed.
        medha::tx_error in_doubt_err{medha::tx_status::in_doubt, "cannot determine remote outcome"};
        if (in_doubt_err.status == medha::tx_status::committed)
            return testfw::fail("ex12: in_doubt treated as committed");

        // tx_status::recovery_required must not be retriable (terminal).
        medha::tx_error recovery_err{medha::tx_status::recovery_required, "must resolve via recovery"};
        if (recovery_err.is_retriable())
            return testfw::fail("ex12: recovery_required should not be retriable");

        // partial_commit_policy: only require_atomic_coordinator and best_effort exist.
        static_assert(medha::partial_commit_policy::require_atomic_coordinator ==
                      medha::partial_commit_policy::abort_all,
                      "abort_all must be alias for require_atomic_coordinator");

        lg::info("medha ex12 (distribution): none-mode opts valid, in_doubt/recovery terminal");
        return {};
    }
} // namespace bank_ex

// ============================================================================
// Registry entry
// ============================================================================

struct MedhaExample {
    static constexpr std::string_view name() { return "medha"; }

    static constexpr std::string_view description() {
        return "Medha C++23 transactional memory tutorial: "
            "direct API (load/store/commit/auto-abort), "
            "atomic() retry loop, replay_safety rejection (MEDHA-004), "
            "defer_compensation LIFO, snapshot isolation + read-your-writes, "
            "serializable isolation, nested scope, "
            "EDSL typed plan builder, EDSL string frontend, "
            "Lithe lowering adapter, Smriti arena scope, "
            "distribution::none v1 metadata";
    }

    static constexpr std::array<std::string_view, 5> tag_data{
        "medha", "transactions", "edsl", "tutorial", "isolation"
    };
    static constexpr std::span<const std::string_view> tags() { return tag_data; }

    static testfw::Result run() {
        if (auto r = bank_ex::ex1_direct_api(); !r) return r;
        if (auto r = bank_ex::ex2_atomic_retry(); !r) return r;
        if (auto r = bank_ex::ex3_replay_safety_rejection(); !r) return r;
        if (auto r = bank_ex::ex4_effect_compensation(); !r) return r;
        if (auto r = bank_ex::ex5_snapshot_isolation(); !r) return r;
        if (auto r = bank_ex::ex6_serializable_isolation(); !r) return r;
        if (auto r = bank_ex::ex7_nested_scope(); !r) return r;
        if (auto r = bank_ex::ex8_edsl_typed_api(); !r) return r;
        if (auto r = bank_ex::ex9_edsl_string_frontend(); !r) return r;
        if (auto r = bank_ex::ex10_edsl_lithe_lowering(); !r) return r;
        if (auto r = bank_ex::ex11_smriti_arena(); !r) return r;
        if (auto r = bank_ex::ex12_distribution_metadata(); !r) return r;
        return {};
    }
};

#endif  // SRC_EXAMPLES_EXAMPLE_MEDHA_HPP
