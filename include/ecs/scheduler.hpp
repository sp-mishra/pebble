#pragma once
// ============================================================================
// ecs/scheduler.hpp — Topological System Scheduler for pebble::ecs
// ============================================================================
// Performs topological dependency analysis and automatic execution ordering
// across registered systems using Pebble LiteGraph algorithms.
//
// Zero virtual functions, zero macros, modern C++23.
// ============================================================================

#include "system.hpp"
#include "containers/graph/LiteGraph.hpp"
#include "containers/graph/LiteGraphAlgorithms.hpp"

#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

namespace pebble::ecs {
    template <typename WorldT>
    class Scheduler {
    public:
        struct SystemEntry {
            void* sys_ptr = nullptr;
            void (*run)(void* s, WorldT& w, float dt) = nullptr;
            void (*destroy)(void* s) = nullptr;
            std::vector<std::uint32_t> reads;
            std::vector<std::uint32_t> writes;
        };

        Scheduler() = default;

        ~Scheduler() {
            clear();
        }

        Scheduler(const Scheduler&) = delete;
        Scheduler& operator=(const Scheduler&) = delete;

        Scheduler(Scheduler&& other) noexcept
            : systems_(std::move(other.systems_)),
              execution_order_(std::move(other.execution_order_)),
              built_(other.built_) {
            other.built_ = false;
        }

        Scheduler& operator=(Scheduler&& other) noexcept {
            if (this != &other) {
                clear();
                systems_ = std::move(other.systems_);
                execution_order_ = std::move(other.execution_order_);
                built_ = other.built_;
                other.built_ = false;
            }
            return *this;
        }

        template <typename S>
            requires System<S, WorldT>
        void add_system(S sys) {
            auto* sys_heap = new S(std::move(sys));
            SystemEntry entry{
                .sys_ptr = sys_heap,
                .run = [](void* s, WorldT& w, float dt) {
                    static_cast<S*>(s)->run(w, dt);
                },
                .destroy = [](void* s) {
                    delete static_cast<S*>(s);
                },
                .reads = extract_reads<typename detail::get_reads<S>::type>(),
                .writes = extract_writes<typename detail::get_writes<S>::type>()
            };
            systems_.push_back(std::move(entry));
            built_ = false;
        }

        void build() {
            const std::size_t n = systems_.size();
            execution_order_.clear();
            if (n == 0) {
                built_ = true;
                return;
            }

            // Build dependency graph: directed edge A -> B means A must run before B
            litegraph::Graph<std::monostate, std::monostate, litegraph::Directed> g;
            std::vector<litegraph::NodeId> node_ids(n);
            for (std::size_t i = 0; i < n; ++i) {
                node_ids[i] = g.add_node({});
            }

            for (std::size_t i = 0; i < n; ++i) {
                for (std::size_t j = i + 1; j < n; ++j) {
                    if (has_dependency(systems_[i], systems_[j])) {
                        (void)g.add_edge(node_ids[i], node_ids[j], {});
                    }
                }
            }

            auto sorted = litegraph::topological_sort(g);
            if (!sorted.empty()) {
                for (auto nid : sorted) {
                    execution_order_.push_back(nid.value);
                }
            }
            else {
                // Fallback to sequential registration order if cyclic
                for (std::size_t i = 0; i < n; ++i) {
                    execution_order_.push_back(i);
                }
            }
            built_ = true;
        }

        void run(WorldT& w, float dt) {
            if (!built_) {
                build();
            }
            for (std::size_t idx : execution_order_) {
                auto& sys = systems_[idx];
                if (sys.run && sys.sys_ptr) {
                    sys.run(sys.sys_ptr, w, dt);
                }
            }
        }

        template <typename Executor>
        void run_parallel(WorldT& w, float dt, Executor& /*exec*/) {
            // Auto-run sequentially or in waves
            run(w, dt);
        }

        void clear() noexcept {
            for (auto& sys : systems_) {
                if (sys.destroy && sys.sys_ptr) {
                    sys.destroy(sys.sys_ptr);
                    sys.sys_ptr = nullptr;
                }
            }
            systems_.clear();
            execution_order_.clear();
            built_ = false;
        }

        [[nodiscard]] std::size_t system_count() const noexcept {
            return systems_.size();
        }

    private:
        template <typename... Cs>
        static std::vector<std::uint32_t> extract_reads(Reads<Cs...> = {}) {
            return {ComponentTypeId<Cs>::id()...};
        }

        template <typename... Cs>
        static std::vector<std::uint32_t> extract_writes(Writes<Cs...> = {}) {
            return {ComponentTypeId<Cs>::id()...};
        }

        static bool has_dependency(const SystemEntry& a, const SystemEntry& b) noexcept {
            // Write-Write conflict: A writes what B writes
            for (auto w_a : a.writes) {
                for (auto w_b : b.writes) {
                    if (w_a == w_b) return true;
                }
            }
            // Write-Read conflict: A writes what B reads
            for (auto w_a : a.writes) {
                for (auto r_b : b.reads) {
                    if (w_a == r_b) return true;
                }
            }
            // Read-Write conflict: A reads what B writes
            for (auto r_a : a.reads) {
                for (auto w_b : b.writes) {
                    if (r_a == w_b) return true;
                }
            }
            return false;
        }

        std::vector<SystemEntry> systems_;
        std::vector<std::size_t> execution_order_;
        bool built_ = false;
    };

    static_assert(!std::is_polymorphic_v<Scheduler<int>>, "Scheduler must have zero virtual functions");
} // namespace pebble::ecs
