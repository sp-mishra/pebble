#include "catch_amalgamated.hpp"
#include "medha/adapters/anukrama.hpp"

#include <string>

TEST_CASE("Medha Anukrama adapter: commits a typed staged batch", "[medha][anukrama][adapter]") {
    anukrama::store<std::string, std::string> values;
    medha::adapters::anukrama_resource<std::string, std::string> resource{values};
    medha::resource_handle handle{resource, medha::resource_id{1, 1}};
    medha::transaction_context context;

    REQUIRE(context.store(handle, std::string{"account"}, std::string{"100"}).has_value());
    REQUIRE(context.commit().has_value());
    CHECK(values.get("account") == "100");
}

TEST_CASE("Medha Anukrama adapter: rejects a stale staged write", "[medha][anukrama][adapter][conflict]") {
    anukrama::store<std::string, std::string> values;
    REQUIRE(values.begin().put("account", "100").commit().has_value());
    medha::adapters::anukrama_resource<std::string, std::string> resource{values};
    medha::resource_handle handle{resource, medha::resource_id{1, 1}};
    medha::transaction_context stale;

    REQUIRE(stale.load(handle, std::string{"account"}) == "100");
    REQUIRE(stale.store(handle, std::string{"account"}, std::string{"50"}).has_value());
    REQUIRE(values.begin().put("account", "200").commit().has_value());

    const auto result = stale.commit();
    CHECK_FALSE(result.has_value());
    CHECK(result.error().status == medha::tx_status::conflict);
    CHECK(values.get("account") == "200");
}
