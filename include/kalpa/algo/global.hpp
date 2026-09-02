#pragma once
// ============================================================================
// kalpa/algo/global.hpp — derivative-free / global optimizers
// ============================================================================
// Gradient-free methods for multimodal or black-box objectives. Each takes a
// plain objective (Vector → scalar) and a seedable RNG policy so a fixed seed
// reproduces the exact trajectory (deterministic replay — a hard requirement
// of the test plan).
//
// Population objective evaluation is embarrassingly parallel and fans out via
// pravaha::lazy_parallel_for when a ParallelEval policy is supplied; the serial
// policy is the zero-dependency default. CMA-ES's covariance eigendecomposition
// delegates to ga::eig_sym (the sampling transform C = B diag(d²) Bᵀ).
// ============================================================================

#ifndef PEBBLE_KALPA_ALGO_GLOBAL_HPP
#define PEBBLE_KALPA_ALGO_GLOBAL_HPP

#include <kalpa/core/solver.hpp>
#include <containers/matrix/dense.hpp>
#include <containers/matrix/solve.hpp>
#include <containers/matrix/eigen.hpp>
#include <pravaha/pravaha.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <numeric>
#include <random>
#include <vector>

namespace kalpa {
    // =======================================================================
    // RNG policy — deterministic, seedable. mt19937_64 wrapper with the small
    // sampling primitives the methods need.
    // =======================================================================
    struct Rng {
        std::mt19937_64 gen;
        explicit Rng(std::uint64_t seed = 0xC0FFEEu) : gen(seed) {}
        void seed(std::uint64_t s) { gen.seed(s); }

        template <typename T>
        T uniform(T a, T b) {
            return std::uniform_real_distribution<T>(a, b)(gen);
        }

        template <typename T>
        T normal(T mean = T{0}, T sd = T{1}) {
            return std::normal_distribution<T>(mean, sd)(gen);
        }

        std::size_t index(std::size_t n) {
            return std::uniform_int_distribution<std::size_t>(0, n - 1)(gen);
        }
    };

    // =======================================================================
    // Eval policies — how a population of candidate points is scored.
    // =======================================================================
    struct SerialEval {
        template <typename F>
        void operator()(const F& f, const std::vector<ga::Vector<double>>& pop,
                        std::vector<double>& out) const {
            out.resize(pop.size());
            for (std::size_t i = 0; i < pop.size(); ++i) out[i] = f(pop[i]);
        }
    };

    // Parallel population scoring via pravaha. `body` receives the element
    // (an index into an index range); range is a std::vector<std::size_t>.
    struct ParallelEval {
        std::size_t chunk{64};

        template <typename F>
        void operator()(const F& f, const std::vector<ga::Vector<double>>& pop,
                        std::vector<double>& out) const {
            out.assign(pop.size(), 0.0);
            std::vector<std::size_t> idx(pop.size());
            std::iota(idx.begin(), idx.end(), std::size_t{0});
            const auto* pop_ptr = &pop;
            auto* out_ptr = &out;
            auto expr = pravaha::lazy_parallel_for(idx,
                                                   [pop_ptr, out_ptr, &f](std::size_t i) { // body gets element i
                                                       (*out_ptr)[i] = f((*pop_ptr)[i]);
                                                   }, chunk);
            pravaha::Runner<pravaha::JThreadBackend> runner;
            runner.submit(std::move(expr));
        }
    };

    // =======================================================================
    // Nelder–Mead simplex — reflection/expansion/contraction/shrink.
    // =======================================================================
    template <typename T = double>
    struct NelderMead {
        std::size_t max_iter{1000};
        T tol{static_cast<T>(1e-8)};
        T step{static_cast<T>(0.5)}; // initial simplex edge
        T alpha{T{1}}, gamma{T{2}}, rho{static_cast<T>(0.5)}, sigma{static_cast<T>(0.5)};

