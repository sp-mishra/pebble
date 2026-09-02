#pragma once
// ============================================================================
// kalpana/layer/layer.hpp — Individual Layer with Composable Effects & Combiner
// ============================================================================
// Supports blend isolation, opacity, layer locking/visibility, clipping to below,
// and configurable node storage.
// ============================================================================

#include "../scene/node.hpp"
#include "../effect/effect_chain.hpp"
#include "layer_combiner.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace kalpana {
    template <typename NodeContainer = containers::dynamic::SmallVector<Node, 128>>
    class BasicLayer {
    public:
        using container_type = NodeContainer;

        explicit BasicLayer(std::string_view name = "Layer")
            : name_(name) {}

        // Content management
        BasicLayer& add(Node node) {
            nodes_.push_back(std::move(node));
            return *this;
        }

        BasicLayer& clear() noexcept {
            nodes_.clear();
            return *this;
        }

        // Properties
        BasicLayer& opacity(float o) noexcept {
            opacity_ = o;
            return *this;
        }

        BasicLayer& blend(BlendMode mode) noexcept {
            blend_ = mode;
            combiner_ = LayerCombiner::blend_mode(mode);
            return *this;
        }

        BasicLayer& combiner(LayerCombiner c) noexcept {
            combiner_ = std::move(c);
            return *this;
        }

        BasicLayer& visible(bool v) noexcept {
            visible_ = v;
            return *this;
        }

        BasicLayer& locked(bool l) noexcept {
            locked_ = l;
            return *this;
        }

        BasicLayer& clip_to_below(bool clip) noexcept {
            clip_ = clip;
            return *this;
        }

        // Per-layer effect chain
        BasicLayer& effect(EffectChain fx) {
            effects_ = std::move(fx);
            return *this;
        }

        BasicLayer& effect(EffectNode fx) {
            effects_ = EffectChain(std::move(fx));
            return *this;
        }

        [[nodiscard]] std::string_view name() const noexcept { return name_; }
        [[nodiscard]] float opacity() const noexcept { return opacity_; }
        [[nodiscard]] BlendMode blend() const noexcept { return blend_; }
        [[nodiscard]] const LayerCombiner& combiner() const noexcept { return combiner_; }
        [[nodiscard]] bool visible() const noexcept { return visible_; }
        [[nodiscard]] bool locked() const noexcept { return locked_; }
        [[nodiscard]] bool clip_to_below() const noexcept { return clip_; }
        [[nodiscard]] const NodeContainer& nodes() const noexcept { return nodes_; }
        [[nodiscard]] NodeContainer& nodes() noexcept { return nodes_; }
        [[nodiscard]] const EffectChain& effect_chain() const noexcept { return effects_; }

    private:
        std::string name_;
        float opacity_ = 1.0f;
        BlendMode blend_ = BlendMode::SrcOver;
        LayerCombiner combiner_{};
        bool visible_ = true;
        bool locked_ = false;
        bool clip_ = false;
        NodeContainer nodes_;
        EffectChain effects_;
    };

    using Layer = BasicLayer<>;
} // namespace kalpana
