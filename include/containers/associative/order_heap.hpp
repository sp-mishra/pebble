#pragma once

#include <cassert>
#include <concepts>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

// ============================================================================
// order_heap<Compare> — activity-ordered binary heap with O(1) membership map
//
// Characteristics
// ---------------
//  • Priority queue keyed by a caller-supplied Compare over element indices
//    (activity_[a] > activity_[b] etc.). Compare is an external, mutable key:
//    priorities change while elements sit in the heap.
//  • O(log n) insert / remove_max / increase (decrease-key symmetric).
//  • O(1) contains via a position map (pos_[i] = slot in heap_ or kNpos).
//  • Universe-bounded: element ids are dense unsigned indices [0, capacity()).
//  • Compare is stored as [[no_unique_address]] — a stateless comparator adds
//    zero bytes; the heap pays only for what a stateful key needs.
//  • No macros, no virtual functions. Header-only, C++23.
//
// Rationale
// ---------
//  CDCL variable ordering (VSIDS/EVSIDS) is the canonical user: pick the max-
//  activity unassigned variable in O(log V), bump activity with increase(),
//  re-insert on backtrack. The container is SMT-agnostic — any priority-by-
//  mutable-key workload (schedulers, Dijkstra-style relaxation) can reuse it.
// ============================================================================

namespace containers::associative {
    // -------------------------------------------------------------------------
    // Concept: HeapCompare
    //   A strict-weak ordering over element ids. `operator()(a, b)` returns true
    //   when a should sit *above* b (closer to the max/root).
    // -------------------------------------------------------------------------
    template <typename C>
    concept HeapCompare = requires(const C& c, std::uint32_t a, std::uint32_t b) {
        { c(a, b) } -> std::convertible_to<bool>;
    };

    template <HeapCompare Compare>
    class order_heap {
    public:
        using index_type = std::uint32_t;
        static constexpr index_type kNpos = std::numeric_limits<index_type>::max();

        order_heap() = default;

        explicit order_heap(Compare cmp) noexcept(std::is_nothrow_move_constructible_v<Compare>)
            : cmp_{std::move(cmp)} {}

        // Ensure ids in [0, n) are addressable by the position map. Idempotent;
        // never shrinks. Elements are not inserted — only made insertable.
        void reserve_universe(const std::size_t n) {
            if (n > pos_.size()) pos_.resize(n, kNpos);
        }

        [[nodiscard]] std::size_t capacity() const noexcept { return pos_.size(); }
        [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
        [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }

        [[nodiscard]] bool contains(const index_type v) const noexcept {
            return v < pos_.size() && pos_[v] != kNpos;
        }

        // Insert element v (no-op if already present). Grows the universe if v
        // is beyond current capacity.
        void insert(const index_type v) {
            if (v >= pos_.size()) pos_.resize(static_cast<std::size_t>(v) + 1, kNpos);
            if (pos_[v] != kNpos) return;
            const auto slot = static_cast<index_type>(heap_.size());
            heap_.push_back(v);
            pos_[v] = slot;
            sift_up(slot);
        }

        // Remove and return the max-priority element. Precondition: !empty().
        [[nodiscard]] index_type remove_max() {
            assert(!heap_.empty());
            const index_type top = heap_.front();
            const index_type last = heap_.back();
            heap_.pop_back();
            pos_[top] = kNpos;
            if (!heap_.empty()) {
                heap_[0] = last;
                pos_[last] = 0;
                sift_down(0);
            }
            return top;
        }

        // Restore heap order after v's priority *increased* (moved toward root).
        // No-op if v is not currently in the heap.
        void increase(const index_type v) noexcept {
            if (v < pos_.size() && pos_[v] != kNpos) sift_up(pos_[v]);
        }

        // Restore heap order after v's priority *decreased* (moved toward leaves).
        void decrease(const index_type v) noexcept {
            if (v < pos_.size() && pos_[v] != kNpos) sift_down(pos_[v]);
        }

        void clear() noexcept {
            for (const index_type v : heap_) pos_[v] = kNpos;
            heap_.clear();
        }

        [[nodiscard]] const Compare& compare() const noexcept { return cmp_; }

    private:
        static constexpr index_type parent(const index_type i) noexcept { return (i - 1) >> 1; }
        static constexpr index_type left(const index_type i) noexcept { return (i << 1) + 1; }
        static constexpr index_type right(const index_type i) noexcept { return (i << 1) + 2; }

        void sift_up(index_type i) noexcept {
            const index_type v = heap_[i];
            while (i != 0) {
                const index_type p = parent(i);
                if (!cmp_(v, heap_[p])) break; // parent already >= v
                heap_[i] = heap_[p];
                pos_[heap_[i]] = i;
                i = p;
            }
            heap_[i] = v;
            pos_[v] = i;
        }

        void sift_down(index_type i) noexcept {
            const index_type v = heap_[i];
            const auto n = static_cast<index_type>(heap_.size());
            for (;;) {
                const index_type l = left(i);
                if (l >= n) break;
                const index_type r = right(i);
                // pick the larger child (r wins ties only when strictly above l)
                index_type child = (r < n && cmp_(heap_[r], heap_[l])) ? r : l;
                if (!cmp_(heap_[child], v)) break; // v already >= best child
                heap_[i] = heap_[child];
                pos_[heap_[i]] = i;
                i = child;
            }
            heap_[i] = v;
            pos_[v] = i;
        }

        std::vector<index_type> heap_; // element ids, max-heap by cmp_
        std::vector<index_type> pos_; // pos_[v] = slot in heap_ or kNpos
        [[no_unique_address]] Compare cmp_{}; // external mutable-key comparator
    };
} // namespace containers::associative
