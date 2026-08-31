// ============================================================================
// containers::reactive — Signal<T> / Computed<F>
// ----------------------------------------------------------------------------
// A minimal, header-only reactive value cell. `Signal<T>` stores a value and a
// list of observers; writing the value notifies observers. `Computed<F>` is a
// lazily-memoized derived value that recomputes on read after any of its
// declared dependencies changes.
//
// Design constraints (shared with the rest of Pebble):
//   * No virtual, no RTTI, no macros, header-only.
//   * Observer callbacks are stored via a small-buffer type-erased callable
//     (SBO + static-constexpr free-function vtable), mirroring
//     spandana::BasicAction — no heap for the common case, move-only.
//   * Observer list uses containers::dynamic::SmallVector so a handful of
//     observers live inline.
//
// This is a generic container primitive: it knows nothing about widgets or
// layout. Higher layers (e.g. drishya) re-export and specialize it.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "../dynamic/SmallVector.hpp"

namespace containers::reactive {

// Stable identifier handed back from Signal::subscribe; pass to unsubscribe.
using ObserverId = std::uint32_t;
inline constexpr ObserverId kInvalidObserver = static_cast<ObserverId>(-1);

// ----------------------------------------------------------------------------
// Callback — move-only, type-erased `void()` with small-buffer storage.
// Mirrors the spandana::BasicAction vtable pattern (no virtual / RTTI / heap for
// callables that fit InlineBytes).
// ----------------------------------------------------------------------------
template <std::size_t InlineBytes = 64, std::size_t InlineAlign = alignof(std::max_align_t)>
class BasicCallback {
public:
    struct Vtable {
        void (*invoke)(void*) noexcept;
        void (*move_construct)(void*, void*) noexcept; // (dst, src)
        void (*destroy)(void*) noexcept;
    };

    BasicCallback() = default;

    template <typename F, typename Decayed = std::remove_cvref_t<F>>
        requires(!std::is_same_v<Decayed, BasicCallback> &&
                 std::is_invocable_v<Decayed &>)
    BasicCallback(F &&fn) noexcept {
        static_assert(sizeof(Decayed) <= InlineBytes,
                      "reactive callback exceeds inline buffer; raise BasicCallback<InlineBytes>.");
        static_assert(alignof(Decayed) <= InlineAlign,
                      "reactive callback over-aligned for inline buffer.");
        static_assert(std::is_nothrow_move_constructible_v<Decayed>,
                      "reactive callback must be nothrow move-constructible for SBO storage.");
        ::new (static_cast<void *>(&storage_)) Decayed(std::forward<F>(fn));
        vtable_ = vtable_for<Decayed>();
    }

    BasicCallback(BasicCallback &&other) noexcept {
        if (other.vtable_) {
            other.vtable_->move_construct(&storage_, &other.storage_);
            vtable_ = other.vtable_;
            other.reset();
        }
    }

    BasicCallback &operator=(BasicCallback &&other) noexcept {
        if (this != &other) {
            reset();
            if (other.vtable_) {
                other.vtable_->move_construct(&storage_, &other.storage_);
                vtable_ = other.vtable_;
                other.reset();
            }
        }
        return *this;
    }

    BasicCallback(const BasicCallback &) = delete;
    BasicCallback &operator=(const BasicCallback &) = delete;

    ~BasicCallback() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return vtable_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    void operator()() noexcept {
        if (vtable_) vtable_->invoke(&storage_);
    }

private:
    void reset() noexcept {
        if (vtable_) {
            vtable_->destroy(&storage_);
            vtable_ = nullptr;
        }
    }

    template <typename F>
    static const Vtable *vtable_for() noexcept {
        static constexpr Vtable vt{
            .invoke = [](void *p) noexcept { (*static_cast<F *>(p))(); },
            .move_construct = [](void *dst, void *src) noexcept {
                ::new (dst) F(std::move(*static_cast<F *>(src)));
            },
            .destroy = [](void *p) noexcept { static_cast<F *>(p)->~F(); },
        };
        return &vt;
    }

    alignas(InlineAlign) std::byte storage_[InlineBytes]{};
    const Vtable *vtable_ = nullptr;
};

using Callback = BasicCallback<>;

// ----------------------------------------------------------------------------
// Signal<T> — an observable value cell.
// ----------------------------------------------------------------------------
template <typename T, std::size_t ObserverInlineBytes = 256>
class Signal {
public:
    using value_type = T;

