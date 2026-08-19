// ============================================================================
// test_nitya.cpp — Unit tests for Nitya: A Generic Durable Log Engine (DLE)
// ============================================================================

#include "catch_amalgamated.hpp"
#include "nitya/nitya.hpp"
#include "containers/cache/adapters/nitya.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <span>

namespace {
    // RAII helper: creates a unique temp folder for Nitya WAL segments and removes on destruction
    struct TmpWalDir {
        std::filesystem::path path;

        TmpWalDir() {
            auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("nitya_test_" + std::to_string(ns));
            std::filesystem::create_directories(path);
        }

        ~TmpWalDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    std::span<const std::byte> as_byte_span(std::string_view s) {
        return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
    }

    std::string_view as_str_view(std::span<const std::byte> b) {
        return {reinterpret_cast<const char*>(b.data()), b.size()};
    }
} // anonymous namespace

// ============================================================================
// § 1  Frame Layout & Encoding/Decoding
// ============================================================================

TEST_CASE("Nitya: Frame layout constants and sizes", "[nitya][framing]") {
    STATIC_REQUIRE(sizeof(nitya::frame_header) == 24);
    STATIC_REQUIRE(sizeof(nitya::frame_trailer) == 8);
    STATIC_REQUIRE(nitya::k_frame_overhead == 32);
    REQUIRE(nitya::k_nitya_magic == 0x4E495459);
}

TEST_CASE("Nitya: Default framing encode and validate", "[nitya][framing]") {
    std::string msg = "SAMPLE_RECORD_DATA_123456";
    std::uint32_t payload_len = static_cast<std::uint32_t>(msg.size());
    std::size_t total_len = nitya::k_frame_overhead + payload_len;

    std::vector<std::byte> buffer(total_len);
    nitya::reservation res{
        .lsn = 1024,
        .buffer = buffer,
        .payload_size = payload_len
    };

    auto payload_buf = res.payload_buffer();
    std::memcpy(payload_buf.data(), msg.data(), payload_len);

    std::uint32_t crc = nitya::default_framing::calculate_crc32(payload_buf.data(), payload_buf.size());
    nitya::default_framing::encode(res, crc);

    // Validate Header
    nitya::frame_header hdr;
    std::memcpy(&hdr, buffer.data(), sizeof(hdr));
    auto val_res = nitya::default_framing::validate_header(hdr, 1024);
    REQUIRE(val_res.has_value());
    CHECK(*val_res == payload_len);

    // Validate Trailer & Payload
    nitya::frame_trailer trl;
    std::memcpy(&trl, buffer.data() + sizeof(hdr) + payload_len, sizeof(trl));
    auto val_trl = nitya::default_framing::validate_payload_and_trailer(payload_buf, trl, crc);
    REQUIRE(val_trl.has_value());

    // Corrupted payload crc detection
    auto bad_trl = nitya::default_framing::validate_payload_and_trailer(payload_buf, trl, crc + 1);
    CHECK_FALSE(bad_trl.has_value());
}

// ============================================================================
// § 2  Basic Reserve, Publish, Sync, Append
// ============================================================================

TEST_CASE("Nitya: Single record append and sync", "[nitya][append]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024 // 1 MB
    };

    nitya::wal<> log{opts};
    CHECK(log.tail_lsn() == 0);
    CHECK(log.flushed_lsn() == 0);

    auto lsn_res = log.append(as_byte_span("TX_BEGIN"));
    REQUIRE(lsn_res.has_value());
    CHECK(*lsn_res == 0);
    CHECK(log.tail_lsn() == nitya::k_frame_overhead + std::string("TX_BEGIN").size());

    REQUIRE(log.sync().has_value());
    CHECK(log.flushed_lsn() == log.tail_lsn());
}

TEST_CASE("Nitya: Multiple appends with sequential LSN offsets", "[nitya][append]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024
    };

    nitya::wal<> log{opts};
    std::vector<std::string> messages = {
        "Record_0001",
        "Record_0002_somewhat_longer",
        "Record_0003_even_longer_payload_for_testing"
    };

    nitya::lsn_t expected_lsn = 0;
    for (const auto& msg : messages) {
        auto res = log.append(as_byte_span(msg));
        REQUIRE(res.has_value());
        CHECK(*res == expected_lsn);
        expected_lsn += nitya::k_frame_overhead + msg.size();
    }

    CHECK(log.tail_lsn() == expected_lsn);
    REQUIRE(log.sync().has_value());
    CHECK(log.flushed_lsn() == expected_lsn);
}

