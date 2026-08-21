#define CATCH_CONFIG_MAIN
#include <catch_amalgamated.hpp>
#include "utils/setu.hpp"
#include <fstream>
#include <cstring>

TEST_CASE (



"Setu - File-backed mapping"
,
"[setu][mapping]"
)
 {
    const char* test_path = "/tmp/setu_test_file.dat";

    SECTION("Create and open file mapping")
    {
        // Create test file
        {
            std::ofstream ofs(test_path, std::ios::binary);
            const char* data = "Hello, Setu!";
            ofs.write(data, std::strlen(data));
        }

        // Open with read-only mapping
        auto map_result = setu::mapping<setu::read_only>::open_existing(test_path);
        REQUIRE(map_result.has_value());

        auto& map = *map_result;
        REQUIRE(map.is_valid());
        REQUIRE(map.size() == 12);

        // Read bytes
        auto bytes = map.as_bytes();
        std::string content(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        REQUIRE(content == "Hello, Setu!");

        // Cleanup
        std::remove(test_path);
    }

    SECTION("Create or open file mapping")
    {
        auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, 4096);
        REQUIRE(map_result.has_value());

        auto& map = *map_result;
        REQUIRE(map.is_valid());
        REQUIRE(map.size() == 4096);

        // Write data
        auto mut_bytes = map.as_bytes();
        const char* data = "Test data";
        std::memcpy(mut_bytes.data(), data, std::strlen(data));

        // Flush
        auto flush_result = map.flush(setu::flush_mode::sync);
        REQUIRE(flush_result.has_value());

        // Cleanup
        std::remove(test_path);
    }
}

TEST_CASE (



"Setu - Anonymous mapping"
,
"[setu][mapping]"
)
 {
    SECTION("Create anonymous private mapping")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(8192);
        REQUIRE(map_result.has_value());

        auto& map = *map_result;
        REQUIRE(map.is_valid());
        REQUIRE(map.size() == 8192);
        REQUIRE(map.kind() == setu::map_kind::anonymous_private);

        // Write and read
        auto bytes = map.as_bytes();
        bytes[0] = std::byte{42};
        REQUIRE(bytes[0] == std::byte{42});
    }

    SECTION("Create anonymous shared mapping")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_shared(4096);
        REQUIRE(map_result.has_value());

        auto& map = *map_result;
        REQUIRE(map.is_valid());
        REQUIRE(map.kind() == setu::map_kind::anonymous_shared);
    }
}

TEST_CASE (



"Setu - Region views"
,
"[setu][region]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Full region")
    {
        auto region = map.full_region();
        REQUIRE(region.is_valid());
        REQUIRE(region.size() == 4096);
        REQUIRE(region.writable());
    }

    SECTION("Subregion")
    {
        auto region_result = map.subregion(100, 200);
        REQUIRE(region_result.has_value());

        auto& region = *region_result;
        REQUIRE(region.size() == 200);
    }
}

// ============================================================================
// Zero-Length Region Semantics Tests
// ============================================================================

TEST_CASE (



"Zero-length region semantics"
,
"[setu][region][zero-length]"
)
 {
    auto map = setu::mapping<setu::read_write>::create_anonymous_private(4096);
    REQUIRE(map);

    auto region = map->full_region();
    REQUIRE(region.is_valid());

    SECTION("Zero-length region at valid offset is valid")
    {
        // Create zero-length region at offset 100
        auto empty = region.subregion(100, 0);
        REQUIRE(empty);
        REQUIRE(empty->is_valid());
        REQUIRE(empty->size() == 0);
        REQUIRE(empty->data() != nullptr);

        // as_bytes should return empty span
        auto bytes = empty->as_bytes();
        REQUIRE(bytes.empty());
        REQUIRE(bytes.size() == 0);
    }

    SECTION("Zero-length region at end boundary is valid")
    {
        // Create zero-length region at end of mapping
        auto empty = region.subregion(region.size(), 0);
        REQUIRE(empty);
        REQUIRE(empty->is_valid());
        REQUIRE(empty->size() == 0);
    }

    SECTION("Zero-length region has zero pages")
    {
        auto empty = region.subregion(100, 0);
        REQUIRE(empty);

        REQUIRE(empty->page_count<256>() == 0);
        REQUIRE(empty->page_count<512>() == 0);
        REQUIRE(empty->page_count<4096>() == 0);
    }

    SECTION("page_at on zero-length region returns invalid page")
    {
        auto empty = region.subregion(100, 0);
        REQUIRE(empty);

        auto page = empty->page_at<256>(0);
        REQUIRE_FALSE(page.is_valid());
    }

    SECTION("page_index_to_offset on zero-length region")
    {
        auto empty = region.subregion(100, 0);
        REQUIRE(empty);

        // No pages fit, so index 0 should be invalid
        auto offset = empty->page_index_to_offset<256>(0);
        REQUIRE_FALSE(offset.has_value());
    }

    SECTION("is_valid_page_index on zero-length region")
    {
        auto empty = region.subregion(100, 0);
        REQUIRE(empty);

        REQUIRE_FALSE(empty->is_valid_page_index<256>(0));
        REQUIRE_FALSE(empty->is_valid_page_index<256>(1));
    }

    SECTION("Typed access on zero-length region fails appropriately")
    {
        auto empty = region.subregion(100, 0);
        REQUIRE(empty);

        // Cannot read any object from zero-length region
        auto result = empty->read_at<uint32_t>(0);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Zero-count array access on valid region succeeds")
    {
        // Reading zero elements at a valid offset should succeed
        auto result = region.read_array<uint32_t>(0, 0);
        REQUIRE(result);
        REQUIRE(result->empty());
        REQUIRE(result->size() == 0);

        // Even at end of region
        auto result2 = region.read_array<uint32_t>(region.size(), 0);
        REQUIRE(result2);
        REQUIRE(result2->empty());
    }

    SECTION("subregion_from at end returns valid empty region")
    {
        auto empty = region.subregion_from(region.size());
        REQUIRE(empty);
        REQUIRE(empty->is_valid());
        REQUIRE(empty->size() == 0);
    }

    SECTION("Default constructed region is invalid")
    {
        setu::region_view invalid;
        REQUIRE_FALSE(invalid.is_valid());
        REQUIRE(invalid.size() == 0);
        REQUIRE(invalid.data() == nullptr);
    }

    SECTION("Writable zero-length region")
    {
        auto empty = region.subregion(100, 0);
        REQUIRE(empty);
        REQUIRE(empty->writable());

        auto bytes = empty->as_bytes();
        REQUIRE(bytes.empty());
    }
}

TEST_CASE (



"Mapping page helpers with zero/small sizes"
,
"[setu][mapping][page][edge-cases]"
)
 {
    SECTION("Mapping with size smaller than page size")
    {
        auto map = setu::mapping<setu::read_write>::create_anonymous_private(100);
        REQUIRE(map);

        // Zero complete pages fit
        REQUIRE(map->page_count<256>() == 0);
        REQUIRE(map->page_count<512>() == 0);
        REQUIRE(map->page_count<4096>() == 0);

        // page_at(0) should return invalid page
        auto page = map->page_at<256>(0);
        REQUIRE_FALSE(page.is_valid());

        // page_index_to_offset should fail
        auto offset = map->page_index_to_offset<256>(0);
        REQUIRE_FALSE(offset.has_value());

        // is_valid_page_index should be false
        REQUIRE_FALSE(map->is_valid_page_index<256>(0));
    }

    SECTION("Mapping with exact page size")
    {
        auto map = setu::mapping<setu::read_write>::create_anonymous_private(256);
        REQUIRE(map);

        // Exactly one page fits
        REQUIRE(map->page_count<256>() == 1);

        // page_at(0) should be valid
        auto page = map->page_at<256>(0);
        REQUIRE(page.is_valid());

        // page_at(1) should be invalid
        auto page2 = map->page_at<256>(1);
        REQUIRE_FALSE(page2.is_valid());
    }
}

TEST_CASE (



"Setu - Typed access"
,
"[setu][typed]"
)
 {
    struct TestStruct {
        int value;
        float data;
    };

    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;
    auto region = map.full_region();

    SECTION("Write and read typed object")
    {
        auto write_result = region.write_at<TestStruct>(0);
        REQUIRE(write_result.has_value());

        auto* obj = *write_result;
        obj->value = 42;
        obj->data = 3.14f;

        auto read_result = region.read_at<TestStruct>(0);
        REQUIRE(read_result.has_value());

        const auto* read_obj = *read_result;
        REQUIRE(read_obj->value == 42);
        REQUIRE(read_obj->data == 3.14f);
    }

    SECTION("Alignment checking")
    {
        auto result = region.write_at<TestStruct>(1); // Misaligned
        REQUIRE(!result.has_value());
        REQUIRE(result.error() == setu::make_error_code(setu::error_code::misaligned_access));
    }

    SECTION("Bounds checking")
    {
        auto result = region.write_at<TestStruct>(4090); // Out of bounds (struct won't fit)
        REQUIRE(!result.has_value());
        // Could be either misaligned_access or out_of_bounds depending on alignment check order
        // Just verify it fails
    }

    SECTION("Array access")
    {
        auto array_result = region.write_array<int>(64, 10);
        REQUIRE(array_result.has_value());

        auto arr = *array_result;
        for (size_t i = 0; i < 10; ++i)
        {
            arr[i] = static_cast<int>(i * 10);
        }

        auto read_result = region.read_array<int>(64, 10);
        REQUIRE(read_result.has_value());

        auto read_arr = *read_result;
        for (size_t i = 0; i < 10; ++i)
        {
            REQUIRE(read_arr[i] == static_cast<int>(i * 10));
        }
    }
}

TEST_CASE (



"Setu - Page view"
,
"[setu][page]"
)
 {
    constexpr size_t page_size = 4096;

    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(page_size * 3);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;
    auto region = map.full_region();

    SECTION("Create page view")
    {
        setu::page_view<page_size> page(region, 0);
        REQUIRE(page.is_valid());
        REQUIRE(page.size() == page_size);
    }

    SECTION("Access page bytes")
    {
        setu::page_view<page_size> page(region, 1); // Second page
        auto bytes = page.page_bytes();
        REQUIRE(bytes.size() == page_size);

        bytes[0] = std::byte{99};
        REQUIRE(bytes[0] == std::byte{99});
    }

    SECTION("Typed access in page")
    {
        struct PageHeader {
            uint32_t magic;
            uint32_t version;
        };

        setu::page_view<page_size> page(region, 0);
        auto header_result = page.write_header<PageHeader>();
        REQUIRE(header_result.has_value());

        auto* header = *header_result;
        header->magic = 0xDEADBEEF;
        header->version = 1;

        auto read_result = page.read_header<PageHeader>();
        REQUIRE(read_result.has_value());
        REQUIRE((*read_result)->magic == 0xDEADBEEF);
        REQUIRE((*read_result)->version == 1);
    }

    SECTION("Page at offset")
    {
        auto page_result = setu::page_view<page_size>::at_offset(region, page_size);
        REQUIRE(page_result.has_value());
        REQUIRE(page_result->is_valid());
    }

    SECTION("Page iteration")
    {
        auto page_range = setu::pages<page_size>(region);
        REQUIRE(page_range.size() == 3);

        size_t count = 0;
        for (auto page : page_range)
        {
            REQUIRE(page.is_valid());
            ++count;
        }
        REQUIRE(count == 3);
    }
}

TEST_CASE (



"Setu - Offset pointer"
,
"[setu][offset_ptr]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;
    auto region = map.full_region();

    SECTION("Basic offset pointer")
    {
        setu::offset_ptr<int> ptr(std::ptrdiff_t(0));
        REQUIRE(!ptr.is_null());
        REQUIRE(ptr.offset() == 0);

        auto write_result = region.write_at<int>(0);
        REQUIRE(write_result.has_value());
        **write_result = 42;

        auto resolved = ptr.try_get_mut(region);
        REQUIRE(resolved.has_value());
        REQUIRE(**resolved == 42);
    }

    SECTION("Null offset pointer")
    {
        setu::offset_ptr<int> ptr(nullptr);
        REQUIRE(ptr.is_null());
        REQUIRE(ptr == nullptr);

        auto result = ptr.try_get(region);
        REQUIRE(!result.has_value());
    }

    SECTION("Linked structure with offset pointers")
    {
        // Simple struct without self-referential offset_ptr
        struct SimpleNode {
            int value;
            std::ptrdiff_t next_offset; // Store offset manually
        };

        auto node1_result = region.write_at<SimpleNode>(0);
        auto node2_result = region.write_at<SimpleNode>(sizeof(SimpleNode));

        REQUIRE(node1_result.has_value());
        REQUIRE(node2_result.has_value());

        auto* node1 = *node1_result;
        auto* node2 = *node2_result;

        node1->value = 1;
        node1->next_offset = sizeof(SimpleNode);
        node2->value = 2;
        node2->next_offset = -1; // Null marker

        // Traverse using offset_ptr
        setu::offset_ptr<SimpleNode> ptr(node1->next_offset);
        auto current = ptr.try_get_mut(region);
        REQUIRE(current.has_value());
        REQUIRE((*current)->value == 2);
        REQUIRE((*current)->next_offset == -1);
    }
}

TEST_CASE (



"Setu - Platform utilities"
,
"[setu][platform]"
)
 {
    SECTION("Page size")
    {
        auto ps = setu::platform::page_size();
        REQUIRE(ps > 0);
        REQUIRE((ps & (ps - 1)) == 0); // Power of 2
    }

    SECTION("Page alignment")
    {
        const size_t page_size = setu::platform::page_size();

        REQUIRE(setu::page::align_up(0, page_size) == 0);
        REQUIRE(setu::page::align_up(1, page_size) == page_size);
        REQUIRE(setu::page::align_up(page_size, page_size) == page_size);
        REQUIRE(setu::page::align_up(page_size + 1, page_size) == page_size * 2);

        REQUIRE(setu::page::align_down(0, page_size) == 0);
        REQUIRE(setu::page::align_down(1, page_size) == 0);
        REQUIRE(setu::page::align_down(page_size, page_size) == page_size);
        REQUIRE(setu::page::align_down(page_size + 1, page_size) == page_size);

        REQUIRE(setu::page::is_aligned(0, page_size));
        REQUIRE(setu::page::is_aligned(page_size, page_size));
        REQUIRE(!setu::page::is_aligned(page_size + 1, page_size));
    }
}


TEST_CASE (



"Setu - Error handling"
,
"[setu][errors]"
)
 {
    SECTION("Invalid file path")
    {
        auto result = setu::mapping<setu::read_only>::open_existing("/nonexistent/path.dat");
        REQUIRE(!result.has_value());
        REQUIRE(result.error() == setu::make_error_code(setu::error_code::file_not_found));
    }

    SECTION("Out of bounds access")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(1024);
        REQUIRE(map_result.has_value());

        auto region_result = map_result->subregion(1000, 100);
        REQUIRE(!region_result.has_value());
        REQUIRE(region_result.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }
}

TEST_CASE (



"Setu - Memory advice operations"
,
"[setu][advice]"
)
 {
    const char* test_path = "/tmp/setu_test_advice.dat";

    // Create test file
    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, 8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Whole mapping advice")
    {
        REQUIRE(map.advise(setu::advice_mode::sequential).has_value());
        REQUIRE(map.advise(setu::advice_mode::random).has_value());
        REQUIRE(map.advise(setu::advice_mode::will_need).has_value());
        REQUIRE(map.advise(setu::advice_mode::dont_need).has_value());
        REQUIRE(map.advise(setu::advice_mode::normal).has_value());
    }

    SECTION("Range advice")
    {
        REQUIRE(map.advise_range(0, 4096, setu::advice_mode::sequential).has_value());
        REQUIRE(map.advise_range(4096, 4096, setu::advice_mode::random).has_value());
    }

    SECTION("Invalid range advice")
    {
        auto result = map.advise_range(8000, 1000, setu::advice_mode::sequential);
        REQUIRE(!result.has_value());
    }

    std::remove(test_path);
}

// ============================================================================
// Overflow-Safe Bounds Checking Tests
// ============================================================================

TEST_CASE (



"Setu - Overflow-safe bounds checking - Basic cases"
,
"[setu][overflow][bounds]"
)
 {
    SECTION("is_valid_range helper - normal cases")
    {
        // Valid ranges
        REQUIRE(setu::detail::is_valid_range(0, 100, 1000));
        REQUIRE(setu::detail::is_valid_range(100, 100, 200));
        REQUIRE(setu::detail::is_valid_range(0, 1000, 1000));
        REQUIRE(setu::detail::is_valid_range(999, 1, 1000));

        // Invalid ranges
        REQUIRE_FALSE(setu::detail::is_valid_range(100, 101, 200)); // offset + size > limit
        REQUIRE_FALSE(setu::detail::is_valid_range(1000, 1, 1000)); // offset > limit
        REQUIRE_FALSE(setu::detail::is_valid_range(500, 600, 1000)); // offset + size > limit
    }

    SECTION("is_valid_range helper - edge cases at boundaries")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // At maximum boundary
        REQUIRE(setu::detail::is_valid_range(0, max_val, max_val));
        REQUIRE(setu::detail::is_valid_range(max_val - 1, 1, max_val));

        // Just over the boundary
        REQUIRE_FALSE(setu::detail::is_valid_range(max_val - 1, 2, max_val));
        REQUIRE_FALSE(setu::detail::is_valid_range(1, max_val, max_val));
    }

    SECTION("is_valid_range helper - overflow protection")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // These would overflow with naive offset + size check
        REQUIRE_FALSE(setu::detail::is_valid_range(max_val, 1, max_val));
        REQUIRE_FALSE(setu::detail::is_valid_range(max_val - 100, 200, max_val));
        REQUIRE_FALSE(setu::detail::is_valid_range(max_val / 2, max_val / 2 + 2, max_val));

        // Large values that don't overflow but exceed limit
        REQUIRE_FALSE(setu::detail::is_valid_range(1000, max_val - 500, max_val));
    }

    SECTION("is_valid_array_range helper - normal cases")
    {
        // Valid array ranges
        REQUIRE(setu::detail::is_valid_array_range(0, 10, sizeof(int), 1000));
        REQUIRE(setu::detail::is_valid_array_range(100, 20, sizeof(double), 1000));
        REQUIRE(setu::detail::is_valid_array_range(0, 0, sizeof(int), 1000)); // Zero count

        // Invalid array ranges
        REQUIRE_FALSE(setu::detail::is_valid_array_range(900, 30, sizeof(int), 1000));
        REQUIRE_FALSE(setu::detail::is_valid_array_range(1000, 1, sizeof(int), 1000));
    }

    SECTION("is_valid_array_range helper - overflow in count * element_size")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // These would overflow when computing count * element_size
        REQUIRE_FALSE(setu::detail::is_valid_array_range(0, max_val, 2, max_val));
        REQUIRE_FALSE(setu::detail::is_valid_array_range(0, max_val / 2, 4, max_val));
        REQUIRE_FALSE(setu::detail::is_valid_array_range(0, max_val / 100, 200, max_val));

        // Large but valid array ranges
        const size_t safe_count = max_val / sizeof(int);
        REQUIRE(setu::detail::is_valid_array_range(0, safe_count, sizeof(int), max_val));
    }
}