    Signal() = default;
    explicit Signal(T initial) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(initial)) {}

    Signal(const Signal &) = delete;
    Signal &operator=(const Signal &) = delete;
    Signal(Signal &&) = default;
    Signal &operator=(Signal &&) = default;

    // --- read -------------------------------------------------------------
    [[nodiscard]] const T &get() const noexcept { return value_; }
    [[nodiscard]] const T &operator()() const noexcept { return value_; }

    // --- write ------------------------------------------------------------
    // Always notifies. Use set_if_changed to skip notification on equal writes.
    void set(T next) {
        value_ = std::move(next);
        notify();
    }

    // Notifies only when the new value differs (requires T to be equality
    // comparable). Returns true if a change (and notification) happened.
    bool set_if_changed(T next) {
        if constexpr (requires(const T &a, const T &b) { { a == b } -> std::convertible_to<bool>; }) {
            if (value_ == next) return false;
        }
        value_ = std::move(next);
        notify();
        return true;
    }

    // In-place mutation then notify. Fn receives a T&.
    template <typename Fn>
        requires std::is_invocable_v<Fn, T &>
    void mutate(Fn &&fn) {
        fn(value_);
        notify();
    }

    // --- observation ------------------------------------------------------
    // Register an observer invoked on every notify(). Returns a stable id that
    // survives later subscribe/unsubscribe of other observers.
    template <typename Fn>
        requires std::is_invocable_v<Fn &>
    ObserverId subscribe(Fn &&fn) {
        const ObserverId id = next_id_++;
        observers_.emplace_back(Entry{id, Callback{std::forward<Fn>(fn)}});
        return id;
    }

    // Remove a previously registered observer. No-op if id is unknown.
    void unsubscribe(ObserverId id) noexcept {
        for (std::size_t i = 0; i < observers_.size(); ++i) {
            if (observers_[i].id == id) {
                observers_[i] = std::move(observers_[observers_.size() - 1]);
                observers_.pop_back();
                return;
            }
        }
    }

    [[nodiscard]] std::size_t observer_count() const noexcept { return observers_.size(); }

    // Fire all observers without changing the value (useful after mutate paths
    // that bypass set()).
    void notify() {
        for (std::size_t i = 0; i < observers_.size(); ++i) {
            observers_[i].cb();
        }
    }

private:
    struct Entry {
        ObserverId id;
        Callback cb;
    };

    T value_{};
    containers::dynamic::SmallVector<Entry, ObserverInlineBytes> observers_{};
    ObserverId next_id_ = 0;
};

// ----------------------------------------------------------------------------
// Computed<F> — a lazily-memoized derived value.
// ----------------------------------------------------------------------------
// F is a nullary callable returning the derived value. Dependencies are declared
// explicitly via depend_on(signal): each subscribes an observer that marks this
// Computed dirty, so the next get() recomputes. This avoids a global dependency
// tracker while keeping recompute lazy (only on read, only after a dep changed).
template <typename F>
class Computed {
public:
    using value_type = std::remove_cvref_t<std::invoke_result_t<F &>>;

    explicit Computed(F fn) noexcept(std::is_nothrow_move_constructible_v<F>)
        : fn_(std::move(fn)) {}

    Computed(const Computed &) = delete;
    Computed &operator=(const Computed &) = delete;
    Computed(Computed &&) = default;
    Computed &operator=(Computed &&) = default;

    // Declare a dependency: when `dep` changes, this Computed becomes dirty.
    template <typename T, std::size_t N>
    void depend_on(Signal<T, N> &dep) {
        dep.subscribe([this]() noexcept { dirty_ = true; });
        dirty_ = true;
    }

    [[nodiscard]] const value_type &get() {
        if (dirty_) {
            cached_ = fn_();
            dirty_ = false;
        }
        return cached_;
    }
    [[nodiscard]] const value_type &operator()() { return get(); }

    // Force recomputation on next read.
    void invalidate() noexcept { dirty_ = true; }

private:
    F fn_;
    value_type cached_{};
    bool dirty_ = true;
};

// Deduction guide so `Computed c{[]{ return 42; }};` works.
template <typename F>
Computed(F) -> Computed<F>;

} // namespace containers::reactive