// ============================================================================
// § 3  Segment Rotation
// ============================================================================

TEST_CASE("Nitya: Auto-rotation across segment boundary", "[nitya][segment]") {
    TmpWalDir tmp;
    // Set a small segment size (e.g. 256 bytes) to trigger rotation
    constexpr std::size_t kSegSize = 256;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = kSegSize,
        .auto_rotate = true
    };

    nitya::wal<> log{opts};

    // Frame overhead is 32. Payload of 100 -> record size is 132.
    // 2 records in one segment = 264 > 256, so second record will rotate to segment 1.
    std::string payload1(100, 'A');
    std::string payload2(100, 'B');

    auto lsn1 = log.append(as_byte_span(payload1));
    REQUIRE(lsn1.has_value());
    CHECK(*lsn1 == 0);

    auto lsn2 = log.append(as_byte_span(payload2));
    REQUIRE(lsn2.has_value());
    // lsn2 must start at beginning of segment 1 (offset 256)
    CHECK(*lsn2 == kSegSize);

    REQUIRE(log.sync().has_value());

    auto segs = log.list_segments();
    REQUIRE(segs.size() == 2);
    CHECK(segs[0].segment_id == 0);
    CHECK(segs[1].segment_id == 1);
}

// ============================================================================
// § 4  Recovery Engine
// ============================================================================

TEST_CASE("Nitya: Streaming recovery scans all committed records", "[nitya][recovery]") {
    TmpWalDir tmp;
    constexpr std::size_t kSegSize = 512;
    std::vector<std::string> written_payloads;

    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize,
            .auto_rotate = true
        };
        nitya::wal<> log{opts};

        for (int i = 0; i < 15; ++i) {
            std::string msg = "MSG_" + std::to_string(i) + "_data";
            written_payloads.push_back(msg);
            REQUIRE(log.append(as_byte_span(msg)).has_value());
        }
        REQUIRE(log.sync().has_value());
    } // WAL closed

    // Reopen WAL and recover
    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize,
            .auto_rotate = true
        };
        nitya::wal<> log{opts};

        std::vector<std::string> recovered;
        auto stream = log.recover(0);
        for (const auto& rec : stream) {
            recovered.emplace_back(as_str_view(rec.payload));
        }

        REQUIRE(recovered.size() == written_payloads.size());
        for (std::size_t i = 0; i < recovered.size(); ++i) {
            CHECK(recovered[i] == written_payloads[i]);
        }

        // Tail LSN should be restored after restart
        CHECK(log.tail_lsn() > 0);
    }
}

// ============================================================================
// § 5  Replication Stream Engine
// ============================================================================

TEST_CASE("Nitya: Replication subscriber streams updates", "[nitya][replication]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024
    };

    nitya::wal<> log{opts};

    for (int i = 0; i < 5; ++i) {
        REQUIRE(log.append(as_byte_span("REP_" + std::to_string(i))).has_value());
    }
    REQUIRE(log.sync().has_value());

    auto stream = log.subscribe(0);
    int count = 0;
    while (auto rec = stream.next()) {
        std::string expected = "REP_" + std::to_string(count);
        CHECK(as_str_view(rec->payload) == expected);
        ++count;
    }
    CHECK(count == 5);
    CHECK(log.replicated_lsn() == log.tail_lsn());
}

// ============================================================================
// § 6  Concurrency & Multi-Threaded Appends
// ============================================================================

TEST_CASE("Nitya: Concurrent multi-threaded appends", "[nitya][concurrency]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 16 * 1024 * 1024, // 16 MB
        .auto_rotate = true
    };

    nitya::wal<> log{opts};

    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 250;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::atomic<std::size_t> append_errors{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&log, &append_errors, t] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::string msg = "T" + std::to_string(t) + "_OP" + std::to_string(i);
                auto res = log.append(as_byte_span(msg));
                if (!res.has_value()) {
                    append_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    REQUIRE(append_errors.load() == 0);
    REQUIRE(log.sync().has_value());
    CHECK(log.flushed_lsn() == log.tail_lsn());

    // Verify all records are recover-readable and have valid checksums
    std::size_t total_recovered = 0;
    auto stream = log.recover(0);
    for (const auto& rec : stream) {
        (void)rec;
        ++total_recovered;
    }

    CHECK(total_recovered == kThreads * kOpsPerThread);
}