        template <typename F>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const F& f, const ga::Vector<T>& x0) const {
            const std::size_t n = x0.size();
            std::vector<ga::Vector<T>> S(n + 1, x0);
            std::vector<T> fv(n + 1);
            for (std::size_t i = 0; i < n; ++i) S[i + 1][i] += step;
            for (std::size_t i = 0; i <= n; ++i) fv[i] = f(S[i]);

            std::vector<std::size_t> ord(n + 1);
            Result<T> r;
            r.status = Status::MaxIterations;
            std::size_t it = 0;
            for (; it < max_iter; ++it) {
                std::iota(ord.begin(), ord.end(), std::size_t{0});
                std::sort(ord.begin(), ord.end(), [&](std::size_t a, std::size_t b) { return fv[a] < fv[b]; });
                reorder(S, fv, ord);
                if (std::abs(fv[n] - fv[0]) <= tol) {
                    r.status = Status::Converged;
                    break;
                }

                // centroid of all but worst
                ga::Vector<T> c(n, T{0});
                for (std::size_t i = 0; i < n; ++i) for (std::size_t j = 0; j < n; ++j) c[j] += S[i][j];
                for (std::size_t j = 0; j < n; ++j) c[j] /= static_cast<T>(n);

                ga::Vector<T> xr = axpy(c, alpha, c, S[n]); // reflect
                const T fr = f(xr);
                if (fr < fv[0]) {
                    ga::Vector<T> xe = axpy(c, gamma, c, S[n]); // expand
                    const T fe = f(xe);
                    if (fe < fr) {
                        S[n] = xe;
                        fv[n] = fe;
                    }
                    else {
                        S[n] = xr;
                        fv[n] = fr;
                    }
                }
                else if (fr < fv[n - 1]) {
                    S[n] = xr;
                    fv[n] = fr;
                }
                else {
                    ga::Vector<T> xc = axpy(c, -rho, c, S[n]); // contract
                    const T fc = f(xc);
                    if (fc < fv[n]) {
                        S[n] = xc;
                        fv[n] = fc;
                    }
                    else { // shrink
                        for (std::size_t i = 1; i <= n; ++i) {
                            for (std::size_t j = 0; j < n; ++j) S[i][j] = S[0][j] + sigma * (S[i][j] - S[0][j]);
                            fv[i] = f(S[i]);
                        }
                    }
                }
            }
            r.x = S[0];
            r.f = fv[0];
            r.iterations = it;
            return r;
        }

    private:
        // returns c + k·(c − worst)  (reflection family; k>0 reflect/expand, k<0 contract-inside)
        static ga::Vector<T> axpy(const ga::Vector<T>& c, T k, const ga::Vector<T>& base, const ga::Vector<T>& worst) {
            ga::Vector<T> out(c.size());
            for (std::size_t j = 0; j < c.size(); ++j) out[j] = base[j] + k * (c[j] - worst[j]);
            return out;
        }

