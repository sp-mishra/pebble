#pragma once

// =============================================================================
// containers/descriptor_registry.hpp — generic descriptor registry
//
// C++23, header-only, no virtual, no macros.
// Namespace: containers
//
// A compile-time descriptor + runtime discovery/index table.
// Shape mirrors rule_registry: stable generational handles via slot_map,
// two SparseSet indices (id → handle, category → [handles]).
//
// RegistrableDescriptor<D> requires:
//   D::stable_id  (uint32_t, builtin < kExtensionIdBase, ext >= kExtensionIdBase)
//   D::name_hash  (uint64_t, FNV-1a of the descriptor's name)
//   D::category   (some unsigned-backed enum)
//
// API: register_desc / find(id) / find_by_name(name_hash) / by_category / discover
// An empty registry allocates nothing; first register() triggers allocation.
// =============================================================================

#include "containers/associative/slot_map.hpp"
#include "containers/associative/SparseSet.hpp"
#include "containers/handle/generational_handle.hpp"

#include <concepts>
#include <string_view>
#include <span>
#include <unordered_map>
#include <vector>

namespace containers {
    // ============================================================================
    // Extension-id base (mirrors emit::kExtensionIdBase in Vākya)
    // ============================================================================
    inline constexpr std::uint32_t kDescRegistryExtensionBase = 1000u;

    // ============================================================================
    // RegistrableDescriptor concept
    // ============================================================================

    template <class D>
    concept RegistrableDescriptor = requires {
        { D::stable_id } -> std::convertible_to<std::uint32_t>;
        { D::name_hash } -> std::convertible_to<std::uint64_t>;
        requires std::is_enum_v<decltype(D::category)> ||
        std::is_integral_v<decltype(D::category)>;
    };

    // ============================================================================
    // descriptor_handle — stable generational handle into a descriptor_registry
    // ============================================================================

    struct descriptor_registry_tag {};

    using descriptor_handle = generational_handle<descriptor_registry_tag, std::uint32_t>;

    // ============================================================================
    // descriptor_registry<Desc>
    //
    // Desc must satisfy RegistrableDescriptor.
    // category_type = decltype(Desc::category) — must be SparseKey-compatible
    //   (unsigned integer or unsigned-backed enum).
    // ============================================================================

    template <RegistrableDescriptor Desc>
    class descriptor_registry {
    public:
        using desc_type = Desc;
        using category_type = decltype(Desc::category);
        using handle_type = descriptor_handle;

        descriptor_registry() = default;

        explicit descriptor_registry(const std::size_t universe_capacity)
            : id_to_handle_(universe_capacity) {}

        // -------------------------------------------------------------------------
        // register_desc — insert a descriptor; returns its stable handle.
        // Duplicate stable_id overwrites the existing entry and reindexes.
        // -------------------------------------------------------------------------
        descriptor_handle register_desc(Desc d) {
            const std::uint32_t id = d.stable_id;
            const std::uint64_t nh = d.name_hash;
            const auto cat = d.category;

            // Check if already present via O(1) single-pass SparseSet lookup
            if (const auto res = id_to_handle_.get(id)) {
                const descriptor_handle existing = res->get();
                if (Desc* ptr = store_.find(existing)) {
                    if (ptr->name_hash != nh) {
                        name_to_handle_.erase(ptr->name_hash);
                    }
                    if (ptr->category != cat) {
                        const std::size_t old_cat_idx = cat_to_index(ptr->category);
                        if (old_cat_idx < cat_buckets_.size()) {
                            std::erase(cat_buckets_[old_cat_idx], existing);
                        }
                        const std::size_t new_cat_idx = cat_to_index(cat);
                        if (new_cat_idx >= cat_buckets_.size())
                            cat_buckets_.resize(new_cat_idx + 1);
                        cat_buckets_[new_cat_idx].push_back(existing);
                    }
                    *ptr = std::move(d);
                }
                name_to_handle_[nh] = existing;
                return existing;
            }

            const descriptor_handle h = store_.insert(std::move(d));
            id_to_handle_.insert_or_update(id, h);
            name_to_handle_[nh] = h;

            // Category index: grow bucket vector if needed
            const std::size_t cat_idx = cat_to_index(cat);
            if (cat_idx >= cat_buckets_.size())
                cat_buckets_.resize(cat_idx + 1);
            cat_buckets_[cat_idx].push_back(h);

            return h;
        }

        // -------------------------------------------------------------------------
        // find — O(1) lookup by stable_id; returns nullptr if absent or stale.
        // -------------------------------------------------------------------------
        [[nodiscard]] const Desc* find(const std::uint32_t id) const noexcept {
            const auto res = id_to_handle_.get(id);
            if (!res) return nullptr;
            return store_.find(res->get());
        }

