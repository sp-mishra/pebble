# Reactive — `Signal`, `Computed` & `Callback`

Header-only C++23 reactive value primitives (`include/containers/reactive/signal.hpp`). No virtual, no
RTTI, no macros. Observer callbacks use small-buffer type-erased storage (SBO + a static-constexpr
free-function vtable, mirroring `spandana::BasicAction`) so the common case never touches the heap, and
the inline observer list rides on `containers::dynamic::SmallVector`. These are generic container
primitives — they know nothing about widgets or layout; higher layers (e.g. `drishya`) re-export and
specialize them.

All three types live in namespace `containers::reactive`.

## `BasicCallback<InlineBytes, InlineAlign>` / `Callback`

A move-only, type-erased `void()` callable with small-buffer optimization.

- `BasicCallback<InlineBytes = 64, InlineAlign = alignof(std::max_align_t)>` stores any nullary callable
  that fits inline; a `static_assert` fires if the callable exceeds `InlineBytes`, is over-aligned, or is
  not nothrow move-constructible (raise `InlineBytes` to accommodate a larger closure).
- Move-only: copy construction and copy assignment are deleted.
- `valid()` / `explicit operator bool()` report whether a callable is held; `operator()()` invokes it
  (no-op when empty). All operations are `noexcept`.
- `using Callback = BasicCallback<>;` is the default 64-byte alias.

## `Signal<T, ObserverInlineBytes>`

An observable value cell: it stores a `T` and a list of observers, and writing the value notifies them.

- **Read**: `get()` and `operator()()` return `const T&`.
- **Write**:
  - `set(T next)` — always notifies.
  - `set_if_changed(T next) -> bool` — notifies only when the new value differs (requires an
    equality-comparable `T`); returns `true` if a change (and notification) happened.
  - `mutate(Fn fn)` — invokes `fn(T&)` for in-place mutation, then notifies.
- **Observation**:
  - `subscribe(Fn fn) -> ObserverId` — registers a zero-arg observer invoked on every `notify()` and
    returns a stable `ObserverId` that survives later subscribe/unsubscribe of other observers.
  - `unsubscribe(ObserverId id)` — removes a registered observer; a no-op for unknown ids.
  - `observer_count() -> std::size_t` — number of live observers.
  - `notify()` — fires all observers without changing the value (useful after mutation paths that bypass
    `set`).
- Move-only (copy deleted); the observer list is inline up to `ObserverInlineBytes` (default 256).

```cpp
#include <containers/reactive/signal.hpp>

using namespace containers::reactive;

Signal<int> count{0};
const ObserverId id = count.subscribe([]{ /* react to change */ });

count.set(1);                 // notifies
count.set_if_changed(1);      // no-op, no notification (value unchanged)
count.mutate([](int& v){ ++v; }); // notifies

count.unsubscribe(id);
```

## `Computed<F>`

A lazily-memoized derived value. `F` is a nullary callable returning the derived value.

- Dependencies are declared explicitly with `depend_on(signal)`: each subscribes an observer that marks
  this `Computed` dirty, so the next read recomputes. This avoids a global dependency tracker while
  keeping recompute lazy (only on read, only after a dependency changed).
- `get()` / `operator()()` return `const value_type&`, recomputing and caching only when dirty.
- `invalidate()` forces recomputation on the next read.
- A deduction guide lets `Computed c{[]{ return 42; }};` work directly.

```cpp
Signal<int> a{2};
Signal<int> b{3};

Computed sum{[&]{ return a.get() + b.get(); }};
sum.depend_on(a);
sum.depend_on(b);

sum.get();       // 5 (computed)
a.set(10);
sum.get();       // 13 (recomputed after dependency changed)
```
