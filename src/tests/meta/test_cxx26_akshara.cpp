#include <catch_amalgamated.hpp>
#include <meta/akshara.hpp>
#include <format>

using namespace akshara::literals;

TEST_CASE("akshara C++26: fixed_string user-defined literal operator (\"\"_fs)", "[cxx26][akshara]") {
    constexpr auto s1 = "hello_world"_fs;
    STATIC_REQUIRE(s1.length == 11);
    STATIC_REQUIRE(s1.view() == "hello_world");
    STATIC_REQUIRE(std::string_view(s1) == "hello_world");

    constexpr auto s2 = ""_fs;
    STATIC_REQUIRE(s2.length == 0);
    STATIC_REQUIRE(s2.empty());
}

TEST_CASE("akshara C++26: variadic concat and static error formatter", "[cxx26][akshara]") {
    constexpr auto combined = akshara::concat("type: "_fs, "uint64_t"_fs, " (size="_fs, "8"_fs, ")"_fs);
    STATIC_REQUIRE(combined == "type: uint64_t (size=8)"_fs);
    STATIC_REQUIRE(combined.length == 23);

    constexpr auto msg = akshara::format_static_error<"error: "_fs, "reflection failed for type"_fs>;
    STATIC_REQUIRE(msg == "error: reflection failed for type"_fs);
}

TEST_CASE("akshara C++26: std::formatter and std::format interop", "[cxx26][akshara]") {
    constexpr auto name = "pebble_engine"_fs;
    std::string formatted = std::format("Welcome to {}", name);
    REQUIRE(formatted == "Welcome to pebble_engine");
}

TEST_CASE("akshara C++26: matcher and charset pattern validation", "[cxx26][akshara]") {
    constexpr auto valid_id = "var_name_123"_fs;
    constexpr auto invalid_id = "123_var"_fs;
    constexpr auto special_id = "foo-bar"_fs;

    STATIC_REQUIRE(akshara::matcher::is_valid_c_identifier(valid_id));
    STATIC_REQUIRE_FALSE(akshara::matcher::is_valid_c_identifier(invalid_id));
    STATIC_REQUIRE_FALSE(akshara::matcher::is_valid_c_identifier(special_id));

    constexpr auto digits = "0123456789"_fs;
    STATIC_REQUIRE(akshara::matcher::matches_all(digits, akshara::cs_digits()));
    STATIC_REQUIRE_FALSE(akshara::matcher::matches_all(valid_id, akshara::cs_digits()));

    REQUIRE(akshara::matcher::matches_all(std::string_view("98765"), akshara::cs_digits()));
}
