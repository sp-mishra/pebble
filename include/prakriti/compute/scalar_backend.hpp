#pragma once
// ============================================================================
// prakriti/compute/scalar_backend.hpp — tier-1 ComputeBackend: plain stride-1 loops.
// Zero dependencies, always available, autovectorizes under -O2/-O3. The default backend.
// ============================================================================
#include "compute_backend.hpp"
#include <algorithm>

namespace prakriti {

struct ScalarBackend {
    void axpy_const_masked(MSpan out, CSpan mask, Scalar k) const noexcept {
        const std::size_t n = out.size();
        for (std::size_t i = 0; i < n; ++i) out[i] += mask[i] * k;
    }
    void predict(MSpan out, CSpan base, CSpan mask, CSpan v, Scalar k) const noexcept {
        const std::size_t n = out.size();
        for (std::size_t i = 0; i < n; ++i) out[i] = base[i] + mask[i] * v[i] * k;
    }
    void sub_scale(MSpan out, CSpan p, CSpan q, Scalar k) const noexcept {
        const std::size_t n = out.size();
        for (std::size_t i = 0; i < n; ++i) out[i] = (p[i] - q[i]) * k;
    }
    void mul_col(MSpan out, CSpan s) const noexcept {
        const std::size_t n = out.size();
        for (std::size_t i = 0; i < n; ++i) out[i] *= s[i];
    }
    void copy(MSpan out, CSpan src) const noexcept {
        std::copy(src.begin(), src.end(), out.begin());
    }
    void clamp(MSpan out, Scalar lo, Scalar hi) const noexcept {
        const std::size_t n = out.size();
        for (std::size_t i = 0; i < n; ++i) out[i] = std::min(std::max(out[i], lo), hi);
    }
    [[nodiscard]] Scalar kinetic_energy(CSpan vx, CSpan vy, CSpan inv_mass) const noexcept {
        const std::size_t n = vx.size();
        Scalar ke = Scalar(0);
        for (std::size_t i = 0; i < n; ++i) {
            if (inv_mass[i] > Scalar(0)) {
                const Scalar m = Scalar(1) / inv_mass[i];
                ke += Scalar(0.5) * m * (vx[i] * vx[i] + vy[i] * vy[i]);
            }
        }
        return ke;
    }
};

static_assert(ComputeBackend<ScalarBackend>);

} // namespace prakriti
