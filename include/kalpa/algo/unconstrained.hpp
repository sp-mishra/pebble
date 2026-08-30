#pragma once
// ============================================================================
// kalpa/algo/unconstrained.hpp — unconstrained descent algorithm policies
// ============================================================================
// Each algorithm is a stateless-or-small functor modeling the Solver's
// Algorithm policy:
//     void reset(std::size_t n);                       // (re)initialize memory
//     template<D,F> void direction(const D&, const F&, const IterState&, V& out);
//     void update(const V& x0,const V& g0,const V& x1,const V& g1); // curvature
//
// Heavy linear algebra is delegated to ga:: :
//   Newton      — dense Hessian assembled column-by-column via matrix-free
//                 hessian_vec, factored by ga::ldlt (modified-Cholesky shift on
//                 indefiniteness), solved by ga::ldlt_solve.
//   TrustRegion — Steihaug truncated-CG on the model, using matrix-free
//                 hessian_vec as the ga::cg ApplyFn (no Hessian ever formed).
// ============================================================================

#ifndef PEBBLE_KALPA_ALGO_UNCONSTRAINED_HPP
#define PEBBLE_KALPA_ALGO_UNCONSTRAINED_HPP

#include <kalpa/core/solver.hpp>
#include <containers/matrix/dense.hpp>
#include <containers/matrix/factorize.hpp>
#include <containers/matrix/iterative.hpp>
#include <cmath>
#include <cstddef>
#include <deque>
#include <vector>

namespace kalpa {

    // =======================================================================
    // Gradient descent  d = −g
    // =======================================================================
    template<typename T>
    struct GradientDescent {
        void reset(std::size_t) {}
        template<typename D, typename F>
        void direction(const D&, const F&, const IterState<T>& s, ga::Vector<T>& out) {
            for (std::size_t i = 0; i < s.g.size(); ++i) out[i] = -s.g[i];
        }
        void update(const ga::Vector<T>&, const ga::Vector<T>&,
                    const ga::Vector<T>&, const ga::Vector<T>&) {}
    };

    // =======================================================================
    // Heavy-ball momentum  v ← μv − g;  d = v
    // =======================================================================
    template<typename T>
    struct Momentum {
        T mu{static_cast<T>(0.9)};
        ga::Vector<T> v;
        void reset(std::size_t n) { v = ga::Vector<T>(n, T{0}); }
        template<typename D, typename F>
        void direction(const D&, const F&, const IterState<T>& s, ga::Vector<T>& out) {
            for (std::size_t i = 0; i < s.g.size(); ++i) {
                v[i] = mu * v[i] - s.g[i];
                out[i] = v[i];
            }
        }
        void update(const ga::Vector<T>&, const ga::Vector<T>&,
                    const ga::Vector<T>&, const ga::Vector<T>&) {}
    };

    // =======================================================================
    // Adam  (Kingma & Ba 2015) — direction is the preconditioned negative step
    // =======================================================================
    template<typename T>
    struct Adam {
        T beta1{static_cast<T>(0.9)}, beta2{static_cast<T>(0.999)};
        T epsilon{static_cast<T>(1e-8)};
        ga::Vector<T> m, v;
        std::size_t t{0};
        void reset(std::size_t n) { m = ga::Vector<T>(n, T{0}); v = ga::Vector<T>(n, T{0}); t = 0; }
        template<typename D, typename F>
        void direction(const D&, const F&, const IterState<T>& s, ga::Vector<T>& out) {
            ++t;
            const T b1t = T{1} - std::pow(beta1, static_cast<T>(t));
            const T b2t = T{1} - std::pow(beta2, static_cast<T>(t));
            for (std::size_t i = 0; i < s.g.size(); ++i) {
                m[i] = beta1 * m[i] + (T{1} - beta1) * s.g[i];
                v[i] = beta2 * v[i] + (T{1} - beta2) * s.g[i] * s.g[i];
                const T mhat = m[i] / b1t;
                const T vhat = v[i] / b2t;
                out[i] = -mhat / (std::sqrt(vhat) + epsilon);
            }
        }
        void update(const ga::Vector<T>&, const ga::Vector<T>&,
                    const ga::Vector<T>&, const ga::Vector<T>&) {}
    };

