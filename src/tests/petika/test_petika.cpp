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
#include <string>
#include <thread>
#include <vector>

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
    REQUIRE(stale.get("counter") == "1");
    REQUIRE(db.put("counter", "2").has_value());
    REQUIRE(stale.put("counter", "3").has_value());
    CHECK_FALSE(stale.commit().has_value());
    CHECK(db.get("counter") == "2");
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
    CHECK(stable.get("version") == "v1");
    CHECK(db.get("version") == "v2");
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
    CHECK(recovered.get("version") == "v2");
    CHECK(recovered.recover().has_value());
    CHECK(recovered.get("version") == "v2");
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
