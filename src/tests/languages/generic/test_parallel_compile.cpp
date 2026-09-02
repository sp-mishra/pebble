#include "catch_amalgamated.hpp"

#include "languages/generic/module/parallel_compile.hpp"

#include <cstddef>
#include <string>
#include <vector>

TEST_CASE (
"generic module compilation preserves source order with Pravaha"
,
"[lang][module][parallel]"
)
 {
    const std::vector<std::string> modules{
        "core", "math", "io", "net", "data", "ui", "app", "tools"};
    pravaha::JThreadBackend backend{2};

    auto result = lang::module::compile_modules_pravaha(
        modules,
        [](const std::string& module_name) noexcept { return module_name.size(); },
        backend,
        {.minimum_parallel_modules = 2, .maximum_tasks_per_worker = 1});

    REQUIRE(result.has_value());
    REQUIRE(result->used_parallelism);
    REQUIRE(result->results.size() == modules.size());
    for (std::size_t index = 0; index < modules.size(); ++index)
        REQUIRE(result->results[index] == modules[index].size());
}

TEST_CASE (
"generic module compilation remains inline below the policy threshold"
,
"[lang][module][parallel]"
)
 {
    const std::vector<std::string> modules{"core", "app"};
    pravaha::JThreadBackend backend{2};

    auto result = lang::module::compile_modules_pravaha(
        modules,
        [](const std::string& module_name) noexcept { return module_name.size(); },
        backend,
        {.minimum_parallel_modules = 8, .maximum_tasks_per_worker = 1});

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->used_parallelism);
    REQUIRE(result->results.size() == modules.size());
}
