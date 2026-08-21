# Anukrama

Anukrama is Pebble's header-only, static-composition versioned-state substrate.
It supplies immutable per-key version chains, stable point snapshots, and
optimistic transaction validation. It has no virtual dispatch, public macros,
background threads, or mandatory dependency on Petika, Nitya, Smriti, Medha,
or Pravaha.

```cpp
#include <containers/anukrama/anukrama.hpp>

anukrama::store<std::string, int> values;
values.begin().put("visits", 1).commit();
auto snapshot = values.snapshot_at_current();
values.begin().put("visits", 2).commit();
assert(snapshot.get("visits").value() == 1);
```

## Default semantics

The default is snapshot isolation with first-writer-wins validation. A snapshot
reads the newest version at or before its captured timestamp. A transaction
reads its own staged writes, and a transaction writing a key changed after its
snapshot fails with `anukrama::error::conflict`. Deletes are tombstones and
remain visible to earlier snapshots.

The default does not claim predicate or range serializability. Select
`optimistic_point_serializable` to validate point reads too; locking or SSI is
required before claiming predicate protection.

## Static extension points

`store<Key, Value, Compare, IndexPolicy, Clock, ConflictPolicy>` accepts an
ordered index policy, a logical clock exposing `now()`/`next()`, and a conflict
policy exposing `validate_reads`. The default index reuses Pebble's
`containers::SkipList`; `atomic_clock` is the default clock.

Durability adapters use `apply_at(writes, commit_timestamp)` after their log
record is durable. `atomic_clock` advances to the supplied timestamp, preserving
the ordering boundary required by recovery replay.

External transaction coordinators use `version_of(key)` and
`commit_if_unchanged(observations, writes)`. Validation and publication occur
under one Anukrama writer lock, eliminating the validate-then-publish race.

Petika can later bind Nitya LSNs as commit timestamps. Medha can consume these
timestamps as resource versions. Smriti allocation and lock-free reclamation
remain opt-in policies rather than mandatory runtime costs.

`petika::MvccJournaledSkipEngine` now provides that Nitya binding: Petika WAL
records are first made durable, then their LSN is supplied to Anukrama through
`apply_at`. Recovery replays the same monotonically ordered LSNs idempotently.
The engine intentionally performs no automatic history pruning, so a live
Petika snapshot can still resolve its retained LSN; applications choose an
explicit retention/checkpoint policy before reclaiming historical versions.

`petika::SkipStore` and `petika::StringSkipStore` select this MVCC engine by
default. `SingleVersionSkipStore` and `SingleVersionStringSkipStore` preserve
the previous explicitly selected single-version behavior.

## Lifetime and cost model

The store must outlive its snapshots and transactions. Point reads do not
allocate. A write allocates one immutable version node, and reclamation is
explicit through `prune()`: there is no background worker or surprise pause.
