#pragma once
// ============================================================================
// drishya/layout_bridge.hpp — WidgetTree → akruti layout solver
// ----------------------------------------------------------------------------
// The bridge translates the retained WidgetTree into an akruti::layout::Engine
// solve, then exposes each widget's resolved rectangle back by NodeId.
//
// Structure changes (insert/remove/move a widget) trigger a full rebuild(): we
// author an akruti LayoutTree in the same pre-order the widget tree walks,
// stamping each layout node's user_tag with the source widget NodeId, bake it,
// and record the NodeId <-> layout-index mapping. Style-only changes reuse the
// baked topology and just re-push a widget's LayoutStyle + mark it dirty, so
// solve_incremental() only reprocesses the affected subtree.
//
// Text metrics reach the solver through akruti::layout::make_text_measure — a
// zero-alloc trampoline over the host's ITextMetrics object (the same metrics
// the painter measures with).
// ============================================================================

#include "drishya/tree.hpp"
#include "drishya/widget_concept.hpp"

#include "akruti/layout.hpp"

#include <cstdint>
#include <vector>

namespace pebble::drishya {

template <typename Metrics, typename Painter_, std::size_t InlineBytes = 512>
    requires ITextMetrics<Metrics> && Painter<Painter_>
class LayoutBridge {
public:
    using tree_type = WidgetTree<Metrics, Painter_, InlineBytes>;
    using measure_ctx = MeasureCtxT<Metrics>;

    explicit LayoutBridge(Metrics& metrics) noexcept : metrics_(&metrics) {
        engine_.text_measure_callback = akruti::layout::make_text_measure(*metrics_);
    }

    // Full rebuild from the widget tree topology. Call on any structural change.
    void rebuild(const tree_type& tree) {
        author_tree_ = akruti::layout::LayoutTree{}; // fresh authoring tree
        to_layout_.assign(tree.capacity(), akruti::layout::kInvalid);

        const NodeId root = tree.root();
        if (root == kInvalidNode) { engine_.clear(); return; }

        // Author the akruti LayoutTree in widget pre-order; stamp user_tag=NodeId.
        author_subtree(tree, root, nullptr);
        engine_.bake(author_tree_);

        // Build NodeId -> layout-index from the baked user_tag column.
        build_index_map(tree);
    }

    void solve(Bounds2D viewport) { engine_.solve(viewport); }
    void solve_incremental(Bounds2D viewport) { engine_.solve_incremental(viewport); }

    // Push a widget's current style into the baked engine and mark it dirty for
    // the next incremental solve (style-only change, topology unchanged).
    void update_style(const tree_type& tree, NodeId id) {
        const std::uint32_t li = layout_index(id);
        if (li == akruti::layout::kInvalid) return;
        engine_.set_style(li, tree.widget(id).style());
        engine_.mark_dirty(li, akruti::layout::Engine::DIRTY_MEASURE |
                                   akruti::layout::Engine::DIRTY_GEOMETRY);
    }

    // --- resolved geometry, addressed by widget NodeId --------------------
    [[nodiscard]] Rect2D rect(NodeId id) const noexcept {
        const std::uint32_t li = layout_index(id);
        return (li != akruti::layout::kInvalid && li < engine_.rect.size())
                   ? engine_.rect[li] : Rect2D{};
    }
    [[nodiscard]] Bounds2D clip(NodeId id) const noexcept {
        const std::uint32_t li = layout_index(id);
        return (li != akruti::layout::kInvalid && li < engine_.clip.size())
                   ? engine_.clip[li] : Bounds2D();
    }

    // Map a viewport hit back to the widget NodeId under (x, y), if any.
    [[nodiscard]] NodeId hit(float x, float y) const {
        const auto li = engine_.hit_test(x, y);
        return li ? layout_to_node(*li) : kInvalidNode;
    }

    [[nodiscard]] std::uint32_t layout_index(NodeId id) const noexcept {
        return id < to_layout_.size() ? to_layout_[id] : akruti::layout::kInvalid;
    }
    [[nodiscard]] NodeId layout_to_node(std::uint32_t li) const noexcept {
        return li < engine_.user_tag.size()
                   ? static_cast<NodeId>(engine_.user_tag[li]) : kInvalidNode;
    }

    [[nodiscard]] akruti::layout::Engine& engine() noexcept { return engine_; }
    [[nodiscard]] const akruti::layout::Engine& engine() const noexcept { return engine_; }

private:
    // Recursively author the LayoutTree; returns the created node pointer.
    akruti::layout::LayoutTree::TreeNode*
    author_subtree(const tree_type& tree, NodeId id,
                   akruti::layout::LayoutTree::TreeNode* parent) {
        akruti::layout::LayoutNode ln{};
        ln.style = tree.widget(id).style();
        ln.user_tag = static_cast<std::uint64_t>(id);
        auto* node = author_tree_.insert(parent, ln);
        tree.for_each_child(id, [&](NodeId child) {
            author_subtree(tree, child, node);
        });
        return node;
    }

    void build_index_map(const tree_type& tree) {
        to_layout_.assign(tree.capacity(), akruti::layout::kInvalid);
        for (std::uint32_t li = 0; li < engine_.user_tag.size(); ++li) {
            const auto nid = static_cast<NodeId>(engine_.user_tag[li]);
            if (nid < to_layout_.size()) to_layout_[nid] = li;
        }
    }

    Metrics* metrics_;
    akruti::layout::Engine engine_;
    akruti::layout::LayoutTree author_tree_{};
    std::vector<std::uint32_t> to_layout_{}; // NodeId -> layout index
};

} // namespace pebble::drishya
