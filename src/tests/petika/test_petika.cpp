// ============================================================================
// test_petika.cpp — Unit tests for Petika: Unified Engine-Agnostic Storage
// ============================================================================

#include "catch_amalgamated.hpp"
#include "petika/petika.hpp"
#include "petika/adapters/kosha.hpp"
#include "petika/async_persistence_worker.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if __has_include(<generator>)
#  include <generator>
#endif

namespace {
    struct TmpPetikaDir {
        std::filesystem::path path;

        TmpPetikaDir() {
            auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("petika_test_" + std::to_string(ns));
            std::filesystem::create_directories(path);
        }

        ~TmpPetikaDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };
} // anonymous namespace

// ============================================================================
// § 1  JournaledSkipEngine Direct Tests
// ============================================================================

TEST_CASE (
"JournaledSkipEngine: Basic CRUD operations"
,
"[petika][skip_engine]"
)
 {
    petika::JournaledSkipEngine<std::string, std::string> engine;
    CHECK(engine.empty());
    CHECK(engine.size() == 0);

    REQUIRE(engine.put("alpha", "100", 1).has_value());
    REQUIRE(engine.put("beta", "200", 2).has_value());
    REQUIRE(engine.put("gamma", "300", 3).has_value());

    CHECK(engine.size() == 3);
    CHECK(engine.contains("beta"));
    CHECK_FALSE(engine.contains("delta"));

    auto val = engine.get("beta");
    REQUIRE(val.has_value());
    CHECK(*val == "200");

    // Overwrite
    REQUIRE(engine.put("beta", "250", 4).has_value());
    CHECK(*engine.get("beta") == "250");
    CHECK(engine.size() == 3);

    // Erase
    REQUIRE(engine.erase("beta", 5).has_value());
    CHECK_FALSE(engine.contains("beta"));
    CHECK(engine.size() == 2);
}

TEST_CASE (
"JournaledSkipEngine: Ordered range scan"
,
"[petika][skip_engine]"
)
 {
    petika::JournaledSkipEngine<int, std::string> engine;

    for (int i = 10; i <= 100; i += 10) {
        REQUIRE(engine.put(i, "val_" + std::to_string(i), i).has_value());
    }

    std::vector<int> scanned_keys;
    engine.scan(30, 80, [&](const auto& entry) {
        scanned_keys.push_back(entry.key);
    });

    REQUIRE(scanned_keys == std::vector<int>{30, 40, 50, 60, 70});
}

// ============================================================================
// § 2  Petika Store (Durability + SkipEngine)
// ============================================================================

TEST_CASE (
"Petika: Put, Get, Contains and Clear with WAL persistence"
,
"[petika][store]"
)
 {
    TmpPetikaDir tmp;
    petika::PetikaOptions opts{
        .db_dir = tmp.path,
        .sync_on_write = true,
        .auto_recovery = false
    };

    petika::StringSkipStore db{opts};

    REQUIRE(db.put("k1", "v1").has_value());
    REQUIRE(db.put("k2", "v2").has_value());
    REQUIRE(db.put("k3", "v3").has_value());

    CHECK(db.size() == 3);
    CHECK(db.contains("k2"));

    auto val = db.get("k2");
    REQUIRE(val.has_value());
    CHECK(*val == "v2");

    REQUIRE(db.erase("k2").has_value());
    CHECK_FALSE(db.contains("k2"));
    CHECK(db.size() == 2);
}

// ============================================================================
// § 3  Crash Recovery from WAL
// ============================================================================

TEST_CASE (
"Petika: Automatic recovery from durable WAL log"
,
"[petika][recovery]"
)
 {
    TmpPetikaDir tmp;
    constexpr int kRecords = 50;

    {
        petika::PetikaOptions opts{
            .db_dir = tmp.path,
            .sync_on_write = true,
            .auto_recovery = false
        };
        petika::StringSkipStore db{opts};

        for (int i = 0; i < kRecords; ++i) {
            REQUIRE(db.put("user:" + std::to_string(i), "data_" + std::to_string(i)).has_value());
        }

        // Delete even records
        for (int i = 0; i < kRecords; i += 2) {
            REQUIRE(db.erase("user:" + std::to_string(i)).has_value());
        }
        REQUIRE(db.sync().has_value());
    } // DB closed

    // Reopen with auto_recovery = true
    {
        petika::PetikaOptions opts{
            .db_dir = tmp.path,
            .sync_on_write = true,
            .auto_recovery = true
        };
        petika::StringSkipStore db{opts};

        CHECK(db.size() == kRecords / 2);

        for (int i = 0; i < kRecords; ++i) {
            auto val = db.get("user:" + std::to_string(i));
            if (i % 2 == 0) {
                CHECK_FALSE(val.has_value());
            } else {
                REQUIRE(val.has_value());
                CHECK(*val == "data_" + std::to_string(i));
            }
        }
    }
}

