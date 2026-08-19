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
    STATIC_REQUIRE(sizeof(nitya::segment_header) == 44);
    STATIC_REQUIRE(sizeof(nitya::frame_header) == 28);
    STATIC_REQUIRE(sizeof(nitya::frame_trailer) == 8);
    STATIC_REQUIRE(nitya::k_frame_overhead == 36);
    REQUIRE(nitya::k_nitya_magic == 0x4E495459);
    REQUIRE(nitya::k_nitya_seg_magic == 0x4E534547);
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
    CHECK(log.tail_lsn() == nitya::k_segment_header_size);
    CHECK(log.flushed_lsn() == nitya::k_segment_header_size);

    auto lsn_res = log.append(as_byte_span("TX_BEGIN"));
    REQUIRE(lsn_res.has_value());
    CHECK(*lsn_res == nitya::k_segment_header_size);
    CHECK(log.tail_lsn() == nitya::k_segment_header_size + nitya::k_frame_overhead + std::string("TX_BEGIN").size());

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

    nitya::lsn_t expected_lsn = nitya::k_segment_header_size;
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
    // Set a segment size (e.g. 256 bytes) to trigger rotation
    // Segment 0: 44 bytes header. First record: 36 overhead + 100 payload = 136 bytes.
    // 44 + 136 = 180 bytes <= 256 bytes.
    // Second record: 180 + 136 = 316 > 256 -> rotates to Segment 1 at 256 + 44 = 300.
    constexpr std::size_t kSegSize = 256;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = kSegSize,
        .auto_rotate = true
    };

    nitya::wal<> log{opts};

    std::string payload1(100, 'A');
    std::string payload2(100, 'B');

    auto lsn1 = log.append(as_byte_span(payload1));
    REQUIRE(lsn1.has_value());
    CHECK(*lsn1 == nitya::k_segment_header_size);

    auto lsn2 = log.append(as_byte_span(payload2));
    REQUIRE(lsn2.has_value());
    // lsn2 must start after segment 1's header (offset 256 + 44 = 300)
    CHECK(*lsn2 == kSegSize + nitya::k_segment_header_size);

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
        CHECK(log.tail_lsn() > nitya::k_segment_header_size);
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

TEST_CASE("Nitya: Append sync and recovery diagnostics", "[nitya][diagnostics]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024
    };

    nitya::wal<> log{opts};

    auto res1 = log.append_sync(as_byte_span("diagnostic_data_1"));
    REQUIRE(res1.has_value());
    CHECK(*res1 == nitya::k_segment_header_size); // First record LSN starts after segment header (44)

    auto res2 = log.append_sync(as_byte_span("diagnostic_data_2"));
    REQUIRE(res2.has_value());
    CHECK(*res2 > nitya::k_segment_header_size);

    // Let's recover the stream
    auto stream = log.recover(0);
    std::vector<std::string> recs;
    for (const auto& rec : stream) {
        recs.push_back(std::string{as_str_view(rec.payload)});
    }

    REQUIRE(recs.size() == 2);
    CHECK(recs[0] == "diagnostic_data_1");
    CHECK(recs[1] == "diagnostic_data_2");

    auto status = stream.status();
    CHECK(status.records_recovered == 2);
    CHECK(status.error == nitya::LogError::EndOfLog);
    CHECK(status.last_valid_lsn == *res2);
    CHECK(status.first_bad_lsn == nitya::k_invalid_lsn);
}

// ============================================================================
// § 9  Advanced Durability & Group Commit Semantics
// ============================================================================

TEST_CASE("Nitya: wait_durable beyond published LSN fails with InvalidArg", "[nitya][durability]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024
    };
    nitya::wal<> log{opts};

    auto res = log.append(as_byte_span("ENTRY"));
    REQUIRE(res.has_value());

    // Published LSN is now *res + k_frame_overhead + payload size
    nitya::lsn_t pub = log.published_lsn();
    auto err = log.wait_durable(pub + 100);
    REQUIRE_FALSE(err.has_value());
    CHECK(err.error() == nitya::LogError::InvalidArg);
}

