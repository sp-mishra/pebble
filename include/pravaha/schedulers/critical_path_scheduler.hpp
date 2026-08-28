#pragma once
#include "pravaha/schedulers/scheduler_policy.hpp"
#include "pravaha/pravaha.hpp"
#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pravaha::sched {
    // ---------------------------------------------------------------------------
    // critical_path_scheduler_policy
    // ---------------------------------------------------------------------------
    // Prioritises tasks by their longest-remaining-path (LRP) rank.
    //
    // Usage:
    //   critical_path_scheduler_policy sched;
    //   sched.init_from_dag(ir);           // one-time call after IR is built
    //   sched.on_task_ready(token);
    //   auto next = sched.select_next_task(worker_id);
    //
    // init_from_dag performs a backward BFS from sink nodes to compute
    // LRP ranks: rank[v] = 1 + max(rank[successor]) for each node v.
    // Tasks with a higher rank are placed earlier in the run queue.
    //
    // task_token::dag_depth is overwritten with the computed rank when
    // on_task_ready is called after init_from_dag.
    class critical_path_scheduler_policy {
    public:
        // -----------------------------------------------------------------------
        // init_from_dag — compute LRP ranks via backward pass over TaskIr.
        //
        // Algorithm:
        //   1. Build a successor-list (forward adjacency) from ir.edges.
        //   2. Compute in-degrees to identify sinks (in-degree from reverse = 0,
        //      i.e. nodes with no outgoing edges in the original DAG).
        //   3. BFS/Kahn from sinks in reverse: rank[v] = 1 + max(rank[succ]).
        // -----------------------------------------------------------------------
        void init_from_dag(const TaskIr& ir) {
            std::scoped_lock lk{mutex_};
            critical_path_rank_.clear();
            max_rank_ = 0;

            if (ir.nodes.empty()) {
                return;
            }

            // Build forward adjacency (from → successors).
            std::unordered_map<std::size_t, std::vector<std::size_t>> successors;
            std::unordered_map<std::size_t, std::size_t> out_degree;

            for (const auto& node : ir.nodes) {
                successors.emplace(node.id.value, std::vector<std::size_t>{});
                out_degree[node.id.value] = 0;
            }

            for (const auto& edge : ir.edges) {
                if (edge.kind != EdgeKind::Sequence && edge.kind != EdgeKind::Data) {
                    continue;
                }
                successors[edge.from.value].push_back(edge.to.value);
                out_degree[edge.from.value]++;
            }

            // Build reverse adjacency for backward pass.
            std::unordered_map<std::size_t, std::vector<std::size_t>> predecessors;
            for (const auto& node : ir.nodes) {
                predecessors.emplace(node.id.value, std::vector<std::size_t>{});
            }
            for (const auto& edge : ir.edges) {
                if (edge.kind != EdgeKind::Sequence && edge.kind != EdgeKind::Data) {
                    continue;
                }
                predecessors[edge.to.value].push_back(edge.from.value);
            }

            // Initialise all ranks to 1 (each node counts itself).
            for (const auto& node : ir.nodes) {
                critical_path_rank_[node.id.value] = 1;
            }

            // Identify sinks: nodes with no outgoing Sequence/Data edges.
            std::deque<std::size_t> worklist;
            std::unordered_map<std::size_t, std::size_t> remaining_out;
            for (const auto& node : ir.nodes) {
                remaining_out[node.id.value] = successors[node.id.value].size();
                if (remaining_out[node.id.value] == 0) {
                    worklist.push_back(node.id.value);
                }
            }

            // Backward relaxation (Kahn-like on reversed edges).
            while (!worklist.empty()) {
                const std::size_t cur = worklist.front();
                worklist.pop_front();

                const std::size_t cur_rank = critical_path_rank_[cur];

                for (const std::size_t pred : predecessors[cur]) {
                    const std::size_t candidate = cur_rank + 1;
                    if (candidate > critical_path_rank_[pred]) {
                        critical_path_rank_[pred] = candidate;
                    }
                    // Process predecessor once all its successors have been ranked.
                    remaining_out[pred]--;
                    if (remaining_out[pred] == 0) {
                        worklist.push_back(pred);
                    }
                }

                if (cur_rank > max_rank_) {
                    max_rank_ = cur_rank;
                }
            }
        }

        // -----------------------------------------------------------------------
        // on_task_ready — enqueue; override dag_depth with computed rank if known.
        // -----------------------------------------------------------------------
        void on_task_ready(task_token t) {
            std::scoped_lock lk{mutex_};
            auto it = critical_path_rank_.find(t.id);
            if (it != critical_path_rank_.end()) {
                t.dag_depth = it->second;
            }
            queue_.push_back(t);
        }

        void on_task_complete(task_token /*t*/) {
            // No bookkeeping required for this policy.
        }

        // -----------------------------------------------------------------------
        // select_next_task — pick the task with the highest dag_depth (longest
        // remaining path).  Ties broken by TaskPriority (High > Normal > Low).
        // -----------------------------------------------------------------------
        [[nodiscard]] std::optional<task_token> select_next_task(std::size_t /*worker_id*/) {
            std::scoped_lock lk{mutex_};
            if (queue_.empty()) {
                return std::nullopt;
            }

            auto best = queue_.begin();
            for (auto it = std::next(queue_.begin()); it != queue_.end(); ++it) {
                if (it->dag_depth > best->dag_depth) {
                    best = it;
                }
                else if (it->dag_depth == best->dag_depth) {
                    // Break ties by priority.
                    const int p_it = static_cast<int>(it->priority);
                    const int p_best = static_cast<int>(best->priority);
                    if (p_it > p_best) {
                        best = it;
                    }
                }
            }

            task_token t = *best;
            queue_.erase(best);
            return t;
        }

        // -----------------------------------------------------------------------
        // Observers
        // -----------------------------------------------------------------------

        // Returns the maximum LRP rank across all nodes (== critical path length).
        [[nodiscard]] std::size_t critical_path_length() const noexcept {
            std::scoped_lock lk{mutex_};
            return max_rank_;
        }

        // Naive Amdahl-style estimate: assumes perfect parallelism on non-critical
        // work.  Returns 1.0 if no DAG has been loaded.
        [[nodiscard]] float expected_speedup() const noexcept {
            std::scoped_lock lk{mutex_};
            if (max_rank_ == 0) {
                return 1.0f;
            }
            const float total_nodes = static_cast<float>(critical_path_rank_.size());
            const float cp = static_cast<float>(max_rank_);
            // speedup ≈ total_work / critical_path  (assuming unlimited workers)
            return (cp > 0.0f) ? (total_nodes / cp) : 1.0f;
        }

        [[nodiscard]] std::size_t pending_count() const noexcept {
            std::scoped_lock lk{mutex_};
            return queue_.size();
        }

    private:
        mutable std::mutex mutex_;
        // Maps TaskId::value → longest-remaining-path rank.
        std::unordered_map<std::size_t, std::size_t> critical_path_rank_;
        std::deque<task_token> queue_;
        std::size_t max_rank_{0};
    };

    static_assert(SchedulerPolicy<critical_path_scheduler_policy>);
} // namespace pravaha::sched
