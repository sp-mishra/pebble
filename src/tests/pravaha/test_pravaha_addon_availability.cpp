#include "catch_amalgamated.hpp"

#include "pravaha/adapters/lithe_runtime.hpp"
#include "pravaha/backends/vulkan_gpu.hpp"

TEST_CASE (
"Pravaha downstream add-ons are safe without Lithe"
,
"[pravaha][addons]"
)
 {
    STATIC_REQUIRE_FALSE(pravaha::adapters::lithe_runtime::available);
    STATIC_REQUIRE_FALSE(pravaha::backends::vulkan::available);
}
