// ============================================================================
// test_member_traits.cpp — Unit tests for member pointer & path introspection
// ============================================================================

#include "catch_amalgamated.hpp"
#include "meta/meta.hpp"
#include <memory>
#include <optional>
#include <string>

namespace {

struct Inner {
    int value{42};
    double get_double() const { return 3.14; }
    void noexcept_action() noexcept {}
    int ref_qualified() & { return value; }
};

struct Middle {
    Inner inner;
    std::optional<Inner> opt_inner;
    std::unique_ptr<Inner> ptr_inner;
};

struct Outer {
    std::string name;
    Middle middle;
    Middle* raw_middle{nullptr};
};

} // namespace

TEST_CASE("meta: member_pointer_traits specializations", "[meta][member_traits]") {
    // 1. Member object pointer
    using MemberValTraits = meta::member_pointer_traits<&Inner::value>;
    STATIC_REQUIRE(MemberValTraits::is_member_object);
    STATIC_REQUIRE(!MemberValTraits::is_member_function);
    STATIC_REQUIRE(std::same_as<MemberValTraits::owner_type, Inner>);
    STATIC_REQUIRE(std::same_as<MemberValTraits::value_type, int>);
    STATIC_REQUIRE(meta::member_object_pointer<&Inner::value>);
    STATIC_REQUIRE(!meta::member_function_pointer<&Inner::value>);
    STATIC_REQUIRE(meta::member_of<Inner, &Inner::value>);

    // 2. Const member function pointer
    using ConstFuncTraits = meta::member_pointer_traits<&Inner::get_double>;
    STATIC_REQUIRE(!ConstFuncTraits::is_member_object);
    STATIC_REQUIRE(ConstFuncTraits::is_member_function);
    STATIC_REQUIRE(ConstFuncTraits::is_const);
    STATIC_REQUIRE(!ConstFuncTraits::is_noexcept);
    STATIC_REQUIRE(std::same_as<ConstFuncTraits::owner_type, Inner>);
    STATIC_REQUIRE(std::same_as<ConstFuncTraits::return_type, double>);
    STATIC_REQUIRE(meta::member_function_pointer<&Inner::get_double>);

    // 3. Noexcept member function pointer
    using NoexceptFuncTraits = meta::member_pointer_traits<&Inner::noexcept_action>;
    STATIC_REQUIRE(NoexceptFuncTraits::is_member_function);
    STATIC_REQUIRE(NoexceptFuncTraits::is_noexcept);

    // 4. Lvalue ref-qualified member function pointer
    using RefFuncTraits = meta::member_pointer_traits<&Inner::ref_qualified>;
    STATIC_REQUIRE(RefFuncTraits::is_member_function);
    STATIC_REQUIRE(std::same_as<RefFuncTraits::return_type, int>);
}

TEST_CASE("meta: member_path traversal & effect bitmasks", "[meta][member_path]") {
    // Single-step direct path
    using DirectPath = meta::member_path<&Inner::value>;
    STATIC_REQUIRE(DirectPath::depth == 1);
    STATIC_REQUIRE(!DirectPath::has_optional);
    STATIC_REQUIRE(!DirectPath::has_indirection);
    STATIC_REQUIRE(!DirectPath::has_nullable);
    STATIC_REQUIRE(std::same_as<DirectPath::result_type, int>);

    // Multi-step direct path
    using OuterToInnerVal = meta::member_path<&Outer::middle, &Middle::inner, &Inner::value>;
    STATIC_REQUIRE(OuterToInnerVal::depth == 3);
    STATIC_REQUIRE(!OuterToInnerVal::has_optional);
    STATIC_REQUIRE(!OuterToInnerVal::has_indirection);
    STATIC_REQUIRE(std::same_as<OuterToInnerVal::result_type, int>);

    // Multi-step path with std::optional
    using OuterToOptInnerVal = meta::member_path<&Outer::middle, &Middle::opt_inner, &Inner::value>;
    STATIC_REQUIRE(OuterToOptInnerVal::depth == 3);
    STATIC_REQUIRE(OuterToOptInnerVal::has_optional);
    STATIC_REQUIRE(!OuterToOptInnerVal::has_indirection);
    STATIC_REQUIRE(std::same_as<OuterToOptInnerVal::result_type, int>);

    // Multi-step path with std::unique_ptr
    using OuterToPtrInnerVal = meta::member_path<&Outer::middle, &Middle::ptr_inner, &Inner::value>;
    STATIC_REQUIRE(OuterToPtrInnerVal::depth == 3);
    STATIC_REQUIRE(OuterToPtrInnerVal::has_indirection);
    STATIC_REQUIRE(OuterToPtrInnerVal::has_nullable);
    STATIC_REQUIRE(!OuterToPtrInnerVal::has_optional);
    STATIC_REQUIRE(std::same_as<OuterToPtrInnerVal::result_type, int>);

    // Multi-step path with raw pointer
    using OuterToRawMiddleInner = meta::member_path<&Outer::raw_middle, &Middle::inner, &Inner::value>;
    STATIC_REQUIRE(OuterToRawMiddleInner::depth == 3);
    STATIC_REQUIRE(OuterToRawMiddleInner::has_indirection);
    STATIC_REQUIRE(OuterToRawMiddleInner::has_nullable);
}

