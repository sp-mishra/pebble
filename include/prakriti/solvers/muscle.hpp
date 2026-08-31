#pragma once
// ============================================================================
// prakriti/solvers/muscle.hpp — XPBD-style active muscle projection solver.
// ============================================================================

#include "../constraints/muscle.hpp"
#include "../state/muscle_store.hpp"
#include "../state/particle_store.hpp"
#include "../core/config.hpp"
#include <containers/cache/kosha.hpp>
#include <containers/matrix/static.hpp>
#include <containers/numeric/math_vector.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(PRAKRITI_ENABLE_PRAVAHA) && __has_include("pravaha/pravaha.hpp")
#define PRAKRITI_HAS_MUSCLE_PRAVAHA 1
#include "pravaha/pravaha.hpp"
#endif

namespace prakriti {

template <typename Cfg = MuscleConstraintCfg<>>
struct MuscleSolver {
    Scalar max_shortening_velocity = Scalar(12.0f);
    Scalar tendon_linear_k = Scalar(35.0f);
    Scalar tendon_toe_strain = Scalar(0.03f);

#if defined(PRAKRITI_HAS_MUSCLE_PRAVAHA)
    explicit MuscleSolver(unsigned threads = 0, std::size_t chunk_size = 1024)
        : chunk_size_(chunk_size),
          backend_(threads ? threads : std::thread::hardware_concurrency()),
          runner_(backend_) {}
#else
    explicit MuscleSolver(unsigned = 0, std::size_t = 1024) {}
#endif

    void solve_substep(MuscleStore<Cfg>& store,
                       ParticleStore& particles,
                       Scalar dt,
                       Scalar inv_dt2) noexcept {
        const std::size_t total = store.size();
        if (total == 0) return;

        if constexpr (!std::is_same_v<typename Cfg::fatigue, NoFatigue>) {
            store.fatigue_col.update(store.fatigue_col.data, store.activation, dt);
        }

        reserve_scratch(total);
        gather_rows(store, particles, total);
        if (active_input_ == 0) return;

        Cfg::fiber::compute_force_batch(
            std::span<const Scalar>{a_eff_.data(), total},
            std::span<const Scalar>{lce_.data(), total},
            std::span<const Scalar>{vce_.data(), total},
            std::span<const Scalar>{lopt_.data(), total},
            max_shortening_velocity,
            std::span<const Scalar>{fmax_.data(), total},
            std::span<const Scalar>{pennation_.data(), total},
            std::span<Scalar>{fiber_force_.data(), total});

        Cfg::tendon::force_batch(
            std::span<const Scalar>{len_.data(), total},
            std::span<const Scalar>{slack_.data(), total},
            tendon_linear_k,
            tendon_toe_strain,
            std::span<Scalar>{tendon_force_.data(), total});

        compute_deltas(store, total, inv_dt2);

        for (std::size_t k = 0; k < total; ++k) {
            if (!valid_[k]) continue;
            const std::uint32_t row = row_[k];
            const Scalar dlambda = dlambda_[k];
            store.lambda_accum[row] = std::clamp(store.lambda_accum[row] + dlambda, Scalar(-1e4f), Scalar(1e4f));

            const ga::Vec2<Scalar> corr{corr_x_[k], corr_y_[k]};
            ga::Vec2<Scalar> pa{particles.pred_x[ia_[k]], particles.pred_y[ia_[k]]};
            ga::Vec2<Scalar> pb{particles.pred_x[ib_[k]], particles.pred_y[ib_[k]]};
            ga::axpy( wa_[k], corr, pa);
            ga::axpy(-wb_[k], corr, pb);
            particles.pred_x[ia_[k]] = pa(0, 0);
            particles.pred_y[ia_[k]] = pa(1, 0);
            particles.pred_x[ib_[k]] = pb(0, 0);
            particles.pred_y[ib_[k]] = pb(1, 0);
        }
    }

    template <typename Cache = kosha::LRUCache<std::uint32_t, Scalar>>
    void cache_lambdas(const MuscleStore<Cfg>& store, Cache& cache) noexcept {
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(store.size()); ++i) {
            (void)cache.put(i, store.lambda_accum[i]);
        }
    }

private:
    void gather_rows(MuscleStore<Cfg>& store, ParticleStore& particles, std::size_t total) {
        active_input_ = 0;

        auto gather_one = [&](std::size_t i) {
            valid_input_[i] = 0;
            row_[i] = static_cast<std::uint32_t>(i);
            ia_[i] = 0;
            ib_[i] = 0;
            wa_[i] = Scalar(0);
            wb_[i] = Scalar(0);
            len_[i] = Scalar(1);
            grad_x_[i] = Scalar(0);
            grad_y_[i] = Scalar(0);
            a_eff_[i] = Scalar(0);
            lopt_[i] = Scalar(1e-4f);
            lce_[i] = Scalar(1e-4f);
            vce_[i] = Scalar(0);
            fmax_[i] = Scalar(1e-3f);
            pennation_[i] = Scalar(0);
            slack_[i] = Scalar(1e-4f);
            rest_[i] = Scalar(1e-4f);

            const Index ia = store.origin[i];
            const Index ib = store.insertion[i];
            if (ia >= particles.size() || ib >= particles.size()) return;

            const Scalar wa = particles.inv_mass[ia];
            const Scalar wb = particles.inv_mass[ib];
            if (wa + wb <= Scalar(0)) return;

            const Scalar dx = particles.pred_x[ia] - particles.pred_x[ib];
            const Scalar dy = particles.pred_y[ia] - particles.pred_y[ib];
            const ga::Vec2<Scalar> d{dx, dy};
            const Scalar len2 = ga::nrm2_sq(d);
            if (len2 <= Scalar(1e-14f)) return;
            const Scalar len = std::sqrt(len2);
            if (len <= Scalar(1e-7f)) return;

            const Scalar inv_len = Scalar(1) / len;
            const Scalar gx = dx * inv_len;
            const Scalar gy = dy * inv_len;

            const Scalar vrel_x = particles.vel_x[ia] - particles.vel_x[ib];
            const Scalar vrel_y = particles.vel_y[ia] - particles.vel_y[ib];
            const ga::Vec2<Scalar> vrel{vrel_x, vrel_y};
            const ga::Vec2<Scalar> grad{gx, gy};

            using FatiguePolicy = typename Cfg::fatigue;
            Scalar a_eff = std::clamp(store.activation[i], Scalar(0), Scalar(1));
            if constexpr (std::is_same_v<FatiguePolicy, NoFatigue>) {
                typename FatiguePolicy::State s{};
                a_eff = FatiguePolicy::effective_activation(s, a_eff);
            } else {
                a_eff = FatiguePolicy::effective_activation(store.fatigue_col.data[i], a_eff);
            }

            ia_[i] = ia;
            ib_[i] = ib;
            wa_[i] = wa;
            wb_[i] = wb;
            len_[i] = len;
            grad_x_[i] = gx;
            grad_y_[i] = gy;
            a_eff_[i] = a_eff;
            lopt_[i] = std::max(store.optimal_fiber_length[i], Scalar(1e-4f));
            lce_[i] = std::max(len - store.tendon_slack_length[i], Scalar(1e-4f));
            vce_[i] = ga::dot(vrel, grad);
            fmax_[i] = std::max(store.max_isometric_force[i], Scalar(1e-3f));
            pennation_[i] = store.pennation_angle[i];
            slack_[i] = std::max(store.tendon_slack_length[i], Scalar(1e-4f));
            rest_[i] = std::max(store.rest_length[i], Scalar(1e-4f));
            valid_input_[i] = 1;
        };

#if defined(PRAKRITI_HAS_MUSCLE_PRAVAHA)
        if (total >= chunk_size_ * 2) {
            auto r = std::views::iota(std::size_t{0}, total);
            auto expr = pravaha::lazy_parallel_for(
                r, [gather_one](std::size_t i) mutable { gather_one(i); }, chunk_size_);
            (void)runner_.submit(std::move(expr));
        } else
#endif
        {
            for (std::size_t i = 0; i < total; ++i) gather_one(i);
        }

        for (std::size_t i = 0; i < total; ++i) {
            active_input_ += valid_input_[i] ? std::size_t{1} : std::size_t{0};
        }
    }

    void compute_deltas(MuscleStore<Cfg>& store, std::size_t total, Scalar inv_dt2) {
        auto solve_one = [&](std::size_t k) {
            if (!valid_input_[k]) {
                valid_[k] = 0;
                return;
            }
            const std::uint32_t row = row_[k];
            const Scalar force_ratio = std::clamp(fiber_force_[k] / fmax_[k], Scalar(0), Scalar(1));
            const Scalar active_shortening = a_eff_[k] * (Scalar(0.18f) + Scalar(0.07f) * force_ratio) * lopt_[k];
            const Scalar tendon_extension = std::clamp(tendon_force_[k] * Scalar(1e-3f), Scalar(0), rest_[k] * Scalar(0.35f));
            const Scalar target_len = std::clamp(
                rest_[k] - active_shortening + tendon_extension,
                rest_[k] * Scalar(0.5f),
                rest_[k] * Scalar(1.35f));

            const Scalar C = len_[k] - target_len;
            const Scalar stiffness_gain = Scalar(1) + Scalar(0.35f) * force_ratio;
            const Scalar compliance = Scalar(1)
                / std::max(fmax_[k] * (Scalar(0.25f) + a_eff_[k]) * stiffness_gain, Scalar(1e-3f));
            const Scalar alpha = compliance * inv_dt2;

            const Scalar denom = wa_[k] + wb_[k] + alpha;
            if (denom <= Scalar(1e-8f)) {
                valid_[k] = 0;
                return;
            }

            const Scalar dlambda = (-C - alpha * store.lambda_accum[row]) / denom;
            if (!std::isfinite(dlambda)) {
                valid_[k] = 0;
                store.lambda_accum[row] = Scalar(0);
                return;
            }

            dlambda_[k] = dlambda;
            corr_x_[k] = grad_x_[k] * dlambda;
            corr_y_[k] = grad_y_[k] * dlambda;
            valid_[k] = 1;
        };

#if defined(PRAKRITI_HAS_MUSCLE_PRAVAHA)
        if (total >= chunk_size_ * 2) {
            auto r = std::views::iota(std::size_t{0}, total);
            auto expr = pravaha::lazy_parallel_for(
                r, [solve_one](std::size_t i) mutable { solve_one(i); }, chunk_size_);
            (void)runner_.submit(std::move(expr));
            return;
        }
#endif
        for (std::size_t k = 0; k < total; ++k) solve_one(k);
    }

    void reserve_scratch(std::size_t n) {
        auto grow = [n](auto& v) {
            if (v.size() < n) v.resize(n);
        };
        grow(row_);
        grow(ia_);
        grow(ib_);
        grow(wa_);
        grow(wb_);
        grow(len_);
        grow(grad_x_);
        grow(grad_y_);
        grow(a_eff_);
        grow(lopt_);
        grow(lce_);
        grow(vce_);
        grow(fmax_);
        grow(pennation_);
        grow(slack_);
        grow(rest_);
        grow(fiber_force_);
        grow(tendon_force_);
        grow(dlambda_);
        grow(corr_x_);
        grow(corr_y_);
        grow(valid_);
        grow(valid_input_);
    }

    std::vector<std::uint32_t> row_;
    std::vector<Index> ia_;
    std::vector<Index> ib_;
    std::vector<Scalar> wa_;
    std::vector<Scalar> wb_;
    std::vector<Scalar> len_;
    std::vector<Scalar> grad_x_;
    std::vector<Scalar> grad_y_;
    std::vector<Scalar> a_eff_;
    std::vector<Scalar> lopt_;
    std::vector<Scalar> lce_;
    std::vector<Scalar> vce_;
    std::vector<Scalar> fmax_;
    std::vector<Scalar> pennation_;
    std::vector<Scalar> slack_;
    std::vector<Scalar> rest_;
    std::vector<Scalar> fiber_force_;
    std::vector<Scalar> tendon_force_;
    std::vector<Scalar> dlambda_;
    std::vector<Scalar> corr_x_;
    std::vector<Scalar> corr_y_;
    std::vector<std::uint8_t> valid_;
    std::vector<std::uint8_t> valid_input_;
    std::size_t active_input_ = 0;

#if defined(PRAKRITI_HAS_MUSCLE_PRAVAHA)
    std::size_t chunk_size_{1024};
    pravaha::JThreadBackend backend_;
    pravaha::Runner<pravaha::JThreadBackend> runner_;
#endif
};

} // namespace prakriti
