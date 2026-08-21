#pragma once

// Internal feature discovery for optional Pravaha integrations.  Core Pravaha
// never requires these headers; an application receives the enabled surface
// solely when it has supplied the relevant downstream include directories.

#if __has_include("edsl/lithe_runtime.hpp")
#define PEBBLE_PRAVAHA_DETAIL_HAS_LITHE_RUNTIME 1
#else
#define PEBBLE_PRAVAHA_DETAIL_HAS_LITHE_RUNTIME 0
#endif

#if __has_include("edsl/backends/lithe_codegen_vulkan_spirv_ir.hpp") && \
    __has_include("edsl/backends/lithe_codegen_vulkan.hpp")
#define PEBBLE_PRAVAHA_DETAIL_HAS_LITHE_VULKAN 1
#else
#define PEBBLE_PRAVAHA_DETAIL_HAS_LITHE_VULKAN 0
#endif

namespace pravaha::addons {
    inline constexpr bool lithe_runtime_available =
        PEBBLE_PRAVAHA_DETAIL_HAS_LITHE_RUNTIME != 0;
    inline constexpr bool lithe_vulkan_headers_available =
        PEBBLE_PRAVAHA_DETAIL_HAS_LITHE_VULKAN != 0;
} // namespace pravaha::addons
