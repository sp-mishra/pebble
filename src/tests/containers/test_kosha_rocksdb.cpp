// ============================================================================
// test_kosha_rocksdb.cpp — Tests for kosha_rocksdb.hpp RocksDBAdapter
// ============================================================================
#if __has_include(<rocksdb/db.h>)
#include "catch_amalgamated.hpp"
#include "containers/cache/adapters/rocksdb.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace {
    // RAII helper: creates a unique tmp path and removes it on destruction.
    struct TmpDB {
        std::string path;

        TmpDB() {
            auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
            path = (std::filesystem::temp_directory_path() /
                ("kosha_test_" + std::to_string(ns))).string();
        }

        ~TmpDB() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    using StrCache = kosha::LRUCache<std::string, std::string>;
    using Adapter = kosha::adapter::RocksDBAdapter<StrCache>;
} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST_CASE (


"RocksDBAdapter: put then get from cache"
,
"[rocksdb]"
)
 {
    TmpDB tmp;
    Adapter a{tmp.path, StrCache{16}};

    REQUIRE(a.put("key1", "val1").has_value());
    auto r = a.get("key1");
    REQUIRE(r.has_value());
    CHECK(*r == "val1");
}

TEST_CASE (


"RocksDBAdapter: get after cache clear reads from RocksDB"
,
"[rocksdb]"
)
 {
    TmpDB tmp;
    Adapter a{tmp.path, StrCache{16}};

    REQUIRE(a.put("key2", "val2").has_value());
    a.clear(); // evict from in-memory cache
    CHECK(a.size() == 0);

    auto r = a.get("key2");
    REQUIRE(r.has_value());
    CHECK(*r == "val2");
    CHECK(a.size() == 1); // re-populated from RocksDB
}

TEST_CASE (


"RocksDBAdapter: erase removes from cache and RocksDB"
,
"[rocksdb]"
)
 {
    TmpDB tmp;
    Adapter a{tmp.path, StrCache{16}};

    REQUIRE(a.put("key3", "val3").has_value());
    a.erase("key3");
    a.clear(); // ensure cache miss forces RocksDB lookup

    auto r = a.get("key3");
    CHECK_FALSE(r.has_value());
}

TEST_CASE (


"RocksDBAdapter: load_all warms cache"
,
"[rocksdb]"
)
 {
    TmpDB tmp;

    {
        Adapter a{tmp.path, StrCache{32}};
        for (int i = 0; i < 10; ++i)
            REQUIRE(a.put(std::to_string(i), "v" + std::to_string(i)).has_value());
    }

    // New adapter, empty in-memory cache.
    Adapter b{tmp.path, StrCache{32}};
    std::size_t loaded = b.load_all(10);
    CHECK(loaded == 10);

    for (int i = 0; i < 10; ++i) {
        auto r = b.get(std::to_string(i));
        REQUIRE(r.has_value());
        CHECK(*r == "v" + std::to_string(i));
    }
}

TEST_CASE (


"RocksDBAdapter: persistence across instances"
,
"[rocksdb]"
)
 {
    TmpDB tmp;

    {
        Adapter a{tmp.path, StrCache{8}};
        REQUIRE(a.put("persist_key", "persist_val").has_value());
    } // adapter1 destroyed, DB closed

    Adapter b{tmp.path, StrCache{8}};
    auto r = b.get("persist_key"); // must read from RocksDB
    REQUIRE(r.has_value());
    CHECK(*r == "persist_val");
}

TEST_CASE (


"RocksDBAdapter: LRU eviction still works with persistence"
,
"[rocksdb]"
)
 {
    TmpDB tmp;
    Adapter a{tmp.path, StrCache{3}}; // cap=3

    for (int i = 0; i < 5; ++i)
        REQUIRE(a.put(std::to_string(i), "v" + std::to_string(i)).has_value());

    // In-memory cache holds at most 3; DB holds all 5.
    CHECK(a.size() <= 3);

    // Evicted keys should still be retrievable from RocksDB.
    int db_hits = 0;
    for (int i = 0; i < 5; ++i) {
        auto r = a.get(std::to_string(i));
        if (r.has_value()) ++db_hits;
    }
    CHECK(db_hits == 5);
}
#endif