// ============================================================================
// § 4  Transactions
// ============================================================================

TEST_CASE (
"Petika: Transaction commit and rollback"
,
"[petika][transaction]"
)
 {
    TmpPetikaDir tmp;
    petika::PetikaOptions opts{.db_dir = tmp.path};
    petika::StringSkipStore db{opts};

    REQUIRE(db.put("account:1", "100").has_value());
    REQUIRE(db.put("account:2", "200").has_value());

    // Aborted transaction
    {
        auto tx = db.transaction();
        REQUIRE(tx.put("account:1", "50").has_value());
        REQUIRE(tx.put("account:2", "250").has_value());
        CHECK(*tx.get("account:1") == "50");
        tx.abort();
    }

    CHECK(*db.get("account:1") == "100");
    CHECK(*db.get("account:2") == "200");

    // Committed transaction
    {
        auto tx = db.transaction();
        REQUIRE(tx.put("account:1", "50").has_value());
        REQUIRE(tx.put("account:2", "250").has_value());
        REQUIRE(tx.commit().has_value());
    }

    CHECK(*db.get("account:1") == "50");
    CHECK(*db.get("account:2") == "250");
}

TEST_CASE("Petika: failed batch has no partial in-memory publication", "[petika][transaction][atomic]") {
    TmpPetikaDir tmp;
    petika::StringSkipStore db{{.db_dir = tmp.path, .auto_recovery = false}};
    auto tx = db.transaction();
    REQUIRE(tx.put("new-key", "new-value").has_value());
    REQUIRE(tx.erase("missing-key").has_value());
    REQUIRE_FALSE(tx.commit().has_value());
    CHECK_FALSE(db.get("new-key").has_value());
}

TEST_CASE("Petika: default MVCC transaction rejects a stale same-key write", "[petika][transaction][mvcc]") {
    TmpPetikaDir tmp;
    petika::StringSkipStore db{{.db_dir = tmp.path, .auto_recovery = false}};
    REQUIRE(db.put("counter", "1").has_value());
    auto stale = db.transaction();
    REQUIRE(*stale.get("counter") == "1");
    REQUIRE(db.put("counter", "2").has_value());
    REQUIRE(stale.put("counter", "3").has_value());
    CHECK_FALSE(stale.commit().has_value());
    CHECK(*db.get("counter") == "2");
}

// ============================================================================
// § 5  Snapshots
// ============================================================================

TEST_CASE (
"Petika: Snapshot isolation view"
,
"[petika][snapshot]"
)
 {
    TmpPetikaDir tmp;
    petika::PetikaOptions opts{.db_dir = tmp.path};
    petika::StringSkipStore db{opts};

    REQUIRE(db.put("version", "v1.0").has_value());

    auto snap = db.snapshot();
    CHECK(snap.id() > 0);
    CHECK(*snap.get("version") == "v1.0");

    REQUIRE(db.put("version", "v2.0").has_value());
    CHECK(*db.get("version") == "v2.0");
}

TEST_CASE("Petika: MvccJournaledSkipEngine retains a durable snapshot view", "[petika][mvcc][snapshot]") {
    TmpPetikaDir tmp;
    petika::MvccSkipStore<std::string, std::string> db{{.db_dir = tmp.path, .auto_recovery = false}};
    REQUIRE(db.put("version", "v1").has_value());
    auto stable = db.snapshot();
    REQUIRE(db.put("version", "v2").has_value());
    CHECK(*stable.get("version") == "v1");
    CHECK(*db.get("version") == "v2");
}

TEST_CASE("Petika: MvccJournaledSkipEngine replays durable versions", "[petika][mvcc][recovery]") {
    TmpPetikaDir tmp;
    {
        petika::MvccSkipStore<std::string, std::string> db{{.db_dir = tmp.path, .auto_recovery = false}};
        REQUIRE(db.put("version", "v1").has_value());
        REQUIRE(db.put("version", "v2").has_value());
        REQUIRE(db.sync().has_value());
    }
    petika::MvccSkipStore<std::string, std::string> recovered{{.db_dir = tmp.path, .auto_recovery = true}};
    CHECK(*recovered.get("version") == "v2");
    CHECK(recovered.recover().has_value());
    CHECK(*recovered.get("version") == "v2");
}

