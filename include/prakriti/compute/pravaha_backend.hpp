#pragma once
// ============================================================================
// prakriti/compute/pravaha_backend.hpp — tier-3 ComputeBackend: Pravaha parallel chunking.
// Expresses uniform column sweeps as chunked multi-core parallel loops via Pravaha's
// task-graph engine.
//
// Guarded on PRAKRITI_ENABLE_PRAVAHA and __has_include("pravaha/pravaha.hpp").
// ============================================================================
#if defined(PRAKRITI_ENABLE_PRAVAHA) && __has_include("pravaha/pravaha.hpp")
#define PRAKRITI_HAS_PRAVAHA_BACKEND 1

#include "compute_backend.hpp"
#include "pravaha/pravaha.hpp"
#include <algorithm>
#include <ranges>
#include <thread>
#include <memory>

namespace prakriti {

class PravahaBackend {
public:
    explicit PravahaBackend(unsigned threads = 0, std::size_t chunk_size = 1024)
        : chunk_size_(chunk_size),
          backend_(std::make_shared<pravaha::JThreadBackend>(threads ? threads : std::thread::hardware_concurrency())),
          runner_(std::make_shared<pravaha::Runner<pravaha::JThreadBackend>>(*backend_)) {}

    void axpy_const_masked(MSpan out, CSpan mask, Scalar k) const {
        const std::size_t n = out.size();
        if (n < chunk_size_) {
            for (std::size_t i = 0; i < n; ++i) out[i] += mask[i] * k;
            return;
        }
        auto r = std::views::iota(std::size_t{0}, n);
        auto expr = pravaha::lazy_parallel_for(
            r, [out, mask, k](std::size_t i) { out[i] += mask[i] * k; }, chunk_size_);
        (void)runner_->submit(std::move(expr));
    }

    void predict(MSpan out, CSpan base, CSpan mask, CSpan v, Scalar k) const {
        const std::size_t n = out.size();
        if (n < chunk_size_) {
            for (std::size_t i = 0; i < n; ++i) out[i] = base[i] + mask[i] * v[i] * k;
            return;
        }
        auto r = std::views::iota(std::size_t{0}, n);
        auto expr = pravaha::lazy_parallel_for(
            r, [out, base, mask, v, k](std::size_t i) { out[i] = base[i] + mask[i] * v[i] * k; }, chunk_size_);
        (void)runner_->submit(std::move(expr));
    }

    void sub_scale(MSpan out, CSpan p, CSpan q, Scalar k) const {
        const std::size_t n = out.size();
        if (n < chunk_size_) {
            for (std::size_t i = 0; i < n; ++i) out[i] = (p[i] - q[i]) * k;
            return;
        }
        auto r = std::views::iota(std::size_t{0}, n);
        auto expr = pravaha::lazy_parallel_for(
            r, [out, p, q, k](std::size_t i) { out[i] = (p[i] - q[i]) * k; }, chunk_size_);
        (void)runner_->submit(std::move(expr));
    }

    void mul_col(MSpan out, CSpan s) const {
        const std::size_t n = out.size();
        if (n < chunk_size_) {
            for (std::size_t i = 0; i < n; ++i) out[i] *= s[i];
            return;
        }
        auto r = std::views::iota(std::size_t{0}, n);
        auto expr = pravaha::lazy_parallel_for(
            r, [out, s](std::size_t i) { out[i] *= s[i]; }, chunk_size_);
        (void)runner_->submit(std::move(expr));
    }

    void copy(MSpan out, CSpan src) const {
        std::copy(src.begin(), src.end(), out.begin());
    }

    void clamp(MSpan out, Scalar lo, Scalar hi) const {
        const std::size_t n = out.size();
        if (n < chunk_size_) {
            for (std::size_t i = 0; i < n; ++i) out[i] = std::min(std::max(out[i], lo), hi);
            return;
        }
        auto r = std::views::iota(std::size_t{0}, n);
        auto expr = pravaha::lazy_parallel_for(
            r, [out, lo, hi](std::size_t i) { out[i] = std::min(std::max(out[i], lo), hi); }, chunk_size_);
        (void)runner_->submit(std::move(expr));
    }

private:
    std::size_t chunk_size_{1024};
    std::shared_ptr<pravaha::JThreadBackend> backend_;
    std::shared_ptr<pravaha::Runner<pravaha::JThreadBackend>> runner_;
};

static_assert(ComputeBackend<PravahaBackend>);

} // namespace prakriti

#endif // PRAKRITI_ENABLE_PRAVAHA