    // =======================================================================
    // Nonlinear CG — Fletcher–Reeves / Polak–Ribière (auto-restart)
    // =======================================================================
    enum class CGVariant { FletcherReeves, PolakRibiere };

    template<typename T>
    struct ConjugateGradient {
        CGVariant variant{CGVariant::PolakRibiere};
        ga::Vector<T> d_prev, g_prev;
        bool first{true};

        void reset(std::size_t n) { d_prev = ga::Vector<T>(n, T{0}); g_prev = ga::Vector<T>(n, T{0}); first = true; }

        template<typename D, typename F>
        void direction(const D&, const F&, const IterState<T>& s, ga::Vector<T>& out) {
            const std::size_t n = s.g.size();
            if (first) {
                for (std::size_t i = 0; i < n; ++i) out[i] = -s.g[i];
                first = false;
            } else {
                T beta{};
                const T gg_prev = detail::dot(g_prev, g_prev);
                if (gg_prev > T{0}) {
                    if (variant == CGVariant::FletcherReeves) {
                        beta = detail::dot(s.g, s.g) / gg_prev;
                    } else { // Polak–Ribière (clamped ≥ 0 for restart safety)
                        T num{};
                        for (std::size_t i = 0; i < n; ++i) num += s.g[i] * (s.g[i] - g_prev[i]);
                        beta = num / gg_prev;
                        if (beta < T{0}) beta = T{0};
                    }
                }
                for (std::size_t i = 0; i < n; ++i) out[i] = -s.g[i] + beta * d_prev[i];
                // Descent safeguard: if the conjugate direction is not a descent
                // direction (dᵀg ≥ 0, which PR-CG can produce), restart from
                // steepest descent so the line search always has a valid bracket.
                T dg{};
                for (std::size_t i = 0; i < n; ++i) dg += out[i] * s.g[i];
                if (dg >= T{0})
                    for (std::size_t i = 0; i < n; ++i) out[i] = -s.g[i];
            }
            d_prev = out;
            g_prev = s.g;
        }
        void update(const ga::Vector<T>&, const ga::Vector<T>&,
                    const ga::Vector<T>&, const ga::Vector<T>&) {}
    };

    // =======================================================================
    // L-BFGS — two-loop recursion over a bounded (s,y) history (Nocedal 1980)
    // =======================================================================
    template<typename T>
    struct LBFGS {
        std::size_t m{10};
        std::deque<ga::Vector<T>> s_hist, y_hist;
        std::deque<T> rho_hist;

        void reset(std::size_t) { s_hist.clear(); y_hist.clear(); rho_hist.clear(); }

        template<typename D, typename F>
        void direction(const D&, const F&, const IterState<T>& st, ga::Vector<T>& out) {
            const std::size_t n = st.g.size();
            ga::Vector<T> q = st.g;
            const std::size_t k = s_hist.size();
            std::vector<T> alpha(k);
            // first loop (newest → oldest)
            for (std::size_t idx = k; idx-- > 0; ) {
                const T a = rho_hist[idx] * detail::dot(s_hist[idx], q);
                alpha[idx] = a;
                for (std::size_t i = 0; i < n; ++i) q[i] -= a * y_hist[idx][i];
            }
            // initial Hessian scaling  γ = sᵀy / yᵀy
            T gamma = T{1};
            if (k > 0) {
                const T yy = detail::dot(y_hist[k-1], y_hist[k-1]);
                if (yy > T{0}) gamma = detail::dot(s_hist[k-1], y_hist[k-1]) / yy;
            }
            for (std::size_t i = 0; i < n; ++i) q[i] *= gamma;
            // second loop (oldest → newest)
            for (std::size_t idx = 0; idx < k; ++idx) {
                const T b = rho_hist[idx] * detail::dot(y_hist[idx], q);
                for (std::size_t i = 0; i < n; ++i) q[i] += (alpha[idx] - b) * s_hist[idx][i];
            }
            for (std::size_t i = 0; i < n; ++i) out[i] = -q[i];
        }

