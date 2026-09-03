#pragma once

// =============================================================================
// containers/associative/slot_map.hpp — generic generational slot map
//
// Stable-address O(1) insert/erase/lookup keyed by generational_handle.
// Backing storage is a std::deque<std::optional<T>> — deque never reallocates
// live elements, so stored-object addresses are stable for the slot's lifetime.
// A free-list recycles erased slots; generation is bumped on erase so any
// outstanding handle to the slot is immediately stale.
//
// CONCURRENCY: NO internal locking.  The caller is responsible for
// synchronization (e.g. an external std::mutex or RWLock).  This matches
// §5.3 of the Lithe execution design: the resource_store layers locking on top.
// Do not call insert/erase from multiple threads without external coordination.
//
// Concepts and usage:
//
//   struct my_tag {};
//   using MyHandle = containers::generational_handle<my_tag>;
//   containers::slot_map<int, MyHandle> m;
//   MyHandle h = m.insert(42);
//   int*     p = m.find(h);     // stable pointer until erase(h)
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

#include "containers/handle/generational_handle.hpp"

namespace containers {
    // =========================================================================
    // slot_map<T, Handle>
    // =========================================================================

    template <class T,
              class Handle = generational_handle<T>>
    class slot_map {
    public:
        using handle_type = Handle;
        using index_type = Handle::index_type;
        using value_type = T;

    private:
        struct slot {
            std::optional<T> value;
            index_type generation = 0;
        };

        // std::deque: stable addresses on push_back; never copies live elements.
        std::deque<slot> slots_;
        std::vector<index_type> free_list_;

    public:
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
        // handles to this slot become stale.  No-op for a stale or null handle.
        void erase(const handle_type h) noexcept {
            if (h.is_null()) return;
            const auto raw = static_cast<std::size_t>(h.index - 1);
            if (raw >= slots_.size()) return;
            slot& s = slots_[raw];
            if (s.generation != h.generation || !s.value) return;
            s.value.reset();
            ++s.generation; // invalidate all handles with the old generation
            free_list_.push_back(h.index);
        }

        // Find a live value by handle.  Returns nullptr for null, stale, or
        // erased handles.  Address is stable until erase(h).
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
        // Yields std::pair<handle_type, T&>.
        struct iterator {
            using SM = slot_map;

            SM* map = nullptr;
            std::size_t pos = 0; // 0-based index into slots_

            // Advance past dead slots.
            void advance() {
                while (pos < map->slots_.size() && !map->slots_[pos].value)
                    ++pos;
            }

            struct value_ref {
                handle_type handle;
                T& value;
            };

            value_ref operator*() const noexcept {
                slot& s = map->slots_[pos];
                return value_ref{
                    .handle = handle_type{static_cast<index_type>(pos + 1), s.generation},
                    .value = *s.value
                };
            }

            iterator& operator++() {
                ++pos;
                advance();
                return *this;
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
            std::size_t pos = 0;

            void advance() {
                while (pos < map->slots_.size() && !map->slots_[pos].value)
                    ++pos;
            }

            struct value_ref {
                handle_type handle;
                const T& value;
            };

            value_ref operator*() const noexcept {
                const slot& s = map->slots_[pos];
                return value_ref{
                    .handle = handle_type{static_cast<index_type>(pos + 1), s.generation},
                    .value = *s.value
                };
            }

            const_iterator& operator++() {
                ++pos;
                advance();
                return *this;
            }

            [[nodiscard]] bool operator==(const const_iterator& o) const noexcept {
                return pos == o.pos;
            }

            [[nodiscard]] bool operator!=(const const_iterator& o) const noexcept {
                return pos != o.pos;
            }
        };

        [[nodiscard]] iterator begin() {
            iterator it{.map = this, .pos = 0};
            it.advance();
            return it;
        }

        [[nodiscard]] iterator end() { return iterator{.map = this, .pos = slots_.size()}; }

        [[nodiscard]] const_iterator begin() const {
            const_iterator it{.map = this, .pos = 0};
            it.advance();
            return it;
        }

        [[nodiscard]] const_iterator end() const {
            return const_iterator{.map = this, .pos = slots_.size()};
        }

        [[nodiscard]] const_iterator cbegin() const { return begin(); }
        [[nodiscard]] const_iterator cend() const { return end(); }
    };
} // namespace containers
