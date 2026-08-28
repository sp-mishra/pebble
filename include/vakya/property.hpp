#pragma once

// vakya/property.hpp — Property System: a lazy, typed, open metadata sidecar
// for Vākya structural representations.
//
// Opt-in via:  #include "vakya/property.hpp"
// Namespace:   vakya  (property_key / property_set / property_store)
//
// Motivation: passes and downstream frameworks need to attach source-locs,
// shapes, backend-hints, and (future) feature vectors to nodes WITHOUT
// polluting the AST or forcing every node to pay for storage it never uses.
//
// Design:
//   - Nodes stay POD. Metadata lives in an EXTERNAL property_store keyed by
//     structural_hash_t (the same overlay pattern pravaha_hetero uses for its
//     execution_context). Zero cost when no property set exists for a key.
//   - property_set<InlineBytes> is a type-erased small-map: SmallVector inline
//     slots, spills to heap only past the inline budget (InlineBytes is a
//     template param, default 24). No virtual — non-trivial payloads are
//     managed through a stateless function-pointer vtable-of-one generated
//     from the payload type (move + destroy thunks).
//   - property_key<T, Name> is an NTTP-named, phantom-typed key; its id is a
//     compile-time hash of the name, so get/set are type-safe and collision
//     domains are stable across translation units.
//   - property_store is thread-safe: shared_mutex protects the hash map
//     (shared_lock for reads, unique_lock for writes).
//
// Constraints: C++23, header-only, no virtual, no macros, pay-for-what-you-use.
// Zero upward dependency: relies only on vakya.hpp + one container.