TEST_CASE("Nitya: append vs append_sync vs sync durability watermarks", "[nitya][durability]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024
    };
    nitya::wal<> log{opts};

    // append only publishes, flushed_lsn is still initial header size
    auto lsn1 = log.append(as_byte_span("ASYNC_1"));
    REQUIRE(lsn1.has_value());
    CHECK(log.published_lsn() > nitya::k_segment_header_size);
    CHECK(log.flushed_lsn() == nitya::k_segment_header_size);

    // sync flushes up to current published LSN
    REQUIRE(log.sync().has_value());
    CHECK(log.flushed_lsn() == log.published_lsn());

    // append_sync flushes immediately
    auto lsn2 = log.append_sync(as_byte_span("SYNC_2"));
    REQUIRE(lsn2.has_value());
    CHECK(log.flushed_lsn() == log.published_lsn());
}

TEST_CASE("Nitya: Zero-byte payload support", "[nitya][correctness]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024
    };
    nitya::wal<> log{opts};

    auto lsn = log.append(std::span<const std::byte>{});
    REQUIRE(lsn.has_value());
    REQUIRE(log.sync().has_value());

    auto stream = log.recover(0);
    int count = 0;
    for (const auto& rec : stream) {
        CHECK(rec.payload.empty());
        ++count;
    }
    CHECK(count == 1);
}

TEST_CASE("Nitya: Reject payload larger than segment", "[nitya][correctness]") {
    TmpWalDir tmp;
    constexpr std::size_t kSegSize = 1024;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = kSegSize
    };
    nitya::wal<> log{opts};

    std::string huge(2048, 'Z');
    auto res = log.append(as_byte_span(huge));
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error() == nitya::LogError::InvalidArg);
}

TEST_CASE("Nitya: Record exact fit and auto-rotate off behavior", "[nitya][segment]") {
    TmpWalDir tmp;
    // Segment size: 100 bytes. Header is 44 bytes.
    // Remaining usable space in segment: 100 - 44 = 56 bytes.
    // First record: 36 overhead + 15 payload = 51 bytes <= 56 bytes (fits).
    // Second record: 51 + 51 = 102 > 56 bytes -> must fail with SegmentFull because auto_rotate = false.
    constexpr std::size_t kSegSize = 100;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = kSegSize,
        .auto_rotate = false
    };
    nitya::wal<> log{opts};

    std::string p1(15, 'A');
    auto lsn1 = log.append(as_byte_span(p1));
    REQUIRE(lsn1.has_value());
    CHECK(*lsn1 == nitya::k_segment_header_size);

    // Second record exceeds segment capacity and auto_rotate is false
    auto lsn2 = log.append(as_byte_span(p1));
    REQUIRE_FALSE(lsn2.has_value());
    CHECK(lsn2.error() == nitya::LogError::SegmentFull);
}

// ============================================================================
// § 10 Background Flusher
// ============================================================================

TEST_CASE("Nitya: Background flusher periodically commits published LSN", "[nitya][flusher]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024,
        .background_flush = true,
        .group_commit_interval = std::chrono::microseconds{5000} // 5ms
    };

    nitya::wal<> log{opts};
    auto lsn = log.append(as_byte_span("BG_DATA"));
    REQUIRE(lsn.has_value());

    // Wait for flusher thread to commit
    auto start = std::chrono::steady_clock::now();
    while (log.flushed_lsn() < log.published_lsn()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
            break;
        }
    }

    CHECK(log.flushed_lsn() == log.published_lsn());
}

// ============================================================================
// § 11 Recovery Modes & Corruption Diagnostics
// ============================================================================

