#pragma once
// =============================================================================
// tarka/features.hpp — Theory Feature Extraction + Capability-Mask Router
//
// Namespace:  tarka::features  (extractor)
//             tarka            (RouterEngine with adaptive routing)
//
// Provides:
//   theory_extractor         — emits: bv_ratio, lra_ratio, lia_ratio, nra_ratio,
//                                     nia_ratio, quantifier_depth, array_flag,
//                                     uf_flag, dag_compression_ratio (9 dims)
//   theory_router<Backends...> — capability-mask filter + adaptive cost dispatch
//                                over a compile-time backend set
//
// Design:
//   - Capability-mask filter: backend eligible iff capabilities() ⊇ formula mask.
//   - Zero-dependency: internal SBO feature_vector and Kosha cache store.
// =============================================================================

#include "tarka/tarka.hpp"
#include "containers/cache/kosha.hpp"

#if __has_include("edsl/lithe_feature_extractor.hpp")
#include "edsl/lithe_feature_extractor.hpp"
#include "edsl/lithe_feature_store.hpp"
#else
namespace lithe::features {
    class feature_vector {
    public:
        static constexpr std::size_t kMaxDims = 16;

        void append(float v) noexcept {
            if (size_ < kMaxDims) data_[size_++] = v;
        }

        [[nodiscard]] float operator[](std::size_t idx) const noexcept {
            return idx < size_ ? data_[idx] : 0.0f;
        }

        [[nodiscard]] std::size_t size() const noexcept { return size_; }

    private:
        float data_[kMaxDims] = {};
        std::size_t size_ = 0;
    };

    enum class feature_source : std::uint8_t { custom, learned };

    class feature_store {
    public:
        static feature_store& global() {
            static feature_store s;
            return s;
        }

        void put(std::uint64_t hash, const feature_vector& fv, feature_source) {
            (void)cache_.put(hash, fv);
        }

        [[nodiscard]] std::optional<feature_vector> get(std::uint64_t hash) {
            auto r = cache_.get(hash);
            if (r.has_value()) return *r;
            return std::nullopt;
        }

    private:
        kosha::ShardedLRUCache<std::uint64_t, feature_vector> cache_{512};
    };

    template <class Extractor, class TermType>
    concept feature_extractor = requires(const Extractor& e, const TermType& t) {
        { e.extract(t) } -> std::same_as<feature_vector>;
    };
} // namespace lithe::features
#endif

#include <cstddef>

namespace tarka::features {
    // =========================================================================
    // theory_extractor
    // =========================================================================

    struct theory_extractor {
        static constexpr std::size_t kDims = 9;