        static void reorder(std::vector<ga::Vector<T>>& S, std::vector<T>& fv,
                            const std::vector<std::size_t>& ord) {
            std::vector<ga::Vector<T>> S2(S.size());
            std::vector<T> f2(fv.size());
            for (std::size_t i = 0; i < ord.size(); ++i) {
                S2[i] = S[ord[i]];
                f2[i] = fv[ord[i]];
            }
            S = std::move(S2);
            fv = std::move(f2);
        }
    };

    // =======================================================================
    // Differential Evolution (DE/rand/1/bin).
    // =======================================================================
    template <typename T = double, typename Eval = SerialEval>
    struct DifferentialEvolution {
        std::size_t pop_size{0}; // 0 → 10·n
        std::size_t max_gen{500};
        T F{static_cast<T>(0.8)}; // differential weight
        T CR{static_cast<T>(0.9)}; // crossover prob
        Eval eval{};

        template <typename Fn>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const Fn& f, const ga::Vector<T>& lo, const ga::Vector<T>& hi, Rng rng = Rng{}) const {
            const std::size_t n = lo.size();
            const std::size_t NP = pop_size ? pop_size : 10 * n;
            std::vector<ga::Vector<T>> pop(NP, ga::Vector<T>(n));
            for (auto& ind : pop)
                for (std::size_t j = 0; j < n; ++j) ind[j] = rng.uniform(lo[j], hi[j]);
            std::vector<T> fit;
            eval(f, pop, fit);

            std::size_t best = argmin(fit);
            for (std::size_t g = 0; g < max_gen; ++g) {
                std::vector<ga::Vector<T>> trial(NP, ga::Vector<T>(n));
                for (std::size_t i = 0; i < NP; ++i) {
                    std::size_t a = pick(rng, NP, i), b = pick(rng, NP, i, a), c = pick(rng, NP, i, a, b);
                    const std::size_t R = rng.index(n);
                    for (std::size_t j = 0; j < n; ++j) {
                        if (rng.template uniform<T>(0, 1) < CR || j == R) {
                            T v = pop[a][j] + F * (pop[b][j] - pop[c][j]);
                            v = std::min(std::max(v, lo[j]), hi[j]);
                            trial[i][j] = v;
                        }
                        else trial[i][j] = pop[i][j];
                    }
                }
                std::vector<T> tf;
                eval(f, trial, tf);
                for (std::size_t i = 0; i < NP; ++i)
                    if (tf[i] <= fit[i]) {
                        pop[i] = trial[i];
                        fit[i] = tf[i];
                    }
                best = argmin(fit);
            }
            Result<T> r;
            r.x = pop[best];
            r.f = fit[best];
            r.iterations = max_gen;
            r.status = Status::MaxIterations;
            return r;
        }

    private:
        static std::size_t argmin(const std::vector<T>& v) {
            return static_cast<std::size_t>(std::min_element(v.begin(), v.end()) - v.begin());
        }

        static std::size_t pick(Rng& rng, std::size_t NP, std::size_t x,
                                std::size_t y = ~std::size_t{0}, std::size_t z = ~std::size_t{0}) {
            std::size_t r;
            do { r = rng.index(NP); }
            while (r == x || r == y || r == z);
            return r;
        }
    };

    // =======================================================================
    // Simulated Annealing — geometric cooling, Gaussian proposals.
    // =======================================================================
    template <typename T = double>
    struct SimulatedAnnealing {
        std::size_t max_iter{10000};
        T T0{T{1}}; // initial temperature
        T cooling{static_cast<T>(0.995)};
        T proposal_sd{static_cast<T>(0.5)};

        template <typename F>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const F& f, const ga::Vector<T>& x0, Rng rng = Rng{}) const {
            const std::size_t n = x0.size();
            ga::Vector<T> x = x0, best = x0, cand(n);
            T fx = f(x), fbest = fx, temp = T0;
            for (std::size_t it = 0; it < max_iter; ++it) {
                for (std::size_t j = 0; j < n; ++j) cand[j] = x[j] + rng.template normal<T>(0, proposal_sd);
                const T fc = f(cand);
                const T dE = fc - fx;
                if (dE <= T{0} || rng.template uniform<T>(0, 1) <
                    std::exp(-dE / std::max(temp, static_cast<T>(1e-12)))) {
                    x = cand;
                    fx = fc;
                    if (fx < fbest) {
                        best = x;
                        fbest = fx;
                    }
                }
                temp *= cooling;
            }
            Result<T> r;
            r.x = best;
            r.f = fbest;
            r.iterations = max_iter;
            r.status = Status::MaxIterations;
            return r;
        }
    };

    // =======================================================================
    // CMA-ES — (μ/μ_w, λ) with rank-μ + rank-one covariance update. The
    // sampling transform y = B·diag(d)·z uses ga::eig_sym on C each update.
    // =======================================================================
    template <typename T = double, typename Eval = SerialEval>
    struct CMAES {
        std::size_t max_gen{500};
        std::size_t lambda{0}; // 0 → 4 + ⌊3 ln n⌋
        T sigma0{static_cast<T>(0.5)};
        T tol{static_cast<T>(1e-11)};
        Eval eval{};

        template <typename F>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const F& f, const ga::Vector<T>& x0, Rng rng = Rng{}) const {
            const std::size_t n = x0.size();
            const std::size_t lam = lambda ? lambda : (4 + static_cast<std::size_t>(3 * std::log((double)n)));
            const std::size_t mu = lam / 2;

            // recombination weights
            std::vector<T> w(mu);
            T wsum{}, wsq{};
            for (std::size_t i = 0; i < mu; ++i) {
                w[i] = std::log((T)mu + T{0.5}) - std::log((T)(i + 1));
                wsum += w[i];
            }
            for (auto& wi : w) wi /= wsum;
            for (auto wi : w) wsq += wi * wi;
            const T mueff = T{1} / wsq;

            // strategy parameters
            const T cc = (T{4} + mueff / (T)n) / ((T)n + T{4} + T{2} * mueff / (T)n);
            const T cs = (mueff + T{2}) / ((T)n + mueff + T{5});
            const T c1 = T{2} / (((T)n + T{1.3}) * ((T)n + T{1.3}) + mueff);
            const T cmu = std::min(
                T{1} - c1, T{2} * (mueff - T{2} + T{1} / mueff) / (((T)n + T{2}) * ((T)n + T{2}) + mueff));
            const T damps = T{1} + T{2} * std::max(T{0}, std::sqrt((mueff - T{1}) / ((T)n + T{1})) - T{1}) + cs;
            const T chiN = std::sqrt((T)n) * (T{1} - T{1} / (T{4} * (T)n) + T{1} / (T{21} * (T)n * (T)n));

            ga::Vector<T> mean = x0, ps(n, T{0}), pc(n, T{0});
            T sigma = sigma0;
            ga::Matrix<T> C = ga::Matrix<T>::identity(n);

            Result<T> r;
            r.x = x0;
            r.f = f(x0);
            r.status = Status::MaxIterations;
            for (std::size_t gen = 0; gen < max_gen; ++gen) {
                // eigendecomposition C = B diag(d²) Bᵀ
                auto es = ga::eig_sym(C);
                std::vector<T> d(n);
                for (std::size_t i = 0; i < n; ++i)
                    d[i] =
                        std::sqrt(std::max(es.eigenvalues[i], static_cast<T>(1e-20)));
                const auto& B = es.eigenvectors;

                // sample λ offspring  x = mean + σ B d z
                std::vector<ga::Vector<T>> pop(lam, ga::Vector<T>(n));
                std::vector<ga::Vector<T>> zs(lam, ga::Vector<T>(n));
                for (std::size_t k = 0; k < lam; ++k) {
                    ga::Vector<T> z(n), Bdz(n, T{0});
                    for (std::size_t i = 0; i < n; ++i) z[i] = rng.template normal<T>(0, 1);
                    for (std::size_t i = 0; i < n; ++i) {
                        T acc{};
                        for (std::size_t j = 0; j < n; ++j) acc += B(i, j) * d[j] * z[j];
                        Bdz[i] = acc;
                    }
                    for (std::size_t i = 0; i < n; ++i) pop[k][i] = mean[i] + sigma * Bdz[i];
                    zs[k] = z;
                }
                std::vector<T> fit;
                eval(f, pop, fit);

                // sort by fitness
                std::vector<std::size_t> ord(lam);
                std::iota(ord.begin(), ord.end(), std::size_t{0});
                std::sort(ord.begin(), ord.end(), [&](std::size_t a, std::size_t b) { return fit[a] < fit[b]; });
                if (fit[ord[0]] < r.f) {
                    r.f = fit[ord[0]];
                    r.x = pop[ord[0]];
                }

                // recombination — new mean + averaged z (in C-normal coords: y = B d z)
                ga::Vector<T> newmean(n, T{0}), zmean(n, T{0}), ymean(n, T{0});
                for (std::size_t i = 0; i < mu; ++i) {
                    const std::size_t k = ord[i];
                    for (std::size_t j = 0; j < n; ++j) {
                        newmean[j] += w[i] * pop[k][j];
                        zmean[j] += w[i] * zs[k][j];
                    }
                }
                for (std::size_t i = 0; i < n; ++i) {
                    T acc{};
                    for (std::size_t j = 0; j < n; ++j) acc += B(i, j) * d[j] * zmean[j];
                    ymean[i] = acc;
                }

                // step-size path  ps = (1−cs)ps + sqrt(cs(2−cs)mueff) B zmean
                ga::Vector<T> Bzmean(n, T{0});
                for (std::size_t i = 0; i < n; ++i) {
                    T a{};
                    for (std::size_t j = 0; j < n; ++j) a += B(i, j) * zmean[j];
                    Bzmean[i] = a;
                }
                const T cscoef = std::sqrt(cs * (T{2} - cs) * mueff);
                for (std::size_t i = 0; i < n; ++i) ps[i] = (T{1} - cs) * ps[i] + cscoef * Bzmean[i];
                const T ps_norm = detail::nrm2(ps);

                // covariance path  pc
                const T hsig = (ps_norm / std::sqrt(T{1} - std::pow(T{1} - cs, T{2} * (T)(gen + 1))) / chiN
                                   < (T{1.4} + T{2} / ((T)n + T{1})))
                                   ? T{1}
                                   : T{0};
                const T pccoef = std::sqrt(cc * (T{2} - cc) * mueff);
                for (std::size_t i = 0; i < n; ++i) pc[i] = (T{1} - cc) * pc[i] + hsig * pccoef * ymean[i];

                // covariance update  C = (1−c1−cmu)C + c1(pc pcᵀ + δ C) + cmu Σ wᵢ yᵢ yᵢᵀ
                const T delta = (T{1} - hsig) * cc * (T{2} - cc);
                for (std::size_t a = 0; a < n; ++a)
                    for (std::size_t bcol = 0; bcol < n; ++bcol) {
                        T rankmu{};
                        for (std::size_t i = 0; i < mu; ++i) {
                            const std::size_t k = ord[i];
                            // yᵢ = B d zᵢ
                            T ya{}, yb{};
                            for (std::size_t j = 0; j < n; ++j) {
                                ya += B(a, j) * d[j] * zs[k][j];
                                yb += B(bcol, j) * d[j] * zs[k][j];
                            }
                            rankmu += w[i] * ya * yb;
                        }
                        C(a, bcol) = (T{1} - c1 - cmu) * C(a, bcol)
                            + c1 * (pc[a] * pc[bcol] + delta * C(a, bcol))
                            + cmu * rankmu;
                    }

                mean = newmean;
                sigma *= std::exp((cs / damps) * (ps_norm / chiN - T{1}));

                if (sigma < tol) {
                    r.status = Status::Converged;
                    r.iterations = gen;
                    break;
                }
            }
            if (r.iterations == 0) r.iterations = max_gen;
            return r;
        }
    };

    // =======================================================================
    // Bayesian optimization — Gaussian-process surrogate + acquisition search.
    //   Fit a GP (RBF kernel, homoscedastic noise) to the observations, then
    //   pick the next query by maximizing an acquisition function over a random
    //   candidate pool inside the box [lo, hi]. Two acquisitions:
    //     Expected Improvement  EI(x) = (f⁺−μ)Φ(z) + s·φ(z),  z=(f⁺−μ)/s
    //     Lower Confidence Bound LCB(x) = μ − β·s     (minimize ⇒ minimize LCB)
    //   The GP posterior is
    //     μ(x*) = k*ᵀ α ,   α = (K + noise·I)⁻¹ y      (SPD Cholesky via ga::solve)
    //     s²(x*) = k(x*,x*) − k*ᵀ (K+noise·I)⁻¹ k*
    //   A seedable Rng makes the initial design and candidate pools reproducible.
    //   Candidate scoring fans out through the Eval policy (serial default).
    //   TPE is a documented follow-on; EI/LCB BO ships here.
    // =======================================================================
    enum class Acquisition { ExpectedImprovement, LowerConfidenceBound };

    template <typename T = double, typename Eval = SerialEval>
    struct BayesianOptimization {
        std::size_t init_samples{8};
        std::size_t max_iter{50};
        std::size_t cand_pool{512};
        T length_scale{T{1}};
        T signal_var{T{1}};
        T noise{static_cast<T>(1e-6)};
        T beta{T{2}};
        Acquisition acq{Acquisition::ExpectedImprovement};
        [[no_unique_address]] Eval eval{};

        template <typename F>
        [[nodiscard]] std::expected<Result<T>, Diagnosis>
        solve(const F& f, const ga::Vector<T>& lo, const ga::Vector<T>& hi,
              Rng rng = Rng{}) const {
            const std::size_t d = lo.size();
            if (d == 0 || hi.size() != d)
                return std::unexpected(Diagnosis{Cause::NumericalError, "bad bounds", 0});

            auto sample = [&](ga::Vector<T>& out) {
                for (std::size_t j = 0; j < d; ++j) out[j] = rng.template uniform<T>(lo[j], hi[j]);
            };
            auto kern = [&](const ga::Vector<T>& a, const ga::Vector<T>& b) {
                T s2{};
                for (std::size_t j = 0; j < d; ++j) {
                    const T dj = a[j] - b[j];
                    s2 += dj * dj;
                }
                return signal_var * std::exp(-s2 / (T{2} * length_scale * length_scale));
            };

            // initial design
            std::vector<ga::Vector<T>> X;
            std::vector<T> y;
            X.reserve(init_samples + max_iter);
            y.reserve(init_samples + max_iter);
            for (std::size_t i = 0; i < init_samples; ++i) {
                ga::Vector<T> xi(d);
                sample(xi);
                X.push_back(xi);
                y.push_back(f(xi));
            }
            std::size_t best = 0;
            for (std::size_t i = 1; i < y.size(); ++i) if (y[i] < y[best]) best = i;

            const T SQRT2 = std::sqrt(T{2});
            const T INV_SQRT_2PI = static_cast<T>(0.3989422804014327);
            auto normcdf = [&](T z) { return T{0.5} * (T{1} + std::erf(z / SQRT2)); };
            auto normpdf = [&](T z) { return INV_SQRT_2PI * std::exp(-T{0.5} * z * z); };

            for (std::size_t it = 0; it < max_iter; ++it) {
                const std::size_t nobs = X.size();
                // Gram matrix K + noise·I  (SPD)
                ga::Matrix<T> K(nobs, nobs, T{0});
                for (std::size_t i = 0; i < nobs; ++i) {
                    for (std::size_t j = 0; j < nobs; ++j) K(i, j) = kern(X[i], X[j]);
                    K(i, i) += noise;
                }
                ga::Vector<T> yv(nobs);
                for (std::size_t i = 0; i < nobs; ++i) yv[i] = y[i];
                ga::Vector<T> alpha = ga::solve(K, yv, ga::MatrixKind::SPD); // α = K⁻¹y
                for (std::size_t i = 0; i < nobs; ++i)
                    if (!std::isfinite(alpha[i]))
                        return std::unexpected(Diagnosis{Cause::NumericalError, "GP solve failed", it});

                const T f_best = y[best];

                // candidate pool + acquisition
                ga::Vector<T> x_next(d);
                bool have_next = false;
                T best_acq = std::numeric_limits<T>::lowest();
                ga::Vector<T> cand(d), kstar(nobs), Kinv_kstar(nobs);
                for (std::size_t c = 0; c < cand_pool; ++c) {
                    sample(cand);
                    for (std::size_t i = 0; i < nobs; ++i) kstar[i] = kern(cand, X[i]);
                    // μ = k*ᵀα
                    T mu{};
                    for (std::size_t i = 0; i < nobs; ++i) mu += kstar[i] * alpha[i];
                    // s² = k** − k*ᵀ K⁻¹ k*
                    Kinv_kstar = ga::solve(K, kstar, ga::MatrixKind::SPD);
                    T kss = kern(cand, cand), quad{};
                    for (std::size_t i = 0; i < nobs; ++i) quad += kstar[i] * Kinv_kstar[i];
                    T var = kss - quad;
                    if (var < T{0}) var = T{0};
                    const T s = std::sqrt(var);

                    T a{};
                    if (acq == Acquisition::LowerConfidenceBound) {
                        a = -(mu - beta * s); // maximize −LCB (we minimize f)
                    }
                    else {
                        if (s > T{0}) {
                            const T z = (f_best - mu) / s;
                            a = (f_best - mu) * normcdf(z) + s * normpdf(z);
                        }
                        else a = T{0};
                    }
                    if (a > best_acq) {
                        best_acq = a;
                        x_next = cand;
                        have_next = true;
                    }
                }
                if (!have_next) break;

                const T fx = f(x_next);
                X.push_back(x_next);
                y.push_back(fx);
                if (fx < y[best]) best = y.size() - 1;
            }

            Result<T> r;
            r.x = X[best];
            r.f = y[best];
            r.grad_norm = T{0};
            r.iterations = max_iter;
            r.status = Status::Converged;
            return r;
        }
    };
} // namespace kalpa

#endif // PEBBLE_KALPA_ALGO_GLOBAL_HPP
