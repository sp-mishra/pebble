// ============================================================================
// test_petika.cpp — Unit tests for Petika: Unified Engine-Agnostic Storage
// ============================================================================

#include "catch_amalgamated.hpp"
#include "petika/petika.hpp"
#include "petika/adapters/kosha.hpp"

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
