#pragma once
// ============================================================================
// drishya/edsl.hpp — compile-time (nested-tuple) tree DSL
// ----------------------------------------------------------------------------
// Where ui.hpp builds a tree imperatively at runtime, edsl.hpp lets you describe
// a *static* tree shape as a value: node(widget, child, child, ...) nests into a
// std::tuple, so the whole hierarchy is one expression with its structure known
// at compile time. Style modifiers compose with operator|:
//
//   using namespace pebble::drishya::edsl;
//   auto view = node(vstack_(16) | pad(12) | flex(1),
//                    label_("Title") | align(Align::Center),
//                    node(hstack_(),
//                         button_("OK"),
//                         button_("Cancel")));
//   view.mount(app);   // realizes the tuple into the App's retained tree
//
// Modifiers return the *same widget by value* with one style field set, so a
// chain `w | pad(8) | flex(1)` folds with zero heap and no intermediate nodes.
// The `_px` literal yields a SizeSpec::Px for terse sizing: `width(240_px)`.
//
// mount() type-erases each widget through the App's AnyWidget exactly as ui.hpp
// does; the tuple only shapes *construction order*, not runtime storage. No
// virtual, no macros.
// ============================================================================

#include "drishya/drishya.hpp"
#include "drishya/widgets/widgets.hpp"

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace pebble::drishya::edsl {

using akruti::layout::Align;
using akruti::layout::Axis;
using akruti::layout::Edges;
using akruti::layout::Justify;
using akruti::layout::SizeSpec;

// ---------------------------------------------------------------------------
// Style modifiers. Each is a small functor applied with operator|; it takes a
// widget by value, mutates one LayoutStyle field via the widget's style_, and
// returns it. Widgets expose style_ (WidgetBase and the input widgets all do);
// modifiers require a mutable style_ member.
// ---------------------------------------------------------------------------
namespace detail {
    template <typename W>
    concept HasStyleField = requires(W& w) { w.style_; };
} // namespace detail

// A modifier is a small functor with a templated operator() applying one style
// change; `operator|` finds it by overload — no macro, no CRTP.

struct Pad {
    Edges e;
    template <detail::HasStyleField W>
    [[nodiscard]] W operator()(W w) const noexcept { w.style_.padding = e; return w; }
};
struct Flex {
    float grow;
    template <detail::HasStyleField W>
    [[nodiscard]] W operator()(W w) const noexcept { w.style_.flex_grow = grow; return w; }
};
struct AlignMod {
    Align a;
    template <detail::HasStyleField W>
    [[nodiscard]] W operator()(W w) const noexcept { w.style_.align_items = a; return w; }
};
struct JustifyMod {
    Justify j;
    template <detail::HasStyleField W>
    [[nodiscard]] W operator()(W w) const noexcept { w.style_.justify_content = j; return w; }
};
struct Width {
    SizeSpec s;
    template <detail::HasStyleField W>
    [[nodiscard]] W operator()(W w) const noexcept { w.style_.width = s; return w; }
};
struct Height {
    SizeSpec s;
    template <detail::HasStyleField W>
    [[nodiscard]] W operator()(W w) const noexcept { w.style_.height = s; return w; }
};

// Modifier factories (call-site sugar).
[[nodiscard]] inline Pad pad(float p) noexcept { return Pad{Edges{p, p, p, p}}; }
[[nodiscard]] inline Pad pad(Edges e) noexcept { return Pad{e}; }
[[nodiscard]] inline Flex flex(float g) noexcept { return Flex{g}; }
[[nodiscard]] inline AlignMod align(Align a) noexcept { return AlignMod{a}; }
[[nodiscard]] inline JustifyMod justify(Justify j) noexcept { return JustifyMod{j}; }
[[nodiscard]] inline Width width(SizeSpec s) noexcept { return Width{s}; }
[[nodiscard]] inline Height height(SizeSpec s) noexcept { return Height{s}; }

// Apply any modifier functor to a widget: `widget | modifier`.
template <typename W, typename Mod>
    requires detail::HasStyleField<std::remove_cvref_t<W>> &&
             requires(Mod m, W w) { m(std::forward<W>(w)); }
[[nodiscard]] auto operator|(W&& w, Mod&& m) {
    return std::forward<Mod>(m)(std::forward<W>(w));
}

// ---------------------------------------------------------------------------
// _px literal → SizeSpec::Px. `240_px`, `1.5_px`.
// ---------------------------------------------------------------------------
[[nodiscard]] inline SizeSpec operator""_px(long double v) noexcept {
    return SizeSpec::Px(static_cast<float>(v));
}
[[nodiscard]] inline SizeSpec operator""_px(unsigned long long v) noexcept {
    return SizeSpec::Px(static_cast<float>(v));
}

// ---------------------------------------------------------------------------
// Node<W, Children...> — a widget plus a tuple of child Nodes (or bare widgets).
// node(w, c0, c1, ...) builds one. mount(app) realizes the whole subtree.
// ---------------------------------------------------------------------------
template <typename W, typename... Children>
struct Node {
    W widget;
    std::tuple<Children...> children;

    // Realize into the App as the root; returns the root NodeId.
    template <typename App>
    NodeId mount(App& app) {
        const NodeId id = app.set_root(typename App::widget_type{std::move(widget)});
        mount_children(app, id);
        return id;
    }
    // Realize under an existing parent; returns this node's NodeId.
    template <typename App>
    NodeId mount_under(App& app, NodeId parent) {
        const NodeId id = app.add_child(parent, typename App::widget_type{std::move(widget)});
        mount_children(app, id);
        return id;
    }

private:
    template <typename App>
    void mount_children(App& app, NodeId self) {
        std::apply(
            [&](auto&... child) { (mount_one(app, self, child), ...); },
            children);
    }
    // A child is either another Node (recurse) or a bare widget (leaf insert).
    template <typename App, typename C>
    static void mount_one(App& app, NodeId self, C& child) {
        if constexpr (requires { child.mount_under(app, self); }) {
            child.mount_under(app, self);
        } else {
            app.add_child(self, typename App::widget_type{std::move(child)});
        }
    }
};

// Build a Node from a widget and zero or more children (widgets or Nodes).
template <typename W, typename... Children>
[[nodiscard]] Node<std::remove_cvref_t<W>, std::remove_cvref_t<Children>...>
node(W&& w, Children&&... cs) {
    return Node<std::remove_cvref_t<W>, std::remove_cvref_t<Children>...>{
        std::forward<W>(w), std::tuple<std::remove_cvref_t<Children>...>{std::forward<Children>(cs)...}};
}

// ---------------------------------------------------------------------------
// Terse widget factories in the edsl namespace (trailing underscore avoids
// clashing with the widgets:: free functions while reading cleanly in a tree).
// ---------------------------------------------------------------------------
[[nodiscard]] inline widgets::Stack vstack_(float p = 0.0f) noexcept { return widgets::vstack(p); }
[[nodiscard]] inline widgets::Stack hstack_(float p = 0.0f) noexcept { return widgets::hstack(p); }
[[nodiscard]] inline widgets::Label label_(std::string t) { return widgets::label(std::move(t)); }
[[nodiscard]] inline widgets::Button button_(std::string t) { return widgets::button(std::move(t)); }
[[nodiscard]] inline widgets::Spacer spacer_() noexcept { return widgets::spacer(); }

} // namespace pebble::drishya::edsl