TEST_CASE("Nitya: Recovery modes and corruption handling", "[nitya][corruption]") {
    TmpWalDir tmp;
    constexpr std::size_t kSegSize = 4096;

    // 1. Write 3 valid records
    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};
        REQUIRE(log.append_sync(as_byte_span("REC_1")).has_value());
        REQUIRE(log.append_sync(as_byte_span("REC_2")).has_value());
        REQUIRE(log.append_sync(as_byte_span("REC_3")).has_value());
    }

    // Corrupt REC_2's payload CRC in the raw segment file
    auto seg_path = tmp.path / "0000000000.log";
    REQUIRE(std::filesystem::exists(seg_path));

    auto map_res = setu::mapping<setu::read_write>::open_or_create(seg_path, kSegSize);
    REQUIRE(map_res.has_value());
    auto bytes = map_res->as_bytes();

    // Offset of REC_2: segment_header(44) + REC_1 overhead(36) + payload(5) = 85
    std::size_t rec2_off = nitya::k_segment_header_size + 36 + 5;
    // Corrupt payload byte inside REC_2
    bytes[rec2_off + sizeof(nitya::frame_header) + 1] = static_cast<std::byte>(0xFF);
    (void)map_res->flush_range(0, kSegSize, setu::flush_mode::sync);

    // 2. Test stop_at_first_error mode (default)
    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};

        auto stream = log.recover(0, nitya::recovery_mode::stop_at_first_error);
        std::vector<std::string> recs;
        for (const auto& rec : stream) {
            recs.emplace_back(as_str_view(rec.payload));
        }

        // Should recover REC_1 and stop on REC_2 corruption
        REQUIRE(recs.size() == 1);
        CHECK(recs[0] == "REC_1");

        auto st = stream.status();
        CHECK(st.records_recovered == 1);
        CHECK(st.error == nitya::LogError::CorruptedPayload);
    }

    // 3. Test salvage mode
    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};

        auto stream = log.recover(0, nitya::recovery_mode::salvage);
        std::vector<std::string> recs;
        for (const auto& rec : stream) {
            recs.emplace_back(as_str_view(rec.payload));
        }

        // Salvage scans past corrupted bytes and finds REC_3
        REQUIRE(recs.size() == 2);
        CHECK(recs[0] == "REC_1");
        CHECK(recs[1] == "REC_3");
    }
}

// ============================================================================
// § 12 Crash Simulation Scenarios
// ============================================================================

TEST_CASE("Nitya: Crash simulation after reservation before publication", "[nitya][crash]") {
    TmpWalDir tmp;
    constexpr std::size_t kSegSize = 4096;

    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};

        // Write 1 committed record
        REQUIRE(log.append_sync(as_byte_span("STABLE_RECORD")).has_value());

        // Reserve a slot (allocating byte space in mapped file) but simulate crash before publish
        auto res = log.reserve(128);
        REQUIRE(res.has_value());
        // Simulating crash: do not publish res, let log destruct
    }

    // Reopen log: recovery should recover STABLE_RECORD cleanly and detect EndOfLog at unwritten slot
    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};

        auto stream = log.recover(0);
        std::vector<std::string> recs;
        for (const auto& rec : stream) {
            recs.emplace_back(as_str_view(rec.payload));
        }

        REQUIRE(recs.size() == 1);
        CHECK(recs[0] == "STABLE_RECORD");
        CHECK(stream.status().error == nitya::LogError::EndOfLog);
    }
}

TEST_CASE("Nitya: Non-allocating list_segments iterator", "[nitya][admin]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 256,
        .auto_rotate = true
    };
    nitya::wal<> log{opts};

    for (int i = 0; i < 5; ++i) {
        std::string payload(100, 'K');
        REQUIRE(log.append_sync(as_byte_span(payload)).has_value());
    }

    containers::static_vector<nitya::segment_descriptor, 16> seg_list;
    auto res = log.list_segments(std::back_inserter(seg_list));
    REQUIRE(res.has_value());

    CHECK(seg_list.size() > 1);
    CHECK(seg_list[0].segment_id == 0);
    CHECK(seg_list[0].begin_lsn == 0);
}

// ============================================================================
// § 13 Segment Header Validation & Corruption Tests
// ============================================================================

