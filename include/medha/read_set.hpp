#pragma once
// =============================================================================
// medha/read_set.hpp — per-attempt read set
//
// C++23, header-only, no virtual, no macros.
//
// Tracks (canonical_key, version_stamp, read_kind) for each key read during
// the attempt. Also tracks range and predicate reads as opaque descriptors
// (phantom prevention is the resource's responsibility for non-point reads).
// Backed by SmallVector (no heap on hot path for small transactions).
// Duplicate point reads: last-wins version record (§20.1).
//
// Serializable conflict detection in Medha core covers point reads only.
// For range/predicate/index reads, see read_kind in key.hpp.
// =============================================================================

#include "medha/key.hpp"
#include "medha/version.hpp"
#include "containers/dynamic/SmallVector.hpp"

#include <cstddef>

namespace medha {
    // ============================================================================
    // read_entry — one point-read entry in the read set
    // ============================================================================

    struct read_entry {
        canonical_key key{};
        version_stamp observed{}; // version at read time
        bool shadowed = false; // true if this key is also in the write set
        read_kind kind = read_kind::point;
    };

    // ============================================================================
    // range_read_entry — a range scan recorded for the read set
    // ============================================================================

    struct range_read_entry {
        range_key rkey{};
        version_stamp observed{}; // resource-provided stamp at scan time (may be 0)
    };

    // ============================================================================
    // predicate_read_entry — a predicate scan recorded for the read set
    // ============================================================================

    struct predicate_read_entry {
        predicate_key pkey{};
        version_stamp observed{}; // resource-provided stamp at scan time (may be 0)
    };

    // ============================================================================
    // read_set — SmallVector-backed, 256 bytes inline (4 entries for typical keys)
    // ============================================================================

    class read_set {
    public:
        using container_type = containers::dynamic::SmallVector<read_entry, 256>;
        using range_container_type = containers::dynamic::SmallVector<range_read_entry, 64>;
        using pred_container_type = containers::dynamic::SmallVector<predicate_read_entry, 64>;

        void record(canonical_key key, version_stamp vs,
                    read_kind kind = read_kind::point) {
            // last-wins: update existing entry if present
            for (auto& e : entries_) {
                if (e.key == key) {
                    e.observed = vs;
                    e.kind = kind;
                    return;
                }
            }
            entries_.push_back(read_entry{key, vs, false, kind});
        }

        void record_range(range_key rkey, version_stamp vs) {
            range_entries_.push_back(range_read_entry{rkey, vs});
        }

        void record_predicate(predicate_key pkey, version_stamp vs) {
            pred_entries_.push_back(predicate_read_entry{pkey, vs});
        }

        // Mark all entries whose key matches as shadowed (owned by write set).
        void mark_shadowed(const canonical_key& key) noexcept {
            for (auto& e : entries_) {
                if (e.key == key) {
                    e.shadowed = true;
                }
            }
        }

        [[nodiscard]] const container_type& entries() const noexcept { return entries_; }
        [[nodiscard]] container_type& entries() noexcept { return entries_; }
        [[nodiscard]] const range_container_type& range_entries() const noexcept { return range_entries_; }
        [[nodiscard]] const pred_container_type& pred_entries() const noexcept { return pred_entries_; }
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

        [[nodiscard]] bool empty() const noexcept {
            return entries_.empty() && range_entries_.empty() && pred_entries_.empty();
        }

        void clear() noexcept {
            entries_.clear();
            range_entries_.clear();
            pred_entries_.clear();
        }

        // Find observed version for a point key; returns nullptr if not read.
        [[nodiscard]] const version_stamp* find(const canonical_key& key) const noexcept {
            for (const auto& e : entries_) {
                if (e.key == key) return &e.observed;
            }
            return nullptr;
        }

    private:
        container_type entries_;
        range_container_type range_entries_;
        pred_container_type pred_entries_;
    };
} // namespace medha