#include "vakya.hpp"
#include "../containers/dynamic/SmallVector.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace vakya {
    // -----------------------------------------------------------------------
    // fixed_string — local, structural NTTP string.
    //   Vākya cannot depend on lithe::fixed_string (that would reverse the
    //   dependency DAG), so it carries its own minimal copy. Value-based
    //   equality + literal type ⇒ usable as a template parameter.
    // -----------------------------------------------------------------------
    template <std::size_t N>
    struct fixed_string {
        char data[N]{};

        consteval fixed_string(const char (&src)[N]) noexcept {
            for (std::size_t i = 0; i < N; ++i) data[i] = src[i];
        }

        [[nodiscard]] constexpr std::string_view view() const noexcept {
            return {data, N - 1};
        }

        [[nodiscard]] constexpr std::size_t size() const noexcept { return N - 1; }

        constexpr bool operator==(const fixed_string&) const noexcept = default;

        template <std::size_t M>
        constexpr bool operator==(const fixed_string<M>&) const noexcept { return false; }
    };

    template <std::size_t N>
    fixed_string(const char (&)[N]) -> fixed_string<N>;

    // Property key id: a stable FNV-1a fold of the key name. Same string ⇒ same
    // id across TUs; different strings collide only with negligible probability.
    using property_id_t = std::uint64_t;

    [[nodiscard]] constexpr property_id_t property_hash(std::string_view s) noexcept {
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for (const char c : s) {
            h ^= static_cast<std::uint8_t>(c);
            h *= 0x100000001b3ULL;
        }
        return h;
    }

    // -----------------------------------------------------------------------
    // property_key<T, Name> — typed, NTTP-named metadata key.
    //   T is the phantom payload type; Name gives the stable id. Two keys are
    //   the same property iff their names hash equal (T is checked at get/set).
    // -----------------------------------------------------------------------
    template <class T, fixed_string Name>
    struct property_key {
        using value_type = T;
        static constexpr std::string_view name = Name.view();
        static constexpr property_id_t id = property_hash(Name.view());
    };

    // -----------------------------------------------------------------------
    // basic_property_set<InlineBytes> — type-erased small-map of {id → payload}.
    //   Trivial payloads are stored/relocated by raw bytes; non-trivial payloads
    //   carry a stateless op-table (move + destroy) generated from the type.
    //   No virtual: the op-table is a POD of function pointers.
    //
    //   InlineBytes controls the inline SBO budget. Default 24 bytes covers
    //   ints, pointers, small structs. Domains with larger inline payloads
    //   (e.g. 64-byte matrix shapes) can increase this without heap allocation.
    // -----------------------------------------------------------------------
    template <std::size_t InlineBytes = 24>
    class basic_property_set {
        struct ops_t {
            void (*move_construct)(void* dst, void* src) noexcept;
            void (*destroy)(void* p) noexcept;
        };

        struct slot {
            property_id_t id{};
            const ops_t* ops{}; // nullptr ⇒ trivial (bytes only)
            std::size_t size{}; // payload size in bytes
            bool heap{}; // storage is heap-owned
            union {
                unsigned char inline_buf[InlineBytes];
                void* heap_ptr;
            };

            slot() noexcept : inline_buf{} {}

            [[nodiscard]] void* storage() noexcept {
                return heap ? heap_ptr : static_cast<void*>(inline_buf);
            }

            [[nodiscard]] const void* storage() const noexcept {
                return heap ? heap_ptr : static_cast<const void*>(inline_buf);
            }
        };

        template <class T>
        static const ops_t* ops_for() noexcept {
            if constexpr (std::is_trivially_copyable_v<T>&&
                std::is_trivially_destructible_v<T>) {
                return nullptr; // trivial fast path — no thunks needed
            }
            else {
                static const ops_t table{
                    +[](void* dst, void* src) noexcept {
                        ::new(dst) T(std::move(*static_cast<T*>(src)));
                    },
                    +[](void* p) noexcept { static_cast<T*>(p)->~T(); }
                };
                return &table;
            }
        }

        void destroy_slot(slot& s) noexcept {
            if (s.ops) s.ops->destroy(s.storage());
            if (s.heap) ::operator delete(s.heap_ptr);
        }

        template <class T, class U>
        void emplace(slot& s, property_id_t id, U&& value) {
            s.id = id;
            s.ops = ops_for<T>();
            s.size = sizeof(T);
            if constexpr (sizeof(T) <= InlineBytes && alignof(T) <= alignof(std::max_align_t)) {
                s.heap = false;
                ::new(static_cast<void*>(s.inline_buf)) T(std::forward<U>(value));
            }
            else {
                s.heap = true;
                s.heap_ptr = ::operator new(sizeof(T));
                ::new(s.heap_ptr) T(std::forward<U>(value));
            }
        }

        containers::dynamic::SmallVector<slot, 4 * sizeof(slot)> slots_;

        [[nodiscard]] slot* find_slot(property_id_t id) noexcept {
            for (auto& s : slots_) if (s.id == id) return &s;
            return nullptr;
        }

        [[nodiscard]] const slot* find_slot(property_id_t id) const noexcept {
            for (const auto& s : slots_) if (s.id == id) return &s;
            return nullptr;
        }

    public:
        basic_property_set() = default;

        basic_property_set(const basic_property_set&) = delete; // payloads are move-only
        basic_property_set& operator=(const basic_property_set&) = delete;

        basic_property_set(basic_property_set&&) = default;
        basic_property_set& operator=(basic_property_set&&) = default;

        ~basic_property_set() { for (auto& s : slots_) destroy_slot(s); }

        [[nodiscard]] bool empty() const noexcept { return slots_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

        template <class Key, class U>
        void set(U&& value) {
            using T = typename Key::value_type;
            static_assert(std::is_constructible_v<T, U&&>,
                          "vakya::property_set::set: value not convertible to key's type");
            if (slot* s = find_slot(Key::id)) {
                destroy_slot(*s);
                *s = slot{};
                emplace<T>(*s, Key::id, std::forward<U>(value));
                return;
            }
            slots_.emplace_back();
            emplace<T>(slots_.back(), Key::id, std::forward<U>(value));
        }

        template <class Key>
        [[nodiscard]] bool has() const noexcept {
            return find_slot(Key::id) != nullptr;
        }

        template <class Key>
        [[nodiscard]] auto get() -> std::optional<typename Key::value_type> {
            using T = typename Key::value_type;
            if (const slot* s = find_slot(Key::id))
                return *static_cast<const T*>(s->storage());
            return std::nullopt;
        }

        template <class Key>
        [[nodiscard]] typename Key::value_type* get_if() noexcept {
            using T = typename Key::value_type;
            if (slot* s = find_slot(Key::id))
                return static_cast<T*>(s->storage());
            return nullptr;
        }

        template <class Key>
        [[nodiscard]] const typename Key::value_type* get_if() const noexcept {
            using T = typename Key::value_type;
            if (const slot* s = find_slot(Key::id))
                return static_cast<const T*>(s->storage());
            return nullptr;
        }

        template <class Key>
        bool erase() noexcept {
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                if (slots_[i].id == Key::id) {
                    destroy_slot(slots_[i]);
                    slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(i));
                    return true;
                }
            }
            return false;
        }
    };

    // -----------------------------------------------------------------------
    // property_set — default alias: basic_property_set<24>.
    //   Existing code using `property_set ps;` / `property_set<Key>` continues
    //   to compile unchanged. Use basic_property_set<N> for a custom budget.
    // -----------------------------------------------------------------------
    using property_set = basic_property_set<>;

    // -----------------------------------------------------------------------
    // property_store — the external overlay. Owns property_set per structural
    // key; nodes stay POD. Absent-key lookup is a single hash probe returning
    // nullptr. attach()/ensure() create on demand.
    //
    // Thread safety: shared_mutex guards all map access.
    //   - find / contains / size: shared_lock (concurrent reads).
    //   - ensure / update_for / clear: unique_lock (exclusive write).
    //
    // IMPORTANT — reference invalidation hazard:
    //   ensure() returns a property_set& that is valid only for the duration
    //   of the caller's exclusive-lock window. The returned reference MUST NOT
    //   be stored across a point where another thread could call ensure() or
    //   clear(), because std::unordered_map rehash invalidates all references.
    //   Single-threaded callers are unaffected.
    //   For multi-threaded callers that need to mutate a slot safely, use
    //   update_for(key, fn) instead — it holds the lock for the entire call.
    // -----------------------------------------------------------------------
    class property_store {
        mutable std::shared_mutex mutex_;
        std::unordered_map<structural_hash_t, property_set> map_;

    public:
        property_store() = default;

        // Non-creating lookup. nullptr when nothing is attached to this key.
        [[nodiscard]] property_set* find(structural_hash_t key) noexcept {
            std::shared_lock lock(mutex_);
            auto it = map_.find(key);
            return it == map_.end() ? nullptr : &it->second;
        }

        [[nodiscard]] const property_set* find(structural_hash_t key) const noexcept {
            std::shared_lock lock(mutex_);
            auto it = map_.find(key);
            return it == map_.end() ? nullptr : &it->second;
        }

        // Creating accessor — returns a reference valid only until the next
        // concurrent ensure/clear (see reference-invalidation note above).
        // Prefer update_for() for multi-threaded mutation.
        [[nodiscard, deprecated("Reference may dangle on rehash. Use update_for() for thread-safe mutation.")]]
        property_set& ensure(structural_hash_t key) {
            std::unique_lock lock(mutex_);
            return map_[key];
        }

        // Thread-safe mutation under a held exclusive lock.
        // fn receives a property_set& and may call set/get/erase on it.
        // The map is NOT rehashed while fn runs because no other ensure/clear
        // can run concurrently.
        template <class Fn>
        void update_for(structural_hash_t key, Fn&& fn) {
            std::unique_lock lock(mutex_);
            fn(map_[key]);
        }

        // Structural-key convenience overloads: attach by expression.
        template <class Expr>
        [[nodiscard, deprecated("Reference may dangle on rehash. Use update_for() for thread-safe mutation.")]]
        property_set& ensure_for(const Expr& e) {
            std::unique_lock lock(mutex_);
            return map_[structural_key(e)];
        }

        template <class Expr>
        [[nodiscard]] property_set* find_for(const Expr& e) noexcept {
            return find(structural_key(e));
        }

        // update_for by expression: fn(property_set&) under exclusive lock.
        template <class Expr, class Fn>
        void update_for(const Expr& e, Fn&& fn) {
            update_for(structural_key(e), std::forward<Fn>(fn));
        }

        [[nodiscard]] bool contains(structural_hash_t key) const noexcept {
            std::shared_lock lock(mutex_);
            return map_.contains(key);
        }

        [[nodiscard]] std::size_t size() const noexcept {
            std::shared_lock lock(mutex_);
            return map_.size();
        }

        void clear() noexcept {
            std::unique_lock lock(mutex_);
            map_.clear();
        }
    };
} // namespace vakya
