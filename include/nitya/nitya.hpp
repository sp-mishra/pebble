#pragma once
// ============================================================================
// nitya/nitya.hpp — A Generic Durable Log Engine (DLE)
// ============================================================================
//
// C++23, header-only, policy-based, zero virtual functions, zero RTTI,
// zero heap allocation on the hot append path.
//
// LSN Model & Physical File Layout:
//   - LSN is a physical byte offset in the WAL file space across segments.
//   - Each segment reserves [segment_base, segment_base + sizeof(segment_header)) for segment metadata.
//   - The first valid record LSN in each segment is segment_base + sizeof(segment_header).
//
// Subsystems:
//   - Byte Offset LSN (Physical byte offset in the log stream)
//   - Reserve -> Publish -> Sync Pipeline (Group commit coordinator)
//   - Versioned Frame & Segment Header Layout (CRC32-C validation)
//   - Setu-backed Mapped Segment Manager (rotation, allocation, header validation, flush)
//   - Smriti-backed Memory & Arena scratch integration
//   - Lock-Free Concurrency (Leader/Follower Group Commit with Result Propagation & Atomic Wait)
//   - Background Durability Flusher (Interval & Byte Watermark Driven, Clean Join Shutdown)
//   - NADI-backed Observability (scopes for reserve, publish, flush, recovery)
//   - EasyRules-backed Administrative Retention & Archival management
//   - Multi-mode Streaming Recovery (Strict, StopAtFirstError, Salvage)
//   - Replication Stream Subscription Engine
// ============================================================================

#include <utility>
#include "utils/setu.hpp"
#include "mem/smriti.hpp"
#include "mem/arena.hpp"
#include "observability/nadi.hpp"
#include "rules/easy_rules.hpp"
#include "containers/static/static_vector.hpp"

#if !defined(NITYA_NO_GOOGLE_CRC32C)
#include "crc32c/crc32c.h"
#endif