TEST_CASE("Nitya: Segment header write and validation on reopen", "[nitya][segment_header]") {
    TmpWalDir tmp;
    constexpr std::size_t kSegSize = 1024 * 1024;

    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};
        REQUIRE(log.append_sync(as_byte_span("SEG_RECORD_1")).has_value());
    }

    // Inspect segment 0 file directly
    auto seg_path = tmp.path / "0000000000.log";
    REQUIRE(std::filesystem::exists(seg_path));

    auto map_res = setu::mapping<setu::read_only>::open_existing(seg_path);
    REQUIRE(map_res.has_value());
    auto bytes = map_res->as_bytes();
    REQUIRE(bytes.size() >= sizeof(nitya::segment_header));

    nitya::segment_header hdr;
    std::memcpy(&hdr, bytes.data(), sizeof(hdr));
    CHECK(hdr.magic == nitya::k_nitya_seg_magic);
    CHECK(hdr.version == nitya::k_nitya_format_version);
    CHECK(hdr.segment_id == 0);
    CHECK(hdr.begin_lsn == 0);
    CHECK(nitya::calculate_segment_header_crc(hdr) == hdr.header_crc);

    // Reopen log successfully
    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};
        CHECK(log.tail_lsn() > nitya::k_segment_header_size);
    }
}

TEST_CASE("Nitya: Corrupted segment header is rejected", "[nitya][segment_header]") {
    TmpWalDir tmp;
    constexpr std::size_t kSegSize = 1024 * 1024;

    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};
        REQUIRE(log.append_sync(as_byte_span("INITIAL_DATA")).has_value());
    }

    // Corrupt the segment header CRC in raw file
    auto seg_path = tmp.path / "0000000000.log";
    auto map_res = setu::mapping<setu::read_write>::open_or_create(seg_path, kSegSize);
    REQUIRE(map_res.has_value());
    auto bytes = map_res->as_bytes();
    // Corrupt magic byte
    bytes[0] = static_cast<std::byte>(0x00);
    (void)map_res->flush_range(0, kSegSize, setu::flush_mode::sync);

    // Reopen log: segment open/validation fails
    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};
        // Appending to segment 0 fails because validation rejects corrupted header
        auto app_res = log.append(as_byte_span("FAIL_DATA"));
        REQUIRE_FALSE(app_res.has_value());
        CHECK(app_res.error() == nitya::LogError::CorruptedHeader);
    }
}

TEST_CASE("Nitya: First record does not overwrite segment header and recovery skips it", "[nitya][segment_header]") {
    TmpWalDir tmp;
    constexpr std::size_t kSegSize = 1024 * 1024;

    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};
        auto lsn = log.append_sync(as_byte_span("FIRST_ENTRY"));
        REQUIRE(lsn.has_value());
        // LSN must be exactly offset 44 (after segment header)
        CHECK(*lsn == nitya::k_segment_header_size);
    }

    // Verify raw segment file starts with segment header magic, not frame magic
    auto seg_path = tmp.path / "0000000000.log";
    auto map_res = setu::mapping<setu::read_only>::open_existing(seg_path);
    REQUIRE(map_res.has_value());
    auto bytes = map_res->as_bytes();

    nitya::segment_header shdr;
    std::memcpy(&shdr, bytes.data(), sizeof(shdr));
    CHECK(shdr.magic == nitya::k_nitya_seg_magic);

    nitya::frame_header fhdr;
    std::memcpy(&fhdr, bytes.data() + sizeof(shdr), sizeof(fhdr));
    CHECK(fhdr.magic == nitya::k_nitya_magic);
    CHECK(fhdr.lsn == nitya::k_segment_header_size);

    // Verify recovery starting from 0 skips the segment header automatically
    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};
        auto stream = log.recover(0);
        int count = 0;
        for (const auto& rec : stream) {
            CHECK(rec.lsn == nitya::k_segment_header_size);
            CHECK(as_str_view(rec.payload) == "FIRST_ENTRY");
            ++count;
        }
        CHECK(count == 1);
    }
}

// ============================================================================
// § 14 Version Validation & Record Flags Round-Trip
// ============================================================================

