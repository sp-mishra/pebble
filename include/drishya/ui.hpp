#pragma once
// ============================================================================
// drishya/ui.hpp — fluent runtime tree builder
// ----------------------------------------------------------------------------
// A small declarative helper for building a widget subtree and mounting it into
// an App as you go. A Builder wraps (App&, NodeId): it has already inserted its
// widget into the retained tree, and its chaining methods insert children under
// it. App is deduced from the app argument, so call sites never name it.
//
//   using namespace pebble::drishya;
//   auto root = ui::root(app, widgets::vstack(16));   // becomes the App root
//   root.child(widgets::label("Title"))               // flat child, returns root
//       .child(widgets::button("OK"));
//   auto row = root.nest(widgets::hstack());          // child container, returns it
//   row.child(widgets::button("Yes")).child(widgets::button("No"));
//
// .child(w) mounts a leaf and returns *this (the parent) for flat chaining.
// .nest(w) mounts a container child and returns a Builder for *that* child so you
// can descend. Mounting is eager: each call reaches straight into App::add_child.
//
// This is the cold build path; it does no work per frame. No virtual, no macros.
// ============================================================================

#include "drishya/drishya.hpp"

#include <type_traits>
#include <utility>

namespace pebble::drishya::ui {

// Wraps an already-mounted node so children can be attached fluently.
template <typename App>
class Builder {
public:
    using widget_type = typename App::widget_type;

    Builder(App& app, NodeId id) noexcept : app_(&app), id_(id) {}

    // Mount a leaf/child widget under this node; return *this to keep chaining
    // siblings at this level.
    template <typename W>
        requires (!std::is_same_v<std::decay_t<W>, Builder>)
    Builder& child(W&& w) {
        app_->add_child(id_, widget_type{std::forward<W>(w)});
        return *this;
    }

    // Mount a child and return a Builder for it so you can descend into it.
    template <typename W>
        requires (!std::is_same_v<std::decay_t<W>, Builder>)
    [[nodiscard]] Builder nest(W&& w) {
        const NodeId cid = app_->add_child(id_, widget_type{std::forward<W>(w)});
        return Builder{*app_, cid};
    }

    [[nodiscard]] NodeId id() const noexcept { return id_; }
    [[nodiscard]] App& app() const noexcept { return *app_; }

private:
    App* app_;
    NodeId id_;
};

// Mount `w` as the App root and return a Builder positioned at it.
template <typename App, typename W>
[[nodiscard]] Builder<App> root(App& app, W&& w) {
    const NodeId id = app.set_root(typename App::widget_type{std::forward<W>(w)});
    return Builder<App>{app, id};
}

// Mount `w` under an existing parent node and return a Builder at the new child.
template <typename App, typename W>
[[nodiscard]] Builder<App> under(App& app, NodeId parent, W&& w) {
    const NodeId id = app.add_child(parent, typename App::widget_type{std::forward<W>(w)});
    return Builder<App>{app, id};
}

} // namespace pebble::drishya::ui
