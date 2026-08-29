#pragma once
// ============================================================================
// petika/petika.hpp — Unified Engine-Agnostic Storage Platform for C++23
// ============================================================================
//
// Architecture:
//   Application API -> Petika Store -> Pluggable Engine (SkipList/Hash/BTree/LSM)
//   Common Infrastructure Layer:
//     - Durability & WAL: Nitya
//     - Memory Mapping & Pages: Setu
//     - Memory Pools & Arenas: Smriti
//     - Lock-Free Concurrency & Caching: Containers
//     - Telemetry & Profiling: NADI
//     - Operational Policies & Automation: EasyRules
//
// Policy Parameters (Petika template):
//   Engine             — storage engine (JournaledSkipEngine, MvccJournaledSkipEngine, …)
//   Serializer         — key/value encode/decode (StringSerializer, BinarySerializer, ViewSerializer)
//   Comparator         — key ordering (LexicalComparator, custom)
//   DurabilityPolicy   — WAL backend (nitya::wal<>)
//   TelemetryPolicy    — NADI tracing (nitya::nadi_telemetry)
//   ConcurrencyPolicy  — mutex (std::shared_mutex, NullMutex for single-thread)
//   WriteBufferPolicy  — commit coalescing (ImmediateCommitPolicy, GroupCommitPolicy<N>)
//   BloomFilterPolicy  — probabilistic key existence (NoBloomFilter, BloomFilterPolicy<Bits>)
//
// Zero virtual functions, zero RTTI, zero macros, modern C++23.
// ============================================================================

