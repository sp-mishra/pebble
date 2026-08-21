#pragma once
// ============================================================================
// prakriti/state/particle_store.hpp — runtime particle state as true split-column SoA.
// Directly reuses pebble::math::vec2. Position/velocity are stored as separate
// x[]/y[] float columns (stride-1, SIMD- and kernel-friendly); scalar attributes
// are one column each. index == particle id.
// ============================================================================
#include "../core/config.hpp"
#include "material_registry.hpp"
#include <vector>

namespace prakriti {

class ParticleStore {
public:
    // Split Vec2 columns: contiguous per-component for stride-1 sweeps / highway SIMD kernels.
    std::vector<Scalar>       pos_x,  pos_y;
    std::vector<Scalar>       pred_x, pred_y;
    std::vector<Scalar>       vel_x,  vel_y;
    std::vector<Scalar>       inv_mass;
    std::vector<Scalar>       temperature;
    std::vector<Scalar>       internal_energy; // latent-heat accumulator
    std::vector<Scalar>       pressure;
    std::vector<Scalar>       density;
    std::vector<Scalar>       f_solid;
    std::vector<Scalar>       f_plastic;
    std::vector<Scalar>       f_liquid;
    std::vector<Scalar>       f_gas;
    std::vector<Scalar>       damage;
    std::vector<MaterialId>   material;

    struct ParticleDesc {
        pebble::math::vec2 position{0.0f, 0.0f};
        pebble::math::vec2 velocity{0.0f, 0.0f};
        Scalar             mass = Scalar(1);        // 0 => static (infinite mass)
        Scalar             temperature = Scalar(20);
        MaterialId         material = 0;
        // Initial phase fractions (must sum to 1; normalized on insert).
        Scalar f_solid = 1, f_plastic = 0, f_liquid = 0, f_gas = 0;
    };

    Index add(const ParticleDesc& d) {
        const Index i = size();
        pos_x.push_back(d.position[0]);  pos_y.push_back(d.position[1]);
        pred_x.push_back(d.position[0]); pred_y.push_back(d.position[1]);
        vel_x.push_back(d.velocity[0]);  vel_y.push_back(d.velocity[1]);
        inv_mass.push_back(d.mass > Scalar(0) ? Scalar(1) / d.mass : Scalar(0));
        temperature.push_back(d.temperature);
        internal_energy.push_back(Scalar(0));
        pressure.push_back(Scalar(0));
        density.push_back(Scalar(0));
        Scalar s = d.f_solid + d.f_plastic + d.f_liquid + d.f_gas;
        if (s <= Scalar(0)) s = Scalar(1);
        f_solid.push_back(d.f_solid / s);
        f_plastic.push_back(d.f_plastic / s);
        f_liquid.push_back(d.f_liquid / s);
        f_gas.push_back(d.f_gas / s);
        damage.push_back(Scalar(0));
        material.push_back(d.material);
        return i;
    }

    void reserve(std::size_t n) {
        pos_x.reserve(n); pos_y.reserve(n); pred_x.reserve(n); pred_y.reserve(n);
        vel_x.reserve(n); vel_y.reserve(n); inv_mass.reserve(n);
        temperature.reserve(n); internal_energy.reserve(n); pressure.reserve(n);
        density.reserve(n); f_solid.reserve(n); f_plastic.reserve(n);
        f_liquid.reserve(n); f_gas.reserve(n); damage.reserve(n); material.reserve(n);
    }

    [[nodiscard]] Index size() const noexcept { return static_cast<Index>(pos_x.size()); }
    [[nodiscard]] bool  is_static(Index i) const noexcept { return inv_mass[i] == Scalar(0); }

    // pebble::math::vec2 value views for irregular (neighbor/edge) solvers.
    [[nodiscard]] pebble::math::vec2 pos_v(Index i)  const noexcept { return pebble::math::vec2(pos_x[i],  pos_y[i]); }
    [[nodiscard]] pebble::math::vec2 pred_v(Index i) const noexcept { return pebble::math::vec2(pred_x[i], pred_y[i]); }
    [[nodiscard]] pebble::math::vec2 vel_v(Index i)  const noexcept { return pebble::math::vec2(vel_x[i],  vel_y[i]); }
    void set_pred(Index i, const pebble::math::vec2& v) noexcept { pred_x[i] = v[0]; pred_y[i] = v[1]; }
};

} // namespace prakriti
