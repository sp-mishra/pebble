Medha — C++23 Transactional Memory & Transaction EDSL
===

## Overview

Medha is a standalone C++23 header-only transactional-memory framework.
Two public surfaces:

1. **Direct C++ API** — runtime transactions (`transaction_context`, `atomic(...)`).
2. **Symbolic EDSL** — builds a transaction plan that lowers into Lithe-compatible region/effect/constraint metadata.

Ownership boundary:

```
Medha    transaction semantics, resources, read/write sets, validation,
          isolation, snapshot acquisition, conflict detection, retry, commit protocol
          effect_mask (uint64_t), proof_status — Medha-owned; no Vakya dependency
Lithe    generic region/effect/constraint representation, diagnostics, IR/AOT metadata
Pravaha  scheduling of attempts, placement, retry/backoff, cancellation, telemetry
Tarka    optional proof discharge (via vakya::types::tarka_smt_backend<z3_backend>)
Smriti   transaction-local storage (arenas, staged-value logs, rollback checkpoints)
```

---

## Table of Contents

- [Overview](#overview)
- [Repository layout](#repository-layout)
- [Direct C++ API](#direct-c-api) — [transaction_context](#transaction_context), [atomic convenience wrapper](#atomic-convenience-wrapper)
- [Resource registration](#resource-registration)
- [Options](#options) — [`replay_safety`](#replay_safety), [`partial_commit_policy`](#partial_commit_policy)
- [Commit report](#commit-report)
- [Transaction memory model](#transaction-memory-model)
- [Isolation levels](#isolation-levels) — [snapshot](#isolationsnapshot), [serializable](#isolationserializable), [`read_kind`](#read_kind-in-keyhpp)
- [Commit algorithms](#commit-algorithms) — [snapshot](#snapshot-isolation-commit-202), [serializable](#serializable-commit-203), [retry loop](#retry-loop-205)
- [Effect model](#effect-model)
- [EDSL API](#edsl-api) — [typed builder](#typed-expression-builder-preferred), [string frontend](#string-based-frontend-parser-compat), [Lithe lowering](#lowering-to-lithe)
- [Adapters](#adapters) — [Smriti](#smriti-medhaadapterssmritihpp), [Pravaha](#pravaha-medhaadapterspravahahpp), [Tarka](#tarka-medhaadapterstarkahpp)
- [Diagnostics](#diagnostics)
- [Versioning](#versioning)
- [Distribution readiness](#distribution-readiness-v1-metadata-only)
- [Design invariants](#design-invariants)
- [Frontend consumers](#frontend-consumers)

---

## Repository layout

```
include/medha/
    medha.hpp            umbrella (core only; adapters opt-in)
    fwd.hpp              forward decls, tx_error, tx_status (incl. partial_commit, rejected)
    options.hpp          isolation/retry/conflict/distribution/replay/partial_commit_policy
    resource_traits.hpp  static type metadata + commit_capability
    resource_handle.hpp  instance-level runtime contract + resource_registry
    key.hpp              canonical_key + FNV-1a canonicalize CPO + resource_id + read_kind
    value.hpp            value capability traits + staging_handle
    version.hpp          version_stamp (ABA-safe) + snapshot_token (§19.4) + distributed_version_stamp
    read_set.hpp         SmallVector-backed read set with point/range/predicate entries
    write_set.hpp        SmallVector-backed write set + read-your-writes
    access_log.hpp       optional ordered log for serial validation
    conflict.hpp         conflict_policy concept + optimistic/pessimistic/deterministic
    isolation.hpp        isolation enum + serializable_protocol + supports_serializable() +
                         range_serializable_available<R>
    retry.hpp            retry_state + none/bounded/backoff policies
    commit.hpp           commit_report + proof_status (Medha-owned) + proof_summary + trace_id
    effects.hpp          effect_mask (Medha-owned uint64_t) + effect class constants
    diagnostics.hpp      MEDHA-* codes via lithe::diag (guarded)
    identity.hpp         transaction_id, attempt_id, idempotency_token
    context.hpp          transaction_context (incl. snapshot_token)
    transaction.hpp      atomic(...) + nested_scope
    edsl.hpp             typed plan builder (tx_expr, var_expr, in_expr, lit_expr) + compile/bindings

include/medha/adapters/
    smriti.hpp           Smriti arena checkpoint/rollback adapter
    lithe.hpp            Lithe HL region + Medha metadata lowering adapter
    pravaha.hpp          Pravaha scheduling adapter + replay_policy
    tarka.hpp            Tarka SMT discharge adapter (medha::proof_status; no Vakya in core)
    vakya_effects.hpp    bidirectional medha::effect_mask ↔ vakya::types::effect_mask
    vakya_proof.hpp      bidirectional medha::proof_status ↔ vakya::types::proof_status
```

---

## Direct C++ API

### transaction_context

`transaction_context::commit()` performs **one** commit attempt and returns immediately. Retry logic is not part of
`commit()` — use `medha::atomic()` if you need automatic retry. Retry-related options (`retry::bounded`,
`retry::backoff`) passed to `transaction_context` directly are ignored.

```cpp
#include "medha/medha.hpp"

// or short alias: namespace tx = medha;

medha::transaction_context ctx{medha::options{
    .isolation = medha::isolation::serializable,
    // No .retry here — commit() does one attempt only.
    // Use medha::atomic() with .retry and .replay for the retry loop.
    .conflict  = medha::conflict::optimistic{},
}};

auto a = ctx.load(accounts, account_a);   // returns std::expected<Value, tx_error>
auto b = ctx.load(accounts, account_b);
if (!a || !b) { /* error */ }

ctx.store(accounts, account_a, *a - amount);
ctx.store(accounts, account_b, *b + amount);

std::expected<medha::commit_report, medha::tx_error> result = ctx.commit();
// ctx destructor auto-aborts if not committed
```

### atomic convenience wrapper

```cpp
auto result = medha::atomic(
    medha::options{
        .isolation = medha::isolation::serializable,
        .retry     = medha::retry::bounded{3},
        .replay    = medha::replay_safety::body_and_effects_idempotent,
    },
    [](auto& t) -> std::expected<void, medha::tx_error> {
        auto a = t.load(accounts, account_a);
        auto b = t.load(accounts, account_b);
        if (!a || !b) return std::unexpected(a ? b.error() : a.error());
        t.store(accounts, account_a, *a - amount);
        t.store(accounts, account_b, *b + amount);
        return {};
    });
// result : std::expected<medha::commit_report, medha::tx_error>
```

`atomic` owns the retry loop: body → commit → on conflict → consult retry policy → re-run.

---

## Resource registration

Specialize `medha::resource_traits<R>` to register a type as transactional:

```cpp
struct AccountStore { /* ... */ };

template <>
struct medha::resource_traits<AccountStore> {
    static constexpr bool transactional            = true;
    static constexpr bool value_trivially_copyable = true;
    static constexpr medha::commit_capability commit_protocol =
        medha::commit_capability::atomic_multi_key_within_resource;  // enables serializable within this resource
    static constexpr bool aba_safe                 = true;
    // ...
    using key_type   = AccountId;
    using value_type = Balance;
};
```

Provide ADL CPO hooks:

```cpp
std::expected<Balance, medha::tx_error>
tx_read(AccountStore& r, medha::transaction_context& ctx, AccountId key);

std::expected<void, medha::tx_error>
tx_stage(AccountStore& r, medha::transaction_context& ctx, AccountId key, Balance val);

std::expected<void, medha::tx_error> tx_validate(AccountStore&, medha::transaction_context&);
std::expected<void, medha::tx_error> tx_commit(AccountStore&, medha::transaction_context&);
void tx_rollback(AccountStore&, medha::transaction_context&) noexcept;
```

---

## Options

```cpp
medha::options{
    .isolation   = medha::isolation::snapshot,      // or serializable
    .retry       = medha::retry::none{},             // or bounded{max}, backoff{max,base,factor}
    .conflict    = medha::conflict::optimistic{},    // or pessimistic{}, deterministic{}
    .distribution= medha::distribution::none{},     // only supported mode in v1
    .replay      = medha::replay_safety::unknown,    // or body_idempotent/body_and_effects_idempotent/unknown_but_retry_allowed
    .partial     = medha::partial_commit_policy::require_atomic_coordinator,  // or best_effort
}
```

### `replay_safety`

| Value                         | Meaning                                                                    |
|-------------------------------|----------------------------------------------------------------------------|
| `unknown`                     | No declaration; retry allowed (caller accepts re-execution risk)           |
| `non_idempotent`              | Body has irreversible effects; any non-`none` retry → MEDHA-004 (rejected) |
| `body_idempotent`             | Body re-execution safe; effect idempotency not declared                    |
| `body_and_effects_idempotent` | Full idempotency; safest for aggressive retry                              |
| `unknown_but_retry_allowed`   | Alias for `unknown`; explicit opt-in annotation for clarity                |

**Rules (§20.5.1)**:

- `replay_safety::non_idempotent` + non-`none` retry → `tx_status::rejected` (MEDHA-004).
- `replay_safety::unknown` + non-`none` retry → **allowed** (permissive default; caller accepts re-execution risk).
- Declare `body_idempotent` or `body_and_effects_idempotent` for self-documenting, auditable retry intent.

### `partial_commit_policy`

| Value                        | Meaning                                                                                                                                                                 |
|------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `require_atomic_coordinator` | Abort all if any prepare fails before any commit begins (default). For full atomicity, use a coordinator, 2PC, resource-provided atomic commit, or compensating action. |
| `best_effort`                | Commit as many resources as possible. Always reports `tx_status::partial_commit` (never `committed`) when any resource fails.                                           |

**Important**: `require_atomic_coordinator` (formerly `abort_all`) means abort before any commit if preparation fails —
it does NOT mean Medha can retroactively undo already-committed resources. For true abort-all semantics, require a
coordinator or 2PC.

`best_effort` commits are **never** reported as `tx_status::committed`. Use `tx_status::partial_commit` to distinguish.

---

## Commit report

```cpp
struct commit_report {
    tx_status    status;              // medha::tx_status (Medha-owned)
    uint32_t     attempts;
    uint32_t     conflicts;
    uint32_t     resources_touched;
    uint32_t     reads;
    uint32_t     writes;
    nanoseconds  elapsed;
    proof_summary proofs;             // medha::proof_status — deferred by default
    optional<trace_id> telemetry;    // nadi "medha.tx" channel
};
```

`proof_summary.status` uses `medha::proof_status` (defined in `medha/commit.hpp`, no Vakya dependency):

| Status        | Meaning                                        |
|---------------|------------------------------------------------|
| `proven`      | All obligations discharged by solver           |
| `refuted`     | Counterexample found — hard failure per policy |
| `unknown`     | Solver returned unknown (not a proof)          |
| `unsupported` | Obligation type not supported by backend       |
| `timeout`     | Solver exceeded resource/time limit            |
| `deferred`    | No SMT backend; not attempted                  |

**Hard rule**: `unknown`, `unsupported`, `timeout`, `deferred` are **never** treated as `proven`. Use
`medha/adapters/vakya_proof.hpp` for bidirectional conversion when Vakya is present.

---

## Transaction memory model

| Rule             | Detail                                                                                                                                                                          |
|------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Nested scopes    | Flattened. Nested commit merges write set into parent. Nested abort discards only nested writes.                                                                                |
| Nested policy    | Nested scopes **inherit** the parent's retry and isolation policies. They cannot override them.                                                                                 |
| Thread affinity  | `transaction_context` is thread-affine by default. Never used concurrently.                                                                                                     |
| Destruction      | Destructor auto-aborts: rolls back staged writes, releases locks. `noexcept`.                                                                                                   |
| Read-your-writes | Lookup order: write_set → parent write_set → resource.                                                                                                                          |
| Loaded values    | `load` returns a value copy. Raw references are **not** stable across validation/commit.                                                                                        |
| Arena abort      | When a transaction-local arena (Smriti) contains non-trivial objects and an abort occurs, destructors run via the arena's rollback path. Abort-time destructors must not throw. |

---

## Isolation levels

### `isolation::snapshot`

Reads are stable within the transaction; write/write conflicts are prevented; **write-skew is still possible** (
documented).

**Snapshot acquisition (§19.4)**:

A `snapshot_token` is established at or before the first read:

1. If the resource provides a `tx_snapshot(R&)` CPO, the resource-native snapshot handle is used.
2. Otherwise, the first read's `version_stamp` becomes the snapshot boundary (first-read-version caching).

The isolation guarantee delivered depends on what the resource can provide:

| Resource capability                          | Guarantee                                                                                                                                                                                                            |
|----------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Resource-native snapshot (`tx_snapshot` CPO) | True snapshot isolation — reads return the value as of the snapshot point, even if the resource has advanced since                                                                                                   |
| Historical / versioned storage               | True snapshot isolation — resource reconstructs old value from version history                                                                                                                                       |
| `read(key)` + `version(key)` only            | **Repeatable-read validation** (not snapshot isolation) — first-read version is cached; re-reads of the same key detect staleness at commit but cannot return the original value if the current version has advanced |
| Neither snapshot nor versioned reads         | Snapshot isolation **unavailable** for this resource; escalate or reject                                                                                                                                             |

A first-read version cache detects concurrent changes; it does **not** reconstruct the old value. Two reads of the same
key return the write-set value (if written) or the resource's current value subject to version validation at commit.

### `isolation::serializable`

Uses `supports_serializable(commit_capability)` — an explicit predicate, not enum-ordering:

| `commit_capability`                | Protocol selected        | Point reads | Range/predicate reads                |
|------------------------------------|--------------------------|-------------|--------------------------------------|
| `atomic_multi_key_within_resource` | `resource_provided`      | ✓           | ✓ (resource responsibility)          |
| `pessimistic`                      | `read_write_key_locking` | ✓           | ✗ — phantom prevention is resource's |
| `serial_validation`                | `serial_validation`      | ✓           | ✗                                    |
| `ssi`                              | `ssi`                    | ✓           | ✓ (resource responsibility)          |
| `deterministic`                    | `deterministic_executor` | ✓           | ✗                                    |
| `none` / `atomic_single_key`       | unavailable              | —           | —                                    |

**Important**: `atomic_multi_key_within_resource` applies to a **single** resource only. It does NOT guarantee atomicity
across multiple resources. Cross-resource atomicity requires a coordinator, 2PC, or resource-provided atomic batch
protocol.

- Missing protocol → diagnostic `MEDHA-SER-011`.
- `commit_capability::none` or `atomic_single_key` → `tx_status::serialization_unavailable`.
- Capability is checked at compile time via `serializable_available<R>` and `supports_serializable(cap)`.

### `read_kind` (in `key.hpp`)

| Value       | Meaning                 | Phantom prevention      |
|-------------|-------------------------|-------------------------|
| `point`     | Single key lookup       | Medha version check     |
| `range`     | Ordered `[lo, hi)` scan | Resource responsibility |
| `predicate` | Arbitrary filter scan   | Resource responsibility |
| `index`     | Secondary index lookup  | Resource-defined        |

**Hard rule (§20.4)**:
> Serializable isolation is **unavailable** for range/predicate reads unless the resource provides range validation,
> predicate locking, SSI, or an equivalent protocol.

Merely recording a range in `read_set` does **not** make it serializable. Only `commit_capability::ssi` or
`::atomic_multi_key_within_resource` satisfy `range_serializable_available<R>`. For all other capabilities, phantom
prevention must be implemented in the resource's `tx_validate` CPO.

`tx_read` and `tx_stage` are **resource hooks** — they must not bypass Medha's read/write-set bookkeeping.

---

## Commit algorithms

### Snapshot-isolation commit (§20.2)

```
1. Build canonical read set + write set (deterministic key order).
2. Acquire write locks in (resource_id, key_hash) order.
3. Validate read versions for unshadowed keys.
4. Validate base versions for written keys.
5. Detect write/write conflicts.
6. Apply staged writes.
7. Publish new version_stamps.
8. Release locks.
On failure: release locks, rollback staged writes, return conflict.
```

### Serializable commit (§20.3)

Medha supports multiple serializable commit algorithms, selected via `commit_capability`:

| Protocol                              | `commit_capability`                | Algorithm                                                               |
|---------------------------------------|------------------------------------|-------------------------------------------------------------------------|
| Pessimistic key-locking               | `pessimistic`                      | Lock ALL read AND write keys before reads; hold through commit          |
| Serial validation                     | `serial_validation`                | Execute serially; validate read set at commit against a global order    |
| Serializable snapshot isolation (SSI) | `ssi`                              | Track anti-dependency cycles; abort on rw-conflict cycle detection      |
| Resource-provided                     | `atomic_multi_key_within_resource` | Resource atomically validates and commits the full key set              |
| Deterministic executor                | `deterministic`                    | Single-threaded execution; all orderings are equivalent by construction |

**Pessimistic key-locking protocol** (`commit_capability::pessimistic`):

```
Serializable commit — pessimistic key-locking protocol
Same as snapshot commit, but acquire locks for ALL read AND write keys in step 2-3.
Phantom prevention for range/predicate reads is NOT provided by this protocol;
it is the resource's responsibility (tx_validate CPO).
```

### Retry loop (§20.5)

```
Pre-flight checks (atomic() only):
  replay_safety::non_idempotent  + non-none retry → reject (MEDHA-004)
  replay_safety::unknown         + non-none retry → allowed (permissive; caller accepts re-execution risk)

attempt = 0
loop:
    reset transaction state (clears snapshot_token)
    run body
    if error (non-conflict): return error
    commit()
    if committed: return report
    if conflict:
        attempt++; if exhausted: return retry_exhausted
        apply backoff; emit MEDHA-TRY-002; continue
```

---

## Effect model

Effect classes layered on `medha::effect_mask` (Medha-owned `uint64_t`; no Vakya dependency):

| Class           | Bit | Meaning                              |
|-----------------|-----|--------------------------------------|
| `pure`          | 0   | No externally visible effect         |
| `reversible`    | 1   | Auto-undone by rollback              |
| `staged`        | 2   | In-tx record; published after commit |
| `idempotent`    | 3   | Safe to replay (retry-safe)          |
| `compensatable` | 4   | Registered compensation action       |
| `irreversible`  | 5   | Cannot rollback or replay            |

**Admission inside a transaction:** `pure`, `reversible`, `staged`, `idempotent`, `compensatable` allowed.
`irreversible` → MEDHA-004 diagnostic (error).

**Transactional outbox pattern:** outbox *append* is `staged` (allowed); *delivery* happens after commit.

### Registering compensatable effects

For effects with class `compensatable`, register the undo action before or immediately after the effect:

```cpp
// compensation_fn must not throw.
ctx.defer_compensation([] noexcept {
    undo_reservation();
});
```

Compensations registered via `defer_compensation` run in **LIFO order** when the transaction aborts. The undo action
must be `noexcept`.

If compensation itself fails (e.g., the undo RPC is unreachable), the transaction outcome transitions to
`tx_status::recovery_required` — not ordinary `tx_status::aborted`. The caller must invoke a recovery protocol to
resolve the inconsistency.

`defer_compensation` may only be called while the transaction is active (phase `active`). Calling it after commit or
abort is a contract violation.

---

## EDSL API

### Typed expression builder (preferred)

```cpp
#include "medha/edsl.hpp"

using namespace medha::dsl;

// Typed store — preferred: value expression is a typed AST node.
auto transfer =
    transaction("transfer")
        .with_resource(resource("accounts"))
        .with_key(key("account_a"))
        .with_key(key("account_b"))
        .with_input(input("amount"))
        .isolation(medha::isolation::serializable)
        .replay(medha::replay_safety::body_and_effects_idempotent)
        .retry(3)
        .let_load("a", "accounts", "account_a")
        .store(
            "accounts",
            "account_a",
            var_expr("a") - in_expr("amount")   // typed AST; no string eval
        )
        .build();

// Typed bindings — type-checked at call site.
auto result = medha::dsl::compile(transfer).run(
    bindings{}
        .bind("amount", 100)
        .bind("account_a", AccountId{42})
        .bind("account_b", AccountId{43})
);
```

### String-based frontend (parser compat)

`store_stmt()` and `bind_string()` are retained as a **parser frontend**. The string is validated by `parse_expr()`,
type-checked, and converted to a typed `tx_expr` AST before storage. Raw string eval never occurs.

```cpp
// store_stmt parses and validates the expression string; produces typed AST.
auto transfer =
    transaction("transfer")
        .with_resource(resource("accounts"))
        .let_load("a", "accounts", "account_a")
        .store_stmt("accounts", "account_a", "a - amount")   // parsed → typed AST
        .build();

// bind_string parses the string as integer if possible, else stores as string.
auto result = medha::dsl::compile(transfer).run(
    bindings{}.bind_string("amount", "100")
);
```

**Grammar** accepted by `parse_expr()`:

```
expr   = term   { ('+' | '-') term   }
term   = factor { ('*' | '/') factor }
factor = ident | integer
ident  = [a-zA-Z_][a-zA-Z0-9_]*
integer= [0-9]+
```

Parse errors produce a `validation_result::errors` entry at `compile()` time — not at runtime.

### Lowering to Lithe

```cpp
#include "medha/adapters/lithe.hpp"

auto desc = medha::adapters::lithe::lower(transfer);
// desc.metadata — dialect version, isolation, retry, conflict, resource hashes
// desc.has_lithe — true when Lithe headers available
```

**Important (R1):** `atomic/validate/commit/abort/retry` are **Medha metadata sections**, not existing Lithe ops. They
are emitted over the generic HL region primitive.

---

## Adapters

### Smriti (`medha/adapters/smriti.hpp`)

```cpp
#include "medha/adapters/smriti.hpp"
smriti::pools::LinearArena arena{64 * 1024};
{
    medha::adapters::arena_scope scope{arena};
    auto p = scope.allocate(256);  // std::expected<void*, tx_error>
    // ...
    scope.commit();  // prevents rollback on scope exit
}
// If scope exits without commit: arena.rollback(checkpoint) is called.
```

Rules: transaction-local arena owned by one attempt; arena exhaustion → `out_of_memory`; abort-time dtors must not
throw.

### Pravaha (`medha/adapters/pravaha.hpp`)

```cpp
#include "medha/adapters/pravaha.hpp"

medha::adapters::pravaha::replay_policy rp{
    .body_replay_safe    = true,
    .effects_idempotent  = true,
    .resources_retry_safe= true,
};
auto st = medha::adapters::pravaha::make_scheduled(body, opts, rp);
auto r  = st.execute();  // retry only if replay-safe
```

Pravaha must not retry unless Medha marks the attempt replay-safe. `in_doubt`/`recovery_required` are terminal for
normal retry.

### Tarka (`medha/adapters/tarka.hpp`)

```cpp
#include "medha/adapters/tarka.hpp"

std::vector<medha::adapters::tarka::medha_proof_obligation> obs = {
    {medha::adapters::tarka::obligation_kind::invariant_preservation,
     0x1234567ULL, "balance non-negative"},
};
vakya::types::no_smt_backend backend{};
auto results = medha::adapters::tarka::discharge(obs, backend);
// results[0].status == medha::proof_status::deferred  (no SMT backend)
```

Uses `medha::proof_status` (Medha-owned in `medha/commit.hpp`). Optional adapter `medha/adapters/vakya_proof.hpp`
provides bidirectional conversion with `vakya::types::proof_status` when Vakya is present.

`deferred` is **not** `proven`. `unknown`, `unsupported`, `timeout`, `deferred` never satisfy a proof-required policy.


---

## Diagnostics

| Code            | Severity | Trigger                                                                                                        |
|-----------------|----------|----------------------------------------------------------------------------------------------------------------|
| `MEDHA-004`     | error    | operation with `irreversible` effect inside transaction, or `replay_safety::non_idempotent` + non-`none` retry |
| `MEDHA-RSF-005` | error    | `replay_safety::unknown` + non-`none` retry (undeclared idempotency)                                           |
| `MEDHA-TRY-002` | warning  | conflict retrying                                                                                              |
| `MEDHA-SER-011` | error    | serializable isolation requested but resource has no valid protocol                                            |
| `MEDHA-CMT-020` | error    | partial multi-resource commit failure                                                                          |
| `MEDHA-ABA-030` | error    | ABA-unsafe resource with serializable isolation                                                                |
| `MEDHA-OOM-040` | error    | arena out of memory during transaction                                                                         |

`tx_status::rejected` is returned when `atomic()` refuses to execute due to a policy conflict (MEDHA-004, MEDHA-RSF-005)
or when `executable_plan::run()` fails validation.

Example (MEDHA-004):

```
error[MEDHA-004]: operation is not allowed inside this transaction
  transfer.cpp:18:9
      send_email(receipt)
reason:  send_email has effect class 'irreversible'
hint:    move this after commit, or use a transactional outbox resource.
```

---

## Versioning

`version_stamp {uint64_t value, uint32_t generation}`:

- Validation compares **full** stamp (value **and** generation) — ABA protection by default.
- `value` overflow → validation failure (conservative).
- `aba_safe = false` on a resource forces snapshot-only or pessimistic locking for serializable.

**ABA-safety condition**: `version_stamp` is ABA-safe **only if** the resource correctly advances and persists the
`generation` field on slot reuse. Medha validates the full stamp but cannot enforce resource-side invariants. Resources
that do not bump `generation` on slot reuse are not ABA-safe.

---

## Distribution readiness (v1 metadata only)

Medha v1 is **local-process** transactional memory.
The following are defined as metadata/contracts only — no network behavior:

- `transaction_id`, `attempt_id`, `idempotency_token` (§5b.1)
- `distributed_version_stamp` (§5b.3)
- `distributed_commit_protocol` enum (§5b.4)
- `transaction_decision_log` concept (§5b.5)
- Distribution-ready `tx_status` values: `in_doubt`, `recovery_required`, `remote_timeout`, `participant_failed`
- Distribution policy stub in options: `distribution::none` (only supported in v1)

`in_doubt` is **never** treated as `committed`.

---

## Design invariants

- C++23/26; header-only core; no virtual on hot paths; no macros.
- `std::expected`-based fallible APIs throughout.
- Concept- and trait-customizable; users pay only for what they use (`[[no_unique_address]]`).
- Core is leaf — does NOT depend on Lithe/Pravaha/Tarka/Sutra.
- Adapters are `__has_include`-guarded: absent deps cost nothing.

---

## Frontend consumers

Crank (`docs/languages/crank/`) is the primary language frontend that consumes Medha.
The canonical crank→Medha type mapping (design §7c.2) lives in
`docs/languages/crank/transactions.md`:

| Crank type           | Medha C++ type                 |
|----------------------|--------------------------------|
| `TxStatus`           | `medha::tx_status`             |
| `TxError`            | `medha::tx_error`              |
| `CommitReport`       | `medha::commit_report`         |
| `TransactionOptions` | `medha::options`               |
| `Isolation`          | `medha::isolation`             |
| `ReplaySafety`       | `medha::replay_safety`         |
| `PartialCommit`      | `medha::partial_commit_policy` |
| `ProofStatus`        | `medha::proof_status`          |

Crank uses Medha **as-is** — no Medha API gap. All policy checks (retry/replay,
cross-resource serializable, async-in-tx) are enforced by crank at compile time
before any Medha runtime call.