// ============================================================================
// § 7  EasyRules Administrative Retention & Archival
// ============================================================================

TEST_CASE("Nitya: EasyRules retention & archival triggers", "[nitya][rules]") {
    TmpWalDir tmp;
    constexpr std::size_t kSegSize = 256;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = kSegSize,
        .auto_rotate = true
    };

    nitya::wal<> log{opts};

    // Write enough to produce multiple segments
    for (int i = 0; i < 10; ++i) {
        std::string s(100, 'X');
        REQUIRE(log.append(as_byte_span(s)).has_value());
    }
    REQUIRE(log.sync().has_value());

    // Replicate all
    log.set_replicated_lsn(log.tail_lsn());

    std::vector<std::uint64_t> archived;
    std::vector<std::uint64_t> deleted;

    log.apply_retention_rules(
        std::chrono::seconds{0}, // immediate expiration for test
        [&](const nitya::segment_descriptor& d) { archived.push_back(d.segment_id); },
        [&](const nitya::segment_descriptor& d) { deleted.push_back(d.segment_id); }
    );

    // Archival rule should trigger for all replicated non-archived segments
    CHECK_FALSE(archived.empty());
}

// ============================================================================
// § 8  kosha::adapter::NityaAdapter Tests
// ============================================================================

TEST_CASE("NityaAdapter: basic put and get hit", "[nitya][adapter]") {
    TmpWalDir tmp;
    using StrCache = kosha::LRUCache<std::string, std::string>;
    kosha::adapter::NityaAdapter<StrCache> adapter{tmp.path, StrCache{16}};

    REQUIRE(adapter.put("user:101", "Alice").has_value());
    auto val = adapter.get("user:101");
    REQUIRE(val.has_value());
    CHECK(*val == "Alice");
}

TEST_CASE("NityaAdapter: read-through recovery after cache clear", "[nitya][adapter]") {
    TmpWalDir tmp;
    using StrCache = kosha::LRUCache<std::string, std::string>;
    kosha::adapter::NityaAdapter<StrCache> adapter{tmp.path, StrCache{16}};

    REQUIRE(adapter.put("k1", "v1").has_value());
    REQUIRE(adapter.put("k2", "v2").has_value());

    adapter.clear(); // Clear memory cache only
    CHECK(adapter.size() == 0);

    // Read-through from Nitya WAL
    auto v1 = adapter.get("k1");
    REQUIRE(v1.has_value());
    CHECK(*v1 == "v1");
    CHECK(adapter.size() == 1);

    auto v2 = adapter.get("k2");
    REQUIRE(v2.has_value());
    CHECK(*v2 == "v2");
    CHECK(adapter.size() == 2);
}

TEST_CASE("NityaAdapter: erase records tombstone in WAL", "[nitya][adapter]") {
    TmpWalDir tmp;
    using StrCache = kosha::LRUCache<std::string, std::string>;
    kosha::adapter::NityaAdapter<StrCache> adapter{tmp.path, StrCache{16}};

    REQUIRE(adapter.put("del_key", "val").has_value());
    REQUIRE(adapter.erase("del_key").has_value());

    adapter.clear();
    auto res = adapter.get("del_key");
    CHECK_FALSE(res.has_value());
    CHECK(res.error() == kosha::adapter::NityaAdapterError::NotFound);
}

TEST_CASE("NityaAdapter: load_all warms in-memory cache", "[nitya][adapter]") {
    TmpWalDir tmp;
    using StrCache = kosha::LRUCache<std::string, std::string>;

    {
        kosha::adapter::NityaAdapter<StrCache> a{tmp.path, StrCache{32}};
        for (int i = 0; i < 10; ++i) {
            REQUIRE(a.put("k" + std::to_string(i), "v" + std::to_string(i)).has_value());
        }
    } // Flush and closed

    // Re-open with new adapter instance
    kosha::adapter::NityaAdapter<StrCache> b{tmp.path, StrCache{32}};
    CHECK(b.size() == 0);

    std::size_t loaded = b.load_all();
    CHECK(loaded == 10);
    CHECK(b.size() == 10);

    for (int i = 0; i < 10; ++i) {
        auto val = b.get("k" + std::to_string(i));
        REQUIRE(val.has_value());
        CHECK(*val == "v" + std::to_string(i));
    }
}