TEST_CASE("Nitya: Frame version validation and UnsupportedVersion error", "[nitya][version]") {
    TmpWalDir tmp;
    std::string payload_str = "VERSIONED_DATA";
    std::size_t total_len = nitya::k_frame_overhead + payload_str.size();
    std::vector<std::byte> buffer(total_len);

    nitya::reservation res{
        .lsn = 100,
        .buffer = buffer,
        .payload_size = static_cast<std::uint32_t>(payload_str.size()),
        .version = 99, // unsupported version
        .flags = 0x1234
    };

    auto payload_buf = res.payload_buffer();
    std::memcpy(payload_buf.data(), payload_str.data(), payload_str.size());
    std::uint32_t crc = nitya::default_framing::calculate_crc32(payload_buf.data(), payload_buf.size());
    nitya::default_framing::encode(res, crc);

    nitya::frame_header hdr;
    std::memcpy(&hdr, buffer.data(), sizeof(hdr));
    auto val_res = nitya::default_framing::validate_header(hdr, 100);
    REQUIRE_FALSE(val_res.has_value());
    CHECK(val_res.error() == nitya::LogError::UnsupportedVersion);
}

TEST_CASE("Nitya: Flags and version round-trip through append and recovery", "[nitya][flags]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024
    };

    constexpr std::uint16_t kCustomFlags = 0xABCD;

    {
        nitya::wal<> log{opts};
        auto lsn = log.append_sync(as_byte_span("FLAGGED_ENTRY"), kCustomFlags);
        REQUIRE(lsn.has_value());
    }

    {
        nitya::wal<> log{opts};
        auto stream = log.recover(0);
        int count = 0;
        for (const auto& rec : stream) {
            CHECK(rec.flags == kCustomFlags);
            CHECK(rec.version == nitya::k_nitya_format_version);
            CHECK(as_str_view(rec.payload) == "FLAGGED_ENTRY");
            ++count;
        }
        CHECK(count == 1);
    }
}

// ============================================================================
// § 15 Segment Descriptor Metadata Population
// ============================================================================

TEST_CASE("Nitya: Segment descriptor populated from segment header metadata", "[nitya][descriptor]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024
    };

    auto before = std::chrono::system_clock::now();

    {
        nitya::wal<> log{opts};
        REQUIRE(log.append_sync(as_byte_span("METADATA_TEST")).has_value());
    }

    auto after = std::chrono::system_clock::now();

    {
        nitya::wal<> log{opts};
        auto segs = log.list_segments();
        REQUIRE(segs.size() == 1);
        CHECK(segs[0].segment_id == 0);
        CHECK(segs[0].begin_lsn == 0);
        CHECK(segs[0].end_lsn == 1024 * 1024);
        CHECK(segs[0].created_at >= before - std::chrono::seconds(1));
        CHECK(segs[0].created_at <= after + std::chrono::seconds(1));
    }
}

// ============================================================================
// § 16 Group Commit & Follower Atomic Wait Tests
// ============================================================================

