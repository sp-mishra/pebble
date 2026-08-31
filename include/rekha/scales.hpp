#pragma once

#include "types.hpp"
#include <cmath>

namespace rekha {

struct LinearScale {
    Range domain{0.0f, 1.0f};
    Range range{0.0f, 1.0f};

    [[nodiscard]] constexpr Scalar map(Scalar v) const noexcept {
        const Scalar d = domain.span();
        if (!(d > 0.0f)) return range.min;
        const Scalar t = (v - domain.min) / d;
        return range.min + t * range.span();
    }
};

struct Log10Scale {
    Range domain{1.0f, 10.0f};
    Range range{0.0f, 1.0f};
    Scalar epsilon = 1e-6f;

    [[nodiscard]] Scalar map(Scalar v) const noexcept {
        const Scalar lo = std::max(domain.min, epsilon);
        const Scalar hi = std::max(domain.max, lo + epsilon);
        const Scalar x = std::max(v, epsilon);
        const Scalar d0 = std::log10(lo);
        const Scalar d1 = std::log10(hi);
        const Scalar span = d1 - d0;
        if (!(span > 0.0f)) return range.min;
        const Scalar t = (std::log10(x) - d0) / span;
        return range.min + t * range.span();
    }
};

} // namespace rekha

