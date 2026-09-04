#pragma once

// ============================================================================
// sanchaya/engine/memory_engine.hpp — In-Memory Fused & Vectorized Execution
// ============================================================================

#include "sanchaya/fwd.hpp"
#include "sanchaya/planner/physical_ir.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include <span>
#include <vector>
#include <algorithm>
#include <utility>

namespace sanchaya::engine {

    // ========================================================================
    // 1. In-Memory Execution Operators & Fused Kernels
    // ========================================================================

    template <class Entity>
    struct memory_sequence_scan {
        using entity_type = Entity;

        template <class Container, class OutputFunc>
        static void execute(const Container& source, OutputFunc&& emit) {
            for (const auto& item : source) {
                emit(item);
            }
        }
    };

    template <class Entity = void, class Predicate = void, class ProjectFunc = void>
    struct memory_filter_project_fused {
        template <class Container, class Pred, class Proj, class OutputCollection>
        static void execute(const Container& source, Pred&& pred, Proj&& proj, OutputCollection& out) {
            for (const auto& item : source) {
                if (pred(item)) {
                    out.emplace_back(proj(item));
                }
            }
        }
    };

    template <class Item, class Compare = std::less<Item>>
    class memory_top_n {
    public:
        explicit memory_top_n(std::size_t n, Compare comp = {})
            : limit_n_(n), comp_(std::move(comp)) {}

        void push(const Item& item) {
            if (limit_n_ == 0) return;
            if (heap_.size() < limit_n_) {
                heap_.push_back(item);
                std::push_heap(heap_.begin(), heap_.end(), comp_);
            } else if (comp_(item, heap_.front())) {
                // Replace the current "worst" (front) when the new item is strictly better.
                // With std::greater: comp_(item, front) ≡ item > front.
                std::pop_heap(heap_.begin(), heap_.end(), comp_);
                heap_.back() = item;
                std::push_heap(heap_.begin(), heap_.end(), comp_);
            }
        }

        [[nodiscard]] std::vector<Item> extract_sorted() {
            // sort_heap with comp_ (e.g. std::greater) produces descending order
            // (largest first), which is exactly what top-N callers expect.
            std::sort_heap(heap_.begin(), heap_.end(), comp_);
            return std::move(heap_);
        }

    private:
        std::size_t limit_n_{0};
        Compare comp_{};
        std::vector<Item> heap_{};
    };


} // namespace sanchaya::engine