TEST_CASE("Nitya: Concurrent group commit waiters receive completion without spinning forever", "[nitya][group_commit]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 16 * 1024 * 1024
    };

    nitya::wal<> log{opts};

    constexpr int kWaiters = 8;
    std::vector<nitya::lsn_t> lsns(kWaiters);
    for (int i = 0; i < kWaiters; ++i) {
        auto res = log.append(as_byte_span("WAIT_REC_" + std::to_string(i)));
        REQUIRE(res.has_value());
        lsns[i] = *res + nitya::k_frame_overhead + std::string("WAIT_REC_" + std::to_string(i)).size();
    }

    std::vector<std::thread> threads;
    threads.reserve(kWaiters);
    std::atomic<std::size_t> completed{0};

    for (int i = 0; i < kWaiters; ++i) {
        threads.emplace_back([&log, &lsns, &completed, i] {
            auto r = log.wait_durable(lsns[i]);
            if (r.has_value()) {
                completed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    CHECK(completed.load() == kWaiters);
    CHECK(log.flushed_lsn() >= lsns.back());
}

// ============================================================================
// § 17 Background Flusher Watermark and Clean Shutdown Tests
// ============================================================================

TEST_CASE("Nitya: Background flusher watermark flush and explicit shutdown join", "[nitya][flusher]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024,
        .background_flush = true,
        .group_commit_interval = std::chrono::microseconds{100000}, // 100ms
        .group_commit_bytes = 256 // small watermark threshold to trigger watermark-based flush
    };

    {
        nitya::wal<> log{opts};

        // Append enough records to trigger watermark flush (256 bytes)
        for (int i = 0; i < 5; ++i) {
            std::string data(64, 'W');
            REQUIRE(log.append(as_byte_span(data)).has_value());
        }

        // Wait for watermark-driven flush
        auto start = std::chrono::steady_clock::now();
        while (log.flushed_lsn() < log.published_lsn()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
                break;
            }
        }

        CHECK(log.flushed_lsn() == log.published_lsn());
    } // Explicit join and final sync in ~wal()

    // Reopen and ensure all records are cleanly recovered
    {
        nitya::wal_options reopen_opts{
            .wal_dir = tmp.path,
            .segment_size = 1024 * 1024
        };
        nitya::wal<> log{reopen_opts};
        auto stream = log.recover(0);
        int count = 0;
        for (const auto& rec : stream) {
            (void)rec;
            ++count;
        }
        CHECK(count == 5);
    }
}

// ============================================================================
// § 18 Segment Sealing & Archival Metadata Persistence
// ============================================================================

TEST_CASE("Nitya: Segment rotation seals previous segment with sealed_lsn and flags", "[nitya][seal]") {
    TmpWalDir tmp;
    constexpr std::size_t kSegSize = 256; // small segment to trigger rapid rotation

    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize,
            .auto_rotate = true
        };
        nitya::wal<> log{opts};

        // Write entry that fits in segment 0 (44 hdr + 36 overhead + 100 payload = 180 <= 256)
        std::string small_rec(100, 'A');
        REQUIRE(log.append_sync(as_byte_span(small_rec)).has_value());

        // Write entry that exceeds remaining capacity of segment 0 (180 + 136 = 316 > 256)
        // -> triggers rotation to segment 1 (fits in 256 since 44 + 136 = 180 <= 256)
        std::string large_rec(100, 'B');
        auto lsn2 = log.append_sync(as_byte_span(large_rec));
        REQUIRE(lsn2.has_value());
        CHECK(*lsn2 >= kSegSize); // in segment 1
    }

    // Inspect list_segments() on reopen to verify sealed_lsn and end_lsn
    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};
        auto segs = log.list_segments();
        REQUIRE(segs.size() >= 2);
        CHECK(segs[0].segment_id == 0);
        CHECK(segs[0].end_lsn == 180); // sealed at exact current LSN (44 + 136)
    }
}

TEST_CASE("Nitya: Archival persistence and mark_segment_archived", "[nitya][archive]") {
    TmpWalDir tmp;
    constexpr std::size_t kSegSize = 1024 * 1024;

    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};
        REQUIRE(log.append_sync(as_byte_span("TEST_DATA")).has_value());

        auto segs = log.list_segments();
        REQUIRE(segs.size() == 1);
        CHECK_FALSE(segs[0].is_archived);

        // Mark archived
        REQUIRE(log.mark_segment_archived(0).has_value());
    }

    // Reopen and check that is_archived persists from durable segment header
    {
        nitya::wal_options opts{
            .wal_dir = tmp.path,
            .segment_size = kSegSize
        };
        nitya::wal<> log{opts};
        auto segs = log.list_segments();
        REQUIRE(segs.size() == 1);
        CHECK(segs[0].is_archived);
    }
}

TEST_CASE("Nitya: flush_to validates target_lsn <= published_lsn", "[nitya][flush_to]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 1024 * 1024
    };

    nitya::wal<> log{opts};
    auto app_res = log.append(as_byte_span("VALID_DATA"));
    REQUIRE(app_res.has_value());

    // Flush valid published range
    CHECK(log.flush_to(log.published_lsn()).has_value());

    // Attempting to flush beyond published LSN returns InvalidArg
    auto bad_flush = log.flush_to(log.published_lsn() + 1000);
    REQUIRE_FALSE(bad_flush.has_value());
    CHECK(bad_flush.error() == nitya::LogError::InvalidArg);
}