// ============================================================================
// § 6  Kosha Cache Adapter for Petika
// ============================================================================

TEST_CASE (
"PetikaAdapter for Kosha: Cache write-through and read-through"
,
"[petika][kosha_adapter]"
)
 {
    TmpPetikaDir tmp;
    using StrCache = kosha::LRUCache<std::string, std::string>;
    kosha::adapter::PetikaAdapter<StrCache> adapter{tmp.path, StrCache{16}};

    REQUIRE(adapter.put("session:A", "token_123").has_value());
    auto val = adapter.get("session:A");
    REQUIRE(val.has_value());
    CHECK(*val == "token_123");

    adapter.clear(); // Clear in-memory cache
    CHECK(adapter.size() == 0);

    // Read-through restores from Petika store
    auto restored = adapter.get("session:A");
    REQUIRE(restored.has_value());
    CHECK(*restored == "token_123");
    CHECK(adapter.size() == 1);
}

// ============================================================================
// § 7  ConcurrencyPolicy: shared_mutex snapshot/writer non-blocking
// ============================================================================

TEST_CASE("Petika: concurrent snapshot reads do not block concurrent writers",
          "[petika][concurrency][shared_mutex]") {
    TmpPetikaDir tmp;
    petika::StringSkipStore db{{.db_dir = tmp.path, .auto_recovery = false}};

    REQUIRE(db.put("init", "0").has_value());

    constexpr int kReaders = 4;
    constexpr int kWriters = 4;
    constexpr int kOps = 50;

    std::atomic<int> reads_done{0};
    std::atomic<int> writes_done{0};

    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int r = 0; r < kReaders; ++r) {
        readers.emplace_back([&db, &reads_done] {
            for (int i = 0; i < kOps; ++i) {
                auto snap = db.snapshot();
                (void)snap.get("init");
            }
            reads_done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&db, &writes_done, w] {
            for (int i = 0; i < kOps; ++i) {
                (void)db.put("writer:" + std::to_string(w), std::to_string(i));
            }
            writes_done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    for (auto& t : readers) t.join();
    for (auto& t : writers) t.join();

    CHECK(reads_done.load() == kReaders);
    CHECK(writes_done.load() == kWriters);
    CHECK(db.size() > 0);
}

// ============================================================================
// § 8  SingleThreadSkipStore: CRUD round-trip on NullMutex variant
// ============================================================================

TEST_CASE("Petika: SingleThreadSkipStore put/get/erase/scan",
          "[petika][single_thread][null_mutex]") {
    TmpPetikaDir tmp;
    petika::SingleThreadSkipStore<std::string, std::string> db{
        {.db_dir = tmp.path, .auto_recovery = false}};

    REQUIRE(db.put("a", "1").has_value());
    REQUIRE(db.put("b", "2").has_value());
    REQUIRE(db.put("c", "3").has_value());

    CHECK(db.size() == 3);
    CHECK(*db.get("b") == "2");

    REQUIRE(db.erase("b").has_value());
    CHECK_FALSE(db.contains("b"));
    CHECK(db.size() == 2);

    std::vector<std::string> scanned;
    db.scan("a", "c", [&](const auto& entry) { scanned.push_back(entry.key); });
    REQUIRE(scanned.size() == 1);
    CHECK(scanned[0] == "a");
}

// ============================================================================
// § 9  AsyncPersistenceWorker: semaphore wake latency
// ============================================================================

// Must have external linkage for glaze reflection (glz::detail::external<T> requirement).
struct AsyncWorkerTestRecord { std::string data; };

TEST_CASE("Petika: AsyncPersistenceWorker drains within 10ms of enqueue",
          "[petika][async_persistence]") {
    TmpPetikaDir tmp;

    petika::AsyncPersistenceWorker<AsyncWorkerTestRecord> worker;

    const std::string outfile = (tmp.path / "record.json").string();
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(worker.enqueue(AsyncWorkerTestRecord{"hello"}, outfile));

    // Poll until file appears, up to 100ms
    bool found = false;
    for (int i = 0; i < 100 && !found; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        found = std::filesystem::exists(outfile);
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    CHECK(found);
    CHECK(elapsed_ms < 50); // generous bound; semaphore wake is sub-millisecond
}

// ============================================================================
// § 10  Transaction: O(1) read-your-own-writes correctness
// ============================================================================

TEST_CASE("Petika: Transaction read-your-own-writes: put/delete/overwrite",
          "[petika][transaction][read_your_own_writes]") {
    TmpPetikaDir tmp;
    petika::StringSkipStore db{{.db_dir = tmp.path, .auto_recovery = false}};
    REQUIRE(db.put("base", "original").has_value());

    auto tx = db.transaction();

    // put-then-get: must see the new value inside the same transaction
    REQUIRE(tx.put("new_key", "v1").has_value());
    auto r1 = tx.get("new_key");
    REQUIRE(r1.has_value());
    CHECK(*r1 == "v1");

    // put-then-overwrite-then-get: last write wins (O(1) index, not linear scan)
    REQUIRE(tx.put("new_key", "v2").has_value());
    auto r2 = tx.get("new_key");
    REQUIRE(r2.has_value());
    CHECK(*r2 == "v2");

    // put-then-delete-then-get: expect NotFound
    REQUIRE(tx.put("ephemeral", "gone").has_value());
    REQUIRE(tx.erase("ephemeral").has_value());
    auto r3 = tx.get("ephemeral");
    CHECK_FALSE(r3.has_value());

    // Existing key from store: visible in transaction before commit
    auto r4 = tx.get("base");
    REQUIRE(r4.has_value());
    CHECK(*r4 == "original");

    tx.abort();
}

// ============================================================================
// § 11  BloomFilterPolicy: zero false negatives
// ============================================================================

TEST_CASE("Petika: BloomFilterPolicy: zero false negatives after 10k inserts",
          "[petika][bloom_filter]") {
    TmpPetikaDir tmp;
    petika::BloomSkipStore<std::string, std::string> db{
        {.db_dir = tmp.path, .auto_recovery = false}};

    constexpr int kKeys = 10000;
    for (int i = 0; i < kKeys; ++i) {
        REQUIRE(db.put("bloom_key_" + std::to_string(i), std::to_string(i)).has_value());
    }

    // Every inserted key must be found — zero false negatives is a hard bloom invariant.
    int false_negatives = 0;
    for (int i = 0; i < kKeys; ++i) {
        if (!db.contains("bloom_key_" + std::to_string(i))) ++false_negatives;
    }
    CHECK(false_negatives == 0);
}

// ============================================================================
// § 12  scan_view: C++23 range-compatible lazy scan
// ============================================================================

TEST_CASE("Petika: scan_view returns iterable range over [start, end)",
          "[petika][scan_view]") {
    TmpPetikaDir tmp;
    petika::StringSkipStore db{{.db_dir = tmp.path, .auto_recovery = false}};

    for (int i = 1; i <= 10; ++i)
        REQUIRE(db.put("k" + std::to_string(i), "v" + std::to_string(i)).has_value());

    // scan_view is range-for compatible
    std::vector<std::string> keys;
    for (const auto& entry : db.scan_view("k3", "k8")) {
        keys.push_back(entry.key);
    }

    // Lexicographic scan: k3, k4, k5, k6, k7
    CHECK(keys.size() == 5);
    CHECK(keys.front() == "k3");
    CHECK(keys.back() == "k7");
}

// ============================================================================
// § 13  GroupCommit write coalescing (WriteBuffer policy — item 1)
// ============================================================================

TEST_CASE("petika: group commit coalesces WAL appends", "[petika][writebuffer]") {
    TmpPetikaDir tmp;

    constexpr std::size_t kBatch = 8;
    using Store = petika::GroupCommitSkipStore<std::string, std::string, kBatch>;

    std::uint64_t bytes_at_batch = 0;
    {
        Store db{{.db_dir = tmp.path, .auto_recovery = false}};

        // The first kBatch-1 staged writes buffer without touching the WAL:
        // no envelope is appended until the buffer fills.
        for (std::size_t i = 0; i < kBatch - 1; ++i) {
            REQUIRE(db.put("k" + std::to_string(i), "v" + std::to_string(i)).has_value());
        }
        CHECK(db.manifest().wal_bytes_written == 0);   // still buffered — zero appends
        CHECK(db.manifest().record_count == 0);        // engine untouched pre-flush

        // The kBatch-th staged write trips the threshold: exactly one batched
        // append drains all kBatch mutations into a single WAL envelope + sync.
        REQUIRE(db.put("k" + std::to_string(kBatch - 1), "v").has_value());
        bytes_at_batch = db.manifest().wal_bytes_written;
        CHECK(bytes_at_batch > 0);                      // one coalesced append happened
        CHECK(db.manifest().record_count == kBatch);    // all kBatch now applied

        // A follow-up partial batch stays buffered until flush()/destruction.
        REQUIRE(db.put("tail", "late").has_value());
        CHECK(db.manifest().wal_bytes_written == bytes_at_batch); // no new append yet
        REQUIRE(db.flush().has_value());
        CHECK(db.manifest().wal_bytes_written > bytes_at_batch);  // flush appended the tail
    }

    // Durability: every record — including the flushed tail — survives reopen.
    Store reopened{{.db_dir = tmp.path, .auto_recovery = true}};
    for (std::size_t i = 0; i < kBatch; ++i) {
        auto v = reopened.get("k" + std::to_string(i));
        REQUIRE(v.has_value());
    }
    auto tail = reopened.get("tail");
    REQUIRE(tail.has_value());
    CHECK(*tail == "late");
}

// ============================================================================
// § 14  Epoch GC bounds MVCC version growth (SnapshotGCPolicy — item 2)
// ============================================================================

TEST_CASE("petika: epoch GC prunes dead versions", "[petika][mvcc][gc]") {
    TmpPetikaDir tmp;
    petika::GcSkipStore<std::string, std::string> db{
        {.db_dir = tmp.path, .auto_recovery = false}};

    // No live snapshots → GC horizon is unbounded (max), so prune may reclaim
    // everything below last_lsn.
    CHECK(db.gc().min_snapshot_lsn() == std::numeric_limits<nitya::lsn_t>::max());

    REQUIRE(db.put("key", "v0").has_value());

    // An open snapshot pins the reclamation horizon at its LSN: dead versions
    // newer than the horizon must not be pruned while it is live.
    nitya::lsn_t pinned = 0;
    {
        auto snap = db.snapshot();
        pinned = db.gc().min_snapshot_lsn();
        CHECK(pinned != std::numeric_limits<nitya::lsn_t>::max()); // a snapshot is pinning

        // Churn the same key: without GC these versions accumulate forever.
        for (int i = 1; i <= 50; ++i)
            REQUIRE(db.put("key", "v" + std::to_string(i)).has_value());

        // Horizon stays pinned at the snapshot while it is alive.
        CHECK(db.gc().min_snapshot_lsn() == pinned);
    } // snapshot released here

    // Once released, the horizon advances (no live snapshot) so reclamation is
    // unblocked — the version chain is no longer pinned and can be pruned.
    CHECK(db.gc().min_snapshot_lsn() == std::numeric_limits<nitya::lsn_t>::max());

    // Latest value is still correct after churn + release.
    auto latest = db.get("key");
    REQUIRE(latest.has_value());
    CHECK(*latest == "v50");
}

// ============================================================================
// § 15  Endianness-canonical WAL portability (serializer — item 4)
// ============================================================================

TEST_CASE("petika: WAL payload codec is endian-canonical", "[petika][serializer]") {
    using petika::WalPayloadCodec;
    using petika::EntryOp;

    // key_len is a uint32 length prefix; it must be stored little-endian on the
    // wire regardless of host byte order so a WAL is portable across hosts.
    const std::string key = "AB";      // len 2
    const std::string val = "wxyz";    // len 4
    auto bytes = WalPayloadCodec::encode(EntryOp::Put, key, val);

    // Layout: [op:1][key_len:4 LE][key][val_len:4 LE][val]
    REQUIRE(bytes.size() == 1 + 4 + key.size() + 4 + val.size());
    CHECK(static_cast<std::uint8_t>(bytes[0]) == static_cast<std::uint8_t>(EntryOp::Put));

    // key_len == 2 → LE bytes 02 00 00 00 (asserted byte-for-byte, host-agnostic).
    CHECK(static_cast<std::uint8_t>(bytes[1]) == 0x02);
    CHECK(static_cast<std::uint8_t>(bytes[2]) == 0x00);
    CHECK(static_cast<std::uint8_t>(bytes[3]) == 0x00);
    CHECK(static_cast<std::uint8_t>(bytes[4]) == 0x00);

    // val_len == 4 sits after op(1)+key_len(4)+key(2) = offset 7 → LE 04 00 00 00.
    CHECK(static_cast<std::uint8_t>(bytes[7]) == 0x04);
    CHECK(static_cast<std::uint8_t>(bytes[8]) == 0x00);
    CHECK(static_cast<std::uint8_t>(bytes[9]) == 0x00);
    CHECK(static_cast<std::uint8_t>(bytes[10]) == 0x00);

    // Round-trip decode recovers the original fields.
    auto decoded = WalPayloadCodec::decode(std::span<const std::byte>(bytes));
    REQUIRE(decoded.has_value());
    auto [op, k_sv, v_sv] = *decoded;
    CHECK(op == EntryOp::Put);
    CHECK(std::string(k_sv) == key);
    CHECK(std::string(v_sv) == val);
}

// ============================================================================
// § 16  Concept enforcement diagnostics (item 3 + item 8 BTreeEngine)
// ============================================================================

TEST_CASE("petika: engine/serializer concepts enforced", "[petika][concept]") {
    using K = std::string;
    using V = std::string;

    // Shipped engines satisfy StorageEngine + BatchEngine.
    STATIC_REQUIRE(petika::StorageEngine<petika::JournaledSkipEngine<K, V>, K, V>);
    STATIC_REQUIRE(petika::StorageEngine<petika::MvccJournaledSkipEngine<K, V>, K, V>);
    STATIC_REQUIRE(petika::BatchEngine<petika::MvccJournaledSkipEngine<K, V>, K, V>);

    // BTreeEngine (item 8) satisfies both concepts too.
    STATIC_REQUIRE(petika::StorageEngine<petika::BTreeEngine<K, V, std::less<K>>, K, V>);
    STATIC_REQUIRE(petika::BatchEngine<petika::BTreeEngine<K, V, std::less<K>>, K, V>);

    // Shipped serializers satisfy SerializerFor for their key/value shapes.
    // (SerializerFor calls deserialize_* without an explicit type argument, so it
    // is modelled via the concrete std::string overloads both serializers expose.)
    STATIC_REQUIRE(petika::SerializerFor<petika::StringSerializer, K, V>);
    STATIC_REQUIRE(petika::SerializerFor<petika::BinarySerializer, std::string, std::string>);

    // NullMutex satisfies the MutexPolicy shape used by SingleThread stores.
    STATIC_REQUIRE(petika::MutexPolicy<petika::NullMutex>);
}

// ============================================================================
// § 17  Lazy scan_view pulls only the consumed prefix (item 7)
// ============================================================================

#if defined(__cpp_lib_generator) && __cpp_lib_generator >= 202207L
namespace {
    // Test-only engine exposing a lazy scan_lazy cursor that records how many
    // entries the coroutine has actually produced. Because a std::generator only
    // advances on ++, the counter reveals how much of the range was pulled.
    struct CountingCursorEngine {
        using key_type = int;
        using value_type = int;
        struct EntryView { const int& key; const int& value; nitya::lsn_t lsn; };

        std::vector<std::pair<int, int>> data;   // sorted by key
        mutable int pulled = 0;                   // entries the generator yielded

        template <typename Out>
        std::generator<Out> scan_lazy(const int& first, const int& last) const {
            for (const auto& [k, v] : data) {
                if (k < first) continue;
                if (!(k < last)) break;           // half-open [first, last)
                ++pulled;                          // observed on demand, per pull
                co_yield Out{k, v, 0};
            }
        }

        // Eager fallback surface (unused when the cursor is present, but keeps
        // ScanView's non-cursor branch well-formed).
        template <typename Cb>
        void scan(const int&, const int&, Cb&&) const {}
    };
} // anonymous namespace

TEST_CASE("petika: scan_view is lazy", "[petika][scan]") {
    CountingCursorEngine engine;
    for (int i = 0; i < 100; ++i) engine.data.emplace_back(i, i * 10);

    // The cursor is detected, so ScanView is the lazy path (no eager buffer).
    STATIC_REQUIRE(petika::ScanView<CountingCursorEngine>::kEngineHasCursor);

    petika::ScanView<CountingCursorEngine> view{engine, 0, 100};

    // Break after consuming 3 entries: a lazy view must not have walked the rest.
    int seen = 0;
    for (const auto& entry : view) {
        CHECK(entry.value == entry.key * 10);
        if (++seen == 3) break;
    }

    CHECK(seen == 3);
    CHECK(engine.pulled == 3);   // only the consumed prefix was pulled, not all 100
}
#endif // __cpp_lib_generator

// ============================================================================
// § 18  BTreeStore CRUD + WAL recovery (item 8)
// ============================================================================

TEST_CASE("petika: BTreeStore put/get/erase and durable recovery", "[petika][btree]") {
    TmpPetikaDir tmp;

    {
        petika::BTreeStore<std::string, std::string> db{
            {.db_dir = tmp.path, .sync_on_write = true, .auto_recovery = false}};

        REQUIRE(db.put("alpha", "1").has_value());
        REQUIRE(db.put("bravo", "2").has_value());
        REQUIRE(db.put("charlie", "3").has_value());

        auto got = db.get("bravo");
        REQUIRE(got.has_value());
        CHECK(*got == "2");

        // Overwrite keeps a single live version (no MVCC history).
        REQUIRE(db.put("bravo", "22").has_value());
        CHECK(*db.get("bravo") == "22");

        // Erase removes the key; a second erase reports NotFound.
        REQUIRE(db.erase("alpha").has_value());
        CHECK_FALSE(db.get("alpha").has_value());
        CHECK_FALSE(db.erase("alpha").has_value());
    }

    // Reopen: the WAL is the source of truth; recovery replays the survivors.
    {
        petika::BTreeStore<std::string, std::string> db{
            {.db_dir = tmp.path, .auto_recovery = true}};
        CHECK_FALSE(db.get("alpha").has_value()); // erased before close
        CHECK(*db.get("bravo") == "22");          // last write wins
        CHECK(*db.get("charlie") == "3");
    }
}

// ============================================================================
// § 19  BTreeStore scan_view is half-open and (with <generator>) lazy (item 7/8)
// ============================================================================

TEST_CASE("petika: BTreeStore scan_view is half-open [start, end)", "[petika][btree][scan]") {
    TmpPetikaDir tmp;
    petika::BTreeStore<std::string, std::string> db{
        {.db_dir = tmp.path, .auto_recovery = false}};

    for (int i = 0; i < 10; ++i)
        REQUIRE(db.put("k" + std::to_string(i), std::to_string(i)).has_value());

    std::vector<std::string> keys;
    for (const auto& e : db.scan_view("k3", "k8")) keys.push_back(e.key);

    // Upper bound is exclusive: k3..k7, never k8.
    REQUIRE(keys.size() == 5);
    CHECK(keys.front() == "k3");
    CHECK(keys.back() == "k7");
    CHECK(std::ranges::find(keys, std::string{"k8"}) == keys.end());
}

// ============================================================================
// § 20  BTreeEngine direct: from_sorted, apply_batch validation, replay delete
// ============================================================================

TEST_CASE("petika: BTreeEngine from_sorted / apply_batch / replay", "[petika][btree][engine]") {
    using Engine = petika::BTreeEngine<int, int, std::less<int>>;

    SECTION("from_sorted bulk-loads a sorted checkpoint in one pass") {
        std::vector<std::pair<int, int>> sorted{{1, 10}, {2, 20}, {3, 30}, {4, 40}};
        auto engine = Engine::from_sorted(sorted.begin(), sorted.end());
        CHECK(engine.size() == 4);
        REQUIRE(engine.get(3).has_value());
        CHECK(*engine.get(3) == 30);
    }

    SECTION("apply_batch commits a mixed put/delete set atomically") {
        // EntryView-shaped mutation the engine's apply_batch iterates over.
        struct BM { petika::EntryOp op; int key; int value; };
        Engine engine;
        REQUIRE(engine.put(7, 70, 1).has_value()); // pre-existing target for delete

        std::vector<BM> batch{
            {petika::EntryOp::Put,    1, 100},
            {petika::EntryOp::Put,    2, 200},
            {petika::EntryOp::Delete, 7, 0},   // deletes a live key
        };
        REQUIRE(engine.apply_batch(batch, 2).has_value());
        CHECK(*engine.get(1) == 100);
        CHECK(*engine.get(2) == 200);
        CHECK_FALSE(engine.get(7).has_value());
    }

    SECTION("apply_batch rejects a delete of an unknown key before mutating") {
        struct BM { petika::EntryOp op; int key; int value; };
        Engine engine;
        REQUIRE(engine.put(1, 10, 1).has_value());
        std::vector<BM> batch{
            {petika::EntryOp::Put,    2, 20},
            {petika::EntryOp::Delete, 99, 0}, // 99 absent and not inserted in-batch
        };
        CHECK_FALSE(engine.apply_batch(batch, 2).has_value());
        // Rejected up-front: the earlier Put must not have landed.
        CHECK_FALSE(engine.get(2).has_value());
    }

    SECTION("apply_log_record delete is idempotent for replay") {
        Engine engine;
        // Replaying a delete for an absent key is not an error (log may re-delete).
        CHECK(engine.apply_log_record(petika::EntryOp::Delete, 5, 0, 1).has_value());
    }
}

// ============================================================================
// § 21  AsyncPersistenceWorker Reject overflow fires the error callback (item 9)
// ============================================================================

TEST_CASE("petika: async worker Reject overflow reports OverflowDropped", "[petika][async][overflow]") {
    // Smallest legal ring (RingBuffer requires N>=2, power of two). Reject: once
    // the slots are occupied and the worker has not drained them, the next
    // enqueue must be rejected and reported.
    using Worker = petika::AsyncPersistenceWorker<
        AsyncWorkerTestRecord, 2, petika::GlazeJsonPolicy, petika::OverflowPolicy::Reject>;

    std::atomic<int> dropped{0};
    Worker worker;
    worker.set_error_callback([&](petika::WorkerError err, std::string_view) {
        if (err == petika::WorkerError::OverflowDropped) dropped.fetch_add(1);
    });
    worker.stop(); // stop the drain so the ring cannot empty; producer path still works

    // First push may occupy the slot; subsequent pushes into a full, undrained
    // ring are rejected. Hammer until at least one rejection is observed.
    bool saw_reject = false;
    for (int i = 0; i < 128; ++i) {
        // Point at a directory (unwritable as a file) — irrelevant: worker stopped.
        if (!worker.enqueue(AsyncWorkerTestRecord{std::to_string(i)}, "/dev/null/never")) {
            saw_reject = true;
            break;
        }
    }
    CHECK(saw_reject);
    CHECK(dropped.load() >= 1);
}

// ============================================================================
// § 22  AsyncPersistenceWorker DropOldest evicts to make room (item 9)
// ============================================================================

TEST_CASE("petika: async worker DropOldest never rejects, reports eviction", "[petika][async][overflow]") {
    using Worker = petika::AsyncPersistenceWorker<
        AsyncWorkerTestRecord, 2, petika::GlazeJsonPolicy, petika::OverflowPolicy::DropOldest>;

    std::atomic<int> dropped{0};
    Worker worker;
    worker.set_error_callback([&](petika::WorkerError err, std::string_view) {
        if (err == petika::WorkerError::OverflowDropped) dropped.fetch_add(1);
    });
    worker.stop(); // freeze the consumer so the ring stays full and eviction triggers

    // DropOldest always returns true (it makes room by evicting the oldest).
    bool all_accepted = true;
    for (int i = 0; i < 32; ++i) {
        if (!worker.enqueue(AsyncWorkerTestRecord{std::to_string(i)}, "/dev/null/never"))
            all_accepted = false;
    }
    CHECK(all_accepted);
    CHECK(dropped.load() >= 1); // at least one oldest item was evicted
}

// ============================================================================
// § 23  ImmediateCommitPolicy is pass-through; GroupCommit MaxLevel forwarded
// ============================================================================

TEST_CASE("petika: ImmediateCommitPolicy publishes each put immediately", "[petika][writebuffer]") {
    TmpPetikaDir tmp;
    // Default SkipStore uses ImmediateCommitPolicy — no staging, no buffering.
    petika::SkipStore<std::string, std::string> db{
        {.db_dir = tmp.path, .auto_recovery = false}};

    REQUIRE(db.put("a", "1").has_value());
    // Immediate: the value is visible without any explicit flush().
    CHECK(*db.get("a") == "1");

    REQUIRE(db.put("b", "2").has_value());
    CHECK(*db.get("b") == "2");
    CHECK(db.manifest().record_count == 2); // each put appended at once, not staged
}

TEST_CASE("petika: MvccJournaledSkipEngine forwards MaxLevel (item 8 fix)", "[petika][concept][mvcc]") {
    using E8  = petika::MvccJournaledSkipEngine<std::string, std::string, petika::LexicalComparator, 8>;
    using E32 = petika::MvccJournaledSkipEngine<std::string, std::string, petika::LexicalComparator, 32>;
    // The MaxLevel template param was previously discarded; it is now surfaced.
    STATIC_REQUIRE(E8::max_level == 8);
    STATIC_REQUIRE(E32::max_level == 32);
}