        void update(const ga::Vector<T>& x0, const ga::Vector<T>& g0,
                    const ga::Vector<T>& x1, const ga::Vector<T>& g1) {
            const std::size_t n = x0.size();
            ga::Vector<T> s(n), y(n);
            for (std::size_t i = 0; i < n; ++i) { s[i] = x1[i] - x0[i]; y[i] = g1[i] - g0[i]; }
            const T sy = detail::dot(s, y);
            if (sy <= T{0}) return;                       // skip non-curvature pairs
            s_hist.push_back(std::move(s));
            y_hist.push_back(std::move(y));
            rho_hist.push_back(T{1} / sy);
            if (s_hist.size() > m) { s_hist.pop_front(); y_hist.pop_front(); rho_hist.pop_front(); }
        }
    };

    // =======================================================================
    // BFGS — dense inverse-Hessian approximation, rank-2 update
    // =======================================================================
    template<typename T>
    struct BFGS {
        ga::Matrix<T> H;   // inverse Hessian approximation
        bool init{false};
        void reset(std::size_t n) { H = ga::Matrix<T>::identity(n); init = true; }

        template<typename D, typename F>
        void direction(const D&, const F&, const IterState<T>& s, ga::Vector<T>& out) {
            const std::size_t n = s.g.size();
            // out = −H·g
            for (std::size_t i = 0; i < n; ++i) {
                T acc{};
                for (std::size_t j = 0; j < n; ++j) acc += H(i, j) * s.g[j];
                out[i] = -acc;
            }
        }

