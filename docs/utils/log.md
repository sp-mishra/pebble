# lg Logging Utility (include/utils/log.hpp)

- Path: `include/utils/log.hpp`
- Header-only, C++23-compatible logging shim
- Namespace: `lg`
- Compile-time control macro: `LG_LOG_ENABLED`

Table of Contents

- Purpose
- Design goals
- Compile-time switches
- Public API
- Configuration functions
- Usage examples
- Build and CMake guidance
- Portability and notes

## Purpose

`include/utils/log.hpp` provides a small header-only logging boundary intended to give projects a single, light-weight
logging API surface without imposing a heavy dependency when logging is disabled. It preserves a concise call-site style
like:

```cpp
lg::info("Hello {}", name);
lg::error("Failed: {}", reason);
```

When logging is enabled the header forwards to `spdlog` for formatting and sinks. When disabled, the calls become no-ops
and avoid generating runtime logging code (arguments are still evaluated unless you wrap them yourself).

## Design goals

- Keep call sites simple and stable: `lg::(info|warn|error|debug|critical)(fmt, ...)`.
- Allow a single compile-time switch to turn logging on/off across the project.
- Prefer a header-only approach with a small configuration surface.
- Provide a safe fallback when logging is disabled so projects can compile without `spdlog` present.
- Minimal runtime overhead when disabled.

## Compile-time switches

- `LG_LOG_ENABLED` (macro)
    - If undefined, defaults to `1` (enabled).
    - If defined as `0`, the header compiles to no-op logging functions; `spdlog` is not required.
    - If defined as `1`, the header includes `<spdlog/spdlog.h>` and forwards logging calls to `spdlog`.

Note: The macro name is kept for historical compatibility. The public C++ namespace in the header is `lg` (not
`groklab`).

Examples:

- Disable logging at compile-time:
    - `-DLG_LOG_ENABLED=0`
- Enable logging at compile-time (default):
    - `-DLG_LOG_ENABLED=1`

## Public API

Namespace: `lg`

Logging functions (templated, forwarding formatting arguments):

```cpp
namespace lg {
    template<typename... Args>
    void info(/* fmt, */ Args&&... args);

    template<typename... Args>
    void warn(/* fmt, */ Args&&... args);

    template<typename... Args>
    void error(/* fmt, */ Args&&... args);

    template<typename... Args>
    void debug(/* fmt, */ Args&&... args);

    template<typename... Args>
    void critical(/* fmt, */ Args&&... args);
}
```

- When `LG_LOG_ENABLED==1`, the first parameter uses `spdlog::format_string_t<Args...>` to enable compile-time format
  checking with `spdlog`'s formatting.
- When `LG_LOG_ENABLED==0`, the first parameter is `std::string_view` and the implementation swallows arguments to avoid
  unused warnings.

Return type: `void`.

### Configuration helpers

The header also exposes small configuration functions under `lg::log` (these are no-ops when logging is disabled):

```cpp
namespace lg::log {
    void set_level(int level) noexcept;       // set logging level (spdlog enum value expected)
    void set_pattern(std::string_view pattern) noexcept;
    void flush_on(int level) noexcept;        // flush on level
}
```

When `LG_LOG_ENABLED==1`, these forward to the matching `spdlog` configuration calls. When disabled, they are harmless
no-ops.

## Usage examples

Minimal example (logging enabled and `spdlog` available):

```cpp
#include "utils/log.hpp"

int main() {
    lg::info("Starting application version {}", 1);
    lg::warn("This is a warning with code {}", 42);
    lg::error("Failure: {}", "disk full");
}
```

When compiled with `-DLG_LOG_ENABLED=0`, the `lg::...` calls are compiled to no-ops and will not emit logging output (
arguments are still evaluated). If you need zero argument evaluation, wrap calls like:

```cpp
if (should_log) lg::info("Expensive: {}", compute_expensive());
// or
lg::info([&]{ return fmt::format("Expensive: {}", compute_expensive()); }()); // but this evaluates anyway
```

If you need absolute zero evaluation when logging is disabled, use a macro or explicit runtime guard at the call site.

## Build and CMake guidance

- If you want logging enabled and want to use `spdlog`, add `spdlog` to your build.

Example CMake snippet (preferred if spdlog is available via find_package):

```cmake
find_package(spdlog CONFIG REQUIRED)
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE spdlog::spdlog)
# make sure include/ path is visible; header will include <spdlog/spdlog.h>
```

If you prefer to build without `spdlog` or do not want to add a dependency, compile with logging disabled:

```bash
c++ -std=c++23 -DLG_LOG_ENABLED=0 -I include -I . src/main.cpp -o main
```

This ensures the project does not need `spdlog` headers or libraries.

## Portability and notes

- Requires C++23 for the header as written (uses `if constexpr` with inline variables and other C++20/23 conveniences).
- When `LG_LOG_ENABLED==1` the header depends on `spdlog` being available.
- The header intentionally keeps the macro name `LG_LOG_ENABLED` for backward compatibility; the C++ namespace is `lg`.

## Troubleshooting

- "Use of variable template requires template arguments" errors at call sites typically mean the compiler picked the
  overload that expects `spdlog::format_string_t<...>` but `spdlog` is not included; ensure `LG_LOG_ENABLED` is set to
  `0` if `spdlog` is not present, or add `spdlog` to the include path.

- If you see link errors for `spdlog` symbols, ensure you link the proper `spdlog` target (e.g., `spdlog::spdlog`) or
  compile with header-only fmt/spdlog builds.

## Change log / notes

- The header previously used the `groklab` namespace in older versions; current header uses `lg` as the public
  namespace. Call sites should use `lg::info` etc.

## API Quick Reference

- `lg::info(fmt, ...)`
- `lg::warn(fmt, ...)`
- `lg::error(fmt, ...)`
- `lg::debug(fmt, ...)`
- `lg::critical(fmt, ...)`
- `lg::log::set_level(int)`
- `lg::log::set_pattern(std::string_view)`
- `lg::log::flush_on(int)`

## Thread Safety and MDC

### Mapped Diagnostic Context (MDC)

`lg::mdc` provides thread-local storage for diagnostic fields:

```cpp
namespace lg::mdc {
    void put(std::string_view key, std::string_view value);
    void remove(std::string_view key);
    void clear();
}
```

- **Thread-local storage**: Each thread has independent MDC context via `spdlog::mdc`
- **No synchronization needed**: MDC operations are inherently thread-safe; no locking required
- **Scope**: MDC values persist for the thread until explicitly removed or cleared

### LogSpan Tracing

`lg::LogSpan` is a RAII span for tracing:

```cpp
{
    lg::LogSpan span("operation_name", {{"field", "value"}});
    // Logs begin event with fields
    // ...
}
// Logs end event with elapsed duration
```

**Format consistency**: Begin/end events use consistent format strings. See `tests/test_log.cpp` for verification that
span logging is properly formatted.

**Thread safety**: Each `LogSpan` is associated with the calling thread; no thread-safe guarantees beyond that.
Different threads can create concurrent spans without interference.


---

*Document generated for repository local documentation.*