TEST_CASE (



"Setu - Overflow-safe bounds checking - mapping operations"
,
"[setu][overflow][mapping]"
)
 {
    const char* test_path = "/tmp/setu_test_overflow_mapping.dat";
    const size_t file_size = 8192;

    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, file_size);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Subregion with overflow-prone values")
    {
        // Attempt to create subregion that would overflow with naive check
        const size_t max_val = std::numeric_limits<size_t>::max();

        // offset + length would overflow SIZE_MAX
        auto result1 = map.subregion(file_size - 100, max_val);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // offset + length would wrap around
        auto result2 = map.subregion(max_val - 1000, 2000);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE(result2.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Large offset beyond file size
        auto result3 = map.subregion(max_val / 2, 100);
        REQUIRE_FALSE(result3.has_value());
        REQUIRE(result3.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Flush range with overflow-prone values")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // offset + length would overflow
        auto result1 = map.flush_range(file_size - 100, max_val, setu::flush_mode::sync);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Wrapping arithmetic
        auto result2 = map.flush_range(max_val - 1000, 2000, setu::flush_mode::sync);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE(result2.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Advise range with overflow-prone values")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // offset + length would overflow
        auto result1 = map.advise_range(file_size - 50, max_val, setu::advice_mode::sequential);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Large offset
        auto result2 = map.advise_range(max_val / 3, 100, setu::advice_mode::random);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE(result2.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Overflow-safe bounds checking - region operations"
,
"[setu][overflow][region]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(8192);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    SECTION("Region subregion with overflow values")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // Would overflow with offset + length
        auto result1 = region.subregion(8000, max_val);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Wrapping arithmetic
        auto result2 = region.subregion(max_val - 100, 200);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE(result2.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Typed read_at with large offset")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // offset + sizeof(int) would overflow or exceed bounds
        auto result1 = region.read_at<int>(max_val - 2);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        auto result2 = region.read_at<int>(8190); // Near end, but int won't fit
        REQUIRE_FALSE(result2.has_value());
    }

    SECTION("Typed write_at with large offset")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        auto result1 = region.write_at<long long>(max_val / 2);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Array read with overflow in count * sizeof(T)")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // count * sizeof(int) would overflow
        auto result1 = region.read_array<int>(0, max_val / 2);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // count * sizeof(long long) would overflow
        auto result2 = region.read_array<long long>(0, max_val / 4);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE(result2.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Array write with overflow-prone values")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // offset + (count * sizeof(T)) would overflow
        auto result1 = region.write_array<int>(8000, max_val / 3);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Large count causing overflow
        auto result2 = region.write_array<double>(0, max_val / 7);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE(result2.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }
}

TEST_CASE (



"Setu - Overflow-safe bounds checking - page_view operations"
,
"[setu][overflow][page]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(8192);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    setu::page_view<4096> page(region, 0);
    REQUIRE(page.is_valid());

    SECTION("Page read_at with overflow values")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // page_offset + sizeof(T) would overflow or exceed page
        auto result1 = page.read_at<int>(max_val - 2);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        auto result2 = page.read_at<long long>(4090); // Near end of page, won't fit
        REQUIRE_FALSE(result2.has_value());
    }

    SECTION("Page write_at with large offset")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        auto result1 = page.write_at<int>(max_val / 2);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Page read_array with overflow in count * sizeof(T)")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // count * sizeof(int) would overflow
        auto result1 = page.read_array<int>(0, max_val / 2);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Valid count but exceeds page
        auto result2 = page.read_array<int>(0, 2000);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE(result2.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Page write_array with overflow values")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // offset + (count * sizeof(T)) would overflow
        auto result1 = page.write_array<int>(4000, max_val / 3);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Page subrange with overflow values")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // page_offset + length would overflow
        auto result1 = page.subrange(4000, max_val);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Wrapping arithmetic
        auto result2 = page.subrange(max_val - 100, 200);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE(result2.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }
}

TEST_CASE (



"Setu - Overflow-safe bounds checking - offset_ptr operations"
,
"[setu][overflow][offset_ptr]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    SECTION("offset_ptr try_get with overflow-prone offsets")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // Negative offset
        setu::offset_ptr<int> ptr1(std::ptrdiff_t(-100));
        auto result1 = ptr1.try_get(region);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Offset beyond region
        setu::offset_ptr<int> ptr2(std::ptrdiff_t(5000));
        auto result2 = ptr2.try_get(region);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE(result2.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Offset + sizeof(T) would overflow (when converted to size_t)
        // Using very large positive ptrdiff_t
        if constexpr (sizeof(std::ptrdiff_t) == sizeof(size_t))
        {
            setu::offset_ptr<int> ptr3(std::numeric_limits<std::ptrdiff_t>::max());
            auto result3 = ptr3.try_get(region);
            REQUIRE_FALSE(result3.has_value());
            REQUIRE(result3.error() == setu::make_error_code(setu::error_code::out_of_bounds));
        }
    }

    SECTION("offset_ptr try_get_mut with overflow values")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // Near end of region, type won't fit
        setu::offset_ptr<long long> ptr1(std::ptrdiff_t(4090));
        auto result1 = ptr1.try_get_mut(region);
        REQUIRE_FALSE(result1.has_value());
        REQUIRE(result1.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Way beyond region
        setu::offset_ptr<int> ptr2(std::ptrdiff_t(max_val / 2));
        auto result2 = ptr2.try_get_mut(region);
        REQUIRE_FALSE(result2.has_value());
        REQUIRE(result2.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("offset_ptr with valid edge case offsets")
    {
        // Valid offset at the very end (if type fits)
        setu::offset_ptr<int> ptr(std::ptrdiff_t(4092)); // 4096 - 4 = valid
        auto result = ptr.try_get(region);
        REQUIRE(result.has_value());

        // Valid offset at start
        setu::offset_ptr<long long> ptr2(std::ptrdiff_t(0));
        auto result2 = ptr2.try_get(region);
        REQUIRE(result2.has_value());
    }
}

TEST_CASE (



"Setu - Overflow-safe bounds checking - Large array stress test"
,
"[setu][overflow][stress]"
)
 {
    // Create reasonably large mapping for stress testing
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(1024 * 1024); // 1MB
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    SECTION("Large but valid array operations")
    {
        // Valid large array
        const size_t count = 100000; // 100k ints = 400KB
        auto result1 = region.read_array<int>(0, count);
        REQUIRE(result1.has_value());
        REQUIRE(result1->size() == count);

        // Valid array near end
        const size_t offset = 1024 * 1024 - 1000 * sizeof(int);
        auto result2 = region.read_array<int>(offset, 1000);
        REQUIRE(result2.has_value());
        REQUIRE(result2->size() == 1000);
    }

    SECTION("Arrays that exceed bounds by 1 byte")
    {
        // Array that would fit except for 1 byte
        const size_t total_size = 1024 * 1024;
        const size_t count = total_size / sizeof(int) + 1; // One too many

        auto result = region.read_array<int>(0, count);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Arrays with offset + count causing overflow")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // Large count that would overflow when multiplied
        auto result1 = region.read_array<long long>(0, max_val / 7);
        REQUIRE_FALSE(result1.has_value());

        // Large offset + valid count exceeding bounds
        auto result2 = region.read_array<int>(1024 * 1000, 100000);
        REQUIRE_FALSE(result2.has_value());
    }
}

TEST_CASE (



"Setu - Overflow-safe bounds checking - Edge case combinations"
,
"[setu][overflow][edge]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Subregion of subregion with overflow potential")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // Create valid subregion
        auto region1_result = map.subregion(1000, 8000);
        REQUIRE(region1_result.has_value());

        // Try to create subregion with overflow-prone values
        auto region2_result = region1_result->subregion(7000, max_val);
        REQUIRE_FALSE(region2_result.has_value());
        REQUIRE(region2_result.error() == setu::make_error_code(setu::error_code::out_of_bounds));

        // Valid nested subregion
        auto region3_result = region1_result->subregion(1000, 2000);
        REQUIRE(region3_result.has_value());
    }

    SECTION("Operations on maximum-sized types")
    {
        auto region = map.full_region();

        // Array of very large structs
        struct LargeStruct {
            std::array<uint64_t, 128> data; // 1024 bytes
        };

        // Valid array
        auto result1 = region.read_array<LargeStruct>(0, 10);
        REQUIRE(result1.has_value());

        // Count that would cause overflow
        const size_t max_val = std::numeric_limits<size_t>::max();
        auto result2 = region.read_array<LargeStruct>(0, max_val / 500);
        REQUIRE_FALSE(result2.has_value());
    }

    SECTION("Zero-size operations with overflow-prone offsets")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();
        auto region = map.full_region();

        // Zero-count array at large offset (should fail on offset check)
        auto result1 = region.read_array<int>(max_val / 2, 0);
        REQUIRE_FALSE(result1.has_value());

        // Zero-count array at valid offset (should succeed)
        auto result2 = region.read_array<int>(100, 0);
        REQUIRE(result2.has_value());
        REQUIRE(result2->size() == 0);
    }
}

TEST_CASE (



"Setu - Overflow-safe bounds checking - Realistic scenarios"
,
"[setu][overflow][realistic]"
)
 {
    const char* test_path = "/tmp/setu_test_overflow_realistic.dat";

    SECTION("Large file with operations near SIZE_MAX")
    {
        // Simulate operations on a conceptually large file
        // We'll use a small file but test with SIZE_MAX-like values
        auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, 4096);
        REQUIRE(map_result.has_value());
        auto& map = *map_result;

        const size_t max_val = std::numeric_limits<size_t>::max();

        // Try to create region at impossible offset
        auto result1 = map.subregion(max_val - 1000, 500);
        REQUIRE_FALSE(result1.has_value());

        // Try to flush at impossible offset
        auto result2 = map.flush_range(max_val - 100, 50, setu::flush_mode::sync);
        REQUIRE_FALSE(result2.has_value());

        std::remove(test_path);
    }

    SECTION("Database-style page operations with overflow potential")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(32768);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        struct DBRecord {
            uint64_t id;
            uint64_t timestamp;
            std::array<char, 128> data;
        };

        const size_t max_val = std::numeric_limits<size_t>::max();

        // Try to read unreasonable number of records
        auto result1 = region.read_array<DBRecord>(0, max_val / 100);
        REQUIRE_FALSE(result1.has_value());

        // Valid record access
        auto result2 = region.write_array<DBRecord>(0, 10);
        REQUIRE(result2.has_value());

        // Try to access records at overflow-prone offset
        auto result3 = region.read_array<DBRecord>(max_val - 1000, 5);
        REQUIRE_FALSE(result3.has_value());
    }

    SECTION("Chained operations with overflow accumulation risk")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(8192);
        REQUIRE(map_result.has_value());

        // Create multiple nested subregions, each time risking overflow
        auto region1 = map_result->full_region();
        auto region2_result = region1.subregion(1000, 6000);
        REQUIRE(region2_result.has_value());

        auto region3_result = region2_result->subregion(1000, 4000);
        REQUIRE(region3_result.has_value());

        // Now try overflow-prone operation on deeply nested region
        const size_t max_val = std::numeric_limits<size_t>::max();
        auto region4_result = region3_result->subregion(3000, max_val);
        REQUIRE_FALSE(region4_result.has_value());

        // Valid operation on nested region
        auto region5_result = region3_result->subregion(500, 1000);
        REQUIRE(region5_result.has_value());
    }
}

TEST_CASE (



"Setu - Flush operations"
,
"[setu][flush]"
)
 {
    const char* test_path = "/tmp/setu_test_flush.dat";

    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, 8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Write some data
    auto bytes = map.as_bytes();
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = std::byte{static_cast<uint8_t>(i & 0xFF)};
    }

    SECTION("Async flush")
    {
        auto result = map.flush(setu::flush_mode::async);
        REQUIRE(result.has_value());
    }

    SECTION("Sync flush")
    {
        auto result = map.flush(setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Range flush")
    {
        auto result = map.flush_range(0, 4096, setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Invalid range flush")
    {
        auto result = map.flush_range(8000, 1000, setu::flush_mode::sync);
        REQUIRE(!result.has_value());
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - File resize operations"
,
"[setu][resize]"
)
 {
    const char* test_path = "/tmp/setu_test_resize.dat";

    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, 4096);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Resize file larger")
    {
        auto result = map.resize_file(8192);
        REQUIRE(result.has_value());
    }

    SECTION("Resize and remap")
    {
        auto result = map.resize_and_remap(16384);
        REQUIRE(result.has_value());
        REQUIRE(map.size() == 16384);
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Remap operations"
,
"[setu][remap]"
)
 {
    const char* test_path = "/tmp/setu_test_remap.dat";

    // Create a larger file
    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, 16384);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Write data to first part
    auto bytes = map.as_bytes();
    for (size_t i = 0; i < 4096; ++i)
    {
        bytes[i] = std::byte{42};
    }
    auto flush_result = map.flush(setu::flush_mode::sync);
    REQUIRE(flush_result.has_value());

    SECTION("Remap to different offset")
    {
        auto result = map.remap(4096, 4096);
        REQUIRE(result.has_value());
        REQUIRE(map.size() == 4096);

        // Data at new offset should be different
        auto new_bytes = map.as_bytes();
        bool all_same = true;
        for (size_t i = 0; i < 100; ++i)
        {
            if (new_bytes[i] != std::byte{42})
            {
                all_same = false;
                break;
            }
        }
        // The remapped region likely hasn't been written yet
        REQUIRE_FALSE(all_same);
    }

    SECTION("Remap with options")
    {
        setu::remap_options opts;
        opts.new_offset = 0;
        opts.new_length = 8192;

        auto result = map.remap_with_options(opts);
        REQUIRE(result.has_value());
        REQUIRE(map.size() == 8192);
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Page math utilities"
,
"[setu][page_math]"
)
 {
    const size_t page_size = 4096;

    SECTION("Page count calculation")
    {
        REQUIRE(setu::page::page_count(0, page_size) == 0);
        REQUIRE(setu::page::page_count(1, page_size) == 1);
        REQUIRE(setu::page::page_count(4096, page_size) == 1);
        REQUIRE(setu::page::page_count(4097, page_size) == 2);
        REQUIRE(setu::page::page_count(8192, page_size) == 2);
    }

    SECTION("Page index and offset")
    {
        REQUIRE(setu::page::page_index(0, page_size) == 0);
        REQUIRE(setu::page::page_index(4096, page_size) == 1);
        REQUIRE(setu::page::page_index(8192, page_size) == 2);

        REQUIRE(setu::page::page_offset(0, page_size) == 0);
        REQUIRE(setu::page::page_offset(1, page_size) == 4096);
        REQUIRE(setu::page::page_offset(2, page_size) == 8192);
    }

    SECTION("Page alignment checks")
    {
        REQUIRE(setu::page::is_page_aligned(0, page_size));
        REQUIRE(setu::page::is_page_aligned(4096, page_size));
        REQUIRE_FALSE(setu::page::is_page_aligned(100, page_size));

        REQUIRE(setu::page::align_to_page(0, page_size) == 0);
        REQUIRE(setu::page::align_to_page(100, page_size) == 4096);
        REQUIRE(setu::page::align_to_page(4096, page_size) == 4096);

        REQUIRE(setu::page::align_to_page_down(0, page_size) == 0);
        REQUIRE(setu::page::align_to_page_down(100, page_size) == 0);
        REQUIRE(setu::page::align_to_page_down(4096, page_size) == 4096);
        REQUIRE(setu::page::align_to_page_down(5000, page_size) == 4096);
    }

    SECTION("Page count for region")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(12288);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        REQUIRE(setu::page::page_count(region, page_size) == 3);
    }
}


TEST_CASE (



"Setu - Copy-on-write mapping"
,
"[setu][cow]"
)
 {
    const char* test_path = "/tmp/setu_test_cow.dat";

    // Create a file with data
    {
        std::ofstream ofs(test_path, std::ios::binary);
        for (int i = 0; i < 1024; ++i)
        {
            ofs.put(static_cast<char>(i & 0xFF));
        }
    }

    SECTION("COW mapping allows writes without modifying file")
    {
        auto map_result = setu::mapping<setu::copy_on_write>::open_existing(test_path);
        REQUIRE(map_result.has_value());
        auto& map = *map_result;

        REQUIRE(map.is_valid());
        REQUIRE(map.writable());

        // Modify in COW mapping
        auto bytes = map.as_bytes();
        bytes[0] = std::byte{255};
        REQUIRE(bytes[0] == std::byte{255});

        // Original file should be unchanged (verify by re-reading)
        std::ifstream ifs(test_path, std::ios::binary);
        char first_byte;
        ifs.get(first_byte);
        REQUIRE(static_cast<uint8_t>(first_byte) == 0);
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Multiple page sizes"
,
"[setu][page]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(32768);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    SECTION("4KB pages")
    {
        auto pages = setu::pages<4096>(region);
        REQUIRE(pages.size() == 8);
    }

    SECTION("8KB pages")
    {
        auto pages = setu::pages<8192>(region);
        REQUIRE(pages.size() == 4);
    }

    SECTION("Custom page size")
    {
        auto pages = setu::pages<1024>(region);
        REQUIRE(pages.size() == 32);
    }
}

TEST_CASE (



"Setu - Complex typed structures"
,
"[setu][typed]"
)
 {
    struct ComplexHeader {
        uint64_t magic;
        uint32_t version;
        uint32_t flags;
        uint64_t data_offset;
        uint64_t data_size;
        std::array<uint8_t, 16> uuid;
    };

    struct Record {
        uint64_t id;
        uint64_t timestamp;
        double value;
        std::array<char, 32> name;
    };

    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(8192);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    SECTION("Write complex header and records")
    {
        // Write header
        auto header_result = region.write_at<ComplexHeader>(0);
        REQUIRE(header_result.has_value());

        auto* header = *header_result;
        header->magic = 0xDEADBEEFCAFEBABE;
        header->version = 1;
        header->flags = 0x0001;
        header->data_offset = sizeof(ComplexHeader);
        header->data_size = sizeof(Record) * 10;

        // Write records after header
        size_t record_offset = sizeof(ComplexHeader);
        auto records_result = region.write_array<Record>(record_offset, 10);
        REQUIRE(records_result.has_value());

        auto records = *records_result;
        for (size_t i = 0; i < 10; ++i)
        {
            records[i].id = i;
            records[i].timestamp = 1000 + i;
            records[i].value = i * 1.5;
            std::snprintf(records[i].name.data(), 32, "Record_%zu", i);
        }

        // Verify header
        auto read_header = region.read_at<ComplexHeader>(0);
        REQUIRE(read_header.has_value());
        REQUIRE((*read_header)->magic == 0xDEADBEEFCAFEBABE);
        REQUIRE((*read_header)->version == 1);

        // Verify records
        auto read_records = region.read_array<Record>(record_offset, 10);
        REQUIRE(read_records.has_value());

        for (size_t i = 0; i < 10; ++i)
        {
            const auto& rec = (*read_records)[i];
            REQUIRE(rec.id == i);
            REQUIRE(rec.timestamp == 1000 + i);
            REQUIRE(rec.value == i * 1.5);
        }
    }
}

TEST_CASE (



"Setu - Offset pointer edge cases"
,
"[setu][offset_ptr]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    SECTION("Null pointer comparisons")
    {
        setu::offset_ptr<int> ptr1(nullptr);
        setu::offset_ptr<int> ptr2(nullptr);
        setu::offset_ptr<int> ptr3(std::ptrdiff_t(100));

        REQUIRE(ptr1 == ptr2);
        REQUIRE(ptr1 == nullptr);
        REQUIRE(ptr2 == nullptr);
        REQUIRE(ptr3 != nullptr);
        REQUIRE(ptr1 != ptr3);
    }

    SECTION("Out of bounds check")
    {
        setu::offset_ptr<int> ptr(std::ptrdiff_t(4096)); // Beyond region
        auto result = ptr.try_get(region);
        REQUIRE(!result.has_value());
        REQUIRE(result.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Negative offset")
    {
        setu::offset_ptr<int> ptr(std::ptrdiff_t(-10));
        auto result = ptr.try_get(region);
        REQUIRE(!result.has_value());
    }
}

TEST_CASE (



"Setu - Page-aligned flush operations"
,
"[setu][flush][page-aligned]"
)
 {
    const char* test_path = "/tmp/setu_test_flush_aligned.dat";
    const size_t page_size = setu::platform::page_size();
    const size_t file_size = page_size * 4; // 4 pages

    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, file_size);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Write test data
    auto bytes = map.as_bytes();
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = std::byte{static_cast<uint8_t>(i & 0xFF)};
    }

    SECTION("Flush single page-aligned range")
    {
        // Flush exactly one page starting at page boundary
        auto result = map.flush_range(page_size, page_size, setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Flush unaligned range at page start")
    {
        // Request flush starting at byte 1, should extend down to page 0
        auto result = map.flush_range(1, 100, setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Flush unaligned range spanning pages")
    {
        // Request flush from middle of page 1 to middle of page 2
        // Should flush both complete pages
        size_t offset = page_size + 100;
        size_t length = page_size - 50;
        auto result = map.flush_range(offset, length, setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Flush small range at page boundary")
    {
        // Tiny flush at exact page boundary
        auto result = map.flush_range(page_size * 2, 16, setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Flush crossing multiple pages")
    {
        // Flush from byte 10 spanning 3 pages
        auto result = map.flush_range(10, page_size * 3, setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Flush at end of mapping - unaligned")
    {
        // Flush last few bytes, should align to include full last page
        size_t offset = file_size - 100;
        size_t length = 100;
        auto result = map.flush_range(offset, length, setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Flush zero-length range")
    {
        // Zero-length flush should succeed without operation
        auto result = map.flush_range(page_size, 0, setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Flush with async mode")
    {
        // Unaligned async flush
        auto result = map.flush_range(50, 200, setu::flush_mode::async);
        REQUIRE(result.has_value());
    }

    SECTION("Flush single byte - triggers full page flush")
    {
        // Flushing a single byte should flush the entire page it belongs to
        for (size_t page_idx = 0; page_idx < 4; ++page_idx)
        {
            size_t byte_offset = page_idx * page_size + page_size / 2;
            auto result = map.flush_range(byte_offset, 1, setu::flush_mode::sync);
            REQUIRE(result.has_value());
        }
    }

    SECTION("Flush boundary between pages")
    {
        // Flush exactly at the boundary between two pages
        auto result = map.flush_range(page_size - 10, 20, setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Flush entire mapping via range")
    {
        // Flush entire mapping using flush_range
        auto result = map.flush_range(0, file_size, setu::flush_mode::sync);
        REQUIRE(result.has_value());
    }

    SECTION("Verify out of bounds flush fails")
    {
        auto result = map.flush_range(file_size - 100, 200, setu::flush_mode::sync);
        REQUIRE(!result.has_value());
        REQUIRE(result.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Verify overflow protection")
    {
        // Try to flush beyond file bounds
        // Using a reasonable large value that would go beyond file size
        auto result = map.flush_range(file_size - 10, file_size, setu::flush_mode::sync);
        // Should fail as it extends beyond the file
        REQUIRE(!result.has_value());
        REQUIRE(result.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Page-aligned advise operations"
,
"[setu][advice][page-aligned]"
)
 {
    const char* test_path = "/tmp/setu_test_advise_aligned.dat";
    const size_t page_size = setu::platform::page_size();
    const size_t file_size = page_size * 8; // 8 pages for more testing room

    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, file_size);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Sequential advice on unaligned range")
    {
        // Advise from byte 100 for 2 pages
        auto result = map.advise_range(100, page_size * 2, setu::advice_mode::sequential);
        REQUIRE(result.has_value());
    }

    SECTION("Random advice on page-aligned range")
    {
        auto result = map.advise_range(page_size * 2, page_size * 3, setu::advice_mode::random);
        REQUIRE(result.has_value());
    }

    SECTION("Will-need advice for small unaligned region")
    {
        // Advise 50 bytes starting at offset 50
        auto result = map.advise_range(50, 50, setu::advice_mode::will_need);
        REQUIRE(result.has_value());
    }

    SECTION("Dont-need advice for large region")
    {
        // Mark middle section as not needed
        auto result = map.advise_range(page_size, page_size * 4, setu::advice_mode::dont_need);
        REQUIRE(result.has_value());
    }

    SECTION("Normal advice on entire mapping via range")
    {
        auto result = map.advise_range(0, file_size, setu::advice_mode::normal);
        REQUIRE(result.has_value());
    }

    SECTION("Advise single byte in each page")
    {
        // Advise one byte from each page - each should extend to full page
        for (size_t page_idx = 0; page_idx < 8; ++page_idx)
        {
            size_t offset = page_idx * page_size + 1;
            auto result = map.advise_range(offset, 1, setu::advice_mode::will_need);
            REQUIRE(result.has_value());
        }
    }

    SECTION("Advise range crossing page boundary")
    {
        // Cross from last byte of page 2 to first byte of page 3
        size_t offset = page_size * 3 - 1;
        auto result = map.advise_range(offset, 2, setu::advice_mode::sequential);
        REQUIRE(result.has_value());
    }

    SECTION("Zero-length advice range")
    {
        auto result = map.advise_range(page_size * 2, 0, setu::advice_mode::random);
        REQUIRE(result.has_value());
    }

    SECTION("Multiple advice modes on same region")
    {
        // Apply different advice to same region
        size_t offset = page_size;
        size_t length = page_size * 2;

        REQUIRE(map.advise_range(offset, length, setu::advice_mode::sequential).has_value());
        REQUIRE(map.advise_range(offset, length, setu::advice_mode::will_need).has_value());
        REQUIRE(map.advise_range(offset, length, setu::advice_mode::random).has_value());
        REQUIRE(map.advise_range(offset, length, setu::advice_mode::normal).has_value());
    }

    SECTION("Advise at file end - unaligned")
    {
        size_t offset = file_size - 50;
        auto result = map.advise_range(offset, 50, setu::advice_mode::dont_need);
        REQUIRE(result.has_value());
    }

    SECTION("Verify out of bounds advice fails")
    {
        auto result = map.advise_range(file_size - 100, 200, setu::advice_mode::sequential);
        REQUIRE(!result.has_value());
        REQUIRE(result.error() == setu::make_error_code(setu::error_code::out_of_bounds));
    }

    SECTION("Advise with offset at page boundary")
    {
        for (size_t page_idx = 0; page_idx < 8; ++page_idx)
        {
            size_t offset = page_idx * page_size;
            auto result = map.advise_range(offset, page_size / 2, setu::advice_mode::sequential);
            REQUIRE(result.has_value());
        }
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Page alignment edge cases"
,
"[setu][page-aligned][edge-cases]"
)
 {
    const size_t page_size = setu::platform::page_size();
    const char* test_path = "/tmp/setu_test_alignment_edge.dat";

    SECTION("Very small file with unaligned operations")
    {
        // File smaller than a page
        size_t small_size = page_size / 4;
        auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, small_size);
        REQUIRE(map_result.has_value());
        auto& map = *map_result;

        // Flush unaligned range in small file
        auto flush_result = map.flush_range(10, 50, setu::flush_mode::sync);
        REQUIRE(flush_result.has_value());

        // Advise unaligned range in small file
        auto advise_result = map.advise_range(20, 30, setu::advice_mode::sequential);
        REQUIRE(advise_result.has_value());

        std::remove(test_path);
    }

    SECTION("File not multiple of page size with operations at end")
    {
        // Create file that's not a multiple of page size
        size_t odd_size = page_size * 2 + page_size / 3;
        auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, odd_size);
        REQUIRE(map_result.has_value());
        auto& map = *map_result;

        // Flush near the end
        size_t offset = odd_size - 100;
        auto flush_result = map.flush_range(offset, 50, setu::flush_mode::sync);
        REQUIRE(flush_result.has_value());

        // Flush the very last byte
        flush_result = map.flush_range(odd_size - 1, 1, setu::flush_mode::sync);
        REQUIRE(flush_result.has_value());

        // Advise near the end
        auto advise_result = map.advise_range(offset, 50, setu::advice_mode::dont_need);
        REQUIRE(advise_result.has_value());

        std::remove(test_path);
    }

    SECTION("Alternating aligned and unaligned operations")
    {
        size_t file_size = page_size * 4;
        auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, file_size);
        REQUIRE(map_result.has_value());
        auto& map = *map_result;

        // Mix of aligned and unaligned flush operations
        REQUIRE(map.flush_range(0, page_size, setu::flush_mode::sync).has_value());
        REQUIRE(map.flush_range(1, page_size - 2, setu::flush_mode::sync).has_value());
        REQUIRE(map.flush_range(page_size * 2, page_size, setu::flush_mode::sync).has_value());
        REQUIRE(map.flush_range(page_size * 2 + 1, page_size - 2, setu::flush_mode::sync).has_value());

        // Mix of aligned and unaligned advise operations
        REQUIRE(map.advise_range(0, page_size, setu::advice_mode::sequential).has_value());
        REQUIRE(map.advise_range(1, page_size - 2, setu::advice_mode::random).has_value());
        REQUIRE(map.advise_range(page_size * 3, page_size, setu::advice_mode::will_need).has_value());
        REQUIRE(map.advise_range(page_size * 3 + 1, page_size - 2, setu::advice_mode::dont_need).has_value());

        std::remove(test_path);
    }

    SECTION("Operations on anonymous mappings")
    {
        // Anonymous mappings should also handle page alignment correctly
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(page_size * 4);
        REQUIRE(map_result.has_value());
        auto& map = *map_result;

        // Unaligned flush on anonymous mapping may not be supported (no backing file)
        // Just verify it doesn't crash - result may vary by platform
        auto flush_result = map.flush_range(100, page_size, setu::flush_mode::sync);
        // Don't require success for anonymous mappings

        // Unaligned advise on anonymous mapping should work
        auto advise_result = map.advise_range(200, page_size * 2, setu::advice_mode::sequential);
        REQUIRE(advise_result.has_value());
    }

    SECTION("Extremely small ranges requiring alignment")
    {
        size_t file_size = page_size * 2;
        auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, file_size);
        REQUIRE(map_result.has_value());
        auto& map = *map_result;

        // Flush/advise just 1 byte at various positions
        std::array<size_t, 7> positions = {
            size_t(0), size_t(1), page_size / 2, page_size - 1,
            page_size, page_size + 1, file_size - 1
        };
        for (size_t pos : positions)
        {
            REQUIRE(map.flush_range(pos, 1, setu::flush_mode::sync).has_value());
            REQUIRE(map.advise_range(pos, 1, setu::advice_mode::sequential).has_value());
        }

        std::remove(test_path);
    }
}

TEST_CASE (



"Setu - Page alignment stress tests"
,
"[setu][page-aligned][stress]"
)
 {
    const size_t page_size = setu::platform::page_size();
    const char* test_path = "/tmp/setu_test_alignment_stress.dat";
    const size_t file_size = page_size * 16; // Larger file for stress testing

    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, file_size);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Sequential unaligned flushes across entire file")
    {
        // Flush overlapping unaligned ranges
        for (size_t offset = 0; offset < file_size - page_size; offset += page_size / 2)
        {
            auto result = map.flush_range(offset + 1, page_size / 2, setu::flush_mode::async);
            REQUIRE(result.has_value());
        }
    }

    SECTION("Random pattern of advice operations")
    {
        // Apply different advice patterns to random unaligned regions
        std::array<setu::advice_mode, 5> modes = {
            setu::advice_mode::normal,
            setu::advice_mode::sequential,
            setu::advice_mode::random,
            setu::advice_mode::will_need,
            setu::advice_mode::dont_need
        };

        for (size_t i = 0; i < 100; ++i)
        {
            size_t offset = (i * 317) % (file_size - page_size); // Pseudo-random
            size_t length = page_size / 4 + (i * 37) % (page_size / 2);
            auto mode = modes[i % modes.size()];

            auto result = map.advise_range(offset, length, mode);
            REQUIRE(result.has_value());
        }
    }

    SECTION("Interleaved flush and advise operations")
    {
        for (size_t i = 0; i < 50; ++i)
        {
            size_t offset = (i * page_size / 3) % (file_size - page_size);
            size_t length = page_size / 2;

            // Alternate between flush and advise
            if (i % 2 == 0)
            {
                REQUIRE(map.flush_range(offset, length, setu::flush_mode::async).has_value());
            }
            else
            {
                REQUIRE(map.advise_range(offset, length, setu::advice_mode::sequential).has_value());
            }
        }
    }

    SECTION("Page boundary spanning operations")
    {
        // Test operations that span exactly N pages with unaligned start
        for (size_t num_pages = 1; num_pages <= 4; ++num_pages)
        {
            size_t offset = page_size * 2 + 7; // Deliberately unaligned
            size_t length = page_size * num_pages - 14; // Unaligned length too

            REQUIRE(map.flush_range(offset, length, setu::flush_mode::sync).has_value());
            REQUIRE(map.advise_range(offset, length, setu::advice_mode::random).has_value());
        }
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Combined page operations with regions"
,
"[setu][page-aligned][regions]"
)
 {
    const size_t page_size = setu::platform::page_size();
    const char* test_path = "/tmp/setu_test_region_alignment.dat";
    const size_t file_size = page_size * 8;

    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, file_size);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Flush operations on subregions")
    {
        // Create a subregion and perform operations on it
        auto region_result = map.subregion(page_size + 100, page_size * 3);
        REQUIRE(region_result.has_value());
        auto& region = *region_result;

        // The region itself starts at unaligned offset
        // Operations within the region should still work correctly
        auto bytes = region.as_bytes();
        std::memset(bytes.data(), 0xAB, bytes.size());

        // These operations work on the region, which has an unaligned base
        auto flush_result = map.flush_range(page_size + 100, page_size, setu::flush_mode::sync);
        REQUIRE(flush_result.has_value());
    }

    SECTION("Page views with flush operations")
    {
        // Use page views and perform flush on their ranges
        for (size_t page_idx = 0; page_idx < 8; ++page_idx)
        {
            auto region = map.full_region();
            setu::page_view<4096> page(region, page_idx);

            // Write to page
            auto bytes = page.page_bytes();
            bytes[0] = std::byte{static_cast<uint8_t>(page_idx)};

            // Flush the page's range (unaligned within the page)
            size_t page_offset = page_idx * page_size;
            auto result = map.flush_range(page_offset + 10, 100, setu::flush_mode::sync);
            REQUIRE(result.has_value());
        }
    }

    std::remove(test_path);
}


#ifndef NDEBUG

TEST_CASE (



"Setu - Generation tracking - generation counter increments"
,
"[setu][generation][debug]"
)
 {
    const char* test_file = "/tmp/setu_test_generation_counter.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Initial generation should be valid
    auto initial_gen = map.current_generation();
    REQUIRE(initial_gen > 0);

    // Create region with current generation
    auto region1 = map.full_region();

    // Remap should increment generation
    auto remap_result = map.remap(0, 8192);
    REQUIRE(remap_result);
    auto gen_after_remap = map.current_generation();
    REQUIRE(gen_after_remap == initial_gen + 1);

    // Resize should increment generation
    auto resize_result = map.resize_file(16384);
    REQUIRE(resize_result);
    auto gen_after_resize = map.current_generation();
    REQUIRE(gen_after_resize == gen_after_remap + 1);

    // Resize and remap should increment generation
    auto resize_remap_result = map.resize_and_remap(8192);
    REQUIRE(resize_remap_result);
    auto gen_after_both = map.current_generation();
    REQUIRE(gen_after_both == gen_after_resize + 1);

    std::remove(test_file);
}

TEST_CASE (



"Setu - Generation tracking - multiple remaps"
,
"[setu][generation][debug]"
)
 {
    const char* test_file = "/tmp/setu_test_generation_multiple_remaps.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 16384);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Track generations through multiple operations
    std::vector<uint64_t> generations;
    generations.push_back(map.current_generation());

    // Perform multiple remaps
    for (size_t i = 0; i < 5; ++i)
    {
        auto remap_result = map.remap(0, 8192);
        REQUIRE(remap_result);
        generations.push_back(map.current_generation());
    }

    // Verify each operation incremented generation
    for (size_t i = 1; i < generations.size(); ++i)
    {
        REQUIRE(generations[i] == generations[i-1] + 1);
    }

    std::remove(test_file);
}

TEST_CASE (



"Setu - Generation tracking - anonymous mapping"
,
"[setu][generation][debug]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    auto initial_gen = map.current_generation();
    auto region1 = map.full_region();

    // Remap anonymous mapping
    auto remap_result = map.remap(0, 8192);
    REQUIRE(remap_result);

    auto gen_after = map.current_generation();
    REQUIRE(gen_after == initial_gen + 1);

    // New region should have new generation
    auto region2 = map.full_region();
    REQUIRE(region2.is_valid());
}

TEST_CASE (



"Setu - Generation tracking - nested subregions"
,
"[setu][generation][debug]"
)
 {
    const char* test_file = "/tmp/setu_test_generation_nested.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 16384);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Create nested subregions
    auto region1 = map.full_region();
    auto subregion1_result = region1.subregion(1024, 8192);
    REQUIRE(subregion1_result);
    auto subregion2_result = subregion1_result->subregion(512, 4096);
    REQUIRE(subregion2_result);
    auto subregion3_result = subregion2_result->subregion(256, 2048);
    REQUIRE(subregion3_result);

    // Write through deepest subregion
    auto bytes = subregion3_result->as_bytes();
    bytes[0] = std::byte{123};

    // Remap invalidates all nested views
    auto remap_result = map.remap(0, 16384);
    REQUIRE(remap_result);

    // Create new nested structure and verify data
    auto region2 = map.full_region();
    auto new_sub1 = region2.subregion(1024, 8192);
    REQUIRE(new_sub1);
    auto new_sub2 = new_sub1->subregion(512, 4096);
    REQUIRE(new_sub2);
    auto new_sub3 = new_sub2->subregion(256, 2048);
    REQUIRE(new_sub3);

    auto new_bytes = new_sub3->as_bytes();
    REQUIRE(new_bytes[0] == std::byte{123});

    std::remove(test_file);
}

TEST_CASE (



"Setu - Generation tracking - page iteration"
,
"[setu][generation][debug]"
)
 {
    const char* test_file = "/tmp/setu_test_generation_page_iter.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 16384);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Create page views through iteration
    auto region1 = map.full_region();
    auto pages1 = setu::pages<4096>(region1);

    // Write to each page
    size_t page_idx = 0;
    for (auto page : pages1)
    {
        auto bytes = page.page_bytes();
        bytes[0] = std::byte{static_cast<uint8_t>(page_idx++)};
    }

    // Remap invalidates all page views
    auto remap_result = map.remap(0, 16384);
    REQUIRE(remap_result);

    // New iteration should work and data should persist
    auto region2 = map.full_region();
    auto pages2 = setu::pages<4096>(region2);

    page_idx = 0;
    for (auto page : pages2)
    {
        auto bytes = page.page_bytes();
        REQUIRE(bytes[0] == std::byte{static_cast<uint8_t>(page_idx++)});
    }

    std::remove(test_file);
}

TEST_CASE (



"Setu - Generation tracking - mixed operations"
,
"[setu][generation][debug]"
)
 {
    const char* test_file = "/tmp/setu_test_generation_mixed.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    auto gen1 = map.current_generation();

    // Mix of operations that increment generation
    auto resize_result = map.resize_file(16384);
    REQUIRE(resize_result);
    auto gen2 = map.current_generation();
    REQUIRE(gen2 == gen1 + 1);

    auto remap_result = map.remap(4096, 8192);
    REQUIRE(remap_result);
    auto gen3 = map.current_generation();
    REQUIRE(gen3 == gen2 + 1);

    auto resize_remap_result = map.resize_and_remap(12288);
    REQUIRE(resize_remap_result);
    auto gen4 = map.current_generation();
    REQUIRE(gen4 == gen3 + 1);

    // Operations that don't increment generation
    auto flush_result = map.flush(setu::flush_mode::sync);
    REQUIRE(flush_result);
    REQUIRE(map.current_generation() == gen4); // No increment

    auto advise_result = map.advise(setu::advice_mode::sequential);
    REQUIRE(advise_result);
    REQUIRE(map.current_generation() == gen4); // No increment

    std::remove(test_file);
}

TEST_CASE (



"Setu - Generation tracking - detect stale region_view"
,
"[setu][generation][debug]"
)
 {
    const char* test_file = "/tmp/setu_test_generation_region.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Create a region view
    auto region1 = map.full_region();
    REQUIRE(region1.is_valid());

    // Write some data
    auto bytes1 = region1.as_bytes();
    bytes1[0] = std::byte{42};

    // Remap - this should invalidate region1
    auto remap_result = map.remap(0, 8192);
    REQUIRE(remap_result);

    // In debug builds, accessing the old region should abort
    // We can't directly test abort(), but we can verify the new region works
    auto region2 = map.full_region();
    REQUIRE(region2.is_valid());

    auto bytes2 = region2.as_bytes();
    // The data should still be there after remap
    REQUIRE(bytes2[0] == std::byte{42});

    std::remove(test_file);

    // Note: In a real debug session, attempting to use region1 after remap
    // would trigger an assertion with a clear error message
}

TEST_CASE (



"Setu - Generation tracking - detect stale page_view"
,
"[setu][generation][debug]"
)
 {
    const char* test_file = "/tmp/setu_test_generation_page.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Create a page view
    auto region1 = map.full_region();
    setu::page_view<4096> page1(region1, 0);
    REQUIRE(page1.is_valid());

    // Write to page
    auto page_bytes1 = page1.page_bytes();
    page_bytes1[0] = std::byte{99};

    // Resize file - this should invalidate page1
    auto resize_result = map.resize_file(16384);
    REQUIRE(resize_result);

    // Create new views from the mapping
    auto region2 = map.full_region();
    setu::page_view<4096> page2(region2, 0);
    REQUIRE(page2.is_valid());

    auto page_bytes2 = page2.page_bytes();
    REQUIRE(page_bytes2[0] == std::byte{99});

    std::remove(test_file);

    // Note: In debug builds, attempting to use page1 after resize
    // would trigger an assertion
}

TEST_CASE (



"Setu - Generation tracking - subregions inherit generation"
,
"[setu][generation][debug]"
)
 {
    const char* test_file = "/tmp/setu_test_generation_subregion.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Create a region and subregion
    auto region1 = map.full_region();
    auto subregion1_result = region1.subregion(1024, 2048);
    REQUIRE(subregion1_result);
    auto subregion1 = *subregion1_result;

    // Write to subregion
    auto bytes1 = subregion1.as_bytes();
    bytes1[0] = std::byte{77};

    // Remap - invalidates both region1 and subregion1
    auto remap_result = map.remap(0, 8192);
    REQUIRE(remap_result);

    // Create new region and verify data
    auto region2 = map.full_region();
    auto subregion2_result = region2.subregion(1024, 2048);
    REQUIRE(subregion2_result);
    auto subregion2 = *subregion2_result;

    auto bytes2 = subregion2.as_bytes();
    REQUIRE(bytes2[0] == std::byte{77});

    std::remove(test_file);

    // Note: Using subregion1 after remap would trigger assertion
}

TEST_CASE (



"Setu - Generation tracking - page subrange inherits generation"
,
"[setu][generation][debug]"
)
 {
    const char* test_file = "/tmp/setu_test_generation_page_subrange.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Create page view and get subrange
    auto region1 = map.full_region();
    setu::page_view<4096> page1(region1, 0);
    auto subrange1_result = page1.subrange(512, 1024);
    REQUIRE(subrange1_result);
    auto subrange1 = *subrange1_result;

    // Write to subrange
    auto bytes1 = subrange1.as_bytes();
    bytes1[0] = std::byte{88};

    // Remap - invalidates page1 and subrange1
    auto remap_result = map.remap(0, 8192);
    REQUIRE(remap_result);

    // Create new views
    auto region2 = map.full_region();
    setu::page_view<4096> page2(region2, 0);
    auto subrange2_result = page2.subrange(512, 1024);
    REQUIRE(subrange2_result);
    auto subrange2 = *subrange2_result;

    auto bytes2 = subrange2.as_bytes();
    REQUIRE(bytes2[0] == std::byte{88});

    std::remove(test_file);
}

TEST_CASE (



"Setu - Generation tracking - resize_and_remap invalidates views"
,
"[setu][generation][debug]"
)
 {
    const char* test_file = "/tmp/setu_test_generation_resize_remap.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 4096);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Create views
    auto region1 = map.full_region();
    auto bytes1 = region1.as_bytes();
    bytes1[0] = std::byte{111};

    // Resize and remap - invalidates region1
    auto resize_remap_result = map.resize_and_remap(8192);
    REQUIRE(resize_remap_result);

    // New region should work
    auto region2 = map.full_region();
    REQUIRE(region2.size() == 8192);

    auto bytes2 = region2.as_bytes();
    REQUIRE(bytes2[0] == std::byte{111});

    std::remove(test_file);
}

#else

TEST_CASE ("Setu - Generation tracking - disabled in release builds", "[setu][generation][release]") {
    // In release builds (NDEBUG defined), generation tracking is compiled out
    // This test just verifies basic functionality still works
    const char* test_file = "/tmp/setu_test_generation_release.dat";

    auto map_result = setu::mapping<setu::read_write>::create_truncate(test_file, 8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    auto region = map.full_region();
    REQUIRE(region.is_valid());

    // Remap
    auto remap_result = map.remap(0, 8192);
    REQUIRE(remap_result);

    // New region still works
    auto region2 = map.full_region();
    REQUIRE(region2.is_valid());

    std::remove(test_file);
}

#endif

// ============================================================================
// Page Convenience Helper Tests
// ============================================================================

TEST_CASE (



"Setu - Page convenience helpers - page_count on region_view"
,
"[setu][page][helpers]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    SECTION("Page count with different page sizes")
    {
        REQUIRE(region.page_count<4096>() == 4);
        REQUIRE(region.page_count<8192>() == 2);
        REQUIRE(region.page_count<16384>() == 1);
        REQUIRE(region.page_count<2048>() == 8);
        REQUIRE(region.page_count<1024>() == 16);
    }

    SECTION("Page count with non-divisible sizes")
    {
        // Region size 16384
        REQUIRE(region.page_count<5000>() == 3); // 3 complete pages
        REQUIRE(region.page_count<10000>() == 1); // 1 complete page
        REQUIRE(region.page_count<20000>() == 0); // 0 complete pages
    }

    SECTION("Page count on subregion")
    {
        auto sub_result = region.subregion(4096, 8192);
        REQUIRE(sub_result.has_value());

        REQUIRE(sub_result->page_count<4096>() == 2);
        REQUIRE(sub_result->page_count<2048>() == 4);
        REQUIRE(sub_result->page_count<8192>() == 1);
    }

    SECTION("Page count with small region")
    {
        auto small_result = region.subregion(0, 100);
        REQUIRE(small_result.has_value());

        REQUIRE(small_result->page_count<4096>() == 0); // No complete 4KB pages
        REQUIRE(small_result->page_count<50>() == 2); // Two 50-byte pages
        REQUIRE(small_result->page_count<100>() == 1); // One 100-byte page
    }
}

TEST_CASE (



"Setu - Page convenience helpers - page_count on mapping"
,
"[setu][page][helpers]"
)
 {
    SECTION("File-backed mapping")
    {
        const char* test_path = "/tmp/setu_test_page_count.dat";
        auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, 12288);
        REQUIRE(map_result.has_value());
        auto& map = *map_result;

        REQUIRE(map.page_count<4096>() == 3);
        REQUIRE(map.page_count<2048>() == 6);
        REQUIRE(map.page_count<8192>() == 1);
        REQUIRE(map.page_count<16384>() == 0);

        std::remove(test_path);
    }

    SECTION("Anonymous mapping")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(20480);
        REQUIRE(map_result.has_value());

        REQUIRE(map_result->page_count<4096>() == 5);
        REQUIRE(map_result->page_count<1024>() == 20);
        REQUIRE(map_result->page_count<10240>() == 2);
    }
}

TEST_CASE (



"Setu - Page convenience helpers - page_at on region_view"
,
"[setu][page][helpers]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    // Write identifiable data to each page
    auto bytes = region.as_bytes();
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = std::byte{static_cast<uint8_t>((i / 4096) + 1)};
    }

    SECTION("Access valid pages")
    {
        auto page0 = region.page_at<4096>(0);
        REQUIRE(page0.is_valid());
        REQUIRE(page0.page_bytes()[0] == std::byte{1});

        auto page1 = region.page_at<4096>(1);
        REQUIRE(page1.is_valid());
        REQUIRE(page1.page_bytes()[0] == std::byte{2});

        auto page2 = region.page_at<4096>(2);
        REQUIRE(page2.is_valid());
        REQUIRE(page2.page_bytes()[0] == std::byte{3});

        auto page3 = region.page_at<4096>(3);
        REQUIRE(page3.is_valid());
        REQUIRE(page3.page_bytes()[0] == std::byte{4});
    }

    SECTION("Access out of bounds page")
    {
        auto invalid_page = region.page_at<4096>(4);
        REQUIRE_FALSE(invalid_page.is_valid());

        auto invalid_page2 = region.page_at<4096>(100);
        REQUIRE_FALSE(invalid_page2.is_valid());
    }

    SECTION("Access with different page sizes")
    {
        auto page_8k_0 = region.page_at<8192>(0);
        REQUIRE(page_8k_0.is_valid());

        auto page_8k_1 = region.page_at<8192>(1);
        REQUIRE(page_8k_1.is_valid());

        auto page_8k_2 = region.page_at<8192>(2);
        REQUIRE_FALSE(page_8k_2.is_valid());
    }

    SECTION("Write through page_at")
    {
        auto page = region.page_at<4096>(1);
        REQUIRE(page.is_valid());

        auto page_bytes = page.page_bytes();
        page_bytes[100] = std::byte{42};

        // Verify through region
        REQUIRE(bytes[4096 + 100] == std::byte{42});
    }
}

TEST_CASE (



"Setu - Page convenience helpers - page_at on mapping"
,
"[setu][page][helpers]"
)
 {
    const char* test_path = "/tmp/setu_test_page_at_mapping.dat";
    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, 8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    // Write test data
    auto bytes = map.as_bytes();
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = std::byte{static_cast<uint8_t>(i % 256)};
    }

    SECTION("Access pages directly from mapping")
    {
        auto page0 = map.page_at<4096>(0);
        REQUIRE(page0.is_valid());
        REQUIRE(page0.page_bytes()[0] == std::byte{0});

        auto page1 = map.page_at<4096>(1);
        REQUIRE(page1.is_valid());
        REQUIRE(page1.page_bytes()[0] == std::byte{0}); // 4096 % 256 = 0
    }

    SECTION("Out of bounds access")
    {
        auto invalid = map.page_at<4096>(2);
        REQUIRE_FALSE(invalid.is_valid());
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Page convenience helpers - page_index_to_offset on region_view"
,
"[setu][page][helpers]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    SECTION("Valid page index conversions")
    {
        auto offset0 = region.page_index_to_offset<4096>(0);
        REQUIRE(offset0.has_value());
        REQUIRE(*offset0 == 0);

        auto offset1 = region.page_index_to_offset<4096>(1);
        REQUIRE(offset1.has_value());
        REQUIRE(*offset1 == 4096);

        auto offset2 = region.page_index_to_offset<4096>(2);
        REQUIRE(offset2.has_value());
        REQUIRE(*offset2 == 8192);

        auto offset3 = region.page_index_to_offset<4096>(3);
        REQUIRE(offset3.has_value());
        REQUIRE(*offset3 == 12288);
    }

    SECTION("Out of bounds page index")
    {
        // Index 4 would give offset 16384, which equals region size (out of bounds)
        auto offset4 = region.page_index_to_offset<4096>(4);
        REQUIRE_FALSE(offset4.has_value());

        auto offset_large = region.page_index_to_offset<4096>(100);
        REQUIRE_FALSE(offset_large.has_value());
    }

    SECTION("Different page sizes")
    {
        auto offset_2k_0 = region.page_index_to_offset<2048>(0);
        REQUIRE(offset_2k_0.has_value());
        REQUIRE(*offset_2k_0 == 0);

        auto offset_2k_7 = region.page_index_to_offset<2048>(7);
        REQUIRE(offset_2k_7.has_value());
        REQUIRE(*offset_2k_7 == 14336);

        auto offset_2k_8 = region.page_index_to_offset<2048>(8);
        REQUIRE_FALSE(offset_2k_8.has_value()); // 8 * 2048 = 16384 = size
    }

    SECTION("Overflow protection")
    {
        const size_t max_val = std::numeric_limits<size_t>::max();

        // Index so large that index * PageSize would overflow
        auto overflow_result = region.page_index_to_offset<4096>(max_val / 2);
        REQUIRE_FALSE(overflow_result.has_value());

        auto overflow_result2 = region.page_index_to_offset<8192>(max_val / 4);
        REQUIRE_FALSE(overflow_result2.has_value());
    }

    SECTION("Edge case - last valid page")
    {
        // Region size is 16384
        // Last page with size 4096 is at index 3 (offset 12288)
        auto last_valid = region.page_index_to_offset<4096>(3);
        REQUIRE(last_valid.has_value());
        REQUIRE(*last_valid == 12288);
        REQUIRE(*last_valid + 4096 == 16384); // Fits exactly

        // Next index would be out of bounds
        auto first_invalid = region.page_index_to_offset<4096>(4);
        REQUIRE_FALSE(first_invalid.has_value());
    }
}

TEST_CASE (



"Setu - Page convenience helpers - page_index_to_offset on mapping"
,
"[setu][page][helpers]"
)
 {
    const char* test_path = "/tmp/setu_test_page_offset_mapping.dat";
    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, 10240);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Valid conversions")
    {
        auto offset0 = map.page_index_to_offset<2048>(0);
        REQUIRE(offset0.has_value());
        REQUIRE(*offset0 == 0);

        auto offset4 = map.page_index_to_offset<2048>(4);
        REQUIRE(offset4.has_value());
        REQUIRE(*offset4 == 8192);
    }

    SECTION("Out of bounds")
    {
        // 5 * 2048 = 10240 = file size (out of bounds)
        auto offset5 = map.page_index_to_offset<2048>(5);
        REQUIRE_FALSE(offset5.has_value());
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Page convenience helpers - is_valid_page_index on region_view"
,
"[setu][page][helpers]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    SECTION("Valid indices")
    {
        REQUIRE(region.is_valid_page_index<4096>(0));
        REQUIRE(region.is_valid_page_index<4096>(1));
        REQUIRE(region.is_valid_page_index<4096>(2));
        REQUIRE(region.is_valid_page_index<4096>(3));
    }

    SECTION("Invalid indices")
    {
        REQUIRE_FALSE(region.is_valid_page_index<4096>(4));
        REQUIRE_FALSE(region.is_valid_page_index<4096>(100));
        REQUIRE_FALSE(region.is_valid_page_index<4096>(std::numeric_limits<size_t>::max()));
    }

    SECTION("Different page sizes")
    {
        // 16384 bytes with 2048-byte pages = 8 pages (indices 0-7 valid)
        REQUIRE(region.is_valid_page_index<2048>(7));
        REQUIRE_FALSE(region.is_valid_page_index<2048>(8));

        // 16384 bytes with 8192-byte pages = 2 pages (indices 0-1 valid)
        REQUIRE(region.is_valid_page_index<8192>(1));
        REQUIRE_FALSE(region.is_valid_page_index<8192>(2));
    }

    SECTION("Edge cases")
    {
        // Exact division
        REQUIRE(region.is_valid_page_index<16384>(0));
        REQUIRE_FALSE(region.is_valid_page_index<16384>(1));

        // Non-divisible page size
        // 16384 / 5000 = 3.2768, so 3 complete pages (indices 0-2 valid)
        REQUIRE(region.is_valid_page_index<5000>(2));
        REQUIRE_FALSE(region.is_valid_page_index<5000>(3));
    }
}

TEST_CASE (



"Setu - Page convenience helpers - is_valid_page_index on mapping"
,
"[setu][page][helpers]"
)
 {
    const char* test_path = "/tmp/setu_test_valid_index_mapping.dat";
    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, 12288);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Valid and invalid indices")
    {
        // 12288 / 4096 = 3 pages (indices 0-2 valid)
        REQUIRE(map.is_valid_page_index<4096>(0));
        REQUIRE(map.is_valid_page_index<4096>(1));
        REQUIRE(map.is_valid_page_index<4096>(2));
        REQUIRE_FALSE(map.is_valid_page_index<4096>(3));
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Page convenience helpers - combined usage"
,
"[setu][page][helpers]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(32768);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;
    auto region = map.full_region();

    constexpr size_t PAGE_SIZE = 4096;

    SECTION("Iterate using page_count and page_at")
    {
        size_t num_pages = region.page_count<PAGE_SIZE>();
        REQUIRE(num_pages == 8);

        for (size_t i = 0; i < num_pages; ++i)
        {
            REQUIRE(region.is_valid_page_index<PAGE_SIZE>(i));

            auto page = region.page_at<PAGE_SIZE>(i);
            REQUIRE(page.is_valid());

            // Write page index to first byte
            auto bytes = page.page_bytes();
            bytes[0] = std::byte{static_cast<uint8_t>(i)};
        }

        // Verify
        auto all_bytes = region.as_bytes();
        for (size_t i = 0; i < num_pages; ++i)
        {
            REQUIRE(all_bytes[i * PAGE_SIZE] == std::byte{static_cast<uint8_t>(i)});
        }
    }

    SECTION("Use page_index_to_offset for direct access")
    {
        size_t page_idx = 3;
        auto offset = region.page_index_to_offset<PAGE_SIZE>(page_idx);
        REQUIRE(offset.has_value());
        REQUIRE(*offset == 12288);

        // Access the region at that offset
        auto bytes = region.as_bytes();
        bytes[*offset] = std::byte{123};

        // Verify through page_at
        auto page = region.page_at<PAGE_SIZE>(page_idx);
        REQUIRE(page.is_valid());
        REQUIRE(page.page_bytes()[0] == std::byte{123});
    }

    SECTION("Validate before accessing")
    {
        size_t test_index = 7;

        if (region.is_valid_page_index<PAGE_SIZE>(test_index))
        {
            auto offset = region.page_index_to_offset<PAGE_SIZE>(test_index);
            REQUIRE(offset.has_value());

            auto page = region.page_at<PAGE_SIZE>(test_index);
            REQUIRE(page.is_valid());
        }

        // Out of bounds index
        size_t invalid_index = 8;
        REQUIRE_FALSE(region.is_valid_page_index<PAGE_SIZE>(invalid_index));

        auto invalid_offset = region.page_index_to_offset<PAGE_SIZE>(invalid_index);
        REQUIRE_FALSE(invalid_offset.has_value());

        auto invalid_page = region.page_at<PAGE_SIZE>(invalid_index);
        REQUIRE_FALSE(invalid_page.is_valid());
    }
}

TEST_CASE (



"Setu - Page convenience helpers - with subregions"
,
"[setu][page][helpers]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(32768);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    // Create a subregion that doesn't start at a page boundary
    auto sub_result = region.subregion(5000, 20000);
    REQUIRE(sub_result.has_value());
    auto& sub = *sub_result;

    SECTION("Page operations on subregion")
    {
        // 20000 bytes with 4096-byte pages = 4 complete pages
        REQUIRE(sub.page_count<4096>() == 4);

        REQUIRE(sub.is_valid_page_index<4096>(0));
        REQUIRE(sub.is_valid_page_index<4096>(3));
        REQUIRE_FALSE(sub.is_valid_page_index<4096>(4));

        auto page0 = sub.page_at<4096>(0);
        REQUIRE(page0.is_valid());

        auto offset1 = sub.page_index_to_offset<4096>(1);
        REQUIRE(offset1.has_value());
        REQUIRE(*offset1 == 4096);
    }

    SECTION("Write through subregion pages")
    {
        auto page = sub.page_at<4096>(2);
        REQUIRE(page.is_valid());

        auto page_bytes = page.page_bytes();
        page_bytes[100] = std::byte{77};

        // Verify through original region
        // Subregion starts at 5000, page 2 is at offset 8192 within subregion
        // So absolute offset is 5000 + 8192 + 100 = 13292
        auto orig_bytes = region.as_bytes();
        REQUIRE(orig_bytes[13292] == std::byte{77});
    }
}

TEST_CASE (



"Setu - Page convenience helpers - consistency checks"
,
"[setu][page][helpers]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();

    constexpr size_t PAGE_SIZE = 4096;

    SECTION("page_count and is_valid_page_index consistency")
    {
        size_t count = region.page_count<PAGE_SIZE>();

        // All indices < count should be valid
        for (size_t i = 0; i < count; ++i)
        {
            REQUIRE(region.is_valid_page_index<PAGE_SIZE>(i));
        }

        // Index == count should be invalid
        REQUIRE_FALSE(region.is_valid_page_index<PAGE_SIZE>(count));
    }

    SECTION("page_index_to_offset and page_at consistency")
    {
        for (size_t i = 0; i < region.page_count<PAGE_SIZE>(); ++i)
        {
            auto offset = region.page_index_to_offset<PAGE_SIZE>(i);
            REQUIRE(offset.has_value());

            auto page = region.page_at<PAGE_SIZE>(i);
            REQUIRE(page.is_valid());

            // The page's data pointer should be at the calculated offset
            auto region_bytes = region.as_bytes();
            auto page_bytes = page.page_bytes();
            REQUIRE(page_bytes.data() == region_bytes.data() + *offset);
        }
    }

    SECTION("page_at and traditional page_view consistency")
    {
        for (size_t i = 0; i < region.page_count<PAGE_SIZE>(); ++i)
        {
            auto page_new = region.page_at<PAGE_SIZE>(i);
            setu::page_view<PAGE_SIZE> page_old(region, i);

            REQUIRE(page_new.is_valid() == page_old.is_valid());

            if (page_new.is_valid())
            {
                auto bytes_new = page_new.page_bytes();
                auto bytes_old = page_old.page_bytes();
                REQUIRE(bytes_new.data() == bytes_old.data());
                REQUIRE(bytes_new.size() == bytes_old.size());
            }
        }
    }
}

TEST_CASE (



"Setu - Page convenience helpers - performance scenario"
,
"[setu][page][helpers]"
)
 {
    const char* test_path = "/tmp/setu_test_page_perf.dat";
    const size_t FILE_SIZE = 1024 * 1024; // 1MB
    const size_t PAGE_SIZE = 4096;

    auto map_result = setu::mapping<setu::read_write>::open_or_create(test_path, FILE_SIZE);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Process all pages efficiently")
    {
        size_t num_pages = map.page_count<PAGE_SIZE>();
        REQUIRE(num_pages == 256); // 1MB / 4KB

        // Process each page
        for (size_t i = 0; i < num_pages; ++i)
        {
            if (!map.is_valid_page_index<PAGE_SIZE>(i))
            {
                continue; // Skip if invalid (shouldn't happen here)
            }

            auto page = map.page_at<PAGE_SIZE>(i);
            if (!page.is_valid())
            {
                continue;
            }

            auto bytes = page.page_bytes();
            // Write page header
            bytes[0] = std::byte{static_cast<uint8_t>(i & 0xFF)};
        }

        // Verify
        auto region = map.full_region();
        for (size_t i = 0; i < num_pages; ++i)
        {
            auto offset = region.page_index_to_offset<PAGE_SIZE>(i);
            REQUIRE(offset.has_value());

            auto bytes = region.as_bytes();
            REQUIRE(bytes[*offset] == std::byte{static_cast<uint8_t>(i & 0xFF)});
        }
    }

    std::remove(test_path);
}

TEST_CASE (



"Setu - Page iterator equality semantics"
,
"[setu][page][iterator]"
)
 {
    SECTION("Iterators from same region at same position are equal")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        auto pages = setu::pages<4096>(region);
        auto it1 = pages.begin();
        auto it2 = pages.begin();

        REQUIRE(it1 == it2);
        REQUIRE_FALSE(it1 != it2);

        ++it1;
        ++it2;
        REQUIRE(it1 == it2);
    }

    SECTION("Iterators from same region at different positions are not equal")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        auto pages = setu::pages<4096>(region);
        auto it1 = pages.begin();
        auto it2 = pages.begin();
        ++it2;

        REQUIRE(it1 != it2);
        REQUIRE_FALSE(it1 == it2);
    }

    SECTION("Iterators from different regions are not equal even at same index")
    {
        auto map1_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
        auto map2_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
        REQUIRE(map1_result.has_value());
        REQUIRE(map2_result.has_value());

        auto region1 = map1_result->full_region();
        auto region2 = map2_result->full_region();

        auto pages1 = setu::pages<4096>(region1);
        auto pages2 = setu::pages<4096>(region2);

        auto it1 = pages1.begin();
        auto it2 = pages2.begin();

        // Different regions, same index - should NOT be equal
        REQUIRE(it1 != it2);
        REQUIRE_FALSE(it1 == it2);
    }

    SECTION("Iterators from different subregions of same mapping are not equal")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(32768);
        REQUIRE(map_result.has_value());
        auto full_region = map_result->full_region();

        auto sub1_result = full_region.subregion(0, 16384);
        auto sub2_result = full_region.subregion(16384, 16384);
        REQUIRE(sub1_result.has_value());
        REQUIRE(sub2_result.has_value());

        auto pages1 = setu::pages<4096>(*sub1_result);
        auto pages2 = setu::pages<4096>(*sub2_result);

        auto it1 = pages1.begin();
        auto it2 = pages2.begin();

        // Different subregions, same index - should NOT be equal
        REQUIRE(it1 != it2);
        REQUIRE_FALSE(it1 == it2);
    }

    SECTION("Iterator comparison after multiple increments")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        auto pages = setu::pages<4096>(region);
        auto it1 = pages.begin();
        auto it2 = pages.begin();

        // Move both forward
        ++it1;
        ++it1;
        ++it2;
        ++it2;

        REQUIRE(it1 == it2);

        // Move one more
        ++it1;
        REQUIRE(it1 != it2);
    }

    SECTION("End iterators are equal")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        auto pages = setu::pages<4096>(region);
        auto end1 = pages.end();
        auto end2 = pages.end();

        REQUIRE(end1 == end2);
    }

    SECTION("Iterator reaches end correctly")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(8192);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        auto pages = setu::pages<4096>(region);
        auto it = pages.begin();
        auto end = pages.end();

        REQUIRE(it != end);
        ++it;
        REQUIRE(it != end);
        ++it;
        REQUIRE(it == end);
    }

    SECTION("Different region sizes prevent false equality")
    {
        auto map1_result = setu::mapping<setu::read_write>::create_anonymous_private(8192);
        auto map2_result = setu::mapping<setu::read_write>::create_anonymous_private(16384);
        REQUIRE(map1_result.has_value());
        REQUIRE(map2_result.has_value());

        auto region1 = map1_result->full_region();
        auto region2 = map2_result->full_region();

        auto pages1 = setu::pages<4096>(region1);
        auto pages2 = setu::pages<4096>(region2);

        auto it1 = pages1.begin();
        auto it2 = pages2.begin();

        // Different region sizes, same index - should NOT be equal
        REQUIRE(it1 != it2);
    }
}

TEST_CASE (



"Setu - Page convenience helpers - edge cases and boundaries"
,
"[setu][page][helpers]"
)
 {
    SECTION("Zero-size region")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        auto empty_result = region.subregion(100, 0);
        REQUIRE(empty_result.has_value());
        auto& empty = *empty_result;

        REQUIRE(empty.page_count<4096>() == 0);
        REQUIRE_FALSE(empty.is_valid_page_index<4096>(0));
        REQUIRE_FALSE(empty.page_index_to_offset<4096>(0).has_value());
        REQUIRE_FALSE(empty.page_at<4096>(0).is_valid());
    }

    SECTION("Single-byte region")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        auto tiny_result = region.subregion(100, 1);
        REQUIRE(tiny_result.has_value());
        auto& tiny = *tiny_result;

        REQUIRE(tiny.page_count<4096>() == 0); // No complete 4KB pages
        REQUIRE(tiny.page_count<1>() == 1); // One 1-byte page
        REQUIRE_FALSE(tiny.is_valid_page_index<4096>(0));
        REQUIRE(tiny.is_valid_page_index<1>(0));
    }

    SECTION("Page size larger than region")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(2048);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        REQUIRE(region.page_count<4096>() == 0);
        REQUIRE_FALSE(region.is_valid_page_index<4096>(0));
        REQUIRE_FALSE(region.page_index_to_offset<4096>(0).has_value());
        REQUIRE_FALSE(region.page_at<4096>(0).is_valid());
    }

    SECTION("Exact page size match")
    {
        auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
        REQUIRE(map_result.has_value());
        auto region = map_result->full_region();

        REQUIRE(region.page_count<4096>() == 1);
        REQUIRE(region.is_valid_page_index<4096>(0));
        REQUIRE_FALSE(region.is_valid_page_index<4096>(1));

        auto offset = region.page_index_to_offset<4096>(0);
        REQUIRE(offset.has_value());
        REQUIRE(*offset == 0);

        auto page = region.page_at<4096>(0);
        REQUIRE(page.is_valid());
    }
}

// ============================================================================
// Endianness Conversion Tests (load/store with static_assert)
// ============================================================================

TEST_CASE (



"Setu - Endianness conversion - supported integral types"
,
"[setu][layout][endianness]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();
    auto bytes = region.as_bytes();

    SECTION("1-byte integral (uint8_t)")
    {
        uint8_t value = 0x42;
        setu::layout::store(bytes.data(), value, std::endian::big);
        auto loaded = setu::layout::load<uint8_t>(bytes.data(), std::endian::big);
        REQUIRE(loaded == value);

        // For 1-byte values, endianness doesn't matter
        auto loaded_little = setu::layout::load<uint8_t>(bytes.data(), std::endian::little);
        REQUIRE(loaded_little == value);
    }

    SECTION("2-byte integral (uint16_t)")
    {
        uint16_t value = 0x1234;
        setu::layout::store(bytes.data(), value, std::endian::big);
        auto loaded = setu::layout::load<uint16_t>(bytes.data(), std::endian::big);
        REQUIRE(loaded == value);

        // Verify byte order is actually big-endian
        if constexpr (std::endian::native == std::endian::little)
        {
            REQUIRE(bytes[0] == std::byte{0x12});
            REQUIRE(bytes[1] == std::byte{0x34});
        }
    }

    SECTION("4-byte integral (uint32_t)")
    {
        uint32_t value = 0x12345678;
        setu::layout::store(bytes.data() + 100, value, std::endian::big);
        auto loaded = setu::layout::load<uint32_t>(bytes.data() + 100, std::endian::big);
        REQUIRE(loaded == value);

        // Verify byte order
        if constexpr (std::endian::native == std::endian::little)
        {
            REQUIRE(bytes[100] == std::byte{0x12});
            REQUIRE(bytes[101] == std::byte{0x34});
            REQUIRE(bytes[102] == std::byte{0x56});
            REQUIRE(bytes[103] == std::byte{0x78});
        }
    }

    SECTION("8-byte integral (uint64_t)")
    {
        uint64_t value = 0x123456789ABCDEF0;
        setu::layout::store(bytes.data() + 200, value, std::endian::little);
        auto loaded = setu::layout::load<uint64_t>(bytes.data() + 200, std::endian::little);
        REQUIRE(loaded == value);

        // Verify byte order for little-endian
        if constexpr (std::endian::native == std::endian::big)
        {
            REQUIRE(bytes[200] == std::byte{0xF0});
            REQUIRE(bytes[201] == std::byte{0xDE});
            REQUIRE(bytes[207] == std::byte{0x12});
        }
    }

    SECTION("Signed integral types")
    {
        int8_t val8 = -42;
        setu::layout::store(bytes.data(), val8, std::endian::big);
        REQUIRE(setu::layout::load<int8_t>(bytes.data(), std::endian::big) == val8);

        int16_t val16 = -12345;
        setu::layout::store(bytes.data() + 10, val16, std::endian::big);
        REQUIRE(setu::layout::load<int16_t>(bytes.data() + 10, std::endian::big) == val16);

        int32_t val32 = -123456789;
        setu::layout::store(bytes.data() + 20, val32, std::endian::little);
        REQUIRE(setu::layout::load<int32_t>(bytes.data() + 20, std::endian::little) == val32);

        int64_t val64 = -9876543210LL;
        setu::layout::store(bytes.data() + 30, val64, std::endian::little);
        REQUIRE(setu::layout::load<int64_t>(bytes.data() + 30, std::endian::little) == val64);
    }
}

TEST_CASE (



"Setu - Endianness conversion - native endianness no-op"
,
"[setu][layout][endianness]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(1024);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();
    auto bytes = region.as_bytes();

    SECTION("Native endianness should not byte-swap")
    {
        uint32_t original = 0xDEADBEEF;
        setu::layout::store(bytes.data(), original, std::endian::native);

        // When using native endianness, the bytes should match the platform's representation
        uint32_t loaded;
        std::memcpy(&loaded, bytes.data(), sizeof(uint32_t));
        REQUIRE(loaded == original);

        // And load should return the same value
        auto loaded_via_api = setu::layout::load<uint32_t>(bytes.data(), std::endian::native);
        REQUIRE(loaded_via_api == original);
    }
}

TEST_CASE (



"Setu - Endianness conversion - cross-endian round-trip"
,
"[setu][layout][endianness]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(1024);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();
    auto bytes = region.as_bytes();

    SECTION("Big-endian round-trip")
    {
        uint16_t val16 = 0xABCD;
        uint32_t val32 = 0x12345678;
        uint64_t val64 = 0x0123456789ABCDEF;

        setu::layout::store(bytes.data(), val16, std::endian::big);
        setu::layout::store(bytes.data() + 10, val32, std::endian::big);
        setu::layout::store(bytes.data() + 20, val64, std::endian::big);

        REQUIRE(setu::layout::load<uint16_t>(bytes.data(), std::endian::big) == val16);
        REQUIRE(setu::layout::load<uint32_t>(bytes.data() + 10, std::endian::big) == val32);
        REQUIRE(setu::layout::load<uint64_t>(bytes.data() + 20, std::endian::big) == val64);
    }

    SECTION("Little-endian round-trip")
    {
        uint16_t val16 = 0xABCD;
        uint32_t val32 = 0x12345678;
        uint64_t val64 = 0x0123456789ABCDEF;

        setu::layout::store(bytes.data(), val16, std::endian::little);
        setu::layout::store(bytes.data() + 10, val32, std::endian::little);
        setu::layout::store(bytes.data() + 20, val64, std::endian::little);

        REQUIRE(setu::layout::load<uint16_t>(bytes.data(), std::endian::little) == val16);
        REQUIRE(setu::layout::load<uint32_t>(bytes.data() + 10, std::endian::little) == val32);
        REQUIRE(setu::layout::load<uint64_t>(bytes.data() + 20, std::endian::little) == val64);
    }

    SECTION("Mixed endianness in same buffer")
    {
        uint32_t value = 0xCAFEBABE;

        setu::layout::store(bytes.data(), value, std::endian::big);
        setu::layout::store(bytes.data() + 10, value, std::endian::little);

        auto loaded_big = setu::layout::load<uint32_t>(bytes.data(), std::endian::big);
        auto loaded_little = setu::layout::load<uint32_t>(bytes.data() + 10, std::endian::little);

        REQUIRE(loaded_big == value);
        REQUIRE(loaded_little == value);

        // The byte representations should be different (unless on mixed-endian system, which is rare)
        if constexpr (std::endian::native == std::endian::little || std::endian::native == std::endian::big)
        {
            bool bytes_differ = false;
            for (size_t i = 0; i < 4; ++i)
            {
                if (bytes[i] != bytes[10 + i])
                {
                    bytes_differ = true;
                    break;
                }
            }
            REQUIRE(bytes_differ);
        }
    }
}

TEST_CASE (



"Setu - Endianness conversion - file format example"
,
"[setu][layout][endianness]"
)
 {
    // Example: Writing a binary file format with specified endianness
    // Use anonymous mapping instead of file-based to avoid file I/O issues
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;
    auto region = map.full_region();
    auto bytes = region.as_bytes();

    struct FileHeader {
        uint32_t magic; // Always big-endian (network byte order)
        uint16_t version; // Always big-endian
        uint16_t flags; // Always big-endian
        uint64_t timestamp; // Always little-endian for this example
        uint32_t checksum; // Always little-endian
    };

    size_t offset = 0;

    // Write magic in big-endian
    uint32_t magic = 0x4D415449; // "MATI"
    setu::layout::store(bytes.data() + offset, magic, std::endian::big);
    offset += sizeof(uint32_t);

    // Write version in big-endian
    uint16_t version = 0x0102; // Version 1.2
    setu::layout::store(bytes.data() + offset, version, std::endian::big);
    offset += sizeof(uint16_t);

    // Write flags in big-endian
    uint16_t flags = 0x0001;
    setu::layout::store(bytes.data() + offset, flags, std::endian::big);
    offset += sizeof(uint16_t);

    // Write timestamp in little-endian
    uint64_t timestamp = 1234567890123456ULL;
    setu::layout::store(bytes.data() + offset, timestamp, std::endian::little);
    offset += sizeof(uint64_t);

    // Write checksum in little-endian
    uint32_t checksum = 0xABCDEF01;
    setu::layout::store(bytes.data() + offset, checksum, std::endian::little);

    // Read back and verify
    offset = 0;

    auto read_magic = setu::layout::load<uint32_t>(bytes.data() + offset, std::endian::big);
    offset += sizeof(uint32_t);
    REQUIRE(read_magic == 0x4D415449);

    auto read_version = setu::layout::load<uint16_t>(bytes.data() + offset, std::endian::big);
    offset += sizeof(uint16_t);
    REQUIRE(read_version == 0x0102);

    auto read_flags = setu::layout::load<uint16_t>(bytes.data() + offset, std::endian::big);
    offset += sizeof(uint16_t);
    REQUIRE(read_flags == 0x0001);

    auto read_timestamp = setu::layout::load<uint64_t>(bytes.data() + offset, std::endian::little);
    offset += sizeof(uint64_t);
    REQUIRE(read_timestamp == 1234567890123456ULL);

    auto read_checksum = setu::layout::load<uint32_t>(bytes.data() + offset, std::endian::little);
    REQUIRE(read_checksum == 0xABCDEF01);
}

TEST_CASE (



"Setu - Endianness conversion - boundary values"
,
"[setu][layout][endianness]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(1024);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();
    auto bytes = region.as_bytes();

    SECTION("Zero values")
    {
        setu::layout::store(bytes.data(), uint8_t(0), std::endian::big);
        setu::layout::store(bytes.data() + 10, uint16_t(0), std::endian::big);
        setu::layout::store(bytes.data() + 20, uint32_t(0), std::endian::little);
        setu::layout::store(bytes.data() + 30, uint64_t(0), std::endian::little);

        REQUIRE(setu::layout::load<uint8_t>(bytes.data(), std::endian::big) == 0);
        REQUIRE(setu::layout::load<uint16_t>(bytes.data() + 10, std::endian::big) == 0);
        REQUIRE(setu::layout::load<uint32_t>(bytes.data() + 20, std::endian::little) == 0);
        REQUIRE(setu::layout::load<uint64_t>(bytes.data() + 30, std::endian::little) == 0);
    }

    SECTION("Maximum values")
    {
        setu::layout::store(bytes.data(), uint8_t(0xFF), std::endian::big);
        setu::layout::store(bytes.data() + 10, uint16_t(0xFFFF), std::endian::big);
        setu::layout::store(bytes.data() + 20, uint32_t(0xFFFFFFFF), std::endian::little);
        setu::layout::store(bytes.data() + 30, uint64_t(0xFFFFFFFFFFFFFFFFULL), std::endian::little);

        REQUIRE(setu::layout::load<uint8_t>(bytes.data(), std::endian::big) == 0xFF);
        REQUIRE(setu::layout::load<uint16_t>(bytes.data() + 10, std::endian::big) == 0xFFFF);
        REQUIRE(setu::layout::load<uint32_t>(bytes.data() + 20, std::endian::little) == 0xFFFFFFFF);
        REQUIRE(setu::layout::load<uint64_t>(bytes.data() + 30, std::endian::little) == 0xFFFFFFFFFFFFFFFFULL);
    }

    SECTION("Alternating bit patterns")
    {
        uint16_t pattern16 = 0xAAAA;
        uint32_t pattern32 = 0xAAAAAAAA;
        uint64_t pattern64 = 0xAAAAAAAAAAAAAAAAULL;

        setu::layout::store(bytes.data(), pattern16, std::endian::big);
        setu::layout::store(bytes.data() + 10, pattern32, std::endian::big);
        setu::layout::store(bytes.data() + 20, pattern64, std::endian::little);

        REQUIRE(setu::layout::load<uint16_t>(bytes.data(), std::endian::big) == pattern16);
        REQUIRE(setu::layout::load<uint32_t>(bytes.data() + 10, std::endian::big) == pattern32);
        REQUIRE(setu::layout::load<uint64_t>(bytes.data() + 20, std::endian::little) == pattern64);
    }
}

TEST_CASE (



"Setu - Endianness conversion - array of values"
,
"[setu][layout][endianness]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();
    auto bytes = region.as_bytes();

    SECTION("Array of uint32_t in big-endian")
    {
        std::array<uint32_t, 10> values = {
            0x00000001, 0x00000100, 0x00010000, 0x01000000,
            0x12345678, 0x87654321, 0xDEADBEEF, 0xCAFEBABE,
            0xFFFFFFFF, 0x00000000
        };

        // Store array
        size_t offset = 0;
        for (const auto& val : values)
        {
            setu::layout::store(bytes.data() + offset, val, std::endian::big);
            offset += sizeof(uint32_t);
        }

        // Load and verify
        offset = 0;
        for (const auto& expected : values)
        {
            auto loaded = setu::layout::load<uint32_t>(bytes.data() + offset, std::endian::big);
            REQUIRE(loaded == expected);
            offset += sizeof(uint32_t);
        }
    }

    SECTION("Mixed sizes in sequential order")
    {
        size_t offset = 0;

        uint8_t val8 = 0x42;
        setu::layout::store(bytes.data() + offset, val8, std::endian::big);
        offset += sizeof(uint8_t);

        uint16_t val16 = 0x1234;
        setu::layout::store(bytes.data() + offset, val16, std::endian::big);
        offset += sizeof(uint16_t);

        uint32_t val32 = 0x56789ABC;
        setu::layout::store(bytes.data() + offset, val32, std::endian::little);
        offset += sizeof(uint32_t);

        uint64_t val64 = 0xDEADBEEFCAFEBABEULL;
        setu::layout::store(bytes.data() + offset, val64, std::endian::little);

        // Verify
        offset = 0;
        REQUIRE(setu::layout::load<uint8_t>(bytes.data() + offset, std::endian::big) == val8);
        offset += sizeof(uint8_t);

        REQUIRE(setu::layout::load<uint16_t>(bytes.data() + offset, std::endian::big) == val16);
        offset += sizeof(uint16_t);

        REQUIRE(setu::layout::load<uint32_t>(bytes.data() + offset, std::endian::little) == val32);
        offset += sizeof(uint32_t);

        REQUIRE(setu::layout::load<uint64_t>(bytes.data() + offset, std::endian::little) == val64);
    }
}

TEST_CASE (



"Setu - Endianness conversion - non-integral types pass through"
,
"[setu][layout][endianness]"
)
 {
    // Non-integral types should work but won't be byte-swapped
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(1024);
    REQUIRE(map_result.has_value());
    auto region = map_result->full_region();
    auto bytes = region.as_bytes();

    SECTION("Float types (no byte-swapping, but should work)")
    {
        float f_val = 3.14159f;
        setu::layout::store(bytes.data(), f_val, std::endian::big);
        // Note: Endianness conversion is not performed for non-integral types
        // So this will store as-is in native format
        auto loaded_f = setu::layout::load<float>(bytes.data(), std::endian::native);
        REQUIRE(loaded_f == f_val);

        double d_val = 2.71828;
        setu::layout::store(bytes.data() + 100, d_val, std::endian::little);
        auto loaded_d = setu::layout::load<double>(bytes.data() + 100, std::endian::native);
        REQUIRE(loaded_d == d_val);
    }

    SECTION("POD struct (no byte-swapping)")
    {
        struct Point {
            float x;
            float y;
        };

        Point p{1.0f, 2.0f};
        setu::layout::store(bytes.data(), p, std::endian::big);
        auto loaded = setu::layout::load<Point>(bytes.data(), std::endian::native);
        REQUIRE(loaded.x == p.x);
        REQUIRE(loaded.y == p.y);
    }
}

// Note: We can't directly test that the static_assert triggers at compile time
// in a runtime test suite, but the fact that the supported types compile and work
// confirms the static_assert is properly allowing them through. Attempting to use
// an unsupported size (like __int128 on platforms that have it) would fail at
// compile time with the static_assert message.

// ============================================================================
// Review fixes: Bounds checking, underflow, alignment, const-correctness
// ============================================================================

TEST_CASE (



"Setu - Bounds and overflow protection"
,
"[setu][bounds][review]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(4096);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Out-of-bounds subregion rejected")
    {
        auto result = map.subregion(map.size() + 100, 10);
        REQUIRE(!result.has_value());
    }

    SECTION("Subregion with overflow size rejected")
    {
        // Request would extend past mapping
        auto result = map.subregion(4000, 1000);
        REQUIRE(!result.has_value());
    }

    SECTION("Valid subregion accepted")
    {
        auto result = map.subregion(0, 100);
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 100);
    }

    SECTION("Subregion at boundary accepted")
    {
        auto result = map.subregion(map.size() - 10, 10);
        REQUIRE(result.has_value());
        REQUIRE(result.value().size() == 10);
    }
}

TEST_CASE (



"Setu - Const correctness in region access"
,
"[setu][const][review]"
)
 {
    auto map_result = setu::mapping<setu::read_only>::create_anonymous_private(1024);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Read-only mapping returns non-writable region")
    {
        auto region = map.full_region();
        REQUIRE(region);
        REQUIRE(!region.writable());
    }

    SECTION("Read-only region data is const")
    {
        auto region = map.full_region();
        const auto* ptr = region.data();
        static_assert(std::is_same_v<decltype(ptr), const std::byte*>);
    }
}

TEST_CASE (



"Setu - Alignment validation"
,
"[setu][alignment][review]"
)
 {
    auto map_result = setu::mapping<setu::read_write>::create_anonymous_private(8192);
    REQUIRE(map_result.has_value());
    auto& map = *map_result;

    SECTION("Mapping address is page-aligned")
    {
        auto bytes = map.as_bytes();
        auto addr = reinterpret_cast<std::uintptr_t>(bytes.data());
        // Page size is typically 4096 on most systems
        REQUIRE((addr % 4096) == 0);
    }

    SECTION("Subregion preserves parent alignment")
    {
        auto sub = map.subregion(0, 256);
        REQUIRE(sub.has_value());
        auto bytes = sub.value().as_bytes();
        auto addr = reinterpret_cast<std::uintptr_t>(bytes.data());
        // Subregion should also be page-aligned or properly aligned
        REQUIRE(addr >= reinterpret_cast<std::uintptr_t>(map.as_bytes().data()));
    }
}
