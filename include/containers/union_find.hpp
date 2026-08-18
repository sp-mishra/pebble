#pragma once

// containers/union_find.hpp — Generic disjoint-set forest.
//
// C++23, header-only, no virtual, no macros.
// Template params: Id (index type, default uint32_t), Rank (rank type, default uint8_t).
//
// Algorithm: union-by-rank + path-splitting (Tarjan/van Leeuwen).
// Amortised inverse-Ackermann α(n) per find/unite.
//
// Generic: knows nothing about type-systems, graphs, or expressions.
// Consumers: vakya/unification.hpp (substitution UF), egraph (internal UF mirror).

#include <cassert>
#include <concepts>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace containers {
    template <std::unsigned_integral Id = std::uint32_t,
              std::unsigned_integral Rank = std::uint8_t>
    class union_find {
    public:
        static constexpr Id kInvalidId = std::numeric_limits < Id > ::max();

        // Make a fresh singleton set; returns the new id.
        Id make_set() {
            Id id = static_cast<Id>(parent_.size());
            parent_.push_back(id);
            rank_.push_back(Rank{0});
            return id;
        }

        // make_set with external initial capacity hint (no observable state change).
        void reserve(std::size_t n) {
            parent_.reserve(n);
            rank_.reserve(n);
        }

        [[nodiscard]] std::size_t size() const noexcept { return parent_.size(); }

        // Find representative (root) of the set containing x.
        // Uses path-splitting: each node on the path points to its grandparent.
        [[nodiscard]] Id find(Id x) noexcept {
            assert(x < static_cast<Id>(parent_.size()));
            while (parent_[x] != x) {
                Id next = parent_[parent_[x]]; // grandparent
                parent_[x] = next; // path-split: skip one level
                x = next;
            }
            return x;
        }

        [[nodiscard]] bool connected(Id a, Id b) noexcept {
            return find(a) == find(b);
        }

        // Unite sets containing a and b.  Returns true iff they were distinct.
        // Union-by-rank: smaller rank root attaches under larger rank root.
        bool unite(Id a, Id b) noexcept {
            Id ra = find(a), rb = find(b);
            if (ra == rb) return false;
            if (rank_[ra] < rank_[rb]) std::swap(ra, rb);
            parent_[rb] = ra; // rb attaches under ra
            if (rank_[ra] == rank_[rb]) ++rank_[ra];
            return true;
        }

        // Unite with merge callback: on_merge(new_root, subsumed_root).
        // Called exactly once per successful merge, after parent update.
        template <std::invocable<Id, Id> OnMerge>
        bool unite(Id a, Id b, OnMerge&& on_merge) noexcept(noexcept(on_merge(a, b))) {
            Id ra = find(a), rb = find(b);
            if (ra == rb) return false;
            if (rank_[ra] < rank_[rb]) std::swap(ra, rb);
            parent_[rb] = ra;
            if (rank_[ra] == rank_[rb]) ++rank_[ra];
            std::forward < OnMerge > (on_merge)(ra, rb);
            return true;
        }

        // True iff x is the root of its set.
        [[nodiscard]] bool is_root(Id x) const noexcept {
            assert(x < static_cast<Id>(parent_.size()));
            return parent_[x] == x;
        }

        void clear() noexcept {
            parent_.clear();
            rank_.clear();
        }

    private:
        std::vector<Id> parent_;
        std::vector<Rank> rank_;
    };
} // namespace containers