        [[nodiscard]] lithe::features::feature_vector extract(const Term& root) const {
            lithe::features::feature_vector fv;

            std::size_t total_nodes = 0;
            std::size_t bv_ops = 0, lra_ops = 0, lia_ops = 0, nra_ops = 0, nia_ops = 0;
            std::size_t quant_depth = 0, array_ops = 0, uf_ops = 0;

            // Post-order walk — count ops and theory bits
            constexpr std::size_t kStackCap = 512;
            const Term* stack_buf[kStackCap];
            std::size_t depth = 0;
            stack_buf[depth++] = &root;

            while (depth > 0) {
                const Term* cur = stack_buf[--depth];
                ++total_nodes;
                const theory_mask bits = get_op_info(cur->op()).theory_bits;

                if (bits & theory_bit(theory_family::bv)) ++bv_ops;
                if (bits & theory_bit(theory_family::lra)) ++lra_ops;
                if (bits & theory_bit(theory_family::lia)) ++lia_ops;
                if (bits & theory_bit(theory_family::nra)) ++nra_ops;
                if (bits & theory_bit(theory_family::nia)) ++nia_ops;
                if (bits & theory_bit(theory_family::array)) ++array_ops;
                if (bits & theory_bit(theory_family::uf)) ++uf_ops;

                const Op op = cur->op();
                if (op == Op::Forall || op == Op::Exists) ++quant_depth;

                for (const Term& c : cur->children()) {
                    if (depth < kStackCap)
                        stack_buf[depth++] = &c;
                }
            }

            const float n = total_nodes > 0 ? static_cast<float>(total_nodes) : 1.0f;
            fv.append(static_cast<float>(bv_ops) / n); // bv_ratio
            fv.append(static_cast<float>(lra_ops) / n); // lra_ratio
            fv.append(static_cast<float>(lia_ops) / n); // lia_ratio
            fv.append(static_cast<float>(nra_ops) / n); // nra_ratio
            fv.append(static_cast<float>(nia_ops) / n); // nia_ratio
            fv.append(static_cast<float>(quant_depth)); // quantifier_depth (raw count)
            fv.append(array_ops > 0 ? 1.0f : 0.0f); // array_flag
            fv.append(uf_ops > 0 ? 1.0f : 0.0f); // uf_flag

            // dag_compression_ratio: unique ptr count / total walk visits.
            {
                constexpr std::size_t kVisitCap = 512;
                const TermImpl* visit_buf[kVisitCap];
                std::size_t vdepth = 0;
                std::size_t unique_nodes = 0;
                std::size_t revisit_count = 0;

                constexpr std::size_t kSetSize = 512;
                const TermImpl* seen[kSetSize] = {};

                auto seen_contains = [&](const TermImpl* p) -> bool {
                    std::size_t idx = (reinterpret_cast<std::size_t>(p) >> 3u) & (kSetSize - 1u);
                    for (std::size_t probe = 0; probe < kSetSize; ++probe) {
                        std::size_t slot = (idx + probe) & (kSetSize - 1u);
                        if (seen[slot] == nullptr) return false;
                        if (seen[slot] == p) return true;
                    }
                    return false;
                };
                auto seen_insert = [&](const TermImpl* p) {
                    std::size_t idx = (reinterpret_cast<std::size_t>(p) >> 3u) & (kSetSize - 1u);
                    for (std::size_t probe = 0; probe < kSetSize; ++probe) {
                        std::size_t slot = (idx + probe) & (kSetSize - 1u);
                        if (seen[slot] == nullptr || seen[slot] == p) {
                            seen[slot] = p;
                            return;
                        }
                    }
                };

                visit_buf[vdepth++] = root.ptr();
                while (vdepth > 0) {
                    const TermImpl* cur = visit_buf[--vdepth];
                    if (seen_contains(cur)) {
                        ++revisit_count;
                        continue;
                    }
                    seen_insert(cur);
                    ++unique_nodes;
                    const Term* ch_ptr = reinterpret_cast<const Term*>(cur + 1);
                    for (std::uint16_t ci = 0; ci < cur->child_count; ++ci) {
                        if (vdepth < kVisitCap)
                            visit_buf[vdepth++] = ch_ptr[ci].ptr();
                    }
                }

                const std::size_t walk_total = unique_nodes + revisit_count;
                const float ratio = walk_total > 0
                                        ? static_cast<float>(unique_nodes) / static_cast<float>(walk_total)
                                        : 1.0f;
                fv.append(ratio); // dag_compression_ratio
            }
            return fv;
        }
    };

    static_assert(lithe::features::feature_extractor<theory_extractor, Term>);

    // =========================================================================
    // theory_mask computation using feature vector
    // =========================================================================

    [[nodiscard]] inline theory_mask mask_from_features(
        const lithe::features::feature_vector& fv) noexcept {
        theory_mask mask = theory_bit(theory_family::core);
        if (fv[0] > 0.0f) mask |= theory_bit(theory_family::bv);
        if (fv[1] > 0.0f) mask |= theory_bit(theory_family::lra);
        if (fv[2] > 0.0f) mask |= theory_bit(theory_family::lia);
        if (fv[3] > 0.0f) mask |= theory_bit(theory_family::nra);
        if (fv[4] > 0.0f) mask |= theory_bit(theory_family::nia);
        if (fv[5] > 0.0f) mask |= theory_bit(theory_family::quantifier);
        if (fv[6] > 0.0f) mask |= theory_bit(theory_family::array);
        if (fv[7] > 0.0f) mask |= theory_bit(theory_family::uf);
        return mask;
    }
} // namespace tarka::features

namespace tarka {
    // =========================================================================
    // theory_router<Backends...>
    // =========================================================================

    template <SmtSolverBackend... Backends>
        requires (sizeof...(Backends) > 0)
    class theory_router {
    public:
        static constexpr std::size_t kNumBackends = sizeof...(Backends);

        theory_router() = default;

        // Returns index of selected backend (0-based)
        [[nodiscard]] std::size_t route(Term t) const {
            features::theory_extractor extractor;
            const auto fv = extractor.extract(t);
            const theory_mask sig = features::mask_from_features(fv);

            // Cache in feature_store by term hash
            [[maybe_unused]] auto& store = lithe::features::feature_store::global();
            store.put(t.hash(), fv, lithe::features::feature_source::custom);

            // Capability filter: keep backends whose caps ⊇ sig
            constexpr theory_mask caps[kNumBackends] = {Backends::capabilities()...};
            for (std::size_t i = 0; i < kNumBackends; ++i)
                if ((caps[i] & sig) == sig) return i;

            return 0; // fallback: first backend
        }

        [[nodiscard]] theory_mask formula_mask(Term t) const {
            return features::mask_from_features(features::theory_extractor{}.extract(t));
        }
    };
} // namespace tarka