// ============================================================================
// § 19 Follower Leadership Retry & Concurrent Durability Tests
// ============================================================================

TEST_CASE("Nitya: Follower retries leadership and completes durability without blocking forever", "[nitya][follower_retry]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 16 * 1024 * 1024
    };

    nitya::wal<> log{opts};

    constexpr int kThreads = 16;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    std::atomic<std::size_t> successful_syncs{0};

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&log, &successful_syncs, i] {
            std::string payload = "CONCURRENT_THREAD_DATA_" + std::to_string(i);
            auto lsn = log.append_sync(as_byte_span(payload));
            if (lsn.has_value()) {
                successful_syncs.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    CHECK(successful_syncs.load() == kThreads);
    CHECK(log.flushed_lsn() == log.published_lsn());
}

// ============================================================================
// § 20 High-Contention Multi-Iteration Follower Promotion Stress Test
// ============================================================================

TEST_CASE("Nitya: Sustained concurrent group commit and follower promotion stress", "[nitya][stress]") {
    TmpWalDir tmp;
    nitya::wal_options opts{
        .wal_dir = tmp.path,
        .segment_size = 32 * 1024 * 1024
    };

    nitya::wal<> log{opts};

    constexpr int kThreads = 16;
    constexpr int kIters = 64;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    std::atomic<std::size_t> completed_appends{0};
    std::array<std::atomic<std::size_t>, 16> error_counts{};
    for (auto& c : error_counts) {
        c.store(0, std::memory_order_relaxed);
    }

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&log, &completed_appends, &error_counts, t] {
            for (int i = 0; i < kIters; ++i) {
                std::string payload = "STRESS_DATA_T" + std::to_string(t) + "_I" + std::to_string(i);
                auto res = log.append_sync(as_byte_span(payload));
                if (res.has_value()) {
                    completed_appends.fetch_add(1, std::memory_order_relaxed);
                }
                else {
                    const auto idx = static_cast<std::size_t>(res.error());
                    if (idx < error_counts.size()) {
                        error_counts[idx].fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    auto error_name = [](nitya::LogError e) -> const char* {
        switch (e) {
            case nitya::LogError::Success: return "Success";
            case nitya::LogError::InvalidArg: return "InvalidArg";
            case nitya::LogError::SegmentFull: return "SegmentFull";
            case nitya::LogError::SegmentOpenFailed: return "SegmentOpenFailed";
            case nitya::LogError::WriteFailed: return "WriteFailed";
            case nitya::LogError::FlushFailed: return "FlushFailed";
            case nitya::LogError::CorruptedHeader: return "CorruptedHeader";
            case nitya::LogError::CorruptedPayload: return "CorruptedPayload";
            case nitya::LogError::CorruptedTrailer: return "CorruptedTrailer";
            case nitya::LogError::ChecksumMismatch: return "ChecksumMismatch";
            case nitya::LogError::LsnMismatch: return "LsnMismatch";
            case nitya::LogError::EndOfLog: return "EndOfLog";
            case nitya::LogError::QueueFull: return "QueueFull";
            case nitya::LogError::UnsupportedVersion: return "UnsupportedVersion";
            case nitya::LogError::InternalError: return "InternalError";
        }
        return "Unknown";
    };

    std::size_t total_failures = 0;
    std::string error_summary;
    for (std::size_t i = 0; i < error_counts.size(); ++i) {
        const auto count = error_counts[i].load(std::memory_order_relaxed);
        if (count == 0) continue;
        total_failures += count;
        error_summary += std::string(error_name(static_cast<nitya::LogError>(i))) + "=" + std::to_string(count) + " ";
    }

    INFO("completed_appends=" << completed_appends.load() << " expected=" << (kThreads * kIters));
    INFO("append_sync_failures=" << total_failures << " details=" << error_summary);

    CHECK(completed_appends.load() == kThreads * kIters);
    CHECK(log.flushed_lsn() == log.published_lsn());
}
