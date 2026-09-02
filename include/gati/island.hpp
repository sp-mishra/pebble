#pragma once
// gati/island.hpp — Contact Island Partitioning and Sleeping Strategy.
#include "rigid_body.hpp"
#include "contact_constraint.hpp"
#include "containers/union_find.hpp"
#include <span>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <algorithm>

namespace gati {
    struct Island {
        std::vector<std::uint32_t> bodies;
        std::vector<std::uint32_t> constraints;
        float kinetic_energy{0.0f};
        bool is_sleeping{false};
    };

    struct IslandSet {
        std::vector<Island> islands;

        [[nodiscard]] std::size_t size() const noexcept { return islands.size(); }
        [[nodiscard]] bool empty() const noexcept { return islands.empty(); }
        auto begin() noexcept { return islands.begin(); }
        auto end() noexcept { return islands.end(); }
        auto begin() const noexcept { return islands.begin(); }
        auto end() const noexcept { return islands.end(); }
    };

    struct UnionFindIslands {
        [[nodiscard]] IslandSet build(std::span<const ContactConstraint> contacts,
                                      std::span<const RigidBody> bodies) const {
            const std::size_t num_bodies = bodies.size();
            if (num_bodies == 0) return IslandSet{};

            containers::union_find<std::uint32_t> uf;
            uf.reserve(num_bodies);
            for (std::size_t i = 0; i < num_bodies; ++i) {
                uf.make_set();
            }

            // Unite dynamic bodies connected by contacts
            for (std::size_t i = 0; i < contacts.size(); ++i) {
                const auto& c = contacts[i];
                if (c.body_a < num_bodies && c.body_b < num_bodies) {
                    const bool a_dynamic = !bodies[c.body_a].is_static();
                    const bool b_dynamic = !bodies[c.body_b].is_static();
                    if (a_dynamic && b_dynamic) {
                        uf.unite(c.body_a, c.body_b);
                    }
                }
            }

            // Map root -> Island index
            std::unordered_map<std::uint32_t, std::size_t> root_to_island;
            IslandSet iset;

            for (std::uint32_t i = 0; i < num_bodies; ++i) {
                if (bodies[i].is_static()) continue;
                const std::uint32_t root = uf.find(i);
                auto it = root_to_island.find(root);
                if (it == root_to_island.end()) {
                    const std::size_t idx = iset.islands.size();
                    root_to_island[root] = idx;
                    Island isl;
                    isl.bodies.push_back(i);
                    isl.kinetic_energy += bodies[i].kinetic_energy();
                    iset.islands.push_back(std::move(isl));
                }
                else {
                    iset.islands[it->second].bodies.push_back(i);
                    iset.islands[it->second].kinetic_energy += bodies[i].kinetic_energy();
                }
            }

            // Assign constraints to islands
            for (std::uint32_t i = 0; i < contacts.size(); ++i) {
                const auto& c = contacts[i];
                std::uint32_t dynamic_body = static_cast<std::uint32_t>(-1);
                if (c.body_a < num_bodies && !bodies[c.body_a].is_static()) dynamic_body = c.body_a;
                else if (c.body_b < num_bodies && !bodies[c.body_b].is_static()) dynamic_body = c.body_b;

                if (dynamic_body != static_cast<std::uint32_t>(-1)) {
                    const std::uint32_t root = uf.find(dynamic_body);
                    auto it = root_to_island.find(root);
                    if (it != root_to_island.end()) {
                        iset.islands[it->second].constraints.push_back(i);
                    }
                }
            }

            return iset;
        }

        void try_sleep(IslandSet& iset, std::span<RigidBody> bodies, float threshold = 0.05f) const noexcept {
            for (auto& isl : iset.islands) {
                const float avg_ke = isl.bodies.empty()
                                         ? 0.0f
                                         : (isl.kinetic_energy / static_cast<float>(isl.bodies.size()));
                if (avg_ke < threshold) {
                    isl.is_sleeping = true;
                    for (std::uint32_t b_idx : isl.bodies) {
                        if (b_idx < bodies.size()) {
                            bodies[b_idx].is_sleeping = true;
                        }
                    }
                }
                else {
                    isl.is_sleeping = false;
                    for (std::uint32_t b_idx : isl.bodies) {
                        if (b_idx < bodies.size()) {
                            bodies[b_idx].is_sleeping = false;
                        }
                    }
                }
            }
        }

        void wake(std::uint32_t body_id, std::span<RigidBody> bodies) const noexcept {
            if (body_id < bodies.size()) {
                bodies[body_id].is_sleeping = false;
            }
        }
    };
} // namespace gati
