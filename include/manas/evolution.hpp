#pragma once

#include "genome.hpp"
#include "metrics.hpp"
#include <algorithm>
#include <functional>
#include <future>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace manas {

// Fitness function signature: given a BrainGenome, return a FitnessMetrics.
using FitnessFn = std::function<FitnessMetrics(const BrainGenome&)>;

template<typename SelectionPolicy, typename MutationOp, typename CrossoverOp>
class EvolutionaryProcess {
public:
    EvolutionaryProcess(SelectionPolicy&& policy, MutationOp&& mutator, CrossoverOp&& crossover)
        : selection_{std::move(policy)},
          mutate_{std::move(mutator)},
          crossover_{std::move(crossover)} {}

    // Seed the initial population.
    void add_genome(BrainGenome genome) {
        population_.push_back(std::move(genome));
    }

    const std::vector<BrainGenome>& population() const noexcept { return population_; }

    // Run one full generation:
    //   1. Evaluate fitness of current population (optionally parallelized).
    //   2. Select parents via SelectionPolicy.
    //   3. Produce children via CrossoverOp.
    //   4. Mutate children via MutationOp.
    //   5. Evaluate children fitness.
    //   6. Merge children into population (replace weakest).
    //   7. Track best genome seen so far.
    void run_generation(const FitnessFn& fitness_fn, size_t num_threads = 1) {
        if (population_.empty()) {
            throw std::runtime_error("EvolutionaryProcess: population is empty");
        }

        // --- 1. Evaluate current population ---
        std::vector<FitnessMetrics> scores(population_.size());
        if (num_threads > 1 && population_.size() > 1) {
            const size_t n = population_.size();
            const size_t chunk = (n + num_threads - 1) / num_threads;
            std::vector<std::future<void>> futures;
            for (size_t t = 0; t < num_threads; ++t) {
                size_t start = t * chunk;
                size_t end = std::min(start + chunk, n);
                if (start < end) {
                    futures.push_back(std::async(std::launch::async, [this, &fitness_fn, &scores, start, end]() {
                        for (size_t i = start; i < end; ++i) {
                            scores[i] = fitness_fn(population_[i]);
                        }
                    }));
                }
            }
            for (auto& f : futures) {
                f.get();
            }
        } else {
            for (size_t i = 0; i < population_.size(); ++i) {
                scores[i] = fitness_fn(population_[i]);
            }
        }

        // Track best genome (highest fitness score).
        {
            size_t best_idx = 0;
            for (size_t i = 1; i < scores.size(); ++i) {
                if (scores[i].score > scores[best_idx].score) {
                    best_idx = i;
                }
            }
            best_ = population_[best_idx];
            best_score_ = scores[best_idx].score;
        }

        // --- 2. Select parents ---
        // SelectionPolicy: callable(population, scores) -> vector<BrainGenome> (parent pairs)
        auto parents = selection_(population_, scores);
        if (parents.empty()) return;

        // --- 3 & 4. Crossover + Mutate to produce children ---
        std::vector<BrainGenome> children;
        children.reserve(parents.size() / 2);
        for (size_t i = 0; i + 1 < parents.size(); i += 2) {
            BrainGenome child = crossover_(parents[i], parents[i + 1]);
            child.generation = parents[i].generation + 1;
            child.parent_id  = parents[i].parent_id;
            mutate_(child);
            children.push_back(std::move(child));
        }

        // --- 5. Evaluate children ---
        std::vector<FitnessMetrics> child_scores;
        child_scores.reserve(children.size());
        for (const auto& c : children) {
            child_scores.push_back(fitness_fn(c));
        }

        // Track if any child beats the current best.
        for (size_t i = 0; i < children.size(); ++i) {
            if (child_scores[i].score > best_score_) {
                best_ = children[i];
                best_score_ = child_scores[i].score;
            }
        }

        // --- 6. Merge: replace the weakest members of the population ---
        // Sort population indices by fitness ascending (weakest first).
        std::vector<size_t> idx(population_.size());
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            return scores[a].score < scores[b].score;
        });

        const size_t replace_count = std::min(children.size(), idx.size());
        for (size_t i = 0; i < replace_count; ++i) {
            if (child_scores[i].score > scores[idx[i]].score) {
                population_[idx[i]] = std::move(children[i]);
            }
        }
    }

    const BrainGenome& best_genome() const { return best_; }
    float best_score() const noexcept { return best_score_; }

private:
    SelectionPolicy selection_;
    MutationOp      mutate_;
    CrossoverOp     crossover_;

    std::vector<BrainGenome> population_;
    BrainGenome best_;
    float best_score_ = -std::numeric_limits<float>::infinity();
};

} // namespace manas