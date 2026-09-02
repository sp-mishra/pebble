# Example Registry (test/example_registry.hpp)

- Path: `include/test/example_registry.hpp`
- Companion example runner: `src/examples/exemain.cpp`
- Component: lightweight header-only example/test runner used by the examples in this tree
- Namespace: `testfw`
- Language: C++23

Table of Contents

- Purpose
- When to use
- Header Summary
    - Key types and concepts
    - Example requirements (the Example concept)
    - Registry API
- Behavior & Reporting
- Examples
    - Writing an Example type (static)
    - Writing an Example type (instance with lifecycle)
    - Running the provided `exemain` example runner
- Implementation notes and portability
- Quick reference / API summary

## Purpose

`example_registry.hpp` provides a compact, header-only registry and runner for small examples, demonstrations, and
lightweight tests. It is intended for use by example programs inside the repository and as a small, dependency-free
framework for organizing example code:

- Collects example types at compile-time into a `Registry` template.
- Provides command-line driven execution: list available examples, run all, run by tag, or run a single example by name.
- Handles simple per-example lifecycle hooks: `setup`, `run`, and `teardown` (either static or instance-based).
- Returns structured error information (`Error`) and uses `std::expected`/`std::unexpected` for result propagation.

The companion `exemain.cpp` demonstrates three example types (a passing static example, an instance-based example using
setup/run/teardown, and a failing example used to show the error reporting).

## When to use

- You need a lightweight way to bundle and run multiple example/demo cases in a single binary.
- You want uniform reporting (running/passed/failed) and a small lifecycle for each example (optional setup/teardown).
- You prefer compile-time registration without dynamic plugin loading.

Not intended for heavy unit-testing frameworks; it's a small convenience runner for examples and demos.

## Header Summary

### Key types

- `struct Error` — carries a short `std::string_view message` and a `std::source_location where` capturing the failure
  site.

- `using Result = std::expected<void, Error>` — `Result` is used as the canonical success/failure return type for
  `setup`, `run`, and `teardown` hooks.

-

`[[nodiscard]] inline constexpr auto fail(std::string_view message, std::source_location where = std::source_location::current()) -> std::unexpected<Error>` —
convenience function to create an `std::unexpected<Error>`.

### Concepts

The header declares several concepts that describe the allowed shapes of example types:

- `HasStaticRun<T>` — type `T` provides a static `T::run()` returning `Result`.
- `HasInstanceRun<T>` — type `T` provides an instance `t.run()` returning `Result` (used when `HasStaticRun` is false).

- Similar `HasStaticSetup`, `HasInstanceSetup`, `HasStaticTeardown`, `HasInstanceTeardown` for lifecycle hooks.

- `ExampleType` concept — a type satisfying the following:
    - `T::name()` -> `std::string_view`
    - `T::description()` -> `std::string_view`
    - `T::tags()` -> `std::span<const std::string_view>`
    - and either `HasStaticRun<T>` or `HasInstanceRun<T>`

This combination ensures the registry can uniformly call name/description/tags and invoke the example's run logic.

### Registry API

The main class template is:

```cpp
template<ExampleType... Examples>
class Registry {
  public:
    static int run_all();          // run all examples; returns exit code (0 success, 2 if any failed)
    static int run_by_name(std::string_view target); // run single example by its name
    static int run_by_tag(std::string_view tag);    // run examples matching tag
    static void print_list();      // print list of available examples
};
```

Behavioral details:

- `run_all()` executes the registered examples in the order they appear on the template parameter pack. It uses a
  fold-expression over a lambda to increment a failure counter when any example fails; it returns `2` if any failed,
  otherwise `0`.

- `run_by_name()` locates a single example by `T::name()`. If not found it reports an error and returns `1`. If found,
  it runs it and returns `0` (success) or `2` (failure), maintaining the same exit code conventions.

- `run_by_tag()` runs all examples for which `tag` is present in `T::tags()`. If no examples match the tag, it reports
  an error and returns `1`.

- `print_list()` prints the available examples (name, description, and tags) using streaming.

### Invocation and lifecycle

Internally the registry calls examples through a unified `invoke<T>()` helper that picks either the static-run path or
instance-run path depending on the concepts. The lifecycle ensures:

