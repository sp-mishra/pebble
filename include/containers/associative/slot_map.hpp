#pragma once

// =============================================================================
// containers/associative/slot_map.hpp — generic generational slot map
//
// Stable-address O(1) insert/erase/lookup keyed by generational_handle.
//
// Storage policies (template parameter StoragePolicy):
//
//   deque_storage        (default)
//     Backing: std::deque<slot> — push_back never moves live elements, so
//     raw pointers into the deque remain stable for the slot's lifetime.
//     Use when callers cache raw T* pointers obtained from find().
//
//   small_vector_storage<N>
//     Backing: SmallVector<slot, N*sizeof(slot)> — inline-first, spills to
//     heap when > N slots are live. Does NOT guarantee pointer stability on
//     reallocation. Safe when callers use only generational_handle (index-
//     based) and never cache raw pointers. Ideal for workspace entity stores
//     where entity counts are small and predictable (N=4 or N=8 is typical).
//
//   std_vector_storage
//     Backing: std::vector<slot> — heap-only, no inline budget. Compatible
//     with std::pmr allocators or any Allocator. Pointer stability NOT
//     guaranteed after reserve/push_back.
//
// CONCURRENCY: NO internal locking. The caller is responsible for
// synchronization (e.g. an external std::mutex or RWLock). This matches
// §5.3 of the Lithe execution design: the resource_store layers locking on top.
//
// Concepts and usage:
//
//   struct my_tag {};
//   using MyHandle = containers::generational_handle<my_tag>;
//
//   // Default (deque, stable pointers):
//   containers::slot_map<int, MyHandle> m;
//
//   // Inline-first (SmallVector, index-based):
//   containers::slot_map<int, MyHandle,
//       std::allocator<int>, containers::small_vector_storage<4>> m;
//
//   MyHandle h = m.insert(42);
//   int*     p = m.find(h);      // stable pointer (deque) or valid until push_back (sv)
//   m.erase(h);
//   assert(m.find(h) == nullptr); // generation mismatch → stale
//
// Dense iteration visits only live slots:
//   for (auto& [handle, value] : m) { ... }
//
// No virtual, no macros.  Header-only C++23.  Zero Lithe dependency.
// =============================================================================

#include <concepts>
#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

#include "containers/dynamic/SmallVector.hpp"
#include "containers/handle/generational_handle.hpp"

namespace containers {

    // =========================================================================
    // Storage policy tags
    // =========================================================================

    /// Heap-backed deque: stable element addresses. Default.
    struct deque_storage {};

    /// Inline-first SmallVector: N inline slots, spills to heap. Index-based only.
    template <std::size_t N>
    struct small_vector_storage {};

    /// Heap-backed std::vector. No inline budget. Any Allocator is supported.
    struct std_vector_storage {};

    // =========================================================================
    // slot_map<T, Handle, Allocator, StoragePolicy>
    // =========================================================================

    template <class T,
              class Handle        = generational_handle<T>,
              class Allocator     = std::allocator<T>,
              class StoragePolicy = deque_storage>
    class slot_map {
    public:
        using handle_type    = Handle;
        using index_type     = typename Handle::index_type;
        using value_type     = T;
        using allocator_type = Allocator;

    private:
        struct slot {
            std::optional<T> value;
            index_type generation = 0;
        };

        using SlotAlloc    = typename std::allocator_traits<Allocator>::template rebind_alloc<slot>;
        using FreeListAlloc = typename std::allocator_traits<Allocator>::template rebind_alloc<index_type>;

        // ---- backing storage selected at compile time ----
        using storage_type = std::conditional_t<
            std::is_same_v<StoragePolicy, deque_storage>,
            std::deque<slot, SlotAlloc>,
            std::conditional_t<
                std::is_same_v<StoragePolicy, std_vector_storage>,
                std::vector<slot, SlotAlloc>,
                // small_vector_storage<N>: inline N*sizeof(slot) bytes
                // We detect N via partial specialisation below; here we just
                // use a SmallVector with a fixed inline budget.
                void   // resolved via partial specialisation guard — see static_assert
            >
        >;

        // Detect small_vector_storage<N> and extract N
        template <class P>
        struct sv_inline_bytes_helper {
            static constexpr std::size_t value = 0; // not small_vector_storage
        };
        template <std::size_t N>
        struct sv_inline_bytes_helper<small_vector_storage<N>> {
            static constexpr std::size_t value = N * sizeof(slot);
        };

        static constexpr bool is_small_vector_policy =
            sv_inline_bytes_helper<StoragePolicy>::value > 0;

        // Resolve the actual storage type (handling SmallVector specially)
        using actual_storage_type = std::conditional_t<
            is_small_vector_policy,
            containers::dynamic::SmallVector<slot, sv_inline_bytes_helper<StoragePolicy>::value, SlotAlloc>,
            storage_type
        >;

        actual_storage_type        slots_;
        std::vector<index_type, FreeListAlloc> free_list_;

    public:
        slot_map() = default;

        explicit slot_map(const Allocator& alloc)
            : slots_(SlotAlloc(alloc)), free_list_(FreeListAlloc(alloc)) {}

        slot_map(const slot_map&)            = default;
        slot_map& operator=(const slot_map&) = default;
        slot_map(slot_map&&) noexcept        = default;
        slot_map& operator=(slot_map&&) noexcept = default;

        void clear() {
            slots_.clear();
            free_list_.clear();
        }

        void reserve(std::size_t cap) {
            free_list_.reserve(cap);
            if constexpr (!std::is_same_v<StoragePolicy, deque_storage>) {
                slots_.reserve(cap);
            }
        }

