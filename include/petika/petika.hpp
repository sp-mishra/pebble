#pragma once
// ============================================================================
// petika/petika.hpp — Unified Engine-Agnostic Storage Platform for C++23
// ============================================================================
//
// Petika decouples the application storage API from underlying storage engines.
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
// Zero virtual functions, zero RTTI, zero macros, modern C++23.
// ============================================================================

#include "petika/engine.hpp"
#include "petika/serializer.hpp"
#include "petika/engines/journaled_skip_engine.hpp"
#include "nitya/nitya.hpp"
#include "utils/setu.hpp"
#include "mem/smriti.hpp"
#include "containers/lockfree/MPMCQueue.hpp"
#include "observability/nadi.hpp"
#include "rules/easy_rules.hpp"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

        explicit Transaction(StoreType& store) : store_{store} {}

        Result<void> put(key_type key, value_type val) {
            mutations_.emplace_back(EntryOp::Put, std::move(key), std::move(val));
            return {};
        }

        Result<void> erase(key_type key) {
            mutations_.emplace_back(EntryOp::Delete, std::move(key), value_type{});
            return {};
        }

        Result<value_type> get(const key_type& key) {
            // Search pending transaction mutations first (write-your-own-writes)
            for (auto it = mutations_.rbegin(); it != mutations_.rend(); ++it) {
                if (it->key == key) {
                    if (it->op == EntryOp::Delete) return std::unexpected(StorageError::NotFound);
                    return it->value;
                }
            }
            return store_.get(key);
        }

        Result<void> commit() {
            auto result = store_.commit_batch(mutations_);
            if (!result) return result;
            mutations_.clear();
            return {};
        }

        void abort() noexcept {
            mutations_.clear();
        }

    private:
        StoreType& store_;
        std::vector<Mutation> mutations_;
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
            return store_.get(key);
        }

    private:
        std::uint64_t id_;
        nitya::lsn_t snapshot_lsn_;
        const StoreType& store_;
    };

    // ============================================================================
    // Main Petika Storage Platform
    // ============================================================================
    template <
        typename Engine,
        typename Serializer = StringSerializer,
        typename Comparator = LexicalComparator,
        typename DurabilityPolicy = nitya::wal<>,
        typename TelemetryPolicy = nitya::nadi_telemetry>
    class Petika {
    public:
        using key_type = typename Engine::key_type;
        using value_type = typename Engine::value_type;
        using engine_type = Engine;
        using serializer_type = Serializer;
        using comparator_type = Comparator;
        // Owning scan value for adapters that retain work beyond the engine lock.
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
            std::unique_lock lock{commit_mutex_};
            auto telemetry = TelemetryPolicy::trace_publish();
            (void)telemetry;

            std::string k_str = Serializer::serialize_key(key);
            std::string v_str = Serializer::serialize_value(value);

            auto encoded = WalPayloadCodec::encode(EntryOp::Put, k_str, v_str);
            auto append_res = wal_->append(std::span{encoded.data(), encoded.size()});
            if (!append_res) return std::unexpected(StorageError::WalError);

            nitya::lsn_t lsn = *append_res;
            if (opts_.sync_on_write) {
                if (auto s_res = wal_->sync(); !s_res) {
                    return std::unexpected(StorageError::WalError);
                }
            }

            auto put_res = engine_.put(key, value, lsn);
            if (!put_res) return put_res;

            manifest_.last_lsn = lsn;
            manifest_.record_count = engine_.size();
            return {};
        }

        Result<value_type> get(const key_type& key) const {
            return engine_.get(key);
        }

        Result<void> erase(const key_type& key) {
            std::unique_lock lock{commit_mutex_};
            std::string k_str = Serializer::serialize_key(key);
            auto encoded = WalPayloadCodec::encode(EntryOp::Delete, k_str, "");

            auto append_res = wal_->append(std::span{encoded.data(), encoded.size()});
            if (!append_res) return std::unexpected(StorageError::WalError);

            nitya::lsn_t lsn = *append_res;
            if (opts_.sync_on_write) {
                if (auto s_res = wal_->sync(); !s_res) {
                    return std::unexpected(StorageError::WalError);
                }
            }

            return engine_.erase(key, lsn);
        }

        [[nodiscard]] bool contains(const key_type& key) const {
            return engine_.contains(key);
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return engine_.size();
        }

        [[nodiscard]] bool empty() const noexcept {
            return engine_.empty();
        }

        Result<void> clear() {
            std::unique_lock lock{commit_mutex_};
            auto encoded = WalPayloadCodec::encode(EntryOp::Clear, "", "");
            auto append_res = wal_->append(std::span{encoded.data(), encoded.size()});
            if (!append_res) return std::unexpected(StorageError::WalError);

            nitya::lsn_t lsn = *append_res;
            if (opts_.sync_on_write) {
                (void)wal_->sync();
            }

            return engine_.clear(lsn);
        }

        using mutation_type = typename Transaction<Petika>::Mutation;
        Result<void> commit_batch(const std::vector<mutation_type>& mutations) {
            std::unique_lock lock{commit_mutex_};
            std::vector<std::vector<std::byte>> records;
            records.reserve(mutations.size());
            for (const auto& m : mutations) {
                records.push_back(WalPayloadCodec::encode(m.op, Serializer::serialize_key(m.key),
                    m.op == EntryOp::Put ? Serializer::serialize_value(m.value) : std::string{}));
            }
            auto envelope = WalPayloadCodec::encode_batch(records);
            auto lsn = wal_->append(std::span{envelope.data(), envelope.size()});
            if (!lsn || (opts_.sync_on_write && !wal_->sync())) return std::unexpected(StorageError::WalError);
            static_assert(requires(Engine& e, const std::vector<mutation_type>& b, nitya::lsn_t at) {
                { e.apply_batch(b, at) } -> std::same_as<Result<void>>;
            }, "transactional Petika engines must implement atomic apply_batch");
            if (auto result = engine_.apply_batch(mutations, *lsn); !result) return result;
            manifest_.last_lsn = *lsn; manifest_.record_count = engine_.size();
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

        // ------------------------------------------------------------------------
        // Transactions & Snapshots
        // ------------------------------------------------------------------------
        Transaction<Petika> transaction() {
            return Transaction<Petika>{*this};
        }

        Snapshot<Petika> snapshot() const {
            std::uint64_t next_id = ++next_snapshot_id_;
            return Snapshot<Petika>{next_id, wal_->tail_lsn(), *this};
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
                        auto item = WalPayloadCodec::decode(frame); if (!item) return std::unexpected(StorageError::CorruptedRecord);
                        auto [item_op, item_key, item_value] = *item;
                        key_type k = Serializer::deserialize_key(item_key); value_type v{};
                        if (item_op == EntryOp::Put) v = Serializer::deserialize_value(item_value);
                        if (item_op != EntryOp::Put && item_op != EntryOp::Delete) return std::unexpected(StorageError::CorruptedRecord);
                        mutations.push_back({item_op, std::move(k), std::move(v)});
                    }
                    if (auto applied = engine_.apply_batch(mutations, rec.lsn); !applied) return std::unexpected(applied.error());
                    ++replayed; manifest_.last_lsn = rec.lsn; continue;
                }
                key_type key{};
                value_type val{};

                if (op == EntryOp::Put) {
                    key = Serializer::deserialize_key(k_sv);
                    val = Serializer::deserialize_value(v_sv);
                }
                else if (op == EntryOp::Delete) {
                    key = Serializer::deserialize_key(k_sv);
                }

                auto res = engine_.apply_log_record(op, key, val, rec.lsn);
                if (res) {
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

        [[nodiscard]] nitya::lsn_t tail_lsn() const noexcept {
            return wal_->tail_lsn();
        }

        [[nodiscard]] const Manifest& manifest() const noexcept {
            return manifest_;
        }

        Engine& engine() noexcept { return engine_; }
        const Engine& engine() const noexcept { return engine_; }
        DurabilityPolicy& wal() noexcept { return *wal_; }
        const DurabilityPolicy& wal() const noexcept { return *wal_; }

    private:
        Result<void> put_unlocked(const key_type& key, const value_type& value) {
            std::string k = Serializer::serialize_key(key), v = Serializer::serialize_value(value);
            auto payload = WalPayloadCodec::encode(EntryOp::Put, k, v);
            auto lsn = wal_->append(std::span{payload.data(), payload.size()});
            if (!lsn || (opts_.sync_on_write && !wal_->sync())) return std::unexpected(StorageError::WalError);
            return engine_.put(key, value, *lsn);
        }
        Result<void> erase_unlocked(const key_type& key) {
            std::string k = Serializer::serialize_key(key);
            auto payload = WalPayloadCodec::encode(EntryOp::Delete, k, "");
            auto lsn = wal_->append(std::span{payload.data(), payload.size()});
            if (!lsn || (opts_.sync_on_write && !wal_->sync())) return std::unexpected(StorageError::WalError);
            return engine_.erase(key, *lsn);
        }
        PetikaOptions opts_;
        Engine engine_;
        std::unique_ptr<DurabilityPolicy> wal_;
        Manifest manifest_;
        mutable std::atomic<std::uint64_t> next_snapshot_id_{0};
        mutable std::mutex commit_mutex_;
    };

    // ============================================================================
    // Type Aliases for Standard Configurations
    // ============================================================================
    template <typename Key, typename Value, typename Comparator = LexicalComparator>
    using SkipStore = Petika<JournaledSkipEngine<Key, Value, Comparator>,
                             BinarySerializer,
                             Comparator>;

    using StringSkipStore = Petika<JournaledSkipEngine<std::string, std::string, LexicalComparator>,
                                   StringSerializer,
                                   LexicalComparator>;
} // namespace petika
