#pragma once
// ============================================================================
// petika/engines/btree_engine.hpp — B+Tree-backed Petika Storage Engine
// ============================================================================
// Thin adapter binding pebble::containers::BPlusMap to the petika::StorageEngine
// and petika::BatchEngine concepts. The B+ tree is a *container* (its surface is
// insert_or_assign/at/find/erase/scan); this adapter wraps it in the LSN-aware
// put/get/erase/apply_log_record contract Petika drives.
//
// Unlike MvccJournaledSkipEngine, a B+ tree keeps a single live version per key
// (no MVCC history), so:
//   - there is no per-entry LSN to report; EntryView.lsn is a 0 sentinel;
//   - there is no snapshot GC to run, so prune() is intentionally absent;
//   - the leaf chain is a lock-free forward cursor, so scan_lazy() can yield a
//     genuine std::generator (see § Lazy scan) — the eager fallback is unused.
//
// Recovery fast-path: rebuild from a sorted checkpoint via from_sorted() in O(N)
// rather than replaying per-record put() (§6.2 of docs/containers/bplus_tree.md).
// ============================================================================

#include "containers/tree/bplus_tree.hpp"
#include "petika/engine.hpp"

#include <functional>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

#if defined(__cpp_lib_generator) && __cpp_lib_generator >= 202207L
#include <generator>
#endif

namespace petika {

    // BTreeEngine — StorageEngine/BatchEngine over BPlusMap.
    //
    // The LSN arguments satisfy the engine contract but are not persisted per key
    // (the B+ tree has no version column); durability/ordering is owned by
    // Petika's DurabilityPolicy (Nitya WAL). `Traits`/`Allocator` forward straight
    // to BPlusMap so a Smriti arena tier (make_smriti_bplus_map, §6.3) can be
    // dropped in by supplying the arena-bound allocator.
    template <
        typename Key,
        typename Value,
        typename Comparator = std::less<Key>,
        typename Traits = pebble::containers::DefaultBPlusTreeTraits<Key, Value>,
        typename Allocator = std::allocator<std::pair<const Key, Value>>
    >
    class BTreeEngine {
    public:
        using key_type = Key;
        using value_type = Value;
        using comparator_type = Comparator;
        using map_type = pebble::containers::BPlusTree<Key, Value, Comparator, Traits, Allocator>;

        // Reported entry for scan/for_each. lsn is a sentinel (B+ tree keeps no
        // per-key version); kept in the shape ScanView/other engines expect.
        struct EntryView {
            const Key& key;
            const Value& value;
            nitya::lsn_t lsn;
        };

        BTreeEngine() = default;
        explicit BTreeEngine(map_type index) : index_{std::move(index)} {}

        // Construct from a sorted, unique checkpoint in O(N) (recovery fast-path).
        template <std::forward_iterator ForwardIt>
        [[nodiscard]] static BTreeEngine from_sorted(ForwardIt first, ForwardIt last,
                                                     const Comparator& comp = Comparator{},
                                                     const Allocator& alloc = Allocator{}) {
            return BTreeEngine{map_type::from_sorted(first, last, comp, alloc)};
        }

        Result<void> put(const Key& key, const Value& value, nitya::lsn_t /*lsn*/) {
            index_.insert_or_assign(key, value);
            return {};
        }

        Result<Value> get(const Key& key) const {
            auto it = index_.find(key);
            if (it == index_.end()) return std::unexpected(StorageError::NotFound);
            return it->second;
        }

        Result<void> erase(const Key& key, nitya::lsn_t /*lsn*/) {
            return index_.erase(key) ? Result<void>{}
                                     : std::unexpected(StorageError::NotFound);
        }