        void update(const ga::Vector<T>& x0, const ga::Vector<T>& g0,
                    const ga::Vector<T>& x1, const ga::Vector<T>& g1) {
            const std::size_t n = x0.size();
            ga::Vector<T> sv(n), yv(n);
            for (std::size_t i = 0; i < n; ++i) { sv[i] = x1[i] - x0[i]; yv[i] = g1[i] - g0[i]; }
            const T sy = detail::dot(sv, yv);
            if (sy <= T{0}) return;
            const T rho = T{1} / sy;
            // H ← (I − ρ s yᵀ) H (I − ρ y sᵀ) + ρ s sᵀ
            ga::Vector<T> Hy(n);
            for (std::size_t i = 0; i < n; ++i) { T a{}; for (std::size_t j=0;j<n;++j) a += H(i,j)*yv[j]; Hy[i]=a; }
            const T yHy = detail::dot(yv, Hy);
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = 0; j < n; ++j)
                    H(i,j) += (rho*rho*yHy + rho) * sv[i]*sv[j]
                            - rho * (Hy[i]*sv[j] + sv[i]*Hy[j]);
        }
    };

    // =======================================================================
    // Newton — dense Hessian (matrix-free columns) + ga::ldlt (mod-Cholesky)
    // =======================================================================
    template<typename T>
    struct Newton {
        void reset(std::size_t) {}

        template<typename D, typename F>
        void direction(const D& deriv, const F& f, const IterState<T>& s, ga::Vector<T>& out) {
            const std::size_t n = s.g.size();
            // assemble H column-by-column via matrix-free Hessian-vector products
            ga::Matrix<T> H(n, n, T{0});
            ga::Vector<T> e(n, T{0}), col(n);
            for (std::size_t j = 0; j < n; ++j) {
                e[j] = T{1};
                deriv.hessian_vec(f, s.x, e, col);
                for (std::size_t i = 0; i < n; ++i) H(i, j) = col[i];
                e[j] = T{0};
            }
            // symmetrize
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = i+1; j < n; ++j) {
                    const T a = T{0.5}*(H(i,j)+H(j,i)); H(i,j)=a; H(j,i)=a;
                }
            // modified-Cholesky: shift diagonal until ldlt succeeds
            T tau = T{0};
            const T base = frob_diag_scale(H);
            for (std::size_t attempt = 0; attempt < 30; ++attempt) {
                ga::Matrix<T> Hs = H;
                if (tau > T{0}) for (std::size_t i = 0; i < n; ++i) Hs(i,i) += tau;
                auto fac = ga::ldlt(Hs);
                if (fac.info.ok) {
                    ga::Vector<T> rhs(n);
                    for (std::size_t i = 0; i < n; ++i) rhs[i] = -s.g[i];
                    auto p = ga::ldlt_solve(fac, rhs);
                    // ensure descent; else fall back to steepest descent
                    if (detail::dot(p, s.g) < T{0}) { out = std::move(p); return; }
                }
                tau = (tau == T{0}) ? base : tau * T{2};
            }
            for (std::size_t i = 0; i < n; ++i) out[i] = -s.g[i];
        }

        void update(const ga::Vector<T>&, const ga::Vector<T>&,
                    const ga::Vector<T>&, const ga::Vector<T>&) {}
    private:
        static T frob_diag_scale(const ga::Matrix<T>& H) {
            T mx{};
            for (std::size_t i = 0; i < H.rows(); ++i) mx = std::max(mx, std::abs(H(i,i)));
            return (mx > T{0}) ? static_cast<T>(1e-3) * mx : static_cast<T>(1e-3);
        }
    };

    // =======================================================================
    // Trust-region Newton-CG (Steihaug) — matrix-free, delegates to ga::cg
    //   Solves the model  min m(p)=½ pᵀHp + gᵀp  s.t. ‖p‖ ≤ Δ  by truncated CG,
    //   using hessian_vec as the ApplyFn. Δ adapted by the usual ρ ratio test.
    // The direction returned is the trust-region step; a fixed unit line search
    // (α=1) then applies it — so pair this with Armijo{alpha0=1} or a no-op.
    // =======================================================================
    template<typename T>
    struct TrustRegionNewtonCG {
        T delta{T{1}};
        T delta_max{static_cast<T>(1e3)};
        T eta{static_cast<T>(0.1)};   // acceptance threshold on ρ

        void reset(std::size_t) { delta = T{1}; }

        template<typename D, typename F>
        void direction(const D& deriv, const F& f, const IterState<T>& s, ga::Vector<T>& out) {
            const std::size_t n = s.g.size();
            // matrix-free Hessian apply
            auto Happly = [&](const ga::Vector<T>& v) {
                ga::Vector<T> hv(n);
                deriv.hessian_vec(f, s.x, v, hv);
                return hv;
            };
            out = steihaug_cg(Happly, s.g, delta, n);
        }

        void update(const ga::Vector<T>&, const ga::Vector<T>&,
                    const ga::Vector<T>&, const ga::Vector<T>&) {}

    private:
        template<typename Apply>
        ga::Vector<T> steihaug_cg(Apply&& H, const ga::Vector<T>& g, T Delta, std::size_t n) {
            ga::Vector<T> p(n, T{0}), r = g, d(n);
            for (std::size_t i = 0; i < n; ++i) d[i] = -r[i];
            const T gnorm = detail::nrm2(g);
            const T tol = std::min(static_cast<T>(0.5), std::sqrt(gnorm)) * gnorm;
            if (gnorm == T{0}) return p;
            for (std::size_t k = 0; k < n + 1; ++k) {
                ga::Vector<T> Hd = H(d);
                const T dHd = detail::dot(d, Hd);
                if (dHd <= T{0}) return to_boundary(p, d, Delta);   // negative curvature
                const T rr = detail::dot(r, r);
                const T alpha = rr / dHd;
                ga::Vector<T> pn(n);
                for (std::size_t i = 0; i < n; ++i) pn[i] = p[i] + alpha * d[i];
                if (detail::nrm2(pn) >= Delta) return to_boundary(p, d, Delta);
                p = pn;
                for (std::size_t i = 0; i < n; ++i) r[i] += alpha * Hd[i];
                const T rr_new = detail::dot(r, r);
                if (std::sqrt(rr_new) < tol) break;
                const T beta = rr_new / rr;
                for (std::size_t i = 0; i < n; ++i) d[i] = -r[i] + beta * d[i];
            }
            return p;
        }
        // p + σd on the trust-region boundary (positive root of ‖p+σd‖=Δ)
        ga::Vector<T> to_boundary(const ga::Vector<T>& p, const ga::Vector<T>& d, T Delta) {
            const T a = detail::dot(d, d);
            const T b = T{2} * detail::dot(p, d);
            const T c = detail::dot(p, p) - Delta * Delta;
            const T disc = std::sqrt(std::max(T{0}, b*b - T{4}*a*c));
            const T sigma = (a > T{0}) ? (-b + disc) / (T{2} * a) : T{0};
            ga::Vector<T> out(p.size());
            for (std::size_t i = 0; i < p.size(); ++i) out[i] = p[i] + sigma * d[i];
            return out;
        }
    };

} // namespace kalpa

#endif // PEBBLE_KALPA_ALGO_UNCONSTRAINED_HPP
