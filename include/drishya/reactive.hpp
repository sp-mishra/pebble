#pragma once
// ============================================================================
// drishya/reactive.hpp — reactive value cells for widgets
// ----------------------------------------------------------------------------
// Re-exports the generic containers::reactive primitives into the drishya
// namespace and adds UI-oriented helpers (property binding). The core Signal /
// Computed / Callback types are generic and live in containers so any subsystem
// can use them; drishya only adds the widget-facing conveniences here.
// ============================================================================

#include "containers/reactive/signal.hpp"

#include <utility>

namespace pebble::drishya {
    // Bring the generic reactive vocabulary into drishya's namespace unchanged.
    template <typename T, std::size_t ObserverInlineBytes = 256>
    using Signal = containers::reactive::Signal<T, ObserverInlineBytes>;

    template <typename F>
    using Computed = containers::reactive::Computed<F>;

    template <std::size_t InlineBytes = 64>
    using BasicCallback = containers::reactive::BasicCallback<InlineBytes>;

    using Callback = containers::reactive::Callback;
    using ObserverId = containers::reactive::ObserverId;
    inline constexpr ObserverId kInvalidObserver = containers::reactive::kInvalidObserver;

    // bind(target, source): mirror the source signal's value into a target setter
    // on every change (and once immediately). `set` is any callable taking the
    // source value. Returns the ObserverId so callers can unbind later.
    //
    //   Signal<int> count{0};
    //   bind(count, [&](int v){ label.set_text(std::to_string(v)); });
    template <typename T, std::size_t N, typename SetFn>
        requires std::is_invocable_v<SetFn, const T&>
    ObserverId bind(Signal<T, N>& source, SetFn set) {
        set(source.get()); // prime with the current value
        return source.subscribe([&source, set = std::move(set)]() mutable {
            set(source.get());
        });
    }

    // bind_signal(dst, src): keep destination signal equal to source signal.
    template <typename T, std::size_t Nd, std::size_t Ns>
    ObserverId bind_signal(Signal<T, Nd>& dst, Signal<T, Ns>& src) {
        dst.set(src.get());
        return src.subscribe([&dst, &src]() { dst.set(src.get()); });
    }
} // namespace pebble::drishya
