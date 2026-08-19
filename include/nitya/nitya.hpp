#pragma once
// ============================================================================
// nitya/nitya.hpp — A Generic Durable Log Engine (DLE)
// ============================================================================
//
// C++23, header-only, policy-based, zero virtual functions, zero RTTI,
// zero heap allocation on the hot append path.
//
// Subsystems:
//   - Byte Offset LSN (Physical byte offset in the log stream)
//   - Reserve -> Publish -> Sync Pipeline (Group commit coordinator)
//   - Frame Layout (Header, payload, trailer with CRC/FNV1a validation)
//   - Setu-backed Mapped Segment Manager (rotation, allocation, flush)
//   - Smriti-backed Memory & Arena scratch integration
//   - Lock-Free Concurrency (MPMCQueue for commit batches)
//   - NADI-backed Observability (scopes for reserve, publish, flush, recovery)
//   - EasyRules-backed Administrative Retention & Archival management
//   - Streaming Recovery & Replication Scanner
// ============================================================================

#include "utils/setu.hpp"
#include "mem/smriti.hpp"
#include "mem/arena.hpp"
#include "containers/lockfree/MPMCQueue.hpp"
#include "observability/nadi.hpp"
#include "rules/easy_rules.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nitya {

    // ============================================================================
    // § 1  Types and Constants
    // ============================================================================

    using lsn_t = std::uint64_t;
    inline constexpr lsn_t k_invalid_lsn = std::numeric_limits<lsn_t>::max();
    inline constexpr std::uint32_t k_nitya_magic = 0x4E495459; // "NITY"

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
        InternalError
    };

    template <typename T>
    using Result = std::expected<T, LogError>;

    // ============================================================================
    // § 2  Binary Frame Layout
    // ============================================================================

    #pragma pack(push, 1)
    struct frame_header {
        std::uint32_t magic{k_nitya_magic};
        std::uint32_t size{0}; // Payload size
        std::uint64_t lsn{0};  // Byte offset LSN
        std::uint32_t header_crc{0};
        std::uint32_t payload_crc{0};
    };

    struct frame_trailer {
        std::uint32_t size{0};
        std::uint32_t payload_crc{0};
    };
    #pragma pack(pop)

    static_assert(sizeof(frame_header) == 24, "frame_header must be packed to 24 bytes");
    static_assert(sizeof(frame_trailer) == 8, "frame_trailer must be packed to 8 bytes");

    inline constexpr std::size_t k_frame_overhead = sizeof(frame_header) + sizeof(frame_trailer);

    struct wal_record {
        lsn_t lsn{0};
        std::span<const std::byte> payload;
    };

    struct reservation {
        lsn_t lsn{k_invalid_lsn};
        std::span<std::byte> buffer; // Total span including frame header, payload, and trailer
        std::uint32_t payload_size{0};

        [[nodiscard]] bool is_valid() const noexcept {
            return lsn != k_invalid_lsn && !buffer.empty();
        }

        [[nodiscard]] std::span<std::byte> payload_buffer() noexcept {
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
    };

    // ============================================================================
    // § 4  Framing Policy
    // ============================================================================

    struct default_framing {
        static constexpr std::uint32_t calculate_crc32(const std::byte* data, std::size_t len) noexcept {
            return setu::layout::fnv1a_32(data, len);
        }

        static void encode(reservation& res, std::uint32_t payload_crc) noexcept {
            assert(res.buffer.size() >= k_frame_overhead + res.payload_size);

            frame_header hdr;
            hdr.magic = k_nitya_magic;
            hdr.size = res.payload_size;
            hdr.lsn = res.lsn;
            hdr.payload_crc = payload_crc;

            // Calculate header crc over (magic, size, lsn, payload_crc)
            hdr.header_crc = 0;
            hdr.header_crc = calculate_crc32(reinterpret_cast<const std::byte*>(&hdr), sizeof(hdr));

            // Write header
            std::memcpy(res.buffer.data(), &hdr, sizeof(frame_header));

            // Write trailer
            frame_trailer trl;
            trl.size = res.payload_size;
            trl.payload_crc = payload_crc;
            std::memcpy(res.buffer.data() + sizeof(frame_header) + res.payload_size, &trl, sizeof(frame_trailer));
        }

        static Result<std::uint32_t> validate_header(const frame_header& hdr, lsn_t expected_lsn) noexcept {
            if (hdr.magic != k_nitya_magic) {
                return std::unexpected(LogError::CorruptedHeader);
            }
            if (hdr.lsn != expected_lsn) {
                return std::unexpected(LogError::LsnMismatch);
            }

            frame_header copy = hdr;
            copy.header_crc = 0;
            std::uint32_t computed_crc = calculate_crc32(reinterpret_cast<const std::byte*>(&copy), sizeof(copy));
            if (computed_crc != hdr.header_crc) {
                return std::unexpected(LogError::ChecksumMismatch);
            }

            return hdr.size;
        }

        static Result<void> validate_payload_and_trailer(
            std::span<const std::byte> payload,
            const frame_trailer& trl,
            std::uint32_t expected_payload_crc) noexcept {
            if (trl.size != payload.size()) {
                return std::unexpected(LogError::CorruptedTrailer);
            }
            if (trl.payload_crc != expected_payload_crc) {
                return std::unexpected(LogError::ChecksumMismatch);
            }
            std::uint32_t actual_crc = calculate_crc32(payload.data(), payload.size());
            if (actual_crc != expected_payload_crc) {
                return std::unexpected(LogError::CorruptedPayload);
            }
            return {};
        }
    };

    // ============================================================================
    // § 5  Storage Policy (Setu-backed mapped segments)
    // ============================================================================

    class setu_storage {
    public:
        struct segment_file {
            std::uint64_t segment_id{0};
            lsn_t begin_lsn{0};
            std::size_t capacity{0};
            std::filesystem::path path;
            setu::mapping<setu::read_write> map;
        };

        explicit setu_storage(wal_options opts) : opts_{std::move(opts)} {
            std::error_code ec;
            std::filesystem::create_directories(opts_.wal_dir, ec);
        }

        ~setu_storage() = default;
        setu_storage(const setu_storage&) = delete;
        setu_storage& operator=(const setu_storage&) = delete;
        setu_storage(setu_storage&&) = delete;
        setu_storage& operator=(setu_storage&&) = delete;

        [[nodiscard]] std::string format_segment_name(std::uint64_t segment_id) const {
            return std::format("{:010d}.log", segment_id);
        }

        Result<std::shared_ptr<segment_file>> get_or_create_segment(std::uint64_t seg_id, lsn_t begin_lsn) {
            std::lock_guard lk{mutex_};
            for (auto& s : active_segments_) {
                if (s->segment_id == seg_id) return s;
            }

            auto path = opts_.wal_dir / format_segment_name(seg_id);
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

            if (active_segments_.size() >= opts_.max_cached_segments) {
                active_segments_.erase(active_segments_.begin());
            }
            active_segments_.push_back(seg);
            return seg;
        }

        Result<void> flush_range(std::uint64_t seg_id, std::size_t offset, std::size_t length, setu::flush_mode mode) {
            std::lock_guard lk{mutex_};
            for (auto& s : active_segments_) {
                if (s->segment_id == seg_id) {
                    auto res = s->map.flush_range(offset, length, mode);
                    if (!res) return std::unexpected(LogError::FlushFailed);
                    return {};
                }
            }
            return {};
        }

        [[nodiscard]] const wal_options& options() const noexcept { return opts_; }

    private:
        wal_options opts_;
        std::mutex mutex_;
        std::vector<std::shared_ptr<segment_file>> active_segments_;
    };

    // ============================================================================
    // § 6  Memory Policy (Smriti-backed Arena)
    // ============================================================================

    class smriti_memory {
    public:
        explicit smriti_memory(std::size_t arena_size = 1024 * 1024)
            : arena_{arena_size} {}

        [[nodiscard]] void* allocate(std::size_t n, std::size_t a = alignof(std::max_align_t)) noexcept {
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
        static constexpr setu::flush_mode flush_type = setu::flush_mode::sync;
        static constexpr bool is_synchronous = true;
    };

    struct async_durability {
        static constexpr setu::flush_mode flush_type = setu::flush_mode::async;
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
    // § 9  Concurrency Policy (Lockfree MPMC Queue for Group Commit)
    // ============================================================================

    template <std::size_t QueueCapacity = 1024>
    class group_commit_concurrency {
    public:
        struct commit_ticket {
            lsn_t lsn{0};
            std::size_t total_bytes{0};
            std::atomic<bool>* completion_flag{nullptr};
        };

        group_commit_concurrency() = default;

        bool enqueue_commit(commit_ticket t) noexcept {
            return queue_.try_push(t);
        }

        std::optional<commit_ticket> dequeue_commit() noexcept {
            return queue_.try_pop();
        }

    private:
        lockfree::MPMCQueue<commit_ticket, QueueCapacity> queue_;
    };

    // ============================================================================
    // § 10  Wal Engine: nitya::wal
    // ============================================================================

    template <
        typename StoragePolicy     = setu_storage,
        typename MemoryPolicy      = smriti_memory,
        typename ConcurrencyPolicy = group_commit_concurrency<1024>,
        typename FramingPolicy     = default_framing,
        typename DurabilityPolicy  = sync_durability,
        typename TelemetryPolicy   = nadi_telemetry
    >
    class wal {
    public:
        explicit wal(wal_options opts = {})
            : opts_{opts}
              , storage_{opts_}
              , memory_{1024 * 1024}
              , concurrency_{}
              , tail_lsn_{0}
              , flushed_lsn_{0}
              , replicated_lsn_{0} {
            init_tail_from_disk();
        }

        ~wal() {
            (void)sync();
        }

        wal(const wal&) = delete;
        wal& operator=(const wal&) = delete;
        wal(wal&&) = delete;
        wal& operator=(wal&&) = delete;

        // ------------------------------------------------------------------------
        // 1. Reserve Phase
        // ------------------------------------------------------------------------
        [[nodiscard]] Result<reservation> reserve(std::uint32_t payload_bytes) {
            auto telemetry = TelemetryPolicy::trace_reserve();
            (void)telemetry;

            const std::size_t total_frame_size = k_frame_overhead + payload_bytes;
            if (total_frame_size > opts_.segment_size) {
                return std::unexpected(LogError::InvalidArg);
            }

            std::unique_lock lk{reservation_mutex_};

            const lsn_t current_lsn = tail_lsn_.load(std::memory_order_relaxed);
            const std::uint64_t seg_id = current_lsn / opts_.segment_size;
            const std::size_t seg_offset = current_lsn % opts_.segment_size;

            // Check if record crosses segment boundary -> align to next segment if auto_rotate
            if (seg_offset + total_frame_size > opts_.segment_size) {
                if (!opts_.auto_rotate) {
                    return std::unexpected(LogError::SegmentFull);
                }
                const lsn_t next_seg_lsn = (seg_id + 1) * opts_.segment_size;
                tail_lsn_.store(next_seg_lsn, std::memory_order_release);
                return reserve_in_segment(next_seg_lsn, payload_bytes, total_frame_size);
            }

            return reserve_in_segment(current_lsn, payload_bytes, total_frame_size);
        }

        // ------------------------------------------------------------------------
        // 2. Publish Phase
        // ------------------------------------------------------------------------
        Result<void> publish(reservation& res) {
            auto telemetry = TelemetryPolicy::trace_publish();
            (void)telemetry;

            if (!res.is_valid()) return std::unexpected(LogError::InvalidArg);

            auto payload = res.payload_buffer();
            std::uint32_t payload_crc = FramingPolicy::calculate_crc32(payload.data(), payload.size());

            // Write Header and Trailer into mapped buffer
            FramingPolicy::encode(res, payload_crc);

            published_lsn_.store(
                std::max(published_lsn_.load(std::memory_order_relaxed), res.lsn + res.buffer.size()),
                std::memory_order_release
            );

            if (opts_.sync_on_publish) {
                return sync();
            }

            return {};
        }

        // ------------------------------------------------------------------------
        // 3. Append Convenience (Reserve + Copy + Publish)
        // ------------------------------------------------------------------------
        Result<lsn_t> append(std::span<const std::byte> payload) {
            auto res_result = reserve(static_cast<std::uint32_t>(payload.size()));
            if (!res_result) return std::unexpected(res_result.error());

            auto res = *res_result;
            auto target = res.payload_buffer();
            std::memcpy(target.data(), payload.data(), payload.size());

            auto pub_res = publish(res);
            if (!pub_res) return std::unexpected(pub_res.error());

            return res.lsn;
        }

        // ------------------------------------------------------------------------
        // 4. Sync / Flush Phase (Group Commit Durability)
        // ------------------------------------------------------------------------
        Result<void> sync() {
            auto telemetry = TelemetryPolicy::trace_flush();
            (void)telemetry;

            std::unique_lock lk{flush_mutex_};
            const lsn_t target_lsn = published_lsn_.load(std::memory_order_acquire);
            const lsn_t current_flushed = flushed_lsn_.load(std::memory_order_relaxed);

            if (target_lsn <= current_flushed) return {};

            // Flush all active affected segment ranges
            lsn_t flush_cur = current_flushed;
            while (flush_cur < target_lsn) {
                const std::uint64_t seg_id = flush_cur / opts_.segment_size;
                const std::size_t seg_offset = flush_cur % opts_.segment_size;
                const lsn_t seg_end_lsn = (seg_id + 1) * opts_.segment_size;
                const lsn_t flush_to_in_seg = std::min(target_lsn, seg_end_lsn);
                const std::size_t flush_len = flush_to_in_seg - flush_cur;

                auto flush_res = storage_.flush_range(seg_id, seg_offset, flush_len, DurabilityPolicy::flush_type);
                if (!flush_res) return std::unexpected(flush_res.error());

                flush_cur = flush_to_in_seg;
            }

            flushed_lsn_.store(target_lsn, std::memory_order_release);
            return {};
        }

        // ------------------------------------------------------------------------
        // 5. Streaming Recovery Engine
        // ------------------------------------------------------------------------
        class recovery_stream {
        public:
            recovery_stream(wal& parent, lsn_t start_lsn)
                : parent_{parent}, cursor_lsn_{start_lsn} {}

            struct recovery_iterator {
                recovery_stream* stream{nullptr};
                std::optional<wal_record> current{};

                recovery_iterator() = default;
                explicit recovery_iterator(recovery_stream* s) : stream{s} {
                    advance();
                }

                const wal_record& operator*() const noexcept { return *current; }
                const wal_record* operator->() const noexcept { return &(*current); }

                recovery_iterator& operator++() {
                    advance();
                    return *this;
                }

                bool operator==(const recovery_iterator& other) const noexcept {
                    return (!current.has_value() && !other.current.has_value());
                }

                bool operator!=(const recovery_iterator& other) const noexcept {
                    return !(*this == other);
                }

            private:
                void advance() {
                    if (!stream) { current = std::nullopt; return; }
                    current = stream->next_record();
                }
            };

            [[nodiscard]] recovery_iterator begin() { return recovery_iterator{this}; }
            [[nodiscard]] recovery_iterator end() { return recovery_iterator{}; }

            std::optional<wal_record> next_record() {
                return parent_.read_record_at(cursor_lsn_);
            }

        private:
            wal& parent_;
            lsn_t cursor_lsn_;
        };

        [[nodiscard]] recovery_stream recover(lsn_t start_lsn = 0) {
            auto telemetry = TelemetryPolicy::trace_recovery();
            (void)telemetry;
            return recovery_stream{*this, start_lsn};
        }

        // ------------------------------------------------------------------------
        // 6. Replication Stream Subscription
        // ------------------------------------------------------------------------
        class replication_stream {
        public:
            replication_stream(wal& parent, lsn_t start_lsn)
                : parent_{parent}, cursor_lsn_{start_lsn} {}

            std::optional<wal_record> next() {
                auto rec = parent_.read_record_at(cursor_lsn_);
                if (rec) {
                    parent_.set_replicated_lsn(rec->lsn + k_frame_overhead + rec->payload.size());
                }
                return rec;
            }

        private:
            wal& parent_;
            lsn_t cursor_lsn_;
        };

        [[nodiscard]] replication_stream subscribe(lsn_t from_lsn = 0) {
            auto telemetry = TelemetryPolicy::trace_replication();
            (void)telemetry;
            return replication_stream{*this, from_lsn};
        }

        // ------------------------------------------------------------------------
        // 7. Retention & Archival Automation (EasyRules integration)
        // ------------------------------------------------------------------------
        void apply_retention_rules(
            std::chrono::seconds max_segment_age,
            std::function<void(const segment_descriptor&)> on_archive = nullptr,
            std::function<void(const segment_descriptor&)> on_delete = nullptr) {

            easy_rules::EasyRuleEngine engine;
            engine.config.verbose = false;

            engine.when("SegmentRetention", [](const easy_rules::Facts& facts) {
                auto age_sec = facts.get<int>("age_seconds").value_or(0);
                auto max_age = facts.get<int>("max_age_seconds").value_or(0);
                auto replicated = facts.get<bool>("is_replicated").value_or(false);
                return replicated && (age_sec >= max_age);
            })
            .then([&](easy_rules::ExecutionContext& ctx) {
                if (on_delete) {
                    segment_descriptor desc;
                    desc.segment_id = static_cast<std::uint64_t>(ctx.facts.get<int>("segment_id").value_or(0));
                    on_delete(desc);
                }
            })
            .with_description("Delete segments past age threshold and fully replicated");

            engine.when("SegmentArchival", [](const easy_rules::Facts& facts) {
                auto replicated = facts.get<bool>("is_replicated").value_or(false);
                auto archived = facts.get<bool>("is_archived").value_or(false);
                return replicated && !archived;
            })
            .then([&](easy_rules::ExecutionContext& ctx) {
                if (on_archive) {
                    segment_descriptor desc;
                    desc.segment_id = static_cast<std::uint64_t>(ctx.facts.get<int>("segment_id").value_or(0));
                    on_archive(desc);
                }
            })
            .with_description("Archive segments that are replicated");

            // Evaluate segments
            auto segments = list_segments();
            for (const auto& seg : segments) {
                easy_rules::ExecutionContext ctx;
                auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now() - seg.created_at).count();
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

        void set_replicated_lsn(lsn_t lsn) noexcept {
            replicated_lsn_.store(std::max(replicated_lsn_.load(std::memory_order_relaxed), lsn), std::memory_order_release);
        }

        [[nodiscard]] std::vector<segment_descriptor> list_segments() const {
            std::vector<segment_descriptor> list;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(opts_.wal_dir, ec)) {
                if (entry.is_regular_file() && entry.path().extension() == ".log") {
                    std::string stem = entry.path().stem().string();
                    try {
                        std::uint64_t seg_id = std::stoull(stem);
                        list.push_back(segment_descriptor{
                            .segment_id = seg_id,
                            .begin_lsn = seg_id * opts_.segment_size,
                            .end_lsn = (seg_id + 1) * opts_.segment_size,
                            .path = entry.path(),
                            .is_archived = false,
                            .is_replicated = (seg_id + 1) * opts_.segment_size <= replicated_lsn_.load(std::memory_order_relaxed)
                        });
                    } catch (...) {}
                }
            }
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                return a.segment_id < b.segment_id;
            });
            return list;
        }

    private:
        wal_options opts_;
        StoragePolicy storage_;
        MemoryPolicy memory_;
        ConcurrencyPolicy concurrency_;

        std::mutex reservation_mutex_;
        std::mutex flush_mutex_;

        std::atomic<lsn_t> tail_lsn_{0};
        std::atomic<lsn_t> published_lsn_{0};
        std::atomic<lsn_t> flushed_lsn_{0};
        std::atomic<lsn_t> replicated_lsn_{0};

        Result<reservation> reserve_in_segment(lsn_t start_lsn, std::uint32_t payload_bytes, std::size_t total_frame_size) {
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
                .payload_size = payload_bytes
            };

            tail_lsn_.store(start_lsn + total_frame_size, std::memory_order_release);
            return res;
        }

        std::optional<wal_record> read_record_at(lsn_t& cursor_lsn) {
            while (true) {
                const std::uint64_t seg_id = cursor_lsn / opts_.segment_size;
                const std::size_t seg_offset = cursor_lsn % opts_.segment_size;
                const lsn_t seg_begin_lsn = seg_id * opts_.segment_size;

                if (seg_offset + sizeof(frame_header) > opts_.segment_size) {
                    cursor_lsn = (seg_id + 1) * opts_.segment_size;
                    continue;
                }

                auto path = opts_.wal_dir / storage_.format_segment_name(seg_id);
                if (!std::filesystem::exists(path)) {
                    return std::nullopt;
                }

                auto seg_res = storage_.get_or_create_segment(seg_id, seg_begin_lsn);
                if (!seg_res) return std::nullopt;

                auto bytes = (*seg_res)->map.as_bytes();
                if (seg_offset + sizeof(frame_header) > bytes.size()) {
                    return std::nullopt;
                }

                frame_header hdr;
                std::memcpy(&hdr, bytes.data() + seg_offset, sizeof(frame_header));

                if (hdr.magic != k_nitya_magic) {
                    // Check if remaining segment is padding or empty
                    if (opts_.auto_rotate) {
                        lsn_t next_seg_lsn = (seg_id + 1) * opts_.segment_size;
                        auto next_path = opts_.wal_dir / storage_.format_segment_name(seg_id + 1);
                        if (std::filesystem::exists(next_path) && cursor_lsn < next_seg_lsn) {
                            cursor_lsn = next_seg_lsn;
                            continue;
                        }
                    }
                    return std::nullopt;
                }

                auto val_res = FramingPolicy::validate_header(hdr, cursor_lsn);
                if (!val_res) return std::nullopt;

                std::uint32_t payload_size = *val_res;
                std::size_t total_size = k_frame_overhead + payload_size;
                if (seg_offset + total_size > bytes.size()) {
                    return std::nullopt;
                }

                std::span<const std::byte> payload{
                    bytes.data() + seg_offset + sizeof(frame_header),
                    payload_size
                };

                frame_trailer trl;
                std::memcpy(&trl, bytes.data() + seg_offset + sizeof(frame_header) + payload_size, sizeof(frame_trailer));

                auto trl_res = FramingPolicy::validate_payload_and_trailer(payload, trl, hdr.payload_crc);
                if (!trl_res) return std::nullopt;

                wal_record rec{
                    .lsn = cursor_lsn,
                    .payload = payload
                };

                cursor_lsn += total_size;
                return rec;
            }
        }

        void init_tail_from_disk() {
            auto segs = list_segments();
            if (segs.empty()) {
                tail_lsn_.store(0, std::memory_order_relaxed);
                published_lsn_.store(0, std::memory_order_relaxed);
                flushed_lsn_.store(0, std::memory_order_relaxed);
                return;
            }

            lsn_t last_valid_end = 0;
            auto stream = recover(0);
            for (const auto& rec : stream) {
                last_valid_end = rec.lsn + k_frame_overhead + rec.payload.size();
            }

            tail_lsn_.store(last_valid_end, std::memory_order_relaxed);
            published_lsn_.store(last_valid_end, std::memory_order_relaxed);
            flushed_lsn_.store(last_valid_end, std::memory_order_relaxed);
        }
    };

} // namespace nitya