- If `setup` is present (static or instance), it runs first and if it fails – the error is propagated immediately.
- Then `run()` is executed.
- If `teardown` is present, it runs after `run()`; teardown errors are reported and returned unless `run()` had already
  failed (in that case `run()`'s error is returned).

This ordering preserves expected semantics: setup must succeed for run to proceed; teardown runs after run and both are
considered for final result selection.

## Behavior & Reporting

- Start and pass messages are printed to `stdout` using `std::cout`:
    - `[RUNNING] <name>`
    - `[PASSED]  <name>`

- Failures and user-facing errors are printed to `stderr` using `std::cerr`:
    - `[FAILED]  <name>: <message> (<file>:<line>)` — `Error::where` provides the file/line captured via
      `std::source_location` when `testfw::fail()` was invoked.
    - Other command-line errors (example not found, no examples with tag) are also printed to `stderr`.

- Exit codes used by the runner (as implemented in `exemain.cpp` and `Registry`):
    - `0` — success, requested example (s) passed
    - `1` — usage error or requested example/tag not found
    - `2` — at least one example ran and failed

## Examples

The repository includes a small runner in `src/examples/exemain.cpp` that demonstrates typical usage. It defines three
example types and registers them with the `Registry`:

- `StaticPass` — a static `run()` example which performs a small compile-time-computable check and returns success.
- `StorageExample` — an instance-based example that demonstrates `setup()`, `run()`, and `teardown()` with an owned
  buffer.
- `FailingExample` — static `run()` that intentionally fails to demonstrate error reporting.

A `using ExampleRegistry = testfw::Registry<StaticPass, StorageExample, FailingExample>;` alias is used by `main()` to
dispatch commands.

### Running the example runner (build & run)

From the project root you can build the examples via CMake. Example commands (POSIX/macOS with `zsh` / `bash`):

```bash
mkdir -p build && cd build
cmake -S .. -B . -DCMAKE_BUILD_TYPE=Debug
cmake --build . --target turbo_twig -j
# The example binary is named 'turbo_twig' by the project's CMakeLists
./turbo_twig --list            # show examples
./turbo_twig --all             # run all examples
./turbo_twig --filter core     # run examples tagged with "core"
./turbo_twig static_pass       # run a single example by name
```

Note: the `CMakeLists.txt` in the project builds the examples into an executable named by `${PROJECT_NAME}`, which in
this repository is `turbo_twig`.

### Expected output samples

Running the example runner with `--all` typically prints something like:

```
[RUNNING] static_pass
[PASSED]  static_pass
[RUNNING] storage_lifecycle
[PASSED]  storage_lifecycle
[RUNNING] failing_example
[FAILED]  failing_example: intentional failure for demonstration (path/to/exemain.cpp:78)
```

If you ask for a non-existing example, stderr will contain:

```
Error: example 'no_such_example' not found
```

If you filter by a tag that does not exist:

```
Error: no examples with tag 'no_tag'
```

## Implementation notes and portability

- The header uses C++23 features: `std::expected` (C++23), `std::source_location`, concepts, fold-expressions, and
  `std::span`.

- Output is intentionally split between `stdout` (running/passed informational messages) and `stderr` (errors/failures)
  so the runner can be invoked in scripts and its failure output redirected separately.

- The header is header-only and depends only on the standard library; no extra files are required.

- The public API is intentionally minimal and stable: the `ExampleType` concept and the `Registry` template are the only
  public-facing building blocks. The implementation may change, but the concepts and Registry API provide a stable
  surface.

## Quick reference / API summary

- `testfw::Result` — `std::expected<void, Error>`
- `testfw::fail(message)` — construct an `std::unexpected<Error>` capturing `std::source_location`

- `Example` requirements:
    - `static std::string_view name()`
    - `static std::string_view description()`
    - `static std::span<const std::string_view> tags()`
    - EITHER `static Result run()` OR `Result run()` on an instance
    - Optional: `setup()` and `teardown()` (static or instance)

- `testfw::Registry<Examples...>` methods:
    - `static int run_all()`
    - `static int run_by_name(std::string_view)`
    - `static int run_by_tag(std::string_view)`
    - `static void print_list()`

## FAQ / Notes

Q: Why are some messages printed to `stderr` and others to `stdout`?

A: Failure/error messages are routed to `stderr` so they are visible even if `stdout` is redirected, and so scripts can
separate normal output from error output.

Q: How are file/line locations captured for failures?

A: Callers use `testfw::fail("message")` which captures `std::source_location::current()` by default. The `Error` stores
the `std::source_location` so the registry can print the file and line where the failure was reported.

Q: Can I register examples dynamically at runtime?

A: No — the `Registry` is a compile-time template over example types. This simplifies implementation and avoids dynamic
registration complexity. If you need runtime registration, wrap the examples or write a small dynamic dispatcher that
delegates to the compile-time registry.

## Registered Examples

| Name             | File                                       | Tags                                                                                                      | Guard                          |
|------------------|--------------------------------------------|-----------------------------------------------------------------------------------------------------------|--------------------------------|
| `vakya`          | `src/examples/example_vakya.hpp`           | vakya, edsl, types, patterns, tutorial                                                                    | always                         |
| `tarka`          | `src/examples/example_tarka.hpp`           | tarka, smt, z3, tutorial                                                                                  | always (Z3 sections: `HAS_Z3`) |
| `z3`             | `src/examples/example_z3.hpp`              | z3, smt                                                                                                   | `HAS_Z3`                       |
| `pravaha`        | `src/examples/example_pravaha.hpp`         | pravaha, async                                                                                            | always                         |
| `medha`          | `src/examples/example_medha.hpp`           | medha, transactions, edsl, tutorial, isolation                                                            | always                         |
| `sutra`          | `src/examples/example_sutra.hpp`           | sutra                                                                                                     | always                         |
| `crank`          | `src/examples/example_crank.hpp`           | crank, parser, language                                                                                   | always                         |
| `crank_tutorial` | `src/examples/example_crank_tutorial.hpp`  | crank, tutorial, beginner, intermediate, advanced, types, generics, data-structures, safety, verification | always                         |
| `asmjit`         | `src/examples/example_asmjit.hpp`          | asmjit, jit                                                                                               | always                         |
| `rocksdb`        | `src/examples/example_rocksdb.hpp`         | rocksdb, storage                                                                                          | always                         |
| `vulkan`         | `src/examples/example_vulkan_moltenvk.hpp` | vulkan, gpu                                                                                               | `HAS_MOLTENVK`                 |
| `axiom`          | `src/examples/example_axiom.hpp`           | axiom                                                                                                     | `HAS_AXIOM`                    |
| `mlx`            | `src/examples/example_mlx.hpp`             | mlx, gpu                                                                                                  | `HAS_MLX`                      |
| `ftxui`          | `src/examples/example_ftxui.hpp`           | ftxui, ui, terminal                                                                                       | always                         |

### FTXUI Example (`ftxui`)

- **File:** `src/examples/example_ftxui.hpp`
- **Namespace:** global (no namespace, uses ftxui namespace internally)
- **Struct:** `FtxuiExample`
- **Purpose:** Demonstrate FTXUI (Functional Terminal UI) — a declarative, composable terminal UI library for C++23.
- **Sections:**

| # | Function                  | What it demonstrates                                           |
|---|---------------------------|----------------------------------------------------------------|
| 1 | `ex1_basic_text()`        | Text elements with colors, bold, basic styling                 |
| 2 | `ex2_text_decorators()`   | Text decorators: bold, dim, underline, inverted, blink         |
| 3 | `ex3_color_palette()`     | Color palette: red, green, blue, yellow, magenta, cyan, white  |
| 4 | `ex4_layout_containers()` | Vertical (vbox) and horizontal (hbox) container composition    |
| 5 | `ex5_dashboard_layout()`  | Complex dashboard with multiple panels, borders, mixed content |

**Usage:**

```bash
./turbo_twig ftxui          # Run FTXUI example
./turbo_twig --filter ui    # Run all UI examples
./turbo_twig --filter terminal  # Run terminal UI examples
```

**Key Concepts Demonstrated:**

- Element-based declarative UI: `text()`, `separator()`, `hbox()`, `vbox()`
- Decorator pipe composition: `element | ftxui::bold | color(Color::Blue) | border`
- Nested layouts combining vertical and horizontal arrangements
- Complex visual structures with borders and multi-level nesting

### Tarka Example (`tarka`)

- **File:** `src/examples/example_tarka.hpp`
- **Namespace:** `tarka_ex`
- **Struct:** `tarka_ex::TarkaExample`
- **Sections:**

| #  | Function                 | What it demonstrates                                                   |
|----|--------------------------|------------------------------------------------------------------------|
| 1  | `ex1_sorts()`            | Context, bv_sort/array sort construction, CSE interning                |
| 2  | `ex2_terms()`            | Symbols, literals, CSE proof (same ptr), Op codes                      |
| 3  | `ex3_operators()`        | Operator overloads recover Context from node, BvAdd, relational        |
| 4  | `ex4_theory_mask()`      | `compute_theory_mask` over BV and Bool formulae                        |
| 5  | `ex5_router_no_solver()` | `RouterEngine<no_solver_backend>` → Deferred, `solve()`                |
| 6  | `ex6_z3_backend()`       | Z3: SAT/UNSAT round-trip, model extraction, `RouterEngine<z3_backend>` |
| 7  | `ex7_features()`         | `theory_extractor` 9-dim feature vector, `mask_from_features`          |
| 8  | `ex8_egraph()`           | `intern_into_egraph`, `egraph_optimize`, class count                   |
| 9  | `ex9_async()`            | `WorkerPool` construction, task submission, drain                      |
| 10 | `ex10_portfolio()`       | `PortfolioEngine` all-deferred → Unknown, no-hang                      |

### Medha Example (`medha`)

- **File:** `src/examples/example_medha.hpp`
- **Namespace:** `bank_ex`
- **Struct:** `MedhaExample`
- **Theme:** In-process banking ledger — all sections share the account-transfer narrative.
- **Sections:**

| #  | Function                        | What it demonstrates                                                        |
|----|---------------------------------|-----------------------------------------------------------------------------|
| 1  | `ex1_direct_api()`              | `transaction_context`: load / store / commit / auto-abort RAII              |
| 2  | `ex2_atomic_retry()`            | `atomic()` retry loop, `retry::bounded`, `body_idempotent`, `commit_report` |
| 3  | `ex3_replay_safety_rejection()` | `replay_safety::non_idempotent` + retry → `tx_status::rejected` (MEDHA-004) |
| 4  | `ex4_effect_compensation()`     | `defer_compensation()` LIFO abort order; clean on commit path               |
| 5  | `ex5_snapshot_isolation()`      | `snapshot_token` established on first read; read-your-writes                |
| 6  | `ex6_serializable_isolation()`  | `isolation::serializable`, `commit_capability::serial_validation`           |
| 7  | `ex7_nested_scope()`            | `nested_scope` merge on inner commit; discard on inner abort                |
| 8  | `ex8_edsl_typed_api()`          | Typed plan builder, `var_expr`/`in_expr`, `compile()`/`run()`               |
| 9  | `ex9_edsl_string_frontend()`    | `store_stmt()` parse → typed AST; `bind_string()` int parse                 |
| 10 | `ex10_edsl_lithe_lowering()`    | `medha::adapters::lithe::lower()` → `lithe_region_descriptor` metadata      |
| 11 | `ex11_smriti_arena()`           | `arena_scope` commit persists; abort rolls back (`MEDHA_HAS_SMRITI`)        |
| 12 | `ex12_distribution_metadata()`  | `distribution::none` v1; `in_doubt`/`recovery_required` terminal            |

### Crank Tutorial (`crank_tutorial`)

- **File:** `src/examples/example_crank_tutorial.hpp`
- **Namespace:** `crank_tutorial`
- **Struct:** `crank_tutorial::CrankTutorial`
- **Purpose:** Load and execute Crank tutorial scripts from `src/examples/resources/crank/`.
- **Structure:** Single struct. `run()` iterates `ex*.ck` files (sorted), runs full pipeline per script: parse →
  analyse → lower_to_hl (fn=`Main`) → execute_via_interpreter → display result. No Crank source in the header.
- **Resource dir:** resolved via `TURBO_TWIG_SOURCE_DIR` compile definition (set by CMake).

**Tutorial Scripts:**

| File                      | Category        | Description                                           |
|---------------------------|-----------------|-------------------------------------------------------|
| `ex1_1_basic_types.ck`    | Types           | Basic type declarations and usage                     |
| `ex1_2_numeric_types.ck`  | Types           | Integer and floating-point operations                 |
| `ex1_3_control_flow.ck`   | Control         | If/else and loops                                     |
| `ex2_1_collections.ck`    | Collections     | Arrays and slices                                     |
| `ex2_2_strings.ck`        | Collections     | String handling                                       |
| `ex2_3_vector.ck`         | Collections     | Dynamic array-like collections (C++ vector semantics) |
| `ex3_1_structs.ck`        | Data Structures | Struct definitions                                    |
| `ex3_2_enums.ck`          | Data Structures | Enum types                                            |
| `ex4_1_basic_generics.ck` | Generics        | Generic types and functions                           |
| `ex4_2_trait_bounds.ck`   | Generics        | Trait constraints                                     |
| `ex5_1_option.ck`         | Safety          | Option/Maybe type                                     |
| `ex5_2_result.ck`         | Safety          | Result/Either type for error handling                 |
| `ex8_1_stack.ck`          | Data Structures | Stack operations                                      |
| `ex8_2_queue.ck`          | Data Structures | Queue operations                                      |
| `ex8_3_linked_list.ck`    | Data Structures | Linked list                                           |
| `ex8_4_bst.ck`            | Data Structures | Binary search tree                                    |
| `ex9_1_cpp_interop.ck`    | Interop         | C++ value integration with Crank computations         |
| `ex10_1_contracts.ck`     | Verification    | Assertion and contract checking                       |

### How to Use the Crank Tutorial

```bash
./turbo_twig crank_tutorial
./turbo_twig --filter tutorial
./turbo_twig --filter crank
```

**Tags:** `crank`, `tutorial`, `beginner`, `intermediate`, `advanced`, `types`, `generics`, `data-structures`, `safety`,
`verification`

## References

- Header: `include/test/example_registry.hpp`
- Runner: `src/examples/exemain.cpp`

---

*Document generated for repository local documentation.*

