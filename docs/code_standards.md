# Pebble Code Standards

**Target Standard**: C++23 / C++26 (macOS-first via Apple Clang; cross-platform headers).

---

## Core Principles

1. **Zero virtual functions** — No `virtual` member functions, `dynamic_cast`, or `typeid`. Use policy templates and
   `this auto& self` instead.
2. **Zero macros** — No `#define` for logic or constants. Use `constexpr`, `if constexpr`, and concept constraints.
3. **Zero runtime overhead by default** — Headers should impose no cost for features not used (-pay only for what you
   use).
4. **Header-only** — Core subsystems are header-only (`.hpp`). Platform-specific code uses `#if __has_include(...)`
   guards.

---

## C++23 Requirements

### Use Explicit Object Parameters (Deducing `this`) — Not CRTP

All "CRTP-style" static polymorphism must migrate to C++23 explicit object parameters.

**Forbidden (CRTP)**:

```cpp
template <typename Derived>
struct Base {
    void foo() { static_cast<Derived*>(this)->foo_impl(); }
};
```

**Required (C++23 deducing `this`)**:

```cpp
struct Base {
    void foo(this auto& self) { self.foo_impl(); }
};
```

### Concepts — Not SFINAE

All template constraints must use `concept` and `requires` clauses. Never use `enable_if` or `void_t` SFINAE patterns.

---

## Prohibited Patterns (AI Slop)

### No Console I/O in Headers

**Forbidden**: `std::cout`, `std::cerr`, `std::endl` inside any `include/` header.

- Debug helpers must take a `std::ostream& os = std::cout` parameter.
- Diagnostic output must route through `utils::nadi` or a `RuleListener` callback.
- Production paths must never touch the terminal.

### No Empty Catch Blocks

**Forbidden**: `catch (...) {}`

All exceptions must either:

1. Propagate as `std::unexpected(ErrorCode)` via `std::expected<T, E>`, or
2. Record diagnostics (e.g., increment an atomic error counter), or
3. Mark the affected item as corrupt/invalid and continue scanning.

### No `thread_local` Dummy Fallback Returns

**Forbidden**: Returning a `thread_local Dummy` object as a fallback on invalid input.

All fallible accessors must return `T*` (nullable) or `std::optional<T>` or `std::expected<T&, E>`.

### No `const_cast` on `mutable` Members

**Forbidden**: `const_cast<T&>(expr)` when the target is already `mutable`.

Declare the field `mutable` and call non-const member functions directly inside `const` methods that are logically
const.

### No Demo Code in Headers

**Forbidden**: `namespace library::examples { inline void demo() { ... } }` blocks inside production headers.

Demo code belongs in `src/examples/` or `tests/`.

---

## Internal Library Reuse (Required)

Prefer existing pebble libraries over re-implementing from scratch:

| Need                          | Use This                      | Not This                      |
|-------------------------------|-------------------------------|-------------------------------|
| Compile-time string (NTTP)    | `akshara::fixed_string<N>`    | Local `FixedString<N>` struct |
| Disjoint-set / union-find     | `disjointset::DisjointSet`    | `containers::union_find`      |
| Generational entity storage   | `containers::slot_map`        | Manual freelist arrays        |
| Ref-counting base             | `smriti::detail::ref_counted` | Manual atomic ref counts      |
| Task scheduling / parallelism | `pravaha` executors           | Ad-hoc `WorkerPool` re-impl   |

---

## Memory Safety

- **RAII everywhere**: All heap-allocated resources must be managed by destructors or smart pointers.
- **Destructor calls before free**: When managing raw memory buffers (e.g., `std::malloc`/`std::free`), element
  destructors must be called for all live objects before freeing the buffer.
- **No raw `new`/`delete`** in policy-driven code: use arena allocation or SBO wrappers.

---

## Function Pointer Safety

- `ObserverRegistry::register_on_add/remove` accepts only **non-capturing function pointers**
  (`void (*)(Event) noexcept`).
- Stateful closures must be stored in a persistent object and passed via function pointer to a static trampoline.

---

## Error Handling

- Use `std::expected<T, E>` for all fallible operations.
- Use typed error enums (not raw `int` or bare exceptions).
- Async operations (futures/promises) must be resolved or explicitly rejected — never left hanging on queue-full
  conditions.

---

## Performance

- **Dispatch tables over linear folds**: When dispatching over a variant/tuple index at runtime, use a `constexpr`
  function-pointer jump table, not a linear fold expression.
- **Condition variables over yield-spin**: Worker pools must sleep on `condition_variable::wait_for`, not spin on
  `std::this_thread::yield()`.
- **Change detection**: Per-entity dirty bitsets or dirty lists rather than full-scan iteration.