        [[nodiscard]] bool contains(const Key& key) const { return index_.contains(key); }
        [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
        [[nodiscard]] bool empty() const noexcept { return index_.empty(); }

        Result<void> clear(nitya::lsn_t /*lsn*/) {
            index_.clear();
            return {};
        }

        // Atomic bulk apply. The B+ tree has no transactional rollback, so this
        // validates deletes up front (mirroring MvccJournaledSkipEngine) before
        // mutating, keeping the failure surface consistent across engines.
        template <class Range>
        Result<void> apply_batch(const Range& mutations, nitya::lsn_t /*lsn*/) {
            // Validate every Delete targets a key present now or inserted earlier
            // in this same batch; reject unknown ops before any mutation.
            std::vector<const Key*> batch_inserts;
            for (const auto& m : mutations) {
                if (m.op == EntryOp::Put) {
                    batch_inserts.push_back(&m.key);
                } else if (m.op == EntryOp::Delete) {
                    const bool from_batch = std::ranges::any_of(
                        batch_inserts, [&](const Key* k) { return equivalent(*k, m.key); });
                    if (!from_batch && !index_.contains(m.key))
                        return std::unexpected(StorageError::NotFound);
                } else {
                    return std::unexpected(StorageError::InvalidArg);
                }
            }
            for (const auto& m : mutations) {
                if (m.op == EntryOp::Put) index_.insert_or_assign(m.key, m.value);
                else                      index_.erase(m.key);
            }
            return {};
        }

        // Half-open range [first, last) — exclusive upper bound, matching the
        // MVCC engine's scan semantics (Petika::scan_view documents [start, end)).
        template <class Callback>
        void scan(const Key& first, const Key& last, Callback&& callback) const {
            const comparator_type less{};
            for (auto it = index_.lower_bound(first); it != index_.end(); ++it) {
                if (!less(it->first, last)) break; // it->first >= last → stop
                callback(EntryView{it->first, it->second, kNoLsn});
            }
        }

        template <class Callback>
        void for_each(Callback&& callback) const {
            index_.scan_all([&](const Key& k, const Value& v) {
                callback(EntryView{k, v, kNoLsn});
            });
        }

#if defined(__cpp_lib_generator) && __cpp_lib_generator >= 202207L
        // § Lazy scan — genuine pull-based cursor over the leaf chain.
        // The B+ tree iterator walks the linked leaves with no lock held, so the
        // coroutine can suspend between elements safely: iterating and breaking
        // early stops the walk at the consumed prefix. `Out` is the caller's
        // element type (ScanView::OwnedEntry) so the generator element type is
        // shared across the view/engine boundary.
        //
        // Precondition: the tree is not mutated during iteration (single live
        // version, no snapshot isolation).
        template <typename Out>
        [[nodiscard]] std::generator<Out> scan_lazy(const Key& first, const Key& last) const {
            const comparator_type less{};
            for (auto it = index_.lower_bound(first); it != index_.end(); ++it) {
                if (!less(it->first, last)) break; // it->first >= last → stop (half-open)
                co_yield Out{it->first, it->second, kNoLsn};
            }
        }
#endif

        Result<void> apply_log_record(EntryOp op, const Key& key, const Value& value, nitya::lsn_t lsn) {
            switch (op) {
            case EntryOp::Put:    return put(key, value, lsn);
            case EntryOp::Delete: return erase_for_replay(key, lsn);
            case EntryOp::Clear:  return clear(lsn);
            case EntryOp::Batch:  return std::unexpected(StorageError::NotSupported);
            }
            return std::unexpected(StorageError::InvalidArg);
        }

        // Expose the underlying map for recovery/bulk-build callers.
        [[nodiscard]] map_type& index() noexcept { return index_; }
        [[nodiscard]] const map_type& index() const noexcept { return index_; }

    private:
        static constexpr nitya::lsn_t kNoLsn{0};

        [[nodiscard]] bool equivalent(const Key& l, const Key& r) const {
            const comparator_type compare{};
            return !compare(l, r) && !compare(r, l);
        }

        // Replay-side delete is idempotent: a missing key is not an error during
        // recovery (the log may re-delete an already-absent key).
        Result<void> erase_for_replay(const Key& key, nitya::lsn_t /*lsn*/) {
            index_.erase(key);
            return {};
        }

        map_type index_;
    };

} // namespace petika