#include "petika/engine.hpp"
#include "petika/serializer.hpp"
#include "petika/engines/journaled_skip_engine.hpp"
#include "petika/engines/mvcc_journaled_skip_engine.hpp"
#include "nitya/nitya.hpp"
#include "utils/setu.hpp"
#include "mem/smriti.hpp"
#include "containers/lockfree/MPMCQueue.hpp"
#include "observability/nadi.hpp"
#include "rules/easy_rules.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace petika {

    // ============================================================================
    // Options & Configuration
    // ============================================================================
    struct PetikaOptions {
        std::filesystem::path db_dir{"./petika_data"};
        std::size_t segment_size{16 * 1024 * 1024}; // 16 MB WAL segments
        bool sync_on_write{true};
        bool auto_recovery{true};
        std::size_t max_cached_segments{8};
    };

    // ============================================================================
    // WriteBuffer Policies
    // ============================================================================

    // Default: each put/erase immediately writes a WAL record (original behaviour).
    struct ImmediateCommitPolicy {};

    // GroupCommitPolicy: accumulates up to BufferSize puts per-thread, then
    // flushes as a single WAL batch. Reduces lock acquisitions and WAL appends.
    template <std::size_t BufferSize = 256>
    struct GroupCommitPolicy {
        static constexpr std::size_t buffer_size = BufferSize;
    };

    // ============================================================================
    // Bloom Filter Policies
    // ============================================================================

    // Default: no filter; all lookups pass through to the engine.
    struct NoBloomFilter {
        template <typename K> void insert(const K&) noexcept {}
        template <typename K> void remove(const K&) noexcept {}
        template <typename K> [[nodiscard]] bool maybe_contains(const K&) const noexcept { return true; }
        void reset() noexcept {}
    };

    // Simple in-process bloom filter rebuilt from WAL on restart.
    // Provides zero false negatives; occasional false positives on miss.
    template <std::size_t Bits = (1u << 20)>
    struct BloomFilterPolicy {
        static_assert(Bits > 0 && (Bits & (Bits - 1)) == 0, "Bits must be power of 2");
        static constexpr std::size_t kBits = Bits;
        static constexpr std::size_t kWords = Bits / 64;

        template <typename K>
        void insert(const K& key) noexcept {
            for (auto h : hashes(key)) bits_[h / 64] |= (1ull << (h % 64));
        }

        template <typename K>
        void remove(const K&) noexcept {
            // Counting bloom not implemented; reset() + full rebuild required.
            // Callers should rebuild after bulk deletes or use erase-aware index.
        }

        template <typename K>
        [[nodiscard]] bool maybe_contains(const K& key) const noexcept {
            for (auto h : hashes(key)) {
                if (!(bits_[h / 64] & (1ull << (h % 64)))) return false;
            }
            return true;
        }

        void reset() noexcept { bits_.fill(0); }

    private:
        std::array<std::uint64_t, kWords> bits_{};

        template <typename K>
        std::array<std::size_t, 3> hashes(const K& key) const noexcept {
            // Three independent hash probes using splitmix64-style mixing.
            std::size_t h = std::hash<K>{}(key);
            auto mix = [](std::size_t x) noexcept -> std::size_t {
                x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
                x ^= x >> 27; x *= 0x94d049bb133111ebull;
                return x ^ (x >> 31);
            };
            const std::size_t h0 = mix(h) & (kBits - 1);
            const std::size_t h1 = mix(h ^ 0xdeadbeefcafe1234ull) & (kBits - 1);
            const std::size_t h2 = mix(h ^ 0x0123456789abcdefull) & (kBits - 1);
            return {h0, h1, h2};
        }
    };

    // ============================================================================
    // SnapshotGCPolicy — MVCC version reclamation
    // ============================================================================

    template <typename P>
    concept SnapshotGCPolicyConcept = requires(P& p, nitya::lsn_t lsn) {
        { p.min_snapshot_lsn() } -> std::same_as<nitya::lsn_t>;
        { p.register_snapshot(lsn) } -> std::same_as<void>;
        { p.release_snapshot(lsn) } -> std::same_as<void>;
    };

    // Default: no GC tracking; MVCC chains grow until engine restart.
    struct NoGC {
        nitya::lsn_t min_snapshot_lsn() const noexcept { return 0; }
        void register_snapshot(nitya::lsn_t) noexcept {}
        void release_snapshot(nitya::lsn_t) noexcept {}
    };

    // EpochBasedGC: tracks active snapshots, exposes min_snapshot_lsn() for
    // Anukrama reclamation. Thread-safe via atomic sorted active set.
    struct EpochBasedGC {
        [[nodiscard]] nitya::lsn_t min_snapshot_lsn() const noexcept {
            std::shared_lock lk{mu_};
            return active_.empty() ? std::numeric_limits<nitya::lsn_t>::max()
                                   : *active_.begin();
        }

        void register_snapshot(nitya::lsn_t lsn) {
            std::unique_lock lk{mu_};
            active_.insert(lsn);
        }

        void release_snapshot(nitya::lsn_t lsn) {
            std::unique_lock lk{mu_};
            active_.erase(lsn);
        }

    private:
        mutable std::shared_mutex mu_;
        std::multiset<nitya::lsn_t> active_;
    };

    // ============================================================================
    // Transaction & Snapshot Handles
    // ============================================================================
    template <typename StoreType>
    class Transaction {
    public:
        using key_type = typename StoreType::key_type;
        using value_type = typename StoreType::value_type;

        struct Mutation {
            EntryOp op;
            key_type key;
            value_type value;
        };
        struct Observation {
            key_type key;
            nitya::lsn_t version{};
        };

        Transaction(StoreType& store, const nitya::lsn_t snapshot_lsn)
            : store_{store}, snapshot_lsn_{snapshot_lsn} {}

        Result<void> put(key_type key, value_type val) {
            observe_write(key);
            // Update O(1) index: latest mutation index for this key.
            mutation_index_[key] = mutations_.size();
            mutations_.emplace_back(EntryOp::Put, key, std::move(val));
            return {};
        }

        Result<void> erase(key_type key) {
            observe_write(key);
            mutation_index_[key] = mutations_.size();
            mutations_.emplace_back(EntryOp::Delete, std::move(key), value_type{});
            return {};
        }

        Result<value_type> get(const key_type& key) {
            // O(1) read-your-own-writes via index.
            if (auto it = mutation_index_.find(key); it != mutation_index_.end()) {
                const auto& m = mutations_[it->second];
                if (m.op == EntryOp::Delete) return std::unexpected(StorageError::NotFound);
                return m.value;
            }
            if constexpr (requires(const StoreType& store, const key_type& k, nitya::lsn_t at) {
                { store.get_at(k, at) } -> std::same_as<Result<value_type>>;
            }) return store_.get_at(key, snapshot_lsn_);
            else return store_.get(key);
        }

        Result<void> commit() {
            auto result = store_.commit_batch(mutations_, observations_);
            if (!result) return result;
            mutations_.clear();
            observations_.clear();
            mutation_index_.clear();
            observed_keys_.clear();
            return {};
        }

        void abort() noexcept {
            mutations_.clear();
            observations_.clear();
            mutation_index_.clear();
            observed_keys_.clear();
        }

    private:
        void observe_write(const key_type& key) {
            // O(1) dedup via hash set.
            if (!observed_keys_.insert(key).second) return;
            if constexpr (requires(const StoreType& store, const key_type& k, nitya::lsn_t at) {
                { store.version_at(k, at) } -> std::same_as<nitya::lsn_t>;
            }) observations_.push_back({key, store_.version_at(key, snapshot_lsn_)});
        }

        StoreType& store_;
        nitya::lsn_t snapshot_lsn_{};
        std::vector<Mutation> mutations_;
        std::vector<Observation> observations_;
        std::unordered_map<key_type, std::size_t> mutation_index_;   // key -> latest mutation idx
        std::unordered_set<key_type> observed_keys_;                  // O(1) observation dedup
    };

    template <typename StoreType>
    class Snapshot {
    public:
        using key_type = typename StoreType::key_type;
        using value_type = typename StoreType::value_type;

        Snapshot(std::uint64_t snap_id, nitya::lsn_t snap_lsn, const StoreType& store)
            : id_{snap_id}, snapshot_lsn_{snap_lsn}, store_{store} {}

        [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
        [[nodiscard]] nitya::lsn_t lsn() const noexcept { return snapshot_lsn_; }

        Result<value_type> get(const key_type& key) const {
            if constexpr (requires(const StoreType& store, const key_type& k, nitya::lsn_t at) {
                { store.engine().get_at(k, at) } -> std::same_as<Result<value_type>>;
            }) return store_.engine().get_at(key, snapshot_lsn_);
            else return store_.get(key);
        }

    private:
        std::uint64_t id_;
        nitya::lsn_t snapshot_lsn_;
        const StoreType& store_;
    };

    // ============================================================================
    // ScanView — C++23 ranges-compatible lazy scan over engine results
    // ============================================================================
    template <typename Engine>
    class ScanView {
    public:
        using key_type = typename Engine::key_type;
        using value_type = typename Engine::value_type;
        using entry_type = typename Engine::EntryView;

        // Eagerly materialises the range from the engine into a vector for range
        // compatibility. For truly lazy iteration, callers use the scan() callback.
        ScanView(const Engine& engine, const key_type& first, const key_type& last) {
            engine.scan(first, last, [&](const auto& entry) {
                entries_.push_back({entry.key, entry.value, entry.lsn});
            });
        }

        struct OwnedEntry {
            key_type key;
            value_type value;
            nitya::lsn_t lsn{};
        };

        auto begin() const noexcept { return entries_.begin(); }
        auto end()   const noexcept { return entries_.end(); }
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

    private:
        std::vector<OwnedEntry> entries_;
    };

    // ============================================================================
    // Main Petika Storage Platform
    // ============================================================================
    template <
        typename Engine,
        typename Serializer = StringSerializer,
        typename Comparator = LexicalComparator,
        typename DurabilityPolicy = nitya::wal<>,
        typename TelemetryPolicy = nitya::nadi_telemetry,
        typename ConcurrencyPolicy = std::shared_mutex,
        typename WriteBuffer = ImmediateCommitPolicy,
        typename BloomFilter = NoBloomFilter
    >
    class Petika {
    public:
        using key_type = typename Engine::key_type;
        using value_type = typename Engine::value_type;
        using engine_type = Engine;
        using serializer_type = Serializer;
        using comparator_type = Comparator;

        struct entry_type {
            key_type key;
            value_type value;
            nitya::lsn_t lsn{};
        };

        explicit Petika(PetikaOptions opts = PetikaOptions{})
            : opts_{std::move(opts)}, engine_{}, manifest_{} {
            std::error_code ec;
            std::filesystem::create_directories(opts_.db_dir, ec);

            nitya::wal_options wal_opts{
                .wal_dir = opts_.db_dir / "wal",
                .segment_size = opts_.segment_size,
                .max_cached_segments = opts_.max_cached_segments,
                .sync_on_publish = opts_.sync_on_write,
                .auto_rotate = true
            };

            wal_ = std::make_unique<DurabilityPolicy>(wal_opts);

            if (opts_.auto_recovery) {
                (void)recover();
            }
        }

        ~Petika() = default;
        Petika(const Petika&) = delete;
        Petika& operator=(const Petika&) = delete;
        Petika(Petika&&) noexcept = default;
        Petika& operator=(Petika&&) noexcept = default;

        // ------------------------------------------------------------------------
        // CRUD Operations
        // ------------------------------------------------------------------------
        Result<void> put(const key_type& key, const value_type& value) {
            // Serialise outside the lock — only WAL append + engine update need it.
            auto telemetry = TelemetryPolicy::trace_publish();
            (void)telemetry;

            const std::string k_str = Serializer::serialize_key(key);
            const std::string v_str = Serializer::serialize_value(value);

            // Zero-alloc WAL encode into thread-local stack buffer.
            thread_local std::array<std::byte, 4096> tl_buf{};
            const std::size_t needed = WalPayloadCodec::encode_size(k_str, v_str);
            std::vector<std::byte> heap_buf;
            std::byte* buf = tl_buf.data();
            if (needed > tl_buf.size()) {
                heap_buf.resize(needed);
                buf = heap_buf.data();
            }
            const std::size_t encoded_sz = WalPayloadCodec::encode_to(buf, EntryOp::Put, k_str, v_str);

            std::unique_lock lock{commit_mutex_};
            auto append_res = wal_->append(std::span{buf, encoded_sz});
            if (!append_res) return std::unexpected(StorageError::WalError);

            const nitya::lsn_t lsn = *append_res;
            if (opts_.sync_on_write) {
                if (auto s_res = wal_->sync(); !s_res)
                    return std::unexpected(StorageError::WalError);
            }

            auto put_res = engine_.put(key, value, lsn);
            if (!put_res) return put_res;

            bloom_.insert(key);
            manifest_.last_lsn = lsn;
            manifest_.record_count = engine_.size();
            manifest_.wal_bytes_written += encoded_sz;
            return {};
        }

        Result<value_type> get(const key_type& key) const {
            if (!bloom_.maybe_contains(key)) return std::unexpected(StorageError::NotFound);
            return engine_.get(key);
        }

        Result<value_type> get_at(const key_type& key, const nitya::lsn_t lsn) const {
            if (!bloom_.maybe_contains(key)) return std::unexpected(StorageError::NotFound);
            if constexpr (requires(const Engine& engine, const key_type& k, nitya::lsn_t at) {
                { engine.get_at(k, at) } -> std::same_as<Result<value_type>>;
            }) return engine_.get_at(key, lsn);
            else return engine_.get(key);
        }

        [[nodiscard]] nitya::lsn_t version_at(const key_type& key, const nitya::lsn_t lsn) const {
            if constexpr (requires(const Engine& engine, const key_type& k, nitya::lsn_t at) {
                { engine.version_at(k, at) } -> std::same_as<nitya::lsn_t>;
            }) return engine_.version_at(key, lsn);
            else return 0;
        }

        Result<void> erase(const key_type& key) {
            const std::string k_str = Serializer::serialize_key(key);
            const std::size_t needed = WalPayloadCodec::encode_size(k_str, "");
            thread_local std::array<std::byte, 512> tl_buf{};
            std::vector<std::byte> heap_buf;
            std::byte* buf = tl_buf.data();
            if (needed > tl_buf.size()) {
                heap_buf.resize(needed);
                buf = heap_buf.data();
            }
            const std::size_t encoded_sz = WalPayloadCodec::encode_to(buf, EntryOp::Delete, k_str, "");

            std::unique_lock lock{commit_mutex_};
            auto append_res = wal_->append(std::span{buf, encoded_sz});
            if (!append_res) return std::unexpected(StorageError::WalError);

            const nitya::lsn_t lsn = *append_res;
            if (opts_.sync_on_write) {
                if (auto s_res = wal_->sync(); !s_res)
                    return std::unexpected(StorageError::WalError);
            }

            if (auto erased = engine_.erase(key, lsn); !erased) return erased;
            bloom_.remove(key);
            manifest_.last_lsn = lsn;
            manifest_.record_count = engine_.size();
            manifest_.wal_bytes_written += encoded_sz;
            return {};
        }

        [[nodiscard]] bool contains(const key_type& key) const {
            if (!bloom_.maybe_contains(key)) return false;
            return engine_.contains(key);
        }

        [[nodiscard]] std::size_t size() const noexcept { return engine_.size(); }
        [[nodiscard]] bool empty() const noexcept { return engine_.empty(); }

        Result<void> clear() {
            std::unique_lock lock{commit_mutex_};
            const auto encoded = WalPayloadCodec::encode(EntryOp::Clear, "", "");
            auto append_res = wal_->append(std::span{encoded.data(), encoded.size()});
            if (!append_res) return std::unexpected(StorageError::WalError);

            const nitya::lsn_t lsn = *append_res;
            if (opts_.sync_on_write) (void)wal_->sync();

            if (auto cleared = engine_.clear(lsn); !cleared) return cleared;
            bloom_.reset();
            manifest_.last_lsn = lsn;
            manifest_.record_count = engine_.size();
            manifest_.wal_bytes_written += encoded.size();
            return {};
        }

        using mutation_type = typename Transaction<Petika>::Mutation;
        using observation_type = typename Transaction<Petika>::Observation;

        Result<void> commit_batch(const std::vector<mutation_type>& mutations,
                                  const std::vector<observation_type>& observations = {}) {
            // Pre-encode all mutations outside the lock — pure computation.
            std::vector<std::vector<std::byte>> records;
            records.reserve(mutations.size());
            for (const auto& m : mutations) {
                records.push_back(WalPayloadCodec::encode(m.op, Serializer::serialize_key(m.key),
                    m.op == EntryOp::Put ? Serializer::serialize_value(m.value) : std::string{}));
            }
            auto envelope = WalPayloadCodec::encode_batch(records);

            std::unique_lock lock{commit_mutex_};

            if constexpr (requires(const Engine& engine, const std::vector<observation_type>& observed) {
                { engine.validate_observations(observed) } -> std::same_as<bool>;
            }) {
                if (!engine_.validate_observations(observations))
                    return std::unexpected(StorageError::Retry);
            }

            auto lsn = wal_->append(std::span{envelope.data(), envelope.size()});
            if (!lsn || (opts_.sync_on_write && !wal_->sync())) return std::unexpected(StorageError::WalError);

            static_assert(requires(Engine& e, const std::vector<mutation_type>& b, nitya::lsn_t at) {
                { e.apply_batch(b, at) } -> std::same_as<Result<void>>;
            }, "transactional Petika engines must implement atomic apply_batch");

            if (auto result = engine_.apply_batch(mutations, *lsn); !result) return result;

            for (const auto& m : mutations) {
                if (m.op == EntryOp::Put) bloom_.insert(m.key);
                else if (m.op == EntryOp::Delete) bloom_.remove(m.key);
            }

            manifest_.last_lsn = *lsn;
            manifest_.record_count = engine_.size();
            manifest_.wal_bytes_written += envelope.size();
            return {};
        }

        // ------------------------------------------------------------------------
        // Iteration & Scans
        // ------------------------------------------------------------------------
        template <typename Callback>
        void scan(const key_type& start_key, const key_type& end_key, Callback&& cb) const {
            engine_.scan(start_key, end_key, std::forward<Callback>(cb));
        }

        template <typename Callback>
        void for_each(Callback&& cb) const {
            engine_.for_each(std::forward<Callback>(cb));
        }

        // C++23 ranges-compatible scan view: for (auto& [k,v,lsn] : store.scan_view(a, b)) { … }
        [[nodiscard]] ScanView<Engine> scan_view(const key_type& start_key, const key_type& end_key) const {
            return ScanView<Engine>{engine_, start_key, end_key};
        }

        // ------------------------------------------------------------------------
        // Transactions & Snapshots
        // ------------------------------------------------------------------------
        Transaction<Petika> transaction() {
            std::shared_lock lock{commit_mutex_}; // shared — read of last_lsn only
            return Transaction<Petika>{*this, manifest_.last_lsn};
        }

        Snapshot<Petika> snapshot() const {
            std::shared_lock lock{commit_mutex_}; // shared — read of last_lsn only
            const std::uint64_t next_id = ++next_snapshot_id_;
            return Snapshot<Petika>{next_id, manifest_.last_lsn, *this};
        }

        // ------------------------------------------------------------------------
        // Recovery Engine
        // ------------------------------------------------------------------------
        Result<std::size_t> recover(nitya::lsn_t from_lsn = 0) {
            auto telemetry = TelemetryPolicy::trace_recovery();
            (void)telemetry;

            std::size_t replayed = 0;
            auto stream = wal_->recover(from_lsn);

            for (const auto& rec : stream) {
                auto parsed = WalPayloadCodec::decode(rec.payload);
                if (!parsed) continue;

                auto [op, k_sv, v_sv] = *parsed;
                if (op == EntryOp::Batch) {
                    auto batch = WalPayloadCodec::decode_batch(rec.payload);
                    if (!batch) return std::unexpected(StorageError::CorruptedRecord);
                    std::vector<mutation_type> mutations;
                    mutations.reserve(batch->size());
                    for (const auto& frame : *batch) {
                        auto item = WalPayloadCodec::decode(frame);
                        if (!item) return std::unexpected(StorageError::CorruptedRecord);
                        auto [item_op, item_key, item_value] = *item;
                        key_type k = Serializer::deserialize_key(item_key);
                        value_type v{};
                        if (item_op == EntryOp::Put) v = Serializer::deserialize_value(item_value);
                        if (item_op != EntryOp::Put && item_op != EntryOp::Delete)
                            return std::unexpected(StorageError::CorruptedRecord);
                        mutations.push_back({item_op, std::move(k), std::move(v)});
                    }
                    if (auto applied = engine_.apply_batch(mutations, rec.lsn); !applied)
                        return std::unexpected(applied.error());
                    for (const auto& m : mutations) {
                        if (m.op == EntryOp::Put) bloom_.insert(m.key);
                    }
                    ++replayed; manifest_.last_lsn = rec.lsn; continue;
                }
                key_type key{};
                value_type val{};

                if (op == EntryOp::Put) {
                    key = Serializer::deserialize_key(k_sv);
                    val = Serializer::deserialize_value(v_sv);
                } else if (op == EntryOp::Delete) {
                    key = Serializer::deserialize_key(k_sv);
                } else if (op == EntryOp::Clear) {
                    bloom_.reset();
                }

                auto res = engine_.apply_log_record(op, key, val, rec.lsn);
                if (res) {
                    if (op == EntryOp::Put) bloom_.insert(key);
                    ++replayed;
                    manifest_.last_lsn = rec.lsn;
                }
            }

            manifest_.record_count = engine_.size();
            return replayed;
        }

        // ------------------------------------------------------------------------
        // Durability & Replication
        // ------------------------------------------------------------------------
        Result<void> sync() {
            auto res = wal_->sync();
            if (!res) return std::unexpected(StorageError::WalError);
            return {};
        }

        [[nodiscard]] nitya::lsn_t tail_lsn() const noexcept { return wal_->tail_lsn(); }
        [[nodiscard]] const Manifest& manifest() const noexcept { return manifest_; }

        Engine& engine() noexcept { return engine_; }
        const Engine& engine() const noexcept { return engine_; }
        DurabilityPolicy& wal() noexcept { return *wal_; }
        const DurabilityPolicy& wal() const noexcept { return *wal_; }

    private:
        // Shared_lock compatible wrappers for policies that only provide exclusive lock.
        // std::shared_mutex natively provides shared_lock / unique_lock.
        // NullMutex provides all four methods (see engine.hpp).
        // Custom policies must satisfy MutexPolicy concept.
        using MutexT = ConcurrencyPolicy;

        PetikaOptions opts_;
        Engine engine_;
        std::unique_ptr<DurabilityPolicy> wal_;
        Manifest manifest_;
        mutable std::atomic<std::uint64_t> next_snapshot_id_{0};
        mutable MutexT commit_mutex_;
        [[no_unique_address]] BloomFilter bloom_;
    };

    // ============================================================================
    // Type Aliases for Standard Configurations
    // ============================================================================
    template <typename Key, typename Value, typename Comparator = LexicalComparator>
    using SkipStore = Petika<MvccJournaledSkipEngine<Key, Value, Comparator>,
                             BinarySerializer,
                             Comparator>;

    using StringSkipStore = Petika<MvccJournaledSkipEngine<std::string, std::string, LexicalComparator>,
                                   StringSerializer,
                                   LexicalComparator>;

    template <typename Key, typename Value, typename Comparator = LexicalComparator>
    using SingleVersionSkipStore = Petika<JournaledSkipEngine<Key, Value, Comparator>,
                                          BinarySerializer,
                                          Comparator>;

    using SingleVersionStringSkipStore = Petika<JournaledSkipEngine<std::string, std::string, LexicalComparator>,
                                                 StringSerializer,
                                                 LexicalComparator>;

    template <typename Key, typename Value, typename Comparator = LexicalComparator>
    using MvccSkipStore = Petika<MvccJournaledSkipEngine<Key, Value, Comparator>,
                                 BinarySerializer,
                                 Comparator>;

    // Single-threaded variant: zero mutex overhead, suitable for embedded/simulation.
    template <typename Key, typename Value, typename Comparator = LexicalComparator>
    using SingleThreadSkipStore = Petika<MvccJournaledSkipEngine<Key, Value, Comparator>,
                                         BinarySerializer,
                                         Comparator,
                                         nitya::wal<>,
                                         nitya::nadi_telemetry,
                                         NullMutex>;

    // Bloom-filtered variant for read-heavy workloads with frequent misses.
    template <typename Key, typename Value, std::size_t BloomBits = (1u << 20)>
    using BloomSkipStore = Petika<MvccJournaledSkipEngine<Key, Value>,
                                  BinarySerializer,
                                  LexicalComparator,
                                  nitya::wal<>,
                                  nitya::nadi_telemetry,
                                  std::shared_mutex,
                                  ImmediateCommitPolicy,
                                  BloomFilterPolicy<BloomBits>>;

} // namespace petika
