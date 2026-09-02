#pragma once
#include "genome.hpp"
#include "metrics.hpp"
#include <algorithm>
#include <functional>
#include <numeric>
#include <random>
#include <vector>

namespace manas {
    using SelectionPolicy = std::function<std::vector<BrainGenome>(const std::vector<BrainGenome> &,
 const std::vector<FitnessMetrics> &)>;

    // Tournament Selection: Randomly choose k candidates and pick the one with highest fitness.
    struct TournamentSelection {
        size_t tournament_size = 3;
        size_t num_parents = 2; // Typically 2 parents per reproduction

        std::vector<BrainGenome> operator()(const std::vector<BrainGenome>& population,
                                            const std::vector<FitnessMetrics>& scores) const {
            if (population.empty()) return {};

            std::vector<BrainGenome> selected;
            selected.reserve(num_parents);

            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<size_t> dist(0, population.size() - 1);

            for (size_t p = 0; p < num_parents; ++p) {
                size_t best_idx = dist(gen);
                for (size_t t = 1; t < tournament_size; ++t) {
                    size_t cand_idx = dist(gen);
                    if (scores[cand_idx].score > scores[best_idx].score) {
                        best_idx = cand_idx;
                    }
                }
                selected.push_back(population[best_idx]);
            }

            return selected;
        }
    };

    // Roulette Wheel (Fitness-Proportional) Selection
    struct RouletteWheelSelection {
        size_t num_parents = 2;

        std::vector<BrainGenome> operator()(const std::vector<BrainGenome>& population,
                                            const std::vector<FitnessMetrics>& scores) const {
            if (population.empty()) return {};

            // Find min score to handle negative fitness values by shifting
            float min_score = 0.0f;
            for (const auto& m : scores) {
                if (m.score < min_score) min_score = m.score;
            }
            const float offset = (min_score < 0.0f) ? -min_score + 1e-4f : 0.0f;

            float total_fitness = 0.0f;
            for (const auto& m : scores) {
                total_fitness += (m.score + offset);
            }

            std::vector<BrainGenome> selected;
            selected.reserve(num_parents);

            if (total_fitness <= 0.0f) {
                // Fallback: pick first N
                for (size_t i = 0; i < std::min(num_parents, population.size()); ++i) {
                    selected.push_back(population[i]);
                }
                return selected;
            }

            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_real_distribution<float> dist(0.0f, total_fitness);

            for (size_t p = 0; p < num_parents; ++p) {
                float r = dist(gen);
                float accum = 0.0f;
                size_t chosen = 0;
                for (size_t i = 0; i < population.size(); ++i) {
                    accum += (scores[i].score + offset);
                    if (accum >= r) {
                        chosen = i;
                        break;
                    }
                }
                selected.push_back(population[chosen]);
            }

            return selected;
        }
    };
} // namespace manas