        // Insert a value; returns a valid handle.  Reuses a free slot when
        // available (bumps the stored generation for the new occupant).
        template <class U>
            requires std::constructible_from<T, U&&>
        [[nodiscard]] handle_type insert(U&& val) {
            if (!free_list_.empty()) {
                const index_type idx = free_list_.back();
                free_list_.pop_back();
                slot& s = slots_[static_cast<std::size_t>(idx) - 1]; // 1-based → 0-based
                s.value.emplace(std::forward<U>(val));
                // generation was already bumped at erase time
                return handle_type{idx, s.generation};
            }
            // New slot: 1-based index (0 is the null handle sentinel).
            const auto idx =
                static_cast<index_type>(slots_.size() + 1); // 1-based
            slots_.push_back(slot{std::optional<T>{std::forward<U>(val)}, 1});
            return handle_type{idx, 1};
        }

        // Erase the slot identified by h.  Bumps generation so all outstanding
        // handles to this slot become stale. Returns true if an element was erased.
        bool erase(const handle_type h) noexcept {
            if (h.is_null()) return false;
            const auto raw = static_cast<std::size_t>(h.index - 1);
            if (raw >= slots_.size()) return false;
            slot& s = slots_[raw];
            if (s.generation != h.generation || !s.value) return false;
            s.value.reset();
            ++s.generation; // invalidate all handles with the old generation
            free_list_.push_back(h.index);
            return true;
        }

        // Find a live value by handle.  Returns nullptr for null, stale, or
        // erased handles.  Address stability: guaranteed with deque_storage;
        // may be invalidated by insert() with small_vector_storage/std_vector_storage.
        [[nodiscard]] T* find(const handle_type h) noexcept {
            if (h.is_null()) return nullptr;
            const auto raw = static_cast<std::size_t>(h.index - 1);
            if (raw >= slots_.size()) return nullptr;
            slot& s = slots_[raw];
            if (s.generation != h.generation || !s.value) return nullptr;
            return &*s.value;
        }

        [[nodiscard]] const T* find(const handle_type h) const noexcept {
            if (h.is_null()) return nullptr;
            const auto raw = static_cast<std::size_t>(h.index - 1);
            if (raw >= slots_.size()) return nullptr;
            const slot& s = slots_[raw];
            if (s.generation != h.generation || !s.value) return nullptr;
            return &*s.value;
        }

        [[nodiscard]] bool contains(const handle_type h) const noexcept {
            return find(h) != nullptr;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return slots_.size() - free_list_.size();
        }

        [[nodiscard]] bool empty() const noexcept { return size() == 0; }

        // Dense iteration — visits only live slots.
        // Yields value_ref{handle_type, T&}.
        struct iterator {
            using SM = slot_map;

            SM*         map = nullptr;
            std::size_t pos = 0; // 0-based index into slots_

            // Advance past dead slots.
            void advance() noexcept {
                while (pos < map->slots_.size() && !map->slots_[pos].value)
                    ++pos;
            }

            struct value_ref {
                handle_type handle;
                T&          value;
            };

            value_ref operator*() const noexcept {
                slot& s = map->slots_[pos];
                return value_ref{
                    .handle = handle_type{static_cast<index_type>(pos + 1), s.generation},
                    .value  = *s.value
                };
            }

            iterator& operator++() noexcept {
                ++pos;
                advance();
                return *this;
            }

            iterator operator++(int) noexcept {
                auto tmp = *this;
                ++*this;
                return tmp;
            }

            [[nodiscard]] bool operator==(const iterator& o) const noexcept {
                return pos == o.pos;
            }

            [[nodiscard]] bool operator!=(const iterator& o) const noexcept {
                return pos != o.pos;
            }
        };

        struct const_iterator {
            const slot_map* map = nullptr;
            std::size_t     pos = 0;

            void advance() noexcept {
                while (pos < map->slots_.size() && !map->slots_[pos].value)
                    ++pos;
            }

            struct value_ref {
                handle_type handle;
                const T&    value;
            };

            value_ref operator*() const noexcept {
                const slot& s = map->slots_[pos];
                return value_ref{
                    .handle = handle_type{static_cast<index_type>(pos + 1), s.generation},
                    .value  = *s.value
                };
            }

            const_iterator& operator++() noexcept {
                ++pos;
                advance();
                return *this;
            }

            const_iterator operator++(int) noexcept {
                auto tmp = *this;
                ++*this;
                return tmp;
            }

            [[nodiscard]] bool operator==(const const_iterator& o) const noexcept {
                return pos == o.pos;
            }

            [[nodiscard]] bool operator!=(const const_iterator& o) const noexcept {
                return pos != o.pos;
            }
        };

        [[nodiscard]] iterator begin() noexcept {
            iterator it{.map = this, .pos = 0};
            it.advance();
            return it;
        }

        [[nodiscard]] iterator end() noexcept { return iterator{.map = this, .pos = slots_.size()}; }

        [[nodiscard]] const_iterator begin() const noexcept {
            const_iterator it{.map = this, .pos = 0};
            it.advance();
            return it;
        }

        [[nodiscard]] const_iterator end() const noexcept {
            return const_iterator{.map = this, .pos = slots_.size()};
        }

        [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
        [[nodiscard]] const_iterator cend()   const noexcept { return end(); }
    };

} // namespace containers

namespace pebble::containers {
    using ::containers::slot_map;
    using ::containers::deque_storage;
    using ::containers::std_vector_storage;
    template <std::size_t N>
    using small_vector_storage = ::containers::small_vector_storage<N>;
} // namespace pebble::containers
