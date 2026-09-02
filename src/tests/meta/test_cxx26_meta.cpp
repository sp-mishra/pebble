#include <catch_amalgamated.hpp>
#include <meta/meta.hpp>

using namespace meta::literals;

namespace {
    struct Vector3D {
        float x;
        float y;
        float z;
    };

    class Calculator {
    public:
        int add(int a, int b) const { return a + b; }
        double compute(double val) noexcept { return val * 2.0; }
        void reset() {}
    };

    struct AnnotatedUser {
        int id;
        std::string_view username;
        std::string_view password_hash;

        friend consteval auto reflect_members(meta::type_tag<AnnotatedUser>) {
            return meta::make_sequence(
                meta::annotated_field < 0, &AnnotatedUser::id, "id" > (
                    meta::attribute < "primary_key" >
            {}
            ),
            meta::annotated_field < 1, &AnnotatedUser::username, "username" > (
                meta::attribute < "indexed" >
            {}
            ),
            meta::annotated_field < 2, &AnnotatedUser::password_hash, "password_hash" > (
                meta::attribute < "sensitive" >
            {},
            meta::attribute < "json_ignore" >
            {}
            )
            );
        }
    };
} // namespace

TEST_CASE (
"meta C++26: Feature detection and pack indexing"
,
"[cxx26][meta]"
)
 {
    using MyList = meta::TypeList<int, double, char, float>;
    STATIC_REQUIRE(std::same_as<MyList::element<0>, int>);
    STATIC_REQUIRE(std::same_as<MyList::element<1>, double>);
    STATIC_REQUIRE(std::same_as<MyList::element<2>, char>);
    STATIC_REQUIRE(std::same_as<MyList::element<3>, float>);

    using MyValues = meta::value_list<10, 20, 30, 40>;
    STATIC_REQUIRE(MyValues::get<0>() == 10);
    STATIC_REQUIRE(MyValues::get<1>() == 20);
    STATIC_REQUIRE(MyValues::get<2>() == 30);
    STATIC_REQUIRE(MyValues::get<3>() == 40);
}

TEST_CASE (
"meta C++26: Method and function introspection"
,
"[cxx26][meta]"
)
 {
    constexpr auto m_add = meta::method<0, &Calculator::add, "add">();
    STATIC_REQUIRE(m_add.name() == "add");
    STATIC_REQUIRE(m_add.arity() == 2);
    STATIC_REQUIRE(m_add.is_const());
    STATIC_REQUIRE_FALSE(m_add.is_noexcept());
    STATIC_REQUIRE(std::same_as<decltype(m_add)::return_type, int>);
    STATIC_REQUIRE(std::same_as<decltype(m_add)::param_types::element<0>, int>);
    STATIC_REQUIRE(std::same_as<decltype(m_add)::param_types::element<1>, int>);

    Calculator calc;
    REQUIRE(m_add.invoke(calc, 4, 6) == 10);

    constexpr auto m_compute = meta::method<1, &Calculator::compute, "compute">();
    STATIC_REQUIRE(m_compute.name() == "compute");
    STATIC_REQUIRE(m_compute.is_noexcept());
    STATIC_REQUIRE_FALSE(m_compute.is_const());
    REQUIRE(m_compute.invoke(calc, 3.5) == 7.0);
}

TEST_CASE (
"meta C++26: Compile-time field annotations and attributes"
,
"[cxx26][meta]"
)
 {
    using Seq = meta::reflect_t<AnnotatedUser>;
    STATIC_REQUIRE(Seq::size == 3);

    using F0 = Seq::element<0>;
    STATIC_REQUIRE(F0::name() == "id");
    STATIC_REQUIRE(F0::has_attribute<"primary_key">());
    STATIC_REQUIRE_FALSE(F0::has_attribute<"sensitive">());

    using F2 = Seq::element<2>;
    STATIC_REQUIRE(F2::name() == "password_hash");
    STATIC_REQUIRE(F2::has_attribute<"sensitive">());
    STATIC_REQUIRE(F2::has_attribute<"json_ignore">());
    STATIC_REQUIRE_FALSE(F2::has_attribute<"primary_key">());
}

TEST_CASE (
"meta C++26: Structure-of-Arrays (SoA) layout transform"
,
"[cxx26][meta]"
)
 {
    meta::soa_storage<Vector3D, 16> soa;
    REQUIRE(soa.empty());

    soa.push_back(Vector3D{1.0f, 2.0f, 3.0f});
    soa.push_back(Vector3D{4.0f, 5.0f, 6.0f});

    REQUIRE(soa.size() == 2);
    REQUIRE(soa.column<0>()[0] == 1.0f);
    REQUIRE(soa.column<1>()[0] == 2.0f);
    REQUIRE(soa.column<2>()[0] == 3.0f);

    REQUIRE(soa.column<0>()[1] == 4.0f);
    REQUIRE(soa.column<1>()[1] == 5.0f);
    REQUIRE(soa.column<2>()[1] == 6.0f);

    Vector3D reconstructed = soa.get(1);
    REQUIRE(reconstructed.x == 4.0f);
    REQUIRE(reconstructed.y == 5.0f);
    REQUIRE(reconstructed.z == 6.0f);
}