        [[nodiscard]] Desc* find(const std::uint32_t id) noexcept {
            const auto res = id_to_handle_.get(id);
            if (!res) return nullptr;
            return store_.find(res->get());
        }

        // -------------------------------------------------------------------------
        // find_by_name — O(1) lookup by name_hash; returns nullptr if absent or stale.
        // -------------------------------------------------------------------------
        [[nodiscard]] const Desc* find_by_name(const std::uint64_t name_hash) const noexcept {
            const auto it = name_to_handle_.find(name_hash);
            if (it == name_to_handle_.end()) return nullptr;
            return store_.find(it->second);
        }

        [[nodiscard]] Desc* find_by_name(const std::uint64_t name_hash) noexcept {
            const auto it = name_to_handle_.find(name_hash);
            if (it == name_to_handle_.end()) return nullptr;
            return store_.find(it->second);
        }

        // -------------------------------------------------------------------------
        // find — O(1) lookup by descriptor_handle; returns nullptr if stale.
        // -------------------------------------------------------------------------
        [[nodiscard]] const Desc* find(descriptor_handle h) const noexcept {
            return store_.find(h);
        }

        [[nodiscard]] Desc* find(descriptor_handle h) noexcept {
            return store_.find(h);
        }

        // -------------------------------------------------------------------------
        // contains — O(1) membership test by stable_id.
        // -------------------------------------------------------------------------
        [[nodiscard]] bool contains(const std::uint32_t id) const noexcept {
            const auto res = id_to_handle_.get(id);
            return res.has_value() && store_.contains(res->get());
        }

        // -------------------------------------------------------------------------
        // contains — O(1) membership test by descriptor_handle.
        // -------------------------------------------------------------------------
        [[nodiscard]] bool contains(descriptor_handle h) const noexcept {
            return store_.contains(h);
        }

        // -------------------------------------------------------------------------
        // handle_for — returns the descriptor_handle for stable_id, or null handle.
        // -------------------------------------------------------------------------
        [[nodiscard]] descriptor_handle handle_for(const std::uint32_t id) const noexcept {
            const auto res = id_to_handle_.get(id);
            if (!res) return descriptor_handle{};
            return res->get();
        }

        // -------------------------------------------------------------------------
        // by_category — range view over all live descriptors in category c.
        // -------------------------------------------------------------------------
        [[nodiscard]] std::vector<const Desc*> by_category(category_type cat) const {
            std::vector<const Desc*> result;
            const std::size_t idx = cat_to_index(cat);
            if (idx >= cat_buckets_.size()) return result;
            for (const descriptor_handle h : cat_buckets_[idx]) {
                if (const Desc* p = store_.find(h)) result.push_back(p);
            }
            return result;
        }

        // -------------------------------------------------------------------------
        // discover — all registered descriptors (dense order)
        // -------------------------------------------------------------------------
        template <class Fn>
        void discover(Fn&& fn) const {
            for (auto ref : store_) {
                fn(ref.value);
            }
        }

        [[nodiscard]] std::vector<const Desc*> all() const {
            std::vector<const Desc*> result;
            for (auto ref : store_) result.push_back(&ref.value);
            return result;
        }

        [[nodiscard]] std::size_t size() const noexcept { return store_.size(); }
        [[nodiscard]] bool empty() const noexcept { return store_.empty(); }

        void clear() {
            store_.clear();
            id_to_handle_.clear();
            name_to_handle_.clear();
            cat_buckets_.clear();
        }

        void reserve(const std::size_t cap) {
            id_to_handle_.reserve(cap);
            name_to_handle_.reserve(cap);
        }

    private:
        static std::size_t cat_to_index(category_type c) noexcept {
            if constexpr (std::is_enum_v<category_type>)
                return static_cast<std::size_t>(
                    static_cast<std::underlying_type_t<category_type>>(c));
            else
                return static_cast<std::size_t>(c);
        }

        containers::slot_map<Desc, descriptor_handle> store_;
        pebble::containers::SparseSet<std::uint32_t, descriptor_handle> id_to_handle_;
        std::unordered_map<std::uint64_t, descriptor_handle> name_to_handle_;
        std::vector<std::vector<descriptor_handle>> cat_buckets_;
    };

    // ============================================================================
    // FNV-1a name hash helper (same algorithm as Vākya's property_key)
    // ============================================================================

    [[nodiscard]] constexpr std::uint64_t desc_name_hash(const std::string_view s) noexcept {
        std::uint64_t h = 14695981039346656037ULL;
        for (const char c : s) {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            h *= 1099511628211ULL;
        }
        return h;
    }
} // namespace containers

namespace pebble::containers {
    using ::containers::RegistrableDescriptor;
    using ::containers::descriptor_registry_tag;
    using ::containers::descriptor_handle;
    using ::containers::descriptor_registry;
    using ::containers::desc_name_hash;
    using ::containers::kDescRegistryExtensionBase;
} // namespace pebble::containers
