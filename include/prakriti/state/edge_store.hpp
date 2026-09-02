#pragma once
// ============================================================================
// prakriti/state/edge_store.hpp — topological structural graph (bonds) as SoA.
// Edges carry rest length (plastically mutable), strain, damage, active flag.
// ============================================================================
#include "../core/config.hpp"
#include <vector>
#include <cstdint>

namespace prakriti {
    class EdgeStore {
    public:
        std::vector<Index> a;
        std::vector<Index> b;
        std::vector<Scalar> rest_len; // L0 — mutated by plastic flow
        std::vector<Scalar> strain;
        std::vector<Scalar> damage;
        std::vector<std::uint8_t> active; // 0 == fractured (compacted lazily)

        Index add(Index pa, Index pb, Scalar L0) {
            const Index i = size();
            a.push_back(pa);
            b.push_back(pb);
            rest_len.push_back(L0);
            strain.push_back(Scalar(0));
            damage.push_back(Scalar(0));
            active.push_back(1);
            return i;
        }

        void reserve(std::size_t n) {
            a.reserve(n);
            b.reserve(n);
            rest_len.reserve(n);
            strain.reserve(n);
            damage.reserve(n);
            active.reserve(n);
        }

        void deactivate(Index i) noexcept { active[i] = 0; }
        [[nodiscard]] bool is_active(Index i) const noexcept { return active[i] != 0; }

        // Remove fractured edges in-place (swap-pop). Invalidates edge ordering.
        void compact() {
            Index w = 0;
            const Index n = size();
            for (Index r = 0; r < n; ++r) {
                if (!active[r]) continue;
                if (w != r) {
                    a[w] = a[r];
                    b[w] = b[r];
                    rest_len[w] = rest_len[r];
                    strain[w] = strain[r];
                    damage[w] = damage[r];
                    active[w] = 1;
                }
                ++w;
            }
            a.resize(w);
            b.resize(w);
            rest_len.resize(w);
            strain.resize(w);
            damage.resize(w);
            active.resize(w);
        }

        [[nodiscard]] Index size() const noexcept { return static_cast<Index>(a.size()); }
    };
} // namespace prakriti
