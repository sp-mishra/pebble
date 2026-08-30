#pragma once
// ============================================================================
// stencil.hppfinite-difference grid operators + WENO advection
// ============================================================================
// All operators act on rank-2 DynamicTensor (height×width grid).
// BC policy: compile-time Dirichlet / Neumann / Wrap / Clamp.
// Kernels run on tensor CompPolicy (Highway interior, Pravaha bands).
// laplacian / gradient / divergence / curl / conv2d / conv_separable
// advect_semilagrangian (Stam stable fluids SIGGRAPH'99)
// weno3 / weno5 (Jiang-Shu 1996)
// jacobi_diffuse / jacobi_pressure (one sweep)
// ============================================================================

#ifndef PEBBLE_CONTAINERS_MATRIX_STENCIL_HPP
#define PEBBLE_CONTAINERS_MATRIX_STENCIL_HPP

#include <containers/tensor/tensor.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace ga {

    // -----------------------------------------------------------------------
    // Boundary condition policy tags (compile-time)
    // -----------------------------------------------------------------------
    struct DirichletBC { static constexpr bool is_dirichlet = true; };
    struct NeumannBC   { static constexpr bool is_neumann   = true; };
    struct WrapBC      { static constexpr bool is_wrap      = true; };
    struct ClampBC     { static constexpr bool is_clamp     = true; };

    // Default
    using DefaultBC = NeumannBC;

    // -----------------------------------------------------------------------
    // Grid2D — thin wrapper around rank-2 DynamicTensor
    // -----------------------------------------------------------------------
    template<typename T,
             typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy>
    struct Grid2D {
        ts::DynamicTensor<T,SP,CP> data;
        T h{T{1}};   // cell spacing

        Grid2D() = default;
        Grid2D(std::size_t rows, std::size_t cols, T spacing = T{1})
            : data(ts::TensorShape{rows, cols}), h(spacing) {
            std::fill(this->data.data(), this->data.data() + rows*cols, T{0});
        }
        Grid2D(ts::DynamicTensor<T,SP,CP> d, T spacing = T{1})
            : data(std::move(d)), h(spacing) {}

        [[nodiscard]] std::size_t rows() const { return data.shape()[0]; }
        [[nodiscard]] std::size_t cols() const { return data.shape()[1]; }
        [[nodiscard]] T& at(std::size_t r, std::size_t c) {
            return data.data()[r * cols() + c];
        }
        [[nodiscard]] const T& at(std::size_t r, std::size_t c) const {
            return data.data()[r * cols() + c];
        }
    };

    // -----------------------------------------------------------------------
    // Internal: boundary-aware index clamping
    // -----------------------------------------------------------------------
    namespace detail {
        template<typename BC>
        inline std::size_t clamp_idx(std::ptrdiff_t i, std::size_t n) {
            if constexpr (requires { BC::is_wrap; }) {
                if constexpr (BC::is_wrap)
                    return static_cast<std::size_t>((i + static_cast<std::ptrdiff_t>(n)) % static_cast<std::ptrdiff_t>(n));
            }
            return static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(i, 0, static_cast<std::ptrdiff_t>(n)-1));
        }

        template<typename T, typename SP, typename CP, typename BC>
        T grid_get(const Grid2D<T,SP,CP>& g, std::ptrdiff_t r, std::ptrdiff_t c) {
            if constexpr (requires { BC::is_dirichlet; }) {
                if (r < 0 || c < 0 ||
                    static_cast<std::size_t>(r) >= g.rows() ||
                    static_cast<std::size_t>(c) >= g.cols()) return T{0};
            }
            std::size_t ri = clamp_idx<BC>(r, g.rows());
            std::size_t ci = clamp_idx<BC>(c, g.cols());
            return g.at(ri, ci);
        }
    } // namespace detail

    // -----------------------------------------------------------------------
    // laplacian — 5-point stencil: (u_{i-1,j}+u_{i+1,j}+u_{i,j-1}+u_{i,j+1}-4u_{i,j}) / h²
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = DefaultBC>
    [[nodiscard]] Grid2D<T,SP,CP> laplacian(const Grid2D<T,SP,CP>& u) {
        const std::size_t R = u.rows(), C = u.cols();
        Grid2D<T,SP,CP> out(R, C, u.h);
        T inv_h2 = T{1} / (u.h * u.h);
        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                T v = detail::grid_get<T,SP,CP,BC>(u, static_cast<std::ptrdiff_t>(i)-1, j)
                    + detail::grid_get<T,SP,CP,BC>(u, static_cast<std::ptrdiff_t>(i)+1, j)
                    + detail::grid_get<T,SP,CP,BC>(u, i, static_cast<std::ptrdiff_t>(j)-1)
                    + detail::grid_get<T,SP,CP,BC>(u, i, static_cast<std::ptrdiff_t>(j)+1)
                    - T{4} * u.at(i, j);
                out.at(i, j) = v * inv_h2;
            }
        return out;
    }

    // -----------------------------------------------------------------------
    // gradient — central differences → (∂u/∂x, ∂u/∂y)
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = DefaultBC>
    [[nodiscard]] std::pair<Grid2D<T,SP,CP>, Grid2D<T,SP,CP>>
    gradient(const Grid2D<T,SP,CP>& u) {
        const std::size_t R = u.rows(), C = u.cols();
        Grid2D<T,SP,CP> gx(R, C, u.h), gy(R, C, u.h);
        T inv2h = T{1} / (T{2} * u.h);
        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                gx.at(i,j) = (detail::grid_get<T,SP,CP,BC>(u, i, static_cast<std::ptrdiff_t>(j)+1)
                             - detail::grid_get<T,SP,CP,BC>(u, i, static_cast<std::ptrdiff_t>(j)-1)) * inv2h;
                gy.at(i,j) = (detail::grid_get<T,SP,CP,BC>(u, static_cast<std::ptrdiff_t>(i)+1, j)
                             - detail::grid_get<T,SP,CP,BC>(u, static_cast<std::ptrdiff_t>(i)-1, j)) * inv2h;
            }
        return {gx, gy};
    }

    // -----------------------------------------------------------------------
    // divergence — ∂vx/∂x + ∂vy/∂y  (central differences)
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = DefaultBC>
    [[nodiscard]] Grid2D<T,SP,CP> divergence(
            const Grid2D<T,SP,CP>& vx,
            const Grid2D<T,SP,CP>& vy) {
        const std::size_t R = vx.rows(), C = vx.cols();
        Grid2D<T,SP,CP> out(R, C, vx.h);
        T inv2h = T{1} / (T{2} * vx.h);
        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                T dvx = (detail::grid_get<T,SP,CP,BC>(vx, i, static_cast<std::ptrdiff_t>(j)+1)
                        - detail::grid_get<T,SP,CP,BC>(vx, i, static_cast<std::ptrdiff_t>(j)-1)) * inv2h;
                T dvy = (detail::grid_get<T,SP,CP,BC>(vy, static_cast<std::ptrdiff_t>(i)+1, j)
                        - detail::grid_get<T,SP,CP,BC>(vy, static_cast<std::ptrdiff_t>(i)-1, j)) * inv2h;
                out.at(i,j) = dvx + dvy;
            }
        return out;
    }

    // -----------------------------------------------------------------------
    // curl — ∂vy/∂x - ∂vx/∂y  (2D scalar curl)
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = DefaultBC>
    [[nodiscard]] Grid2D<T,SP,CP> curl(
            const Grid2D<T,SP,CP>& vx,
            const Grid2D<T,SP,CP>& vy) {
        const std::size_t R = vx.rows(), C = vx.cols();
        Grid2D<T,SP,CP> out(R, C, vx.h);
        T inv2h = T{1} / (T{2} * vx.h);
        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                T dvy_dx = (detail::grid_get<T,SP,CP,BC>(vy, i, static_cast<std::ptrdiff_t>(j)+1)
                           - detail::grid_get<T,SP,CP,BC>(vy, i, static_cast<std::ptrdiff_t>(j)-1)) * inv2h;
                T dvx_dy = (detail::grid_get<T,SP,CP,BC>(vx, static_cast<std::ptrdiff_t>(i)+1, j)
                           - detail::grid_get<T,SP,CP,BC>(vx, static_cast<std::ptrdiff_t>(i)-1, j)) * inv2h;
                out.at(i,j) = dvy_dx - dvx_dy;
            }
        return out;
    }

    // -----------------------------------------------------------------------
    // conv2d — 2D convolution with arbitrary kernel k
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = DefaultBC>
    [[nodiscard]] Grid2D<T,SP,CP> conv2d(
            const Grid2D<T,SP,CP>& u,
            const std::vector<std::vector<T>>& k) {
        const std::size_t R = u.rows(), C = u.cols();
        const std::size_t kr = k.size(), kc = k.empty() ? 0 : k[0].size();
        const std::ptrdiff_t hr = static_cast<std::ptrdiff_t>(kr) / 2;
        const std::ptrdiff_t hc = static_cast<std::ptrdiff_t>(kc) / 2;
        Grid2D<T,SP,CP> out(R, C, u.h);
        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                T s = T{0};
                for (std::size_t ki = 0; ki < kr; ++ki)
                    for (std::size_t kj = 0; kj < kc; ++kj) {
                        std::ptrdiff_t ri = static_cast<std::ptrdiff_t>(i) + static_cast<std::ptrdiff_t>(ki) - hr;
                        std::ptrdiff_t ci2 = static_cast<std::ptrdiff_t>(j) + static_cast<std::ptrdiff_t>(kj) - hc;
                        s += k[ki][kj] * detail::grid_get<T,SP,CP,BC>(u, ri, ci2);
                    }
                out.at(i,j) = s;
            }
        return out;
    }

    // -----------------------------------------------------------------------
    // conv_separable — separable 2D convolution via kx (row) and ky (col)
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = DefaultBC>
    [[nodiscard]] Grid2D<T,SP,CP> conv_separable(
            const Grid2D<T,SP,CP>& u,
            const std::vector<T>& kx,  // kernel along columns (x/j direction)
            const std::vector<T>& ky)  // kernel along rows    (y/i direction)
    {
        const std::size_t R = u.rows(), C = u.cols();
        const std::ptrdiff_t hx = static_cast<std::ptrdiff_t>(kx.size()) / 2;
        const std::ptrdiff_t hy = static_cast<std::ptrdiff_t>(ky.size()) / 2;
        Grid2D<T,SP,CP> tmp(R, C, u.h), out(R, C, u.h);
        // horizontal pass
        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                T s = T{0};
                for (std::size_t k = 0; k < kx.size(); ++k)
                    s += kx[k] * detail::grid_get<T,SP,CP,BC>(u, i, static_cast<std::ptrdiff_t>(j)+static_cast<std::ptrdiff_t>(k)-hx);
                tmp.at(i,j) = s;
            }
        // vertical pass
        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                T s = T{0};
                for (std::size_t k = 0; k < ky.size(); ++k)
                    s += ky[k] * detail::grid_get<T,SP,CP,BC>(tmp, static_cast<std::ptrdiff_t>(i)+static_cast<std::ptrdiff_t>(k)-hy, j);
                out.at(i,j) = s;
            }
        return out;
    }

    // -----------------------------------------------------------------------
    // advect_semilagrangian — Stam 1999 stable fluids backward-trace + bilinear
    // Advects field f by velocity (vx, vy) over timestep dt.
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = DefaultBC>
    [[nodiscard]] Grid2D<T,SP,CP> advect_semilagrangian(
            const Grid2D<T,SP,CP>& f,
            const Grid2D<T,SP,CP>& vx,
            const Grid2D<T,SP,CP>& vy,
            T dt) {
        const std::size_t R = f.rows(), C = f.cols();
        Grid2D<T,SP,CP> out(R, C, f.h);
        const T inv_h = T{1} / f.h;

        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                // backward trace: x_prev = x - dt·v(x)
                T xj = static_cast<T>(j) - dt * vx.at(i,j) * inv_h;
                T xi = static_cast<T>(i) - dt * vy.at(i,j) * inv_h;

                // bilinear interpolation
                std::ptrdiff_t x0 = static_cast<std::ptrdiff_t>(std::floor(xj));
                std::ptrdiff_t y0 = static_cast<std::ptrdiff_t>(std::floor(xi));
                T fx = xj - static_cast<T>(x0);
                T fy = xi - static_cast<T>(y0);

                T f00 = detail::grid_get<T,SP,CP,BC>(f, y0,   x0);
                T f10 = detail::grid_get<T,SP,CP,BC>(f, y0,   x0+1);
                T f01 = detail::grid_get<T,SP,CP,BC>(f, y0+1, x0);
                T f11 = detail::grid_get<T,SP,CP,BC>(f, y0+1, x0+1);

                out.at(i,j) = (T{1}-fy)*((T{1}-fx)*f00 + fx*f10)
                            +       fy *((T{1}-fx)*f01 + fx*f11);
            }
        return out;
    }

    // -----------------------------------------------------------------------
    // WENO helpers — one-sided 3rd-order reconstructions at cell interface
    // -----------------------------------------------------------------------
    namespace detail {
        template<typename T>
        inline T weno3_flux(T vm1, T v0, T vp1) {
            // 3rd-order WENO upwind reconstruction (Jiang-Shu stencil variant)
            T beta0 = (v0 - vm1) * (v0 - vm1);
            T beta1 = (vp1 - v0) * (vp1 - v0);
            constexpr T eps = T{1e-6};
            T w0 = T{1.0/3.0} / ((eps + beta0) * (eps + beta0));
            T w1 = T{2.0/3.0} / ((eps + beta1) * (eps + beta1));
            T ws = w0 + w1;
            w0 /= ws; w1 /= ws;
            T q0 = T{0.5}*vm1 + T{0.5}*v0;
            T q1 = T{-0.5}*v0 + T{1.5}*vp1;
            return w0*q0 + w1*q1;
        }

        template<typename T>
        inline T weno5_flux(T vm2, T vm1, T v0, T vp1, T vp2) {
            // 5th-order WENO (Jiang-Shu 1996)
            T beta0 = (T{13.0/12.0})*(vm2-2*vm1+v0)*(vm2-2*vm1+v0) + T{0.25}*(vm2-4*vm1+3*v0)*(vm2-4*vm1+3*v0);
            T beta1 = (T{13.0/12.0})*(vm1-2*v0+vp1)*(vm1-2*v0+vp1) + T{0.25}*(vm1-vp1)*(vm1-vp1);
            T beta2 = (T{13.0/12.0})*(v0-2*vp1+vp2)*(v0-2*vp1+vp2) + T{0.25}*(3*v0-4*vp1+vp2)*(3*v0-4*vp1+vp2);
            constexpr T eps = T{1e-6};
            T w0 = T{0.1} / ((eps+beta0)*(eps+beta0));
            T w1 = T{0.6} / ((eps+beta1)*(eps+beta1));
            T w2 = T{0.3} / ((eps+beta2)*(eps+beta2));
            T ws = w0+w1+w2; w0/=ws; w1/=ws; w2/=ws;
            T q0 = T{1.0/3.0}*vm2 - T{7.0/6.0}*vm1 + T{11.0/6.0}*v0;
            T q1 = -T{1.0/6.0}*vm1 + T{5.0/6.0}*v0 + T{1.0/3.0}*vp1;
            T q2 = T{1.0/3.0}*v0 + T{5.0/6.0}*vp1 - T{1.0/6.0}*vp2;
            return w0*q0 + w1*q1 + w2*q2;
        }
    } // namespace detail

    // -----------------------------------------------------------------------
    // weno3 — 3rd-order WENO advection: df/dt + vx·df/dx + vy·df/dy = 0
    // Forward Euler time step (CFL stability: dt < h / max(|v|))
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = WrapBC>
    [[nodiscard]] Grid2D<T,SP,CP> weno3(
            const Grid2D<T,SP,CP>& f,
            const Grid2D<T,SP,CP>& vx,
            const Grid2D<T,SP,CP>& vy,
            T dt) {
        const std::size_t R = f.rows(), C = f.cols();
        Grid2D<T,SP,CP> out(R, C, f.h);
        T inv_h = T{1} / f.h;

        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                auto gi = [&](std::ptrdiff_t di, std::ptrdiff_t dj) {
                    return detail::grid_get<T,SP,CP,BC>(f, static_cast<std::ptrdiff_t>(i)+di, static_cast<std::ptrdiff_t>(j)+dj);
                };
                T u_x = vx.at(i,j), u_y = vy.at(i,j);
                // x-direction (j) flux
                T flux_x = (u_x >= T{0})
                    ? detail::weno3_flux<T>(gi(0,-1), gi(0,0), gi(0,1))
                    : detail::weno3_flux<T>(gi(0, 2), gi(0,1), gi(0,0));
                T flux_xm = (u_x >= T{0})
                    ? detail::weno3_flux<T>(gi(0,-2), gi(0,-1), gi(0,0))
                    : detail::weno3_flux<T>(gi(0, 1), gi(0,0), gi(0,-1));
                // y-direction (i) flux
                T flux_y = (u_y >= T{0})
                    ? detail::weno3_flux<T>(gi(-1,0), gi(0,0), gi(1,0))
                    : detail::weno3_flux<T>(gi( 2,0), gi(1,0), gi(0,0));
                T flux_ym = (u_y >= T{0})
                    ? detail::weno3_flux<T>(gi(-2,0), gi(-1,0), gi(0,0))
                    : detail::weno3_flux<T>(gi( 1,0), gi(0,0), gi(-1,0));
                T dfdx = (flux_x - flux_xm) * inv_h;
                T dfdy = (flux_y - flux_ym) * inv_h;
                out.at(i,j) = f.at(i,j) - dt * (u_x*dfdx + u_y*dfdy);
            }
        return out;
    }

    // -----------------------------------------------------------------------
    // weno5 — 5th-order WENO advection
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = WrapBC>
    [[nodiscard]] Grid2D<T,SP,CP> weno5(
            const Grid2D<T,SP,CP>& f,
            const Grid2D<T,SP,CP>& vx,
            const Grid2D<T,SP,CP>& vy,
            T dt) {
        const std::size_t R = f.rows(), C = f.cols();
        Grid2D<T,SP,CP> out(R, C, f.h);
        T inv_h = T{1} / f.h;

        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                auto gi = [&](std::ptrdiff_t di, std::ptrdiff_t dj) {
                    return detail::grid_get<T,SP,CP,BC>(f, static_cast<std::ptrdiff_t>(i)+di, static_cast<std::ptrdiff_t>(j)+dj);
                };
                T u_x = vx.at(i,j), u_y = vy.at(i,j);
                T fx = (u_x >= T{0})
                    ? detail::weno5_flux<T>(gi(0,-2),gi(0,-1),gi(0,0),gi(0,1),gi(0,2))
                    : detail::weno5_flux<T>(gi(0, 3),gi(0, 2),gi(0,1),gi(0,0),gi(0,-1));
                T fxm = (u_x >= T{0})
                    ? detail::weno5_flux<T>(gi(0,-3),gi(0,-2),gi(0,-1),gi(0,0),gi(0,1))
                    : detail::weno5_flux<T>(gi(0, 2),gi(0, 1),gi(0,0),gi(0,-1),gi(0,-2));
                T fy = (u_y >= T{0})
                    ? detail::weno5_flux<T>(gi(-2,0),gi(-1,0),gi(0,0),gi(1,0),gi(2,0))
                    : detail::weno5_flux<T>(gi( 3,0),gi( 2,0),gi(1,0),gi(0,0),gi(-1,0));
                T fym = (u_y >= T{0})
                    ? detail::weno5_flux<T>(gi(-3,0),gi(-2,0),gi(-1,0),gi(0,0),gi(1,0))
                    : detail::weno5_flux<T>(gi( 2,0),gi( 1,0),gi(0,0),gi(-1,0),gi(-2,0));
                T dfdx = (fx - fxm) * inv_h;
                T dfdy = (fy - fym) * inv_h;
                out.at(i,j) = f.at(i,j) - dt * (u_x*dfdx + u_y*dfdy);
            }
        return out;
    }

    // -----------------------------------------------------------------------
    // jacobi_diffuse — one Jacobi sweep of heat equation
    // u_new = (u + D·dt/h²·(laplacian neighbors)) / (1 + 4·D·dt/h²)
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = NeumannBC>
    [[nodiscard]] Grid2D<T,SP,CP> jacobi_diffuse(
            const Grid2D<T,SP,CP>& u,
            T D, T dt) {
        const std::size_t R = u.rows(), C = u.cols();
        Grid2D<T,SP,CP> out(R, C, u.h);
        T alpha = D * dt / (u.h * u.h);
        T denom = T{1} + T{4} * alpha;
        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                T nb = detail::grid_get<T,SP,CP,BC>(u, static_cast<std::ptrdiff_t>(i)-1, j)
                     + detail::grid_get<T,SP,CP,BC>(u, static_cast<std::ptrdiff_t>(i)+1, j)
                     + detail::grid_get<T,SP,CP,BC>(u, i, static_cast<std::ptrdiff_t>(j)-1)
                     + detail::grid_get<T,SP,CP,BC>(u, i, static_cast<std::ptrdiff_t>(j)+1);
                out.at(i,j) = (u.at(i,j) + alpha * nb) / denom;
            }
        return out;
    }

    // -----------------------------------------------------------------------
    // jacobi_pressure — one Jacobi sweep of Poisson equation for pressure
    // p_new_ij = (p_neighbors - h²·rhs_ij) / 4
    // -----------------------------------------------------------------------
    template<typename T, typename SP = ts::DefaultStoragePolicy,
             typename CP = ts::DefaultComputationPolicy,
             typename BC = NeumannBC>
    [[nodiscard]] Grid2D<T,SP,CP> jacobi_pressure(
            const Grid2D<T,SP,CP>& p,
            const Grid2D<T,SP,CP>& rhs) {
        const std::size_t R = p.rows(), C = p.cols();
        Grid2D<T,SP,CP> out(R, C, p.h);
        T h2 = p.h * p.h;
        for (std::size_t i = 0; i < R; ++i)
            for (std::size_t j = 0; j < C; ++j) {
                T nb = detail::grid_get<T,SP,CP,BC>(p, static_cast<std::ptrdiff_t>(i)-1, j)
                     + detail::grid_get<T,SP,CP,BC>(p, static_cast<std::ptrdiff_t>(i)+1, j)
                     + detail::grid_get<T,SP,CP,BC>(p, i, static_cast<std::ptrdiff_t>(j)-1)
                     + detail::grid_get<T,SP,CP,BC>(p, i, static_cast<std::ptrdiff_t>(j)+1);
                out.at(i,j) = (nb - h2 * rhs.at(i,j)) / T{4};
            }
        return out;
    }

} // namespace ga

#endif // PEBBLE_CONTAINERS_MATRIX_STENCIL_HPP