// Optional SIMD acceleration for salvage resynchronisation. Gated exactly like
// containers/tree/bplus_tree.hpp: present only when Highway headers are available.
#if __has_include(<hwy/highway.h>)
#include <hwy/highway.h>
#define PEBBLE_HAS_HIGHWAY 1
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nitya {
    // ============================================================================
    // § 1  Types, Constants & Atomic Helpers
    // ============================================================================

    using lsn_t = std::uint64_t;
    inline constexpr lsn_t k_invalid_lsn = std::numeric_limits<lsn_t>::max();
    inline constexpr std::uint32_t k_nitya_magic = 0x4E495459; // "NITY"
    inline constexpr std::uint32_t k_nitya_seg_magic = 0x4E534547; // "NSEG"
    inline constexpr std::uint16_t k_nitya_format_version = 1;

    inline void atomic_max_lsn(std::atomic<lsn_t>& target, const lsn_t value) noexcept {
        lsn_t cur = target.load(std::memory_order_relaxed);
        while (cur < value &&
            !target.compare_exchange_weak(
                cur,
                value,
                std::memory_order_release,
                std::memory_order_relaxed)) {}
    }

    enum class LogError : std::uint8_t {
        Success = 0,
        InvalidArg,
        SegmentFull,
        SegmentOpenFailed,
        WriteFailed,
        FlushFailed,
        CorruptedHeader,
        CorruptedPayload,
        CorruptedTrailer,
        ChecksumMismatch,
        LsnMismatch,
        EndOfLog,
        QueueFull,
        UnsupportedVersion,
        InternalError
    };

    template <typename T>
    using Result = std::expected<T, LogError>;

    enum class recovery_mode : std::uint8_t {
        strict = 0,
        stop_at_first_error = 1,
        salvage = 2
    };

    struct recovery_status {
        lsn_t last_valid_lsn{k_invalid_lsn};
        lsn_t first_bad_lsn{k_invalid_lsn};
        LogError error{LogError::Success};
        std::size_t records_recovered{0};
        std::size_t bytes_recovered{0};
        std::uint64_t segment_id{0};
    };

    struct durability_health {
        LogError last_error{LogError::Success};
        lsn_t last_error_lsn{k_invalid_lsn};

        [[nodiscard]] bool healthy() const noexcept { return last_error == LogError::Success; }
    };

    struct wal_metrics {
        std::uint64_t records_published{0};
        std::uint64_t bytes_published{0};
        std::uint64_t flush_operations{0};
        std::uint64_t flush_failures{0};
        std::uint64_t group_commit_waiters{0};
        std::uint64_t replication_records{0};
        std::uint64_t replication_bytes{0};
    };

    struct replication_checkpoint {
        std::string replica_id;
        lsn_t acknowledged_lsn{0};
    };

    // ============================================================================
    // § 2  Binary Frame & Segment Header Layout
    // ============================================================================

#pragma pack(push, 1)
    struct segment_header {
        std::uint32_t magic{k_nitya_seg_magic};
        std::uint16_t version{k_nitya_format_version};
        std::uint16_t flags{0};
        std::uint64_t segment_id{0};
        lsn_t begin_lsn{0};
        lsn_t sealed_lsn{0};
        std::uint64_t created_at_unix_ns{0};
        std::uint32_t header_crc{0};
    };

    struct frame_header {
        std::uint32_t magic{k_nitya_magic};
        std::uint16_t version{k_nitya_format_version};
        std::uint16_t flags{0};
        std::uint32_t size{0}; // Payload size
        std::uint64_t lsn{0}; // Byte offset LSN
        std::uint32_t header_crc{0};
        std::uint32_t payload_crc{0};
    };

    struct frame_trailer {
        std::uint32_t size{0};
        std::uint32_t payload_crc{0};
    };
#pragma pack(pop)

    static_assert(sizeof(segment_header) == 44, "segment_header must be packed to 44 bytes");
    static_assert(sizeof(frame_header) == 28, "frame_header must be packed to 28 bytes");
    static_assert(sizeof(frame_trailer) == 8, "frame_trailer must be packed to 8 bytes");

    inline constexpr std::size_t k_segment_header_size = sizeof(segment_header);
    inline constexpr std::size_t k_frame_overhead = sizeof(frame_header) + sizeof(frame_trailer);

    inline constexpr std::uint16_t k_segment_archived = 1 << 0;
    inline constexpr std::uint16_t k_segment_sealed = 1 << 1;

    struct wal_record {
        lsn_t lsn{0};
        std::span<const std::byte> payload;
        std::uint16_t version{k_nitya_format_version};
        std::uint16_t flags{0};
    };

    struct reservation {
        lsn_t lsn{k_invalid_lsn};
        std::span<std::byte> buffer; // Total span including frame header, payload, and trailer
        std::uint32_t payload_size{0};
        std::uint16_t version{k_nitya_format_version};
        std::uint16_t flags{0};
        // Keeps the owning segment_file alive from reserve() through publish().
        // Type-erased so reservation does not depend on StoragePolicy.
        std::shared_ptr<void> segment_pin;

        [[nodiscard]] bool is_valid() const noexcept {
            return lsn != k_invalid_lsn && !buffer.empty();
        }

        [[nodiscard]] std::span<std::byte> payload_buffer() const noexcept {
            if (buffer.size() < k_frame_overhead) return {};
            return buffer.subspan(sizeof(frame_header), payload_size);
        }
    };

    // ============================================================================
    // § 3  Segment Descriptor & Options
    // ============================================================================

    struct segment_descriptor {
        std::uint64_t segment_id{0};
        lsn_t begin_lsn{0};
        lsn_t end_lsn{0};
        std::filesystem::path path;
        bool is_archived{false};
        bool is_replicated{false};
        std::chrono::system_clock::time_point created_at{std::chrono::system_clock::now()};
    };

    struct wal_options {
        std::filesystem::path wal_dir{"./nitya_wal"};
        std::size_t segment_size{64 * 1024 * 1024}; // 64 MB default
        std::size_t max_cached_segments{8};
        bool sync_on_publish{false};
        bool auto_rotate{true};
        bool background_flush{false};
        std::chrono::microseconds group_commit_interval{1000};
        std::size_t group_commit_bytes{4 * 1024 * 1024};
    };

    // ============================================================================
    // § 4  Framing Policy (CRC32-C / Castagnoli Checksums)
    // ============================================================================

    namespace detail {
        inline constexpr std::uint32_t crc32c_byte(std::uint32_t crc, const std::uint8_t byte) noexcept {
            crc ^= byte;
            for (int i = 0; i < 8; ++i) {
                crc = (crc >> 1) ^ (0x82F63B78u * (crc & 1));
            }
            return crc;
        }

        constexpr std::uint32_t calculate_crc32c_fallback(const std::byte* data, const std::size_t len) noexcept {
            std::uint32_t crc = ~0u;
            for (std::size_t i = 0; i < len; ++i) {
                crc = crc32c_byte(crc, static_cast<std::uint8_t>(data[i]));
            }
            return ~crc;
        }
    } // namespace detail

    struct default_framing {
        // ---- Physical-format ownership (Item 5: version evolution seam) --------
        // The framing policy owns the on-disk format version and structural sizes.
        // The 44/28/8-byte layout is asserted *here*, where the policy that emits
        // it lives, rather than as free-standing global asserts. A future v2 ships
        // as a distinct policy with its own version/sizes/encode/decode; recovery
        // negotiates via supports_version() instead of hard-rejecting.
        static constexpr std::uint16_t format_version = k_nitya_format_version;
        static constexpr std::size_t header_size = sizeof(frame_header);
        static constexpr std::size_t trailer_size = sizeof(frame_trailer);
        static constexpr std::size_t segment_header_size = sizeof(segment_header);
        static constexpr std::size_t frame_overhead = header_size + trailer_size;

        static_assert(sizeof(segment_header) == 44, "segment_header must be packed to 44 bytes");
        static_assert(sizeof(frame_header) == 28, "frame_header must be packed to 28 bytes");
        static_assert(sizeof(frame_trailer) == 8, "frame_trailer must be packed to 8 bytes");

        // Version negotiation hook: recovery consults this rather than comparing
        // against a single constant, so a multi-version policy can accept several.
        [[nodiscard]] static constexpr bool supports_version(const std::uint16_t v) noexcept {
            return v == format_version;
        }

        static std::uint32_t calculate_checksum32(const std::byte* data, const std::size_t len) noexcept {
#if !defined(NITYA_NO_GOOGLE_CRC32C)
            return crc32c::Crc32c(reinterpret_cast<const std::uint8_t*>(data), len);
#else
            return detail::calculate_crc32c_fallback(data, len);
#endif
        }

        static std::uint32_t calculate_crc32(const std::byte* data, const std::size_t len) noexcept {
            return calculate_checksum32(data, len);
        }

        static void encode(reservation& res, const std::uint32_t payload_crc) noexcept {
            assert(res.buffer.size() >= k_frame_overhead + res.payload_size);

            frame_header hdr;
            hdr.magic = k_nitya_magic;
            hdr.version = res.version;
            hdr.flags = res.flags;
            hdr.size = res.payload_size;
            hdr.lsn = res.lsn;
            hdr.payload_crc = payload_crc;

            // Calculate header checksum over packed struct with header_crc initialized to 0
            hdr.header_crc = 0;
            hdr.header_crc = calculate_checksum32(reinterpret_cast<const std::byte*>(&hdr), sizeof(hdr));

            // Write header
            std::memcpy(res.buffer.data(), &hdr, sizeof(frame_header));

            // Write trailer
            frame_trailer trl;
            trl.size = res.payload_size;
            trl.payload_crc = payload_crc;
            std::memcpy(res.buffer.data() + sizeof(frame_header) + res.payload_size, &trl, sizeof(frame_trailer));
        }

        static Result<std::uint32_t> validate_header(const frame_header& hdr, const lsn_t expected_lsn) noexcept {
            if (hdr.magic != k_nitya_magic) {
                return std::unexpected(LogError::CorruptedHeader);
            }
            if (!supports_version(hdr.version)) {
                return std::unexpected(LogError::UnsupportedVersion);
            }
            if (hdr.lsn != expected_lsn) {
                return std::unexpected(LogError::LsnMismatch);
            }

            frame_header copy = hdr;
            copy.header_crc = 0;
            if (const std::uint32_t computed_crc = calculate_checksum32(reinterpret_cast<const std::byte*>(&copy),
                                                                        sizeof(copy)); computed_crc != hdr.header_crc) {
                return std::unexpected(LogError::ChecksumMismatch);
            }

            return hdr.size;
        }

        static Result<void> validate_payload_and_trailer(
            const std::span<const std::byte> payload,
            const frame_trailer& trl,
            const std::uint32_t expected_payload_crc) noexcept {
            if (trl.size != payload.size()) {
                return std::unexpected(LogError::CorruptedTrailer);
            }
            if (trl.payload_crc != expected_payload_crc) {
                return std::unexpected(LogError::ChecksumMismatch);
            }
            if (const std::uint32_t actual_crc = calculate_checksum32(payload.data(), payload.size()); actual_crc !=
                expected_payload_crc) {
                return std::unexpected(LogError::CorruptedPayload);
            }
            return {};
        }
    };

    // Segment-header CRC calculation, routed through a FramingPolicy so a custom
    // framing (xxHash, hardware-CRC-off build, ...) applies uniformly to segment
    // headers and frames. Defaulted to default_framing so existing free-function
    // call sites (`calculate_segment_header_crc(hdr)`) remain byte-identical.
    template <typename Framing = default_framing>
    inline std::uint32_t calculate_segment_header_crc(segment_header hdr) noexcept {
        hdr.header_crc = 0;
        return Framing::calculate_checksum32(
            reinterpret_cast<const std::byte*>(&hdr),
            sizeof(hdr)
        );
    }

    // ============================================================================
    // § 4b Clock Policy (segment stamping & retention time source)
    // ============================================================================

    // Wall-clock source. Segment stamping and retention consult it so tests can
    // inject a deterministic clock and simulation-time deployments can drive
    // logical time. Default wraps std::chrono::system_clock (byte-identical).
    struct system_clock_source {
        [[nodiscard]] static std::uint64_t now_unix_ns() noexcept {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                .count());
        }
    };

    // ============================================================================
    // § 5  Storage Policy (Setu-backed mapped segments)
    // ============================================================================

    // Setu-backed mapped-segment storage. Templated on the FramingPolicy so segment
    // header checksums use the same policy as frames (Item 3 uniformity). The public
    // `setu_storage` alias below binds it to default_framing, byte-identical to before.
    // (Constrained via the FramingPolicyLike concept at the `wal` boundary; the concept
    // itself is defined further below, so this param is unconstrained here by ordering.)
    template <typename Framing = default_framing, typename Clock = system_clock_source>
    class setu_storage_t {
    public:
        // Policy-owned segment filename format (Item 4). A stateful/tiered storage
        // policy can override this to namespace segments; default is "{:010d}.log".
        static constexpr std::string_view segment_name_format = "{:010d}.log";

        struct segment_file {
            std::uint64_t segment_id{0};
            lsn_t begin_lsn{0};
            std::size_t capacity{0};
            std::filesystem::path path;
            setu::mapping<setu::read_write> map;
        };

        explicit setu_storage_t(wal_options opts) : opts_{std::move(opts)} {
            std::error_code ec;
            std::filesystem::create_directories(opts_.wal_dir, ec);
        }

        ~setu_storage_t() = default;
        setu_storage_t(const setu_storage_t&) = delete;
        setu_storage_t& operator=(const setu_storage_t&) = delete;
        setu_storage_t(setu_storage_t&&) = delete;
        setu_storage_t& operator=(setu_storage_t&&) = delete;

        // Static so existing call sites (`nitya::setu_storage::format_segment_name(id)`)
        // keep compiling; a static method still satisfies the member-style concept call.
        static std::string format_segment_name(std::uint64_t segment_id) {
            return std::vformat(segment_name_format, std::make_format_args(segment_id));
        }

        static Result<void> validate_segment_header(
            const segment_file& seg,
            const std::uint64_t expected_segment_id,
            const lsn_t expected_begin_lsn) {
            const auto bytes = seg.map.as_bytes();
            if (bytes.size() < sizeof(segment_header)) {
                return std::unexpected(LogError::CorruptedHeader);
            }

            segment_header hdr;
            std::memcpy(&hdr, bytes.data(), sizeof(hdr));

            if (hdr.magic != k_nitya_seg_magic) {
                return std::unexpected(LogError::CorruptedHeader);
            }

            if (!Framing::supports_version(hdr.version)) {
                return std::unexpected(LogError::UnsupportedVersion);
            }

            if (hdr.segment_id != expected_segment_id || hdr.begin_lsn != expected_begin_lsn) {
                return std::unexpected(LogError::LsnMismatch);
            }

            if (const auto computed = calculate_segment_header_crc<Framing>(hdr); computed != hdr.header_crc) {
                return std::unexpected(LogError::ChecksumMismatch);
            }

            return {};
        }

        Result<std::shared_ptr<segment_file>> get_or_create_segment(const std::uint64_t seg_id, const lsn_t begin_lsn) {
            std::lock_guard lk{mutex_};
            for (auto& s : active_segments_) {
                if (s->segment_id == seg_id) return s;
            }

            const auto path = opts_.wal_dir / format_segment_name(seg_id);
            const bool is_new = !std::filesystem::exists(path);
            auto map_res = setu::mapping<setu::read_write>::open_or_create(path, opts_.segment_size);
            if (!map_res) {
                return std::unexpected(LogError::SegmentOpenFailed);
            }

            auto seg = std::make_shared<segment_file>(segment_file{
                .segment_id = seg_id,
                .begin_lsn = begin_lsn,
                .capacity = opts_.segment_size,
                .path = path,
                .map = std::move(*map_res)
            });

            if (is_new) {
                // Initialize durable segment header at offset 0
                auto bytes = seg->map.as_bytes();
                if (bytes.size() >= sizeof(segment_header)) {
                    segment_header shdr;
                    shdr.magic = k_nitya_seg_magic;
                    shdr.version = k_nitya_format_version;
                    shdr.flags = 0;
                    shdr.segment_id = seg_id;
                    shdr.begin_lsn = begin_lsn;
                    shdr.sealed_lsn = 0;
                    shdr.created_at_unix_ns = Clock::now_unix_ns();
                    shdr.header_crc = calculate_segment_header_crc<Framing>(shdr);

                    std::memcpy(bytes.data(), &shdr, sizeof(segment_header));
                    if (const auto hdr_flush = seg->map.flush_range(0, sizeof(segment_header), setu::flush_mode::sync);
                        !hdr_flush) {
                        return std::unexpected(LogError::FlushFailed);
                    }
                }
            }

            // Validate segment header for both new and existing segments
            if (auto val_res = validate_segment_header(*seg, seg_id, begin_lsn); !val_res) {
                return std::unexpected(val_res.error());
            }

            if (active_segments_.size() >= opts_.max_cached_segments) {
                active_segments_.erase(active_segments_.begin());
            }
            active_segments_.push_back(seg);
            return seg;
        }

        Result<void> seal_segment(const std::uint64_t seg_id, const lsn_t sealed_lsn) {
            std::lock_guard lk{mutex_};

            auto seal_mapped_header = [&](auto& mapping) -> Result<void> {
                auto bytes = mapping.as_bytes();
                if (bytes.size() < sizeof(segment_header)) {
                    return std::unexpected(LogError::CorruptedHeader);
                }

                segment_header hdr;
                std::memcpy(&hdr, bytes.data(), sizeof(hdr));
                hdr.sealed_lsn = sealed_lsn;
                hdr.flags |= k_segment_sealed;
                hdr.header_crc = calculate_segment_header_crc<Framing>(hdr);
                std::memcpy(bytes.data(), &hdr, sizeof(hdr));

                auto flush_res = mapping.flush_range(0, sizeof(segment_header), setu::flush_mode::sync);
                if (!flush_res) {
                    return std::unexpected(LogError::FlushFailed);
                }
                return {};
            };

            for (const auto& s : active_segments_) {
                if (s->segment_id == seg_id) {
                    return seal_mapped_header(s->map);
                }
            }

            const auto path = opts_.wal_dir / format_segment_name(seg_id);
            if (!std::filesystem::exists(path)) {
                return std::unexpected(LogError::SegmentOpenFailed);
            }

            auto map_res = setu::mapping<setu::read_write>::open_or_create(path, opts_.segment_size);
            if (!map_res) {
                return std::unexpected(LogError::SegmentOpenFailed);
            }

            return seal_mapped_header(*map_res);
        }

        Result<void> mark_archived(const std::uint64_t seg_id) {
            std::lock_guard lk{mutex_};
            const auto path = opts_.wal_dir / format_segment_name(seg_id);
            if (!std::filesystem::exists(path)) return std::unexpected(LogError::InvalidArg);

            for (const auto& s : active_segments_) {
                if (s->segment_id == seg_id) {
                    auto bytes = s->map.as_bytes();
                    if (bytes.size() >= sizeof(segment_header)) {
                        segment_header hdr;
                        std::memcpy(&hdr, bytes.data(), sizeof(hdr));
                        hdr.flags |= k_segment_archived;
                        hdr.header_crc = calculate_segment_header_crc<Framing>(hdr);
                        std::memcpy(bytes.data(), &hdr, sizeof(hdr));
                        if (const auto flush_res = s->map.flush_range(0, sizeof(segment_header), setu::flush_mode::sync)
                            ; !flush_res)
                            return std::unexpected(LogError::FlushFailed);
                    }
                    return {};
                }
            }

            auto map_res = setu::mapping<setu::read_write>::open_or_create(path, opts_.segment_size);
            if (!map_res) return std::unexpected(LogError::SegmentOpenFailed);
            auto bytes = map_res->as_bytes();
            if (bytes.size() >= sizeof(segment_header)) {
                segment_header hdr;
                std::memcpy(&hdr, bytes.data(), sizeof(hdr));
                hdr.flags |= k_segment_archived;
                hdr.header_crc = calculate_segment_header_crc<Framing>(hdr);
                std::memcpy(bytes.data(), &hdr, sizeof(hdr));
                if (const auto flush_res = map_res->flush_range(0, sizeof(segment_header), setu::flush_mode::sync); !
                    flush_res)
                    return std::unexpected(LogError::FlushFailed);
            }
            return {};
        }

        Result<void> flush_range(const std::uint64_t seg_id, const std::size_t offset, const std::size_t length,
                                 const setu::flush_mode mode) {
            std::lock_guard lk{mutex_};
            for (const auto& s : active_segments_) {
                if (s->segment_id == seg_id) {
                    if (const auto res = s->map.flush_range(offset, length, mode); !res)
                        return std::unexpected(
                            LogError::FlushFailed);
                    return {};
                }
            }

            const auto path = opts_.wal_dir / format_segment_name(seg_id);
            if (!std::filesystem::exists(path)) {
                return std::unexpected(LogError::SegmentOpenFailed);
            }

            const auto map_res = setu::mapping<setu::read_write>::open_or_create(path, opts_.segment_size);
            if (!map_res) {
                return std::unexpected(LogError::SegmentOpenFailed);
            }

            if (const auto res = map_res->flush_range(offset, length, mode); !res) {
                return std::unexpected(LogError::FlushFailed);
            }
            return {};
        }

        [[nodiscard]] const wal_options& options() const noexcept { return opts_; }

    private:
        wal_options opts_;
        std::mutex mutex_;
        std::vector<std::shared_ptr<segment_file>> active_segments_;
    };

    // Public storage alias bound to the default framing policy. Byte-identical to the
    // pre-templatized `setu_storage`; keeps `nitya::setu_storage` usable as a type and
    // for the static `format_segment_name` call.
    using setu_storage = setu_storage_t<default_framing>;

    // ============================================================================
    // § 6  Memory Policy (Smriti-backed Arena)
    // ============================================================================

    class smriti_memory {
    public:
        explicit smriti_memory(const std::size_t arena_size = 1024 * 1024)
            : arena_{arena_size} {}

        [[nodiscard]] void* allocate(const std::size_t n, const std::size_t a = alignof(std::max_align_t)) noexcept {
            return arena_.allocate(n, a);
        }

        void reset() noexcept {
            arena_.reset();
        }

    private:
        smriti::pools::LinearArena arena_;
    };

    // ============================================================================
    // § 7  Durability Policy
    // ============================================================================

    struct sync_durability {
        static constexpr auto flush_type = setu::flush_mode::sync;
        static constexpr bool is_synchronous = true;
    };

    struct async_durability {
        static constexpr auto flush_type = setu::flush_mode::async;
        static constexpr bool is_synchronous = false;
    };

    // ============================================================================
    // § 8  Telemetry Policy (NADI-backed compile-time scopes)
    // ============================================================================

    struct nadi_telemetry {
        using Sink = utils::nadi::NoSink; // Zero-overhead no-op by default or custom sink

        template <utils::nadi::FixedString Category>
        struct Scope {
            utils::nadi::PulseScope<Sink, Category> scope;
            explicit Scope() noexcept : scope{} {}
        };

        static auto trace_reserve() { return Scope<"wal_reserve">{}; }
        static auto trace_publish() { return Scope<"wal_publish">{}; }
        static auto trace_flush() { return Scope<"wal_flush">{}; }
        static auto trace_recovery() { return Scope<"recovery_scan">{}; }
        static auto trace_replication() { return Scope<"replication_send">{}; }
    };

    // ============================================================================
    // § 9  Concurrency Policy (durability coordination)
    // ============================================================================
    //
    // The durability path needs a coordinator. Two are shipped:
    //
    //  * flush_gate_concurrency (DEFAULT) — the honest, first-waiter-flushes-all
    //    surface. `wait_durable` serializes on the wal's own `flush_mutex_`, the
    //    first waiter flushes the current published watermark in one batched
    //    msync, and every concurrent waiter it covers returns after observing
    //    `flushed_lsn`. No ticket queue exists, so callers pay for nothing beyond
    //    a mutex. `uses_ticket_queue == false` selects this path via `if constexpr`.
    //
    //  * group_commit_concurrency<Cap> (OPT-IN) — a genuine leader/follower
    //    batched group commit for high-fan-in `append_sync` workloads. Waiters
    //    enqueue a ticket; the leader drains all waiting tickets, flushes the
    //    covering range once, and marks each completion. `uses_ticket_queue == true`
    //    routes `wait_durable` through `execute_leader_flush`.
    //
    // Both keep the append fast path lock-free with respect to the flush mutex.

    // Default coordinator: honest flush gate. Stateless — the wal owns the mutex
    // and watermark; this policy only advertises which durability path to run.
    class flush_gate_concurrency {
    public:
        static constexpr bool uses_ticket_queue = false;
        flush_gate_concurrency() = default;
    };

    template <std::size_t QueueCapacity = 1024>
    class group_commit_concurrency {
    public:
        static constexpr bool uses_ticket_queue = true;

        // A ticket can outlive the caller that queued it: the durability
        // watermark may satisfy that caller before a later leader drains the
        // queue.  Keep its completion state owned by both the caller and the
        // queue so a deferred drain never dereferences a departed stack frame.
        struct commit_completion {
            std::atomic<bool> done{false};
            std::atomic<LogError> result{LogError::Success};
        };

        struct commit_ticket {
            lsn_t lsn{0};
            std::size_t total_bytes{0};
            std::shared_ptr<commit_completion> completion{};
        };

        group_commit_concurrency() = default;

        bool enqueue_commit(commit_ticket t) {
            std::lock_guard lock{mutex_};
            if (queue_.size() == QueueCapacity) return false;
            queue_.push_back(std::move(t));
            return true;
        }

        std::optional<commit_ticket> dequeue_commit() {
            std::lock_guard lock{mutex_};
            if (queue_.empty()) return std::nullopt;
            auto ticket = std::move(queue_.front());
            queue_.pop_front();
            return ticket;
        }

        static constexpr std::size_t capacity() noexcept { return QueueCapacity; }

    private:
        std::mutex mutex_;
        std::deque<commit_ticket> queue_;
    };

    // ============================================================================
    // § 9c Publish-Tracker Policy (out-of-order publish interval bookkeeping)
    // ============================================================================
    //
    // Writers reserve contiguous byte ranges but may publish them out of order.
    // The tracker records not-yet-contiguous published intervals and advances the
    // contiguous `published_lsn` watermark as gaps fill. The default keeps the
    // fixed-capacity `static_vector` (zero heap, common case is near-contiguous
    // with a handful of gaps) and adds an O(1) append-tail fast path for the
    // dominant "extends the last interval" case. A high-gap workload can supply
    // an interval-tree policy without touching the WAL.
    //
    // Contract (all methods called under the wal's publish-tracker mutex):
    //   bool insert(from, to)  — record an interval; false if capacity exhausted.
    //   void drain(lsn_t& cur) — advance cur over intervals it now covers.
    template <std::size_t Capacity>
    class static_vector_publish_tracker {
    public:
        static constexpr std::size_t capacity() noexcept { return Capacity; }

        // Advance `cur` over every interval whose start is already covered,
        // compacting the survivors to the front. O(k) in drained intervals.
        void drain(lsn_t& cur) noexcept {
            std::size_t drained = 0;
            while (drained < intervals_.size() && intervals_[drained].first <= cur) {
                cur = std::max(cur, intervals_[drained].second);
                ++drained;
            }
            if (drained > 0) {
                const std::size_t remaining = intervals_.size() - drained;
                for (std::size_t i = 0; i < remaining; ++i) {
                    intervals_[i] = std::move(intervals_[drained + i]);
                }
                while (intervals_.size() > remaining) {
                    intervals_.pop_back();
                }
            }
        }

        // Record an out-of-contiguous interval [from, to). Returns false only when
        // the fixed capacity is exhausted and the interval cannot be merged.
        [[nodiscard]] bool insert(lsn_t from, lsn_t to) noexcept {
            // O(1) append-tail fast path: the common case is an interval that
            // extends (or abuts) the current last interval — no shift, no scan.
            if (!intervals_.empty()) {
                if (auto& last = intervals_.back(); from <= last.second && to >= last.first) {
                    last.first = std::min(last.first, from);
                    last.second = std::max(last.second, to);
                    return true;
                }
                if (from > intervals_.back().second) {
                    // Strictly after the tail: append without shifting.
                    return intervals_.push_back({from, to});
                }
            }
            else {
                return intervals_.push_back({from, to});
            }

            // General path: merge into an overlapping interval or sorted-insert.
            for (std::size_t i = 0; i < intervals_.size(); ++i) {
                if (auto& [fst, snd] = intervals_[i]; from <= snd && to >= fst) {
                    fst = std::min(fst, from);
                    snd = std::max(snd, to);
                    while (i + 1 < intervals_.size() && snd >= intervals_[i + 1].first) {
                        snd = std::max(snd, intervals_[i + 1].second);
                        for (std::size_t j = i + 1; j + 1 < intervals_.size(); ++j) {
                            intervals_[j] = std::move(intervals_[j + 1]);
                        }
                        intervals_.pop_back();
                    }
                    return true;
                }
            }

            if (intervals_.size() >= intervals_.capacity()) {
                return false;
            }

            std::size_t idx = 0;
            while (idx < intervals_.size() && intervals_[idx].first < from) {
                ++idx;
            }
            (void)intervals_.push_back({from, to});
            for (std::size_t i = intervals_.size() - 1; i > idx; --i) {
                intervals_[i] = std::move(intervals_[i - 1]);
            }
            intervals_[idx] = {from, to};
            return true;
        }

    private:
        containers::static_vector<std::pair<lsn_t, lsn_t>, Capacity> intervals_;
    };

    // ============================================================================
    // § 10 Concepts
    // ============================================================================

    template <typename F>
    concept FramingPolicyLike = requires(
        reservation& res,
        const frame_header& hdr,
        const frame_trailer& trl,
        std::span<const std::byte> payload,
        const std::byte* data,
        std::size_t len,
        lsn_t lsn,
        std::uint32_t checksum
    ) {
            { F::calculate_checksum32(data, len) } -> std::same_as<std::uint32_t>;
            { F::encode(res, checksum) } noexcept;
            { F::validate_header(hdr, lsn) } -> std::same_as<Result<std::uint32_t>>;
            { F::validate_payload_and_trailer(payload, trl, checksum) } -> std::same_as<Result<void>>;
            // Physical-format ownership (Item 5): version + structural sizes.
            { F::format_version } -> std::convertible_to<std::uint16_t>;
            { F::supports_version(std::uint16_t{}) } -> std::same_as<bool>;
        };

    template <typename S>
    concept StoragePolicyLike = requires(S& s, std::uint64_t seg_id, lsn_t lsn, std::size_t off, std::size_t len,
                                         setu::flush_mode mode) {
        { s.get_or_create_segment(seg_id, lsn) };
        { s.flush_range(seg_id, off, len, mode) } -> std::same_as<Result<void>>;
        { s.format_segment_name(seg_id) } -> std::same_as<std::string>;
        { s.options() } -> std::convertible_to<const wal_options&>;
    };

    template <typename M>
    concept MemoryPolicyLike = requires(M& m, std::size_t n, std::size_t a) {
        { m.allocate(n, a) } -> std::same_as<void*>;
        { m.reset() } noexcept;
    };

    // A concurrency policy advertises which durability path it drives. A ticket
    // queue is required only when it opts into batched group commit; the honest
    // default (flush_gate_concurrency) supplies only the tag.
    template <typename C>
    concept TicketQueueConcurrency = requires(C& c, typename C::commit_ticket ticket) {
        typename C::commit_completion;
        { c.enqueue_commit(ticket) } -> std::same_as<bool>;
        { c.dequeue_commit() } -> std::same_as<std::optional<typename C::commit_ticket>>;
        { C::capacity() } -> std::convertible_to<std::size_t>;
    };

    template <typename C>
    concept ConcurrencyPolicyLike = requires {
        { C::uses_ticket_queue } -> std::convertible_to<bool>;
    } && (!C::uses_ticket_queue || TicketQueueConcurrency<C>);

    template <typename P>
    concept PublishTrackerPolicyLike = requires(P& p, lsn_t v, lsn_t from, lsn_t to) {
        { p.insert(from, to) } -> std::same_as<bool>;
        { p.drain(v) } noexcept;
    };

    template <typename D>
    concept DurabilityPolicyLike = requires {
        { D::flush_type } -> std::same_as<const setu::flush_mode&>;
        { D::is_synchronous } -> std::same_as<const bool&>;
    };

    template <typename T>
    concept TelemetryPolicyLike = requires {
        { T::trace_reserve() };
        { T::trace_publish() };
        { T::trace_flush() };
        { T::trace_recovery() };
        { T::trace_replication() };
    };

    template <typename C>
    concept ClockPolicyLike = requires {
        { C::now_unix_ns() } -> std::same_as<std::uint64_t>;
    };

    // ============================================================================
    // § 11 Wal Engine: nitya::wal
    // ============================================================================

    template <
        StoragePolicyLike StoragePolicy = setu_storage,
        MemoryPolicyLike MemoryPolicy = smriti_memory,
        ConcurrencyPolicyLike ConcurrencyPolicy = flush_gate_concurrency,
        FramingPolicyLike FramingPolicy = default_framing,
        DurabilityPolicyLike DurabilityPolicy = sync_durability,
        TelemetryPolicyLike TelemetryPolicy = nadi_telemetry,
        ClockPolicyLike ClockPolicy = system_clock_source,
        std::size_t PublishTrackerCapacity = 1024,
        PublishTrackerPolicyLike PublishTrackerPolicy =
        static_vector_publish_tracker<PublishTrackerCapacity>>
        requires (PublishTrackerCapacity > 0)
    class wal {
    public:
        explicit wal(wal_options opts = {})
            : opts_{std::move(opts)}
              , storage_{opts_}
              , memory_{1024 * 1024}
              , concurrency_{}
              , tail_lsn_{k_segment_header_size}
              , published_lsn_{k_segment_header_size}
              , flushed_lsn_{k_segment_header_size}
              , replicated_lsn_{k_segment_header_size}
              , stop_background_flusher_{false} {
            init_tail_from_disk();

            if (opts_.background_flush) {
                background_thread_ = std::jthread([this](const std::stop_token& st) {
                    flusher_worker(st);
                });
            }
        }

        ~wal() {
            stop_flusher();
            (void)sync();
        }

        wal(const wal&) = delete;
        wal& operator=(const wal&) = delete;
        wal(wal&&) = delete;
        wal& operator=(wal&&) = delete;

        // ------------------------------------------------------------------------
        // 1. Reserve Phase
        // ------------------------------------------------------------------------
        [[nodiscard]] Result<reservation> reserve(const std::uint32_t payload_bytes, const std::uint16_t flags = 0,
                                                  const std::uint16_t version = k_nitya_format_version) {
            auto telemetry = TelemetryPolicy::trace_reserve();
            (void)telemetry;

            const std::size_t total_frame_size = k_frame_overhead + payload_bytes;
            if (total_frame_size + k_segment_header_size > opts_.segment_size) {
                return std::unexpected(LogError::InvalidArg);
            }

            std::unique_lock lk{reservation_mutex_};

            lsn_t current_lsn = tail_lsn_.load(std::memory_order_relaxed);
            const std::uint64_t seg_id = current_lsn / opts_.segment_size;
            std::size_t seg_offset = current_lsn % opts_.segment_size;

            if (seg_offset < k_segment_header_size) {
                current_lsn = seg_id * opts_.segment_size + k_segment_header_size;
                seg_offset = k_segment_header_size;
                tail_lsn_.store(current_lsn, std::memory_order_relaxed);
            }

            // Check if record crosses segment boundary -> align to next segment if auto_rotate
            if (seg_offset + total_frame_size > opts_.segment_size) {
                if (!opts_.auto_rotate) {
                    return std::unexpected(LogError::SegmentFull);
                }
                const lsn_t next_seg_lsn = (seg_id + 1) * opts_.segment_size + k_segment_header_size;

                // Zero-fill trailing gap bytes so recovery sees clean EndOfLog padding
                // instead of stale mapped-file data. Must happen before tail_lsn_ advances.
                {
                    auto seg_res = storage_.get_or_create_segment(seg_id, seg_id * opts_.segment_size);
                    if (seg_res) {
                        auto seg_bytes = (*seg_res)->map.as_bytes();
                        const std::size_t gap_start = current_lsn % opts_.segment_size;
                        if (gap_start < seg_bytes.size()) {
                            std::fill(seg_bytes.data() + gap_start, seg_bytes.data() + seg_bytes.size(), std::byte{0});
                        }
                    }
                }

                tail_lsn_.store(next_seg_lsn, std::memory_order_release);

                // Seal the completed segment on disk and ensure metadata flush succeeds
                auto seal_res = storage_.seal_segment(seg_id, current_lsn);
                if (!seal_res) return std::unexpected(seal_res.error());

                // Mark the padding gap from current_lsn to next_seg_lsn as published
                // so the contiguous published watermark is never stalled by rotation.
                if (auto gap_res = mark_gap_published(current_lsn, next_seg_lsn); !gap_res)
                    return std::unexpected(
                        gap_res.error());

                return reserve_in_segment_locked(next_seg_lsn, payload_bytes, total_frame_size, flags, version);
            }

            return reserve_in_segment_locked(current_lsn, payload_bytes, total_frame_size, flags, version);
        }

        // ------------------------------------------------------------------------
        // 2. Publish Phase
        // ------------------------------------------------------------------------
        Result<void> publish(reservation& res) {
            auto telemetry = TelemetryPolicy::trace_publish();
            (void)telemetry;

            if (!res.is_valid()) return std::unexpected(LogError::InvalidArg);

            auto payload = res.payload_buffer();
            std::uint32_t payload_crc = FramingPolicy::calculate_checksum32(payload.data(), payload.size());

            // Write Header and Trailer into mapped buffer
            FramingPolicy::encode(res, payload_crc);

            const lsn_t record_end = res.lsn + res.buffer.size();

            if (auto mark_res = mark_published_range(res.lsn, record_end); !mark_res)
                return std::unexpected(
                    mark_res.error());

            records_published_.fetch_add(1, std::memory_order_relaxed);
            bytes_published_.fetch_add(res.payload_size, std::memory_order_relaxed);

            if (opts_.background_flush) {
                flusher_cv_.notify_one();
            }

            if (opts_.sync_on_publish) {
                return sync();
            }

            return {};
        }

        // ------------------------------------------------------------------------
        // 3. Append Convenience (Reserve + Copy + Publish)
        // ------------------------------------------------------------------------
        Result<lsn_t> append(const std::span<const std::byte> payload, const std::uint16_t flags = 0,
                             const std::uint16_t version = k_nitya_format_version) {
            auto res_result = reserve(static_cast<std::uint32_t>(payload.size()), flags, version);
            if (!res_result) return std::unexpected(res_result.error());

            auto res = *res_result;
            auto target = res.payload_buffer();
            if (!payload.empty()) {
                std::memcpy(target.data(), payload.data(), payload.size());
            }

            if (auto pub_res = publish(res); !pub_res) return std::unexpected(pub_res.error());

            return res.lsn;
        }

        Result<lsn_t> append_sync(const std::span<const std::byte> payload, const std::uint16_t flags = 0,
                                  const std::uint16_t version = k_nitya_format_version) {
            auto append_res = append(payload, flags, version);
            if (!append_res) return std::unexpected(append_res.error());

            const lsn_t record_end = *append_res + k_frame_overhead + payload.size();

            // Wait until contiguous published watermark reaches our record_end before requesting durability
            lsn_t pub = published_lsn_.load(std::memory_order_acquire);
            while (pub < record_end) {
                published_lsn_.wait(pub, std::memory_order_acquire);
                pub = published_lsn_.load(std::memory_order_acquire);
            }

            if (auto sync_res = wait_durable(record_end); !sync_res) return std::unexpected(sync_res.error());

            return *append_res;
        }

        // ------------------------------------------------------------------------
        // 4. Physical Flushing & Group Commit Durability
        // ------------------------------------------------------------------------
        Result<void> flush_to(const lsn_t target_lsn) {
            if (const lsn_t published = published_lsn_.load(std::memory_order_acquire); target_lsn > published) {
                return std::unexpected(LogError::InvalidArg);
            }

            std::unique_lock lk{flush_mutex_};
            const lsn_t current_flushed = flushed_lsn_.load(std::memory_order_relaxed);
            if (target_lsn <= current_flushed) {
                return {};
            }

            if (auto flush_res = flush_range_to_locked(current_flushed, target_lsn); !flush_res) {
                record_durability_error(flush_res.error(), target_lsn);
                return flush_res;
            }

            flushed_lsn_.store(target_lsn, std::memory_order_release);
            flushed_lsn_.notify_all();
            flush_operations_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }

        Result<void> wait_durable(lsn_t target_lsn) {
            if (const auto error = last_durability_error_.load(std::memory_order_acquire);
                error != LogError::Success) {
                return std::unexpected(error);
            }
            if (const lsn_t published = published_lsn_.load(std::memory_order_acquire); target_lsn > published) {
                return std::unexpected(LogError::InvalidArg);
            }

            // Fast path: already durable.
            if (flushed_lsn_.load(std::memory_order_acquire) >= target_lsn) {
                return {};
            }

            auto telemetry = TelemetryPolicy::trace_flush();
            (void)telemetry;
            group_commit_waiters_.fetch_add(1, std::memory_order_relaxed);

            if constexpr (ConcurrencyPolicy::uses_ticket_queue) {
                // Opt-in batched group commit: enqueue a ticket, then either become
                // the leader (drain all waiting tickets, flush the covering range
                // once) or wait for a leader to satisfy this ticket's watermark.
                return wait_durable_via_ticket_queue(target_lsn);
            }
            else {
                // Default honest path: durability is inherently serialized by the
                // storage device.  The first waiter owns one bounded critical
                // section and flushes the current published watermark, completing
                // every concurrent waiter already covered by it without lock-free
                // ticket hand-off/liveness hazards. The append path remains
                // lock-free with respect to this mutex.
                std::unique_lock lock{flush_mutex_};
                if (flushed_lsn_.load(std::memory_order_relaxed) >= target_lsn) return {};

                const lsn_t batch_end = published_lsn_.load(std::memory_order_acquire);
                if (batch_end < target_lsn) return std::unexpected(LogError::InvalidArg);
                const lsn_t current = flushed_lsn_.load(std::memory_order_relaxed);
                if (auto flushed = flush_range_to_locked(current, batch_end); !flushed) {
                    record_durability_error(flushed.error(), batch_end);
                    return flushed;
                }
                flushed_lsn_.store(batch_end, std::memory_order_release);
                flushed_lsn_.notify_all();
                flush_operations_.fetch_add(1, std::memory_order_relaxed);
                return {};
            }
        }

        Result<void> sync() {
            if (const auto error = last_durability_error_.load(std::memory_order_acquire);
                error != LogError::Success) {
                return std::unexpected(error);
            }
            const lsn_t pub = published_lsn_.load(std::memory_order_acquire);
            // Fast path: nothing to flush.
            if (flushed_lsn_.load(std::memory_order_acquire) >= pub) return {};
            return wait_durable(pub);
        }

        // ------------------------------------------------------------------------
        // 5. Streaming Recovery Engine
        // ------------------------------------------------------------------------
        class recovery_stream {
        public:
            using iterator_concept = std::input_iterator_tag;
            using iterator_category = std::input_iterator_tag;
            using value_type = wal_record;
            using difference_type = std::ptrdiff_t;

            recovery_stream(wal& parent, lsn_t start_lsn, const recovery_mode mode)
                : parent_{parent}, cursor_lsn_{start_lsn}, mode_{mode}, status_{} {
                status_.last_valid_lsn = k_invalid_lsn;
                status_.first_bad_lsn = k_invalid_lsn;
                status_.error = LogError::Success;
                status_.records_recovered = 0;
                status_.bytes_recovered = 0;
                status_.segment_id = start_lsn / parent.options().segment_size;
            }

            struct recovery_iterator {
                using iterator_concept = std::input_iterator_tag;
                using iterator_category = std::input_iterator_tag;
                using value_type = wal_record;
                using difference_type = std::ptrdiff_t;
                using reference = const wal_record&;
                using pointer = const wal_record*;

                recovery_stream* stream{nullptr};
                std::optional<wal_record> current{};
                lsn_t record_lsn{k_invalid_lsn};

                recovery_iterator() = default;

                explicit recovery_iterator(recovery_stream* s) : stream{s} {
                    advance();
                }

                reference operator*() const noexcept { return *current; }
                pointer operator->() const noexcept { return &(*current); }

                recovery_iterator& operator++() {
                    advance();
                    return *this;
                }

                void operator++(int) {
                    ++(*this);
                }

                bool operator==(const recovery_iterator& other) const noexcept {
                    if (!current.has_value() && !other.current.has_value()) return true;
                    if (current.has_value() != other.current.has_value()) return false;
                    return record_lsn == other.record_lsn;
                }

                bool operator!=(const recovery_iterator& other) const noexcept {
                    return !(*this == other);
                }

            private:
                void advance() {
                    if (!stream) {
                        current = std::nullopt;
                        record_lsn = k_invalid_lsn;
                        return;
                    }
                    current = stream->next_record();
                    record_lsn = current ? current->lsn : k_invalid_lsn;
                }
            };

            [[nodiscard]] recovery_iterator begin() { return recovery_iterator{this}; }
            [[nodiscard]] recovery_iterator end() { return recovery_iterator{}; }

            std::optional<wal_record> next_record() {
                auto rec = parent_.read_record_at(cursor_lsn_, &status_, mode_, /*stage_scratch=*/true);
                return rec;
            }

            [[nodiscard]] const recovery_status& status() const noexcept {
                return status_;
            }

        private:
            wal& parent_;
            lsn_t cursor_lsn_;
            recovery_mode mode_;
            recovery_status status_;
        };

        [[nodiscard]] recovery_stream recover(lsn_t start_lsn = 0,
                                              recovery_mode mode = recovery_mode::stop_at_first_error) {
            auto telemetry = TelemetryPolicy::trace_recovery();
            (void)telemetry;
            return recovery_stream{*this, start_lsn, mode};
        }

        // ------------------------------------------------------------------------
        // 6. Replication Stream Subscription
        // ------------------------------------------------------------------------
        class replication_stream {
        public:
            replication_stream(wal& parent, std::string replica_id, const lsn_t start_lsn)
                : parent_{parent}, replica_id_{std::move(replica_id)}, cursor_lsn_{start_lsn} {}

            std::optional<wal_record> next() {
                recovery_status st;
                auto rec = parent_.read_record_at(cursor_lsn_, &st, recovery_mode::stop_at_first_error,
                                                  /*stage_scratch=*/true);
                if (rec) {
                    cursor_lsn_ = rec->lsn + k_frame_overhead + rec->payload.size();
                    parent_.replication_records_.fetch_add(1, std::memory_order_relaxed);
                    parent_.replication_bytes_.fetch_add(rec->payload.size(), std::memory_order_relaxed);
                }
                return rec;
            }

            [[nodiscard]] lsn_t next_lsn() const noexcept { return cursor_lsn_; }

            [[nodiscard]] replication_checkpoint checkpoint() const {
                return {.replica_id = replica_id_, .acknowledged_lsn = acknowledged_lsn_};
            }

            Result<void> acknowledge(const lsn_t durable_lsn) noexcept {
                if (durable_lsn > cursor_lsn_) return std::unexpected(LogError::InvalidArg);
                if (durable_lsn < acknowledged_lsn_) return std::unexpected(LogError::InvalidArg);
                acknowledged_lsn_ = durable_lsn;
                parent_.set_replicated_lsn(durable_lsn);
                return {};
            }

        private:
            wal& parent_;
            std::string replica_id_;
            lsn_t cursor_lsn_;
            lsn_t acknowledged_lsn_{k_segment_header_size};
        };

        [[nodiscard]] replication_stream subscribe(std::string replica_id = {}, lsn_t from_lsn = 0) {
            auto telemetry = TelemetryPolicy::trace_replication();
            (void)telemetry;
            return replication_stream{*this, std::move(replica_id), from_lsn};
        }

        [[nodiscard]] replication_stream subscribe(const lsn_t from_lsn) {
            return subscribe({}, from_lsn);
        }

        // ------------------------------------------------------------------------
        // ------------------------------------------------------------------------
        // 7. Retention & Archival Automation (EasyRules & Pravaha integration)
        // ------------------------------------------------------------------------
        void apply_retention_rules(
            const std::chrono::seconds max_segment_age,
            const std::function<void(const segment_descriptor&)>& on_archive = nullptr,
            const std::function<void(const segment_descriptor&)>& on_delete = nullptr) {
            easy_rules::EasyRuleEngine engine;
            engine.config.verbose = false;

            engine.when("SegmentRetention", [](const easy_rules::Facts& facts) {
                      const auto age_sec = facts.get<int>("age_seconds").value_or(0);
                      const auto max_age = facts.get<int>("max_age_seconds").value_or(0);
                      const auto replicated = facts.get<bool>("is_replicated").value_or(false);
                      return replicated && (age_sec >= max_age);
                  })
                  .then([&](const easy_rules::ExecutionContext& ctx) {
                      if (on_delete) {
                          segment_descriptor desc;
                          desc.segment_id = static_cast<std::uint64_t>(ctx.facts.get<int>("segment_id").value_or(0));
                          on_delete(desc);
                      }
                  })
                  .with_description("Delete segments past age threshold and fully replicated");

            engine.when("SegmentArchival", [](const easy_rules::Facts& facts) {
                      const auto replicated = facts.get<bool>("is_replicated").value_or(false);
                      const auto archived = facts.get<bool>("is_archived").value_or(false);
                      return replicated && !archived;
                  })
                  .then([&](const easy_rules::ExecutionContext& ctx) {
                      if (on_archive) {
                          segment_descriptor desc;
                          desc.segment_id = static_cast<std::uint64_t>(ctx.facts.get<int>("segment_id").value_or(0));
                          on_archive(desc);
                      }
                  })
                  .with_description("Archive segments that are replicated");

            // Evaluate segments. "Now" comes from the injected ClockPolicy so
            // retention can be driven by a deterministic/simulated clock in tests.
            const auto now_ns = ClockPolicy::now_unix_ns();
            for (auto segments = list_segments(); const auto& seg : segments) {
                easy_rules::ExecutionContext ctx;
                const auto created_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        seg.created_at.time_since_epoch()).count());
                const auto age = (now_ns > created_ns)
                                     ? static_cast<std::int64_t>((now_ns - created_ns) / 1'000'000'000ULL)
                                     : std::int64_t{0};
                ctx.facts.set("segment_id", static_cast<int>(seg.segment_id));
                ctx.facts.set("age_seconds", static_cast<int>(age));
                ctx.facts.set("max_age_seconds", static_cast<int>(max_segment_age.count()));
                ctx.facts.set("is_replicated", seg.end_lsn <= replicated_lsn_.load(std::memory_order_relaxed));
                ctx.facts.set("is_archived", seg.is_archived);

                engine.run(ctx);
            }
        }

        // ------------------------------------------------------------------------
        // 8. Watermarks & Metadata
        // ------------------------------------------------------------------------
        [[nodiscard]] lsn_t tail_lsn() const noexcept { return tail_lsn_.load(std::memory_order_relaxed); }
        [[nodiscard]] lsn_t flushed_lsn() const noexcept { return flushed_lsn_.load(std::memory_order_relaxed); }
        [[nodiscard]] lsn_t replicated_lsn() const noexcept { return replicated_lsn_.load(std::memory_order_relaxed); }
        [[nodiscard]] const wal_options& options() const noexcept { return opts_; }

        [[nodiscard]] durability_health health() const noexcept {
            return {
                .last_error = last_durability_error_.load(std::memory_order_acquire),
                .last_error_lsn = last_durability_error_lsn_.load(std::memory_order_acquire)
            };
        }

        [[nodiscard]] wal_metrics metrics() const noexcept {
            return {
                .records_published = records_published_.load(std::memory_order_relaxed),
                .bytes_published = bytes_published_.load(std::memory_order_relaxed),
                .flush_operations = flush_operations_.load(std::memory_order_relaxed),
                .flush_failures = flush_failures_.load(std::memory_order_relaxed),
                .group_commit_waiters = group_commit_waiters_.load(std::memory_order_relaxed),
                .replication_records = replication_records_.load(std::memory_order_relaxed),
                .replication_bytes = replication_bytes_.load(std::memory_order_relaxed)
            };
        }

        void set_replicated_lsn(const lsn_t lsn) noexcept {
            atomic_max_lsn(replicated_lsn_, lsn);
        }

        [[nodiscard]] lsn_t published_lsn() const noexcept { return published_lsn_.load(std::memory_order_relaxed); }

        Result<void> mark_segment_archived(std::uint64_t seg_id) {
            return storage_.mark_archived(seg_id);
        }

        // Fixed-capacity admin listing; avoids dynamic segment list allocation.
        template <typename OutputIt>
        Result<void> list_segments(OutputIt out) const {
            containers::static_vector<segment_descriptor, 1024> list;

            for (std::error_code ec; const auto& entry : std::filesystem::directory_iterator(opts_.wal_dir, ec)) {
                if (entry.is_regular_file() && entry.path().extension() == ".log") {
                    std::string stem = entry.path().stem().string();
                    try {
                        const std::uint64_t seg_id = std::stoull(stem);
                        const lsn_t seg_begin_lsn = seg_id * opts_.segment_size;

                        segment_descriptor desc{
                            .segment_id = seg_id,
                            .begin_lsn = seg_begin_lsn,
                            .end_lsn = (seg_id + 1) * opts_.segment_size,
                            .path = entry.path(),
                            .is_archived = false,
                            .is_replicated = (seg_id + 1) * opts_.segment_size <= replicated_lsn_.load(
                                std::memory_order_relaxed),
                            .created_at = std::chrono::system_clock::now()
                        };

                        // Read and validate durable segment header to populate accurate metadata
                        if (const auto map_res = setu::mapping<setu::read_only>::open_existing(entry.path())) {
                            auto bytes = map_res->as_bytes();
                            if (bytes.size() >= sizeof(segment_header)) {
                                segment_header hdr;
                                std::memcpy(&hdr, bytes.data(), sizeof(hdr));
                                if (hdr.magic == k_nitya_seg_magic &&
                                    FramingPolicy::supports_version(hdr.version) &&
                                    hdr.segment_id == seg_id &&
                                    hdr.begin_lsn == seg_begin_lsn &&
                                    calculate_segment_header_crc<FramingPolicy>(hdr) == hdr.header_crc) {
                                    desc.segment_id = hdr.segment_id;
                                    desc.begin_lsn = hdr.begin_lsn;
                                    desc.end_lsn = (hdr.sealed_lsn != 0)
                                                       ? hdr.sealed_lsn
                                                       : (hdr.segment_id + 1) * opts_.segment_size;
                                    desc.is_archived = (hdr.flags & k_segment_archived) != 0;
                                    desc.created_at = std::chrono::system_clock::time_point(
                                        std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                            std::chrono::nanoseconds(hdr.created_at_unix_ns)
                                        )
                                    );
                                }
                            }
                        }

                        if (!list.push_back(desc)) {
                            return std::unexpected(LogError::QueueFull);
                        }
                    }
                    catch (...) {
                        // Segment file is corrupt or unreadable; mark as invalid and continue
                        // scanning remaining segments so callers can detect and handle gaps.
                        segment_descriptor corrupt_desc{};
                        corrupt_desc.path = entry.path();
                        corrupt_desc.segment_id = ~std::uint64_t{0}; // sentinel for corrupt
                        (void)list.push_back(corrupt_desc);
                    }
                }
            }

            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                return a.segment_id < b.segment_id;
            });

            for (auto& item : list) {
                *out++ = std::move(item);
            }
            return {};
        }

        // Allocating convenience overload
        [[nodiscard]] std::vector<segment_descriptor> list_segments() const {
            std::vector<segment_descriptor> list;
            (void)list_segments(std::back_inserter(list));
            return list;
        }

    private:
        wal_options opts_;
        StoragePolicy storage_;
        MemoryPolicy memory_;
        ConcurrencyPolicy concurrency_;

        std::mutex reservation_mutex_;
        std::mutex flush_mutex_;
        std::mutex publish_tracker_mutex_;
        std::mutex memory_mutex_; // guards MemoryPolicy scratch during recovery/replication

        std::atomic<lsn_t> tail_lsn_{k_segment_header_size};
        std::atomic<lsn_t> published_lsn_{k_segment_header_size};
        std::atomic<lsn_t> flushed_lsn_{k_segment_header_size};
        std::atomic<lsn_t> replicated_lsn_{k_segment_header_size};
        std::atomic<LogError> last_durability_error_{LogError::Success};
        std::atomic<lsn_t> last_durability_error_lsn_{k_invalid_lsn};

        std::atomic<std::uint64_t> records_published_{0};
        std::atomic<std::uint64_t> bytes_published_{0};
        std::atomic<std::uint64_t> flush_operations_{0};
        std::atomic<std::uint64_t> flush_failures_{0};
        std::atomic<std::uint64_t> group_commit_waiters_{0};
        std::atomic<std::uint64_t> replication_records_{0};
        std::atomic<std::uint64_t> replication_bytes_{0};

        PublishTrackerPolicy publish_tracker_;

        std::condition_variable flusher_cv_;
        std::mutex flusher_mutex_;
        std::atomic<bool> stop_background_flusher_{false};
        std::jthread background_thread_;

        void flusher_worker(const std::stop_token& st) {
            while (!st.stop_requested() && !stop_background_flusher_.load(std::memory_order_relaxed)) {
                {
                    std::unique_lock lk{flusher_mutex_};
                    flusher_cv_.wait_for(lk, opts_.group_commit_interval, [&] {
                        if (st.stop_requested() || stop_background_flusher_.load(std::memory_order_relaxed))
                            return
                                true;
                        const lsn_t pub = published_lsn_.load(std::memory_order_acquire);
                        const lsn_t flu = flushed_lsn_.load(std::memory_order_acquire);
                        return pub > flu && (pub - flu >= opts_.group_commit_bytes);
                    });
                }
                (void)flush_to(published_lsn_.load(std::memory_order_acquire));
            }
            (void)flush_to(published_lsn_.load(std::memory_order_acquire));
        }

        void stop_flusher() {
            if (opts_.background_flush) {
                stop_background_flusher_.store(true, std::memory_order_release);
                flusher_cv_.notify_all();
                if (background_thread_.joinable()) {
                    background_thread_.request_stop();
                    background_thread_.join();
                }
            }
        }

        // Internal helper: assumes flush_mutex_ is held
        // Opt-in batched group-commit durability. A waiter enqueues its ticket,
        // then contends for the flush mutex: the winner becomes the leader and
        // drains every waiting ticket in one covering flush (execute_leader_flush),
        // while losers wait on their ticket completion (satisfied by the leader)
        // or the flushed watermark. Only compiled when the selected
        // ConcurrencyPolicy opts into the ticket queue.
        // Only instantiated when the selected ConcurrencyPolicy opts into the
        // ticket queue: templating on C defers the signature's dependence on
        // C::commit_completion / commit_ticket to the point of call, so a
        // flush-gate policy (no ticket types) never triggers it.
        template <typename C = ConcurrencyPolicy>
        Result<void> wait_durable_via_ticket_queue(lsn_t target_lsn) {
            auto completion = std::make_shared<typename C::commit_completion>();
            typename C::commit_ticket ticket{
                .lsn = target_lsn,
                .total_bytes = 0,
                .completion = completion
            };
            if (!concurrency_.enqueue_commit(ticket)) {
                return std::unexpected(LogError::QueueFull);
            }

            for (;;) {
                if (flushed_lsn_.load(std::memory_order_acquire) >= target_lsn) return {};
                if (completion->done.load(std::memory_order_acquire)) {
                    if (const auto err = completion->result.load(std::memory_order_relaxed);
                        err != LogError::Success) {
                        return std::unexpected(err);
                    }
                    return {};
                }

                // Try to become leader for this batch.
                if (std::unique_lock lock{flush_mutex_, std::try_to_lock}) {
                    if (completion->done.load(std::memory_order_acquire) ||
                        flushed_lsn_.load(std::memory_order_relaxed) >= target_lsn) {
                        continue;
                    }
                    return execute_leader_flush(target_lsn, completion);
                }

                // Follower: wait until covered by the watermark or resolved by a leader.
                const lsn_t observed = flushed_lsn_.load(std::memory_order_acquire);
                if (observed >= target_lsn) return {};
                if (completion->done.load(std::memory_order_acquire)) continue;
                flushed_lsn_.wait(observed, std::memory_order_acquire);
            }
        }

        template <typename C = ConcurrencyPolicy>
        Result<void> execute_leader_flush(
            const lsn_t target_lsn,
            const std::shared_ptr<typename C::commit_completion>& own_completion) {
            auto own_err = LogError::Success;
            bool own_resolved = false;

            for (;;) {
                lsn_t max_ticket_lsn = target_lsn;
                containers::static_vector < typename C::commit_ticket, C::capacity() > follower_tickets;

                while (follower_tickets.size() < follower_tickets.capacity()) {
                    auto t = concurrency_.dequeue_commit();
                    if (!t) break;

                    max_ticket_lsn = std::max(max_ticket_lsn, t->lsn);
                    (void)follower_tickets.push_back(*t);
                }

                const lsn_t current_flushed = flushed_lsn_.load(std::memory_order_relaxed);
                auto flush_err = LogError::Success;

                if (max_ticket_lsn > current_flushed) {
                    if (auto flush_res = flush_range_to_locked(current_flushed, max_ticket_lsn); !flush_res) {
                        flush_err = flush_res.error();
                        record_durability_error(flush_err, max_ticket_lsn);
                    }
                    else {
                        flushed_lsn_.store(max_ticket_lsn, std::memory_order_release);
                        flushed_lsn_.notify_all();
                        flush_operations_.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                if (!own_resolved && max_ticket_lsn >= target_lsn) {
                    own_err = flush_err;
                    own_resolved = true;
                }

                for (auto& t : follower_tickets) {
                    if (t.completion == own_completion) {
                        continue;
                    }

                    if (t.completion) {
                        t.completion->result.store(flush_err, std::memory_order_relaxed);
                        t.completion->done.store(true, std::memory_order_release);
                        t.completion->done.notify_one();
                    }
                }

                // Wake followers not drained in this iteration but now covered by flushed watermark.
                flushed_lsn_.notify_all();

                if (follower_tickets.size() < follower_tickets.capacity() ||
                    flush_err != LogError::Success) {
                    break;
                }
            }

            if (!own_resolved) {
                own_err = LogError::InternalError;
            }

            own_completion->result.store(own_err, std::memory_order_relaxed);
            own_completion->done.store(true, std::memory_order_release);
            own_completion->done.notify_one();

            if (own_err != LogError::Success) {
                return std::unexpected(own_err);
            }

            return {};
        }

        // Internal helper: assumes flush_mutex_ is held
        Result<void> flush_range_to_locked(const lsn_t from_lsn, const lsn_t to_lsn) {
            lsn_t flush_cur = from_lsn;
            while (flush_cur < to_lsn) {
                const std::uint64_t seg_id = flush_cur / opts_.segment_size;
                const std::size_t seg_offset = flush_cur % opts_.segment_size;
                const lsn_t seg_end_lsn = (seg_id + 1) * opts_.segment_size;
                const lsn_t flush_to_in_seg = std::min(to_lsn, seg_end_lsn);
                const std::size_t flush_len = flush_to_in_seg - flush_cur;

                auto flush_res = storage_.flush_range(seg_id, seg_offset, flush_len, DurabilityPolicy::flush_type);
                if (!flush_res) {
                    return std::unexpected(flush_res.error());
                }

                flush_cur = flush_to_in_seg;
            }
            return {};
        }

        void record_durability_error(const LogError error, const lsn_t lsn) noexcept {
            if (error == LogError::Success) return;
            LogError expected = LogError::Success;
            if (last_durability_error_.compare_exchange_strong(
                expected, error, std::memory_order_release, std::memory_order_relaxed)) {
                last_durability_error_lsn_.store(lsn, std::memory_order_release);
            }
            flush_failures_.fetch_add(1, std::memory_order_relaxed);
        }

        // Advance the contiguous published watermark, recording out-of-order
        // ranges in the PublishTrackerPolicy. The contiguous-append fast path
        // (from <= cur) never touches the tracker; only genuine gaps do.
        Result<void> mark_published_range(lsn_t from, lsn_t to) {
            if (from >= to) return {};
            std::unique_lock lk{publish_tracker_mutex_};

            lsn_t cur = published_lsn_.load(std::memory_order_relaxed);
            const lsn_t original_cur = cur;

            auto publish = [&] {
                if (cur != original_cur) {
                    published_lsn_.store(cur, std::memory_order_release);
                    published_lsn_.notify_all();
                }
            };

            if (from <= cur) {
                cur = std::max(cur, to);
                publish_tracker_.drain(cur);
                publish();
                return {};
            }

            // Drain any already-contiguous intervals, then retry the fast path.
            publish_tracker_.drain(cur);
            if (from <= cur) {
                cur = std::max(cur, to);
                publish_tracker_.drain(cur);
                publish();
                return {};
            }

            // Genuine gap: hand the interval to the tracker policy.
            if (!publish_tracker_.insert(from, to)) {
                return std::unexpected(LogError::QueueFull);
            }

            publish_tracker_.drain(cur);
            publish();
            return {};
        }

        Result<void> mark_gap_published(const lsn_t from, const lsn_t to) {
            return mark_published_range(from, to);
        }

        // Must be called with reservation_mutex_ held
        Result<reservation> reserve_in_segment_locked(
            const lsn_t start_lsn,
            const std::uint32_t payload_bytes,
            std::size_t total_frame_size,
            const std::uint16_t flags,
            const std::uint16_t version) {
            const std::uint64_t seg_id = start_lsn / opts_.segment_size;
            const std::size_t seg_offset = start_lsn % opts_.segment_size;
            const lsn_t seg_begin_lsn = seg_id * opts_.segment_size;

            auto seg_res = storage_.get_or_create_segment(seg_id, seg_begin_lsn);
            if (!seg_res) return std::unexpected(seg_res.error());

            auto seg = *seg_res;
            auto bytes = seg->map.as_bytes();
            if (bytes.size() < seg_offset + total_frame_size) {
                return std::unexpected(LogError::SegmentFull);
            }

            reservation res{
                .lsn = start_lsn,
                .buffer = bytes.subspan(seg_offset, total_frame_size),
                .payload_size = payload_bytes,
                .version = version,
                .flags = flags,
                // Pin the segment_file so its setu::mapping cannot be munmap'd
                // by cache eviction between reserve() and publish().
                .segment_pin = seg
            };

            tail_lsn_.store(start_lsn + total_frame_size, std::memory_order_release);
            return res;
        }

        // Salvage resynchronisation (Item 7): find the next byte offset in `bytes`
        // at or after `from_offset` whose 4-byte little-endian word equals the frame
        // magic. Replaces the O(n) one-byte-at-a-time crawl with a vectorised sweep
        // (Highway) that jumps whole corrupt runs; scalar fallback is identical in
        // behaviour. Returns the offset, or bytes.size() when no magic remains.
        static std::size_t find_next_frame_magic(
            std::span<const std::byte> bytes, std::size_t from_offset) noexcept {
            if (bytes.size() < sizeof(std::uint32_t)) return bytes.size();
            const std::size_t last = bytes.size() - sizeof(std::uint32_t);
            const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());

            auto matches_at = [&](std::size_t off) noexcept {
                std::uint32_t word;
                std::memcpy(&word, data + off, sizeof(word));
                return word == k_nitya_magic;
            };

            std::size_t i = from_offset;
#if defined(PEBBLE_HAS_HIGHWAY)
            // Vectorised first-byte pre-filter: the frame magic's lowest byte is a
            // rare sentinel, so most windows are rejected without a 4-byte compare.
            namespace hn = hwy::HWY_NAMESPACE;
            const hn::ScalableTag<std::uint8_t> d;
            const std::size_t N = hn::Lanes(d);
            constexpr auto first_byte = static_cast<std::uint8_t>(k_nitya_magic & 0xFF);
            const auto needle = hn::Set(d, first_byte);
            for (; i + N <= last + 1; i += N) {
                const auto chunk = hn::LoadU(d, data + i);
                const auto mask = hn::Eq(chunk, needle);
                if (!hn::AllFalse(d, mask)) {
                    for (std::size_t j = i; j < i + N && j <= last; ++j) {
                        if (data[j] == first_byte && matches_at(j)) return j;
                    }
                }
            }
#endif
            for (; i <= last; ++i) {
                if (matches_at(i)) return i;
            }
            return bytes.size();
        }

        // read_record_at optionally stages the validated payload into the
        // MemoryPolicy arena (Item 2). Recovery and replication set stage_scratch
        // so the returned span survives eviction of the source segment from the
        // storage cache (max_cached_segments) and is served from zero-alloc
        // arena scratch rather than the volatile mmap. Falls back to the direct
        // mmap span if the arena cannot satisfy the request.
        std::optional<wal_record> read_record_at(
            lsn_t& cursor_lsn,
            recovery_status* status,
            const recovery_mode mode,
            const bool stage_scratch = false) {
            auto record_err = [&](const LogError err, const lsn_t bad_lsn = k_invalid_lsn) {
                if (status) {
                    status->error = err;
                    if (bad_lsn != k_invalid_lsn) status->first_bad_lsn = bad_lsn;
                }
            };

            while (true) {
                const std::uint64_t seg_id = cursor_lsn / opts_.segment_size;
                const lsn_t seg_base = seg_id * opts_.segment_size;

                // Ensure cursor skips segment metadata area
                if (cursor_lsn < seg_base + k_segment_header_size) {
                    cursor_lsn = seg_base + k_segment_header_size;
                }

                const std::size_t seg_offset = cursor_lsn % opts_.segment_size;

                if (status) {
                    status->segment_id = seg_id;
                }

                if (seg_offset + sizeof(frame_header) > opts_.segment_size) {
                    cursor_lsn = (seg_id + 1) * opts_.segment_size + k_segment_header_size;
                    continue;
                }

                if (auto path = opts_.wal_dir / storage_.format_segment_name(seg_id); !std::filesystem::exists(path)) {
                    record_err(LogError::EndOfLog);
                    return std::nullopt;
                }

                auto seg_res = storage_.get_or_create_segment(seg_id, seg_base);
                if (!seg_res) {
                    record_err(seg_res.error(), cursor_lsn);
                    return std::nullopt;
                }

                auto bytes = (*seg_res)->map.as_bytes();
                if (seg_offset + sizeof(frame_header) > bytes.size()) {
                    record_err(LogError::EndOfLog);
                    return std::nullopt;
                }

                // Salvage resync: jump cursor_lsn to the next frame-magic boundary
                // within this segment (vectorised), or to the next segment when the
                // rest is corrupt. Equivalent to the old +1 crawl, far fewer probes.
                auto salvage_resync = [&] {
                    const std::size_t next = find_next_frame_magic(bytes, seg_offset + 1);
                    if (next < bytes.size()) {
                        cursor_lsn = seg_base + next;
                    }
                    else {
                        cursor_lsn = (seg_id + 1) * opts_.segment_size + k_segment_header_size;
                    }
                };

                frame_header hdr;
                std::memcpy(&hdr, bytes.data() + seg_offset, sizeof(frame_header));

                if (hdr.magic != k_nitya_magic) {
                    // Check if entire rest of segment is padding / zero
                    if (opts_.auto_rotate) {
                        const lsn_t next_seg_lsn = (seg_id + 1) * opts_.segment_size + k_segment_header_size;
                        if (auto next_path = opts_.wal_dir / storage_.format_segment_name(seg_id + 1);
                            std::filesystem::exists(next_path) && cursor_lsn < next_seg_lsn) {
                            cursor_lsn = next_seg_lsn;
                            continue;
                        }
                    }
                    bool is_zero = true;
                    for (std::size_t i = seg_offset; i < bytes.size(); ++i) {
                        if (static_cast<std::uint8_t>(bytes[i]) != 0) {
                            is_zero = false;
                            break;
                        }
                    }
                    if (is_zero) {
                        record_err(LogError::EndOfLog);
                        return std::nullopt;
                    }

                    // Non-zero bytes with bad magic
                    record_err(LogError::CorruptedHeader, cursor_lsn);

                    if (mode == recovery_mode::salvage) {
                        // Resync to the next frame-magic boundary.
                        salvage_resync();
                        continue;
                    }
                    return std::nullopt;
                }

                auto val_res = FramingPolicy::validate_header(hdr, cursor_lsn);
                if (!val_res) {
                    record_err(val_res.error(), cursor_lsn);
                    if (mode == recovery_mode::salvage) {
                        salvage_resync();
                        continue;
                    }
                    return std::nullopt;
                }

                std::uint32_t payload_size = *val_res;
                const std::size_t total_size = k_frame_overhead + payload_size;
                if (seg_offset + total_size > bytes.size()) {
                    record_err(LogError::CorruptedTrailer, cursor_lsn);
                    if (mode == recovery_mode::salvage) {
                        salvage_resync();
                        continue;
                    }
                    return std::nullopt;
                }

                std::span<const std::byte> payload{
                    bytes.data() + seg_offset + sizeof(frame_header),
                    payload_size
                };

                // Stage payload into MemoryPolicy scratch when requested: gives the
                // caller an eviction-stable, zero-alloc copy. Arena is reset per
                // record (input-iterator: only one record is live at a time).
                if (stage_scratch && payload_size > 0) {
                    std::lock_guard scratch_lk{memory_mutex_};
                    memory_.reset();
                    if (void* buf = memory_.allocate(payload_size, alignof(std::max_align_t))) {
                        std::memcpy(buf, payload.data(), payload_size);
                        payload = std::span<const std::byte>{
                            static_cast<const std::byte*>(buf), payload_size
                        };
                    }
                    // else: arena exhausted — fall back to the direct mmap span.
                }

                frame_trailer trl;
                std::memcpy(&trl, bytes.data() + seg_offset + sizeof(frame_header) + payload_size,
                            sizeof(frame_trailer));

                auto trl_res = FramingPolicy::validate_payload_and_trailer(payload, trl, hdr.payload_crc);
                if (!trl_res) {
                    record_err(trl_res.error(), cursor_lsn);
                    if (mode == recovery_mode::salvage) {
                        salvage_resync();
                        continue;
                    }
                    return std::nullopt;
                }

                wal_record rec{
                    .lsn = cursor_lsn,
                    .payload = payload,
                    .version = hdr.version,
                    .flags = hdr.flags
                };

                if (status) {
                    status->last_valid_lsn = cursor_lsn;
                    status->records_recovered++;
                    status->bytes_recovered += total_size;
                }

                cursor_lsn += total_size;
                return rec;
            }
        }

        void init_tail_from_disk() {
            auto segs = list_segments();
            if (segs.empty()) {
                tail_lsn_.store(k_segment_header_size, std::memory_order_relaxed);
                published_lsn_.store(k_segment_header_size, std::memory_order_relaxed);
                flushed_lsn_.store(k_segment_header_size, std::memory_order_relaxed);
                return;
            }

            const lsn_t start_lsn = segs.front().begin_lsn + k_segment_header_size;
            lsn_t last_valid_end = start_lsn;
            for (auto stream = recover(start_lsn, recovery_mode::stop_at_first_error); const auto& rec : stream) {
                last_valid_end = rec.lsn + k_frame_overhead + rec.payload.size();
            }

            // If later empty allocated segments exist, advance to their data start.
            for (const auto& seg : segs) {
                if (const lsn_t seg_data_start = seg.begin_lsn + k_segment_header_size; seg_data_start >
                    last_valid_end) {
                    last_valid_end = seg_data_start;
                    break;
                }
            }

            tail_lsn_.store(last_valid_end, std::memory_order_relaxed);
            published_lsn_.store(last_valid_end, std::memory_order_relaxed);
            flushed_lsn_.store(last_valid_end, std::memory_order_relaxed);
        }
    };
} // namespace nitya
