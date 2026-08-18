# Setu Memory Mapping Library (include/utils/setu.hpp)

**Path**: `include/utils/setu.hpp`  
**Namespace**: `setu`  
**Design**: Header-only, C++23 memory mapping abstraction for systems programming

---

## Overview

Setu provides a type-safe, bounds-checked memory mapping interface for file-backed and anonymous mappings. Core design:
pay only for what you use (no virtual functions, minimal overhead).

---

## Features

- **Type-safe memory views** with `region_view` (non-owning)
- **Bounds checking** with overflow/underflow protection
- **Alignment guarantees** (page-aligned, platform-aware)
- **Const-correctness** (read-only vs read-write modes)
- **Typed access** via `as_span<T>` (requires trivially copyable types)
- **Flush operations** (async/sync to disk)
- **Remap support** (resize mappings in place)
- **Portable** (macOS/POSIX, extensible backend abstraction)

---

## API Reference

### mapping<access_mode>

Template class representing a file or anonymous mapping.

```cpp
template<access_mode Mode>
class mapping {
public:
    // File-backed operations
    static result<mapping> open_existing(const std::filesystem::path &path);
    static result<mapping> open_or_create(const std::filesystem::path &path, size_t size);
    static result<mapping> create_truncate(const std::filesystem::path &path, size_t size);

    // Anonymous mappings
    static result<mapping> create_anonymous_private(size_t size);
    static result<mapping> create_anonymous_shared(size_t size);

    // Query and access
    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] bool is_file_backed() const noexcept;

    // Memory views
    [[nodiscard]] region_view full_region() const noexcept;
    [[nodiscard]] result<region_view> subregion(size_t offset, size_t length) const;

    // Flush operations (file-backed only)
    [[nodiscard]] result<void> flush(flush_mode mode = flush_mode::async);
    [[nodiscard]] result<void> flush_range(size_t offset, size_t length, 
                                            flush_mode mode = flush_mode::async) const;

    // Remap/resize
    [[nodiscard]] result<void> remap(size_t new_size);
};
```

### region_view

Non-owning view into mapped memory.

```cpp
class region_view {
public:
    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] bool writable() const noexcept;
    [[nodiscard]] const std::byte *data() const noexcept;

    // Byte access
    [[nodiscard]] std::span<std::byte> as_bytes();
    [[nodiscard]] std::span<const std::byte> as_bytes() const;

    // Typed access (requires trivially_copyable + standard_layout)
    template<trivially_copyable T>
    [[nodiscard]] result<std::span<T>> as_span();

    template<trivially_copyable T>
    [[nodiscard]] result<const T*> as_pointer(size_t offset);
};
```

---

## Safety and Correctness

### Bounds Checking

All operations validate:

- **Offset underflow**: `offset <= limit` is checked first
- **Size overflow**: `count * element_size <= SIZE_MAX` is verified
- **Range validity**: `offset + size <= mapping.size()` is enforced

**Review Fix**: Underflow/overflow checks are in place and prioritized correctly. See `is_valid_range()` and
`is_valid_array_range()` helpers.

### Alignment Guarantees

- **Mapping base**: Page-aligned (4096 bytes on most systems)
- **Subregions**: Inherit parent alignment; pointer arithmetic is safe
- **Typed access**: Caller responsible for ensuring type alignment

**Review Fix**: Pointer arithmetic uses page-size masking (`addr & ~(page_size - 1)`) to guarantee alignment.

### Const-Correctness

- **read_only mode**: `full_region()` returns `writable_=false` view; mutation prevents errors
- **read_write mode**: `full_region()` returns `writable_=true` view; all operations permitted
- **flush_range()**: Const method on read_only mapping; backend accepts const void* safely

**Review Fix**: const-cast in `flush_range()` is safe because backend signature is `flush(const void*, ...)`. flush
operations don't modify data; const-correctness is preserved.

---

## Usage Examples

### Basic file reading

```cpp
auto map_result = setu::mapping<setu::access_mode::read_only>::open_existing("data.bin");
if (!map_result) {
    std::cerr << "Failed to map: " << map_result.error();
    return;
}

auto &mapping = *map_result;
auto region = mapping.full_region();

// Read as bytes
auto bytes = region.as_bytes();
std::cout << "Mapped " << bytes.size() << " bytes\n";
```

### Typed access

```cpp
struct Record {
    uint32_t id;
    double value;
};

auto records = region.as_span<Record>();
if (!records) {
    std::cerr << "Alignment or size mismatch\n";
    return;
}

for (const auto &rec : *records) {
    std::cout << rec.id << ": " << rec.value << "\n";
}
```

### Write and flush

```cpp
auto map_result = setu::mapping<setu::access_mode::read_write>::create_truncate("output.bin", 4096);
auto &mapping = *map_result;

auto region = mapping.full_region();
auto bytes = region.as_bytes();
std::memcpy(bytes.data(), "Hello", 5);

// Flush to disk
auto flush_result = mapping.flush(setu::flush_mode::sync);
if (!flush_result) {
    std::cerr << "Flush failed\n";
}
```

### Subregions

```cpp
// Map file and access a specific range
auto sub = mapping.subregion(1024, 256);
if (!sub) {
    std::cerr << "Out of bounds\n";
    return;
}

auto sub_region = *sub;
auto sub_bytes = sub_region.as_bytes();
// Work with 256-byte subregion
```

---

## Thread Safety

- **Const methods**: Safe for concurrent reads from multiple threads
- **Mutable methods** (flush, remap): NOT thread-safe; coordinate via external synchronization
- **region_view**: Non-owning; lifetime tied to parent mapping

---

## Portability

- **C++ Standard**: C++23 with modern features
- **Platforms**: macOS (POSIX mmap/msync/madvise). Linux/Windows support via backend abstraction.
- **Architecture**: All platforms where page-aligned memory is available

---

## Error Handling

Returns `std::expected<T, std::error_code>` for all operations:

```cpp
enum class error_code {
    invalid_argument,     // Config/param error
    file_not_found,       // File does not exist
    permission_denied,    // Access denied
    out_of_bounds,        // Offset/size out of range
    alignment_error,      // Type alignment mismatch
    backend_error,        // OS-level mmap/msync failure
    flush_failed,         // msync returned error
};
```

---

## Testing

Unit tests in `src/tests/test_setu.cpp` verify:

- Bounds checking (overflow, underflow, out-of-bounds)
- Alignment guarantees (page alignment, subregion alignment)
- Const-correctness (read-only vs read-write modes)
- Typed access safety (via `as_span<T>`)

**Review Coverage**: Tests confirm review fixes for bounds/underflow, alignment, and const-correctness.

---

## Notes

- **Remap invalidation**: All views/spans become invalid after `remap()`. Caller must not use stale views.
- **Typed access**: Only safe for `std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>`.
- **Endianness**: No automatic conversion; caller responsible for byte order.
- **Sparse files**: Supported (untouched pages may not consume disk space).
