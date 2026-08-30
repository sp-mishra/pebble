#pragma once
// ============================================================================
// field.hpp — Ganita multi-channel Eulerian grid Field<Channels,T,Backend>
// ============================================================================
// Channels co-located grids of type T backed by tensor's CompPolicy (Backend).
// Methods delegate to stencil.hpp: laplacian, gradient, advect, diffuse, project.
// Per-channel views; conservation-safe Neumann BC default.
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_FIELD_HPP
#define PEBBLE_CONTAINERS_MATRIX_FIELD_HPP

#include <containers/matrix/stencil.hpp>
#include <containers/matrix/iterative.hpp>
#include <array>
#include <cstddef>
#include <stdexcept>

namespace ga {

    // -----------------------------------------------------------------------
    // Field<Channels, T, Backend>
    // Backend = tensor CompPolicy type
    // -----------------------------------------------------------------------
    template<std::size_t Channels,
             typename T = float,
             typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    class Field {
    public:
        static constexpr std::size_t channels = Channels;
        using grid_type = Grid2D<T, SP, CP>;

        // Construction
        Field(std::size_t rows, std::size_t cols, T spacing = T{1}) {
            for (std::size_t c = 0; c < Channels; ++c)
                grids_[c] = grid_type(rows, cols, spacing);
        }

        // Per-channel access
        [[nodiscard]] grid_type& channel(std::size_t c) {
            if (c >= Channels) throw std::out_of_range("Field::channel out of range");
            return grids_[c];
        }
        [[nodiscard]] const grid_type& channel(std::size_t c) const {
            if (c >= Channels) throw std::out_of_range("Field::channel out of range");
            return grids_[c];
        }

        [[nodiscard]] std::size_t rows() const { return grids_[0].rows(); }
        [[nodiscard]] std::size_t cols() const { return grids_[0].cols(); }
        [[nodiscard]] T spacing()  const { return grids_[0].h; }

        // Laplacian of channel c
        template<typename BC = NeumannBC>
        [[nodiscard]] grid_type laplacian_ch(std::size_t c) const {
            return ga::laplacian<T,SP,CP,BC>(grids_[c]);
        }

        // Gradient of channel c → (gx, gy)
        template<typename BC = NeumannBC>
        [[nodiscard]] std::pair<grid_type,grid_type> grad_ch(std::size_t c) const {
            return ga::gradient<T,SP,CP,BC>(grids_[c]);
        }

        // Advect all channels by velocity field (channel vx_ch, vy_ch)
        template<typename BC = WrapBC>
        void advect(std::size_t vx_ch, std::size_t vy_ch, T dt) {
            const grid_type& vx = grids_[vx_ch];
            const grid_type& vy = grids_[vy_ch];
            for (std::size_t c = 0; c < Channels; ++c) {
                if (c == vx_ch || c == vy_ch) continue; // skip velocity channels
                grids_[c] = ga::advect_semilagrangian<T,SP,CP,BC>(grids_[c], vx, vy, dt);
            }
        }

        // Advect all channels (including velocity) — self-advection
        template<typename BC = WrapBC>
        void advect_all(std::size_t vx_ch, std::size_t vy_ch, T dt) {
            std::array<grid_type, Channels> new_grids = grids_;
            const grid_type& vx = grids_[vx_ch];
            const grid_type& vy = grids_[vy_ch];
            for (std::size_t c = 0; c < Channels; ++c)
                new_grids[c] = ga::advect_semilagrangian<T,SP,CP,BC>(grids_[c], vx, vy, dt);
            grids_ = new_grids;
        }

        // Diffuse channel c with diffusion coefficient D for timestep dt
        // Iterates n_sweeps Jacobi diffusion sweeps (operator-splitting)
        template<typename BC = NeumannBC>
        void diffuse(std::size_t c, T D, T dt, std::size_t n_sweeps = 20) {
            for (std::size_t s = 0; s < n_sweeps; ++s)
                grids_[c] = ga::jacobi_diffuse<T,SP,CP,BC>(grids_[c], D, dt);
        }

        // Project velocity field to be divergence-free (Helmholtz decomp)
        // Solves Poisson: ∇²p = ∇·v, then v -= ∇p
        template<typename BC = NeumannBC>
        void project(std::size_t vx_ch, std::size_t vy_ch,
                     std::size_t n_jacobi = 40) {
            grid_type div = ga::divergence<T,SP,CP,BC>(grids_[vx_ch], grids_[vy_ch]);
            // Solve Poisson with Jacobi iterations
            grid_type p(div.rows(), div.cols(), div.h);
            for (std::size_t s = 0; s < n_jacobi; ++s)
                p = ga::jacobi_pressure<T,SP,CP,BC>(p, div);
            // Subtract gradient of p from velocity
            auto [gx, gy] = ga::gradient<T,SP,CP,BC>(p);
            const std::size_t R = rows(), C = cols();
            for (std::size_t i = 0; i < R; ++i)
                for (std::size_t j = 0; j < C; ++j) {
                    grids_[vx_ch].at(i,j) -= gx.at(i,j);
                    grids_[vy_ch].at(i,j) -= gy.at(i,j);
                }
        }

        // Total mass (integral) of channel c (Neumann BC conserves this)
        [[nodiscard]] T mass(std::size_t c) const {
            const grid_type& g = grids_[c];
            T s = T{0};
            for (std::size_t i = 0; i < g.rows(); ++i)
                for (std::size_t j = 0; j < g.cols(); ++j)
                    s += g.at(i,j);
            return s * g.h * g.h;
        }

    private:
        std::array<grid_type, Channels> grids_;
    };

    // -----------------------------------------------------------------------
    // Convenience aliases matching the design doc
    // -----------------------------------------------------------------------
    template<std::size_t Ch, typename T = float>
    using ScalarField = Field<Ch, T>;

    // Velocity + scalar: vx=0, vy=1, density=2
    template<typename T = float>
    using FluidField = Field<3, T>;

    // Paint: vx=0, vy=1, pigment channels start at 2
    template<std::size_t PigmentChannels, typename T = float>
    using PaintField = Field<2 + PigmentChannels, T>;

} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_FIELD_HPP
