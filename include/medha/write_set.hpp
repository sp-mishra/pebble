#pragma once
// =============================================================================
// medha/write_set.hpp — per-attempt write set with read-your-writes lookup
//
// C++23, header-only, no virtual, no macros.
//
// Tracks staged writes: (canonical_key, base_version, staged_value/handle).
// Duplicate writes: last-wins staged value.
// Read-your-writes: lookup order is write_set → parent_write_set → resource.
// Value storage: inline bytes for trivially-copyable; staging_handle for resource-owned.
// =============================================================================

#include "medha/key.hpp"
#include "medha/value.hpp"
#include "medha/version.hpp"
#include "containers/dynamic/SmallVector.hpp"

#include <cstddef>

namespace medha {
    // ============================================================================
    // write_entry — one staged write
    // ============================================================================

    // Inline value storage: up to 64 bytes stored inline (covers most scalars/structs).
    inline constexpr std::size_t kInlineValueBytes = 64;

    struct write_entry {
        canonical_key key{};
        version_stamp base{}; // resource version at time of first read (ABA guard)
        version_stamp observed{}; // same as base initially; tracks RYW updates

        value_storage_kind storage = value_storage_kind::resource_owned;

        // Inline storage for trivially-copyable values.
        alignas(std::max_align_t) std::byte inline_bytes[kInlineValueBytes]{};
        std::size_t inline_size = 0;

        // Resource-owned staging handle.
        staging_handle handle{};

        // True if this entry was written in a nested scope (not yet merged to parent).
        bool nested = false;
    };

    // ============================================================================
    // write_set
    // ============================================================================

    class write_set {
    public:
        using container_type = containers::dynamic::SmallVector<write_entry, 512>;

        // Stage a trivially-copyable value inline.
        template <class V>
        void stage_inline(canonical_key key, version_stamp base, const V& value) {
            static_assert(sizeof(V) <= kInlineValueBytes,
                          "value too large for inline write_set storage");
            auto* e = find_or_insert(key, base);
            e->storage = value_storage_kind::inline_copy;
            e->inline_size = sizeof(V);
            __builtin_memcpy(e->inline_bytes, &value, sizeof(V));
        }

        // Stage via resource-owned handle.
        void stage_handle(canonical_key key, version_stamp base, staging_handle h) {
            auto* e = find_or_insert(key, base);
            e->storage = value_storage_kind::resource_owned;
            e->handle = h;
        }

        // Read-your-writes: returns pointer to inline bytes or handle if key is staged.
        [[nodiscard]] const write_entry* find(const canonical_key& key) const noexcept {
            for (const auto& e : entries_) {
                if (e.key == key) return &e;
            }
            return nullptr;
        }

        [[nodiscard]] write_entry* find(const canonical_key& key) noexcept {
            for (auto& e : entries_) {
                if (e.key == key) return &e;
            }
            return nullptr;
        }

        [[nodiscard]] const container_type& entries() const noexcept { return entries_; }
        [[nodiscard]] container_type& entries() noexcept { return entries_; }
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
        [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

        void clear() noexcept { entries_.clear(); }

        // Merge src into this (nested → parent on nested commit). Last-wins.
        void merge_from(const write_set& src) {
            for (const auto& e : src.entries_) {
                auto* existing = find(e.key);
                if (existing) {
                    *existing = e;
                }
                else {
                    entries_.push_back(e);
                }
            }
        }

    private:
        write_entry* find_or_insert(const canonical_key& key, version_stamp base) {
            for (auto& e : entries_) {
                if (e.key == key) return &e;
            }
            write_entry e{};
            e.key = key;
            e.base = base;
            e.observed = base;
            entries_.push_back(e);
            return &entries_.back();
        }

        container_type entries_;
    };
} // namespace medha
