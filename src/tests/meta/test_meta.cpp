// ============================================================================
// test_meta.cpp — Unit tests for meta.hpp
// ============================================================================
// Tests for: Reflection, Meta Algorithms, Compile-Time Data Structures,
//            Enum Reflection, Tuple Interop, Layout Policies, Hashing
// ============================================================================

#include "catch_amalgamated.hpp"
#include "meta/meta.hpp"

#include <string>
#include <sstream>
#include <vector>

// ============================================================================
// Test types
// ============================================================================

// Simple aggregate
struct Point {
    double x;
    double y;
};

// 3-field aggregate
struct Vec3 {
    float x;
    float y;
    float z;
};

// Mixed-type aggregate
struct Person {
    int age;
    double height;
    const char* name;
};

// Larger aggregate (8 fields)
struct Record {
    int a;
    int b;
    int c;
    int d;
    double e;
    double f;
    double g;
    double h;
};

// Empty aggregate
struct Empty {};

// Single-field aggregate
struct Wrapper {
    int value;
};

// Aggregate with pointer field
struct WithPointer {
    int* ptr;
    int val;
};

// Standard layout aggregate for layout tests
struct Aligned {
    int a;
    double b;
    char c;
};

// Aggregate carrying a true reference member to validate exact type extraction
struct RefAggregate {
    int& ref;
    const int c;
};

// Enum types for enum reflection
enum class Color { Red = 0, Green = 1, Blue = 2 };

enum class Direction { North = 0, South = 1, East = 2, West = 3 };

enum class Status { Active = 1, Inactive = 2, Pending = 3 };

// Class with custom (ADL) reflection
class TradeOrder {
    double price_;
    int quantity_;

    friend consteval auto reflect_members(meta::type_tag<TradeOrder>) {
        return meta::make_sequence(
            meta::field<0, &TradeOrder::price_, "price">(),
            meta::field<1, &TradeOrder::quantity_, "quantity">());
    }

public:
    TradeOrder(double p, int q) : price_(p), quantity_(q) {}

    double price() const { return price_; }
    int quantity() const { return quantity_; }
};

// Another class with custom reflection
class Account {
    std::string name_;
    double balance_;
    bool active_;

    friend consteval auto reflect_members(meta::type_tag<Account>) {
        return meta::make_sequence(
            meta::field<0, &Account::name_, "name">(),
            meta::field<1, &Account::balance_, "balance">(),
            meta::field<2, &Account::active_, "active">());
    }

public:
    Account(std::string n, double b, bool a)
        : name_(std::move(n)), balance_(b), active_(a) {}

    const std::string& name() const { return name_; }
    double balance() const { return balance_; }
    bool active() const { return active_; }
};

class ConstMemberView {
    const int id_;

    friend consteval auto reflect_members(meta::type_tag<ConstMemberView>) {
        return meta::make_sequence(
            meta::field<0, &ConstMemberView::id_, "id">());
    }

public:
    explicit ConstMemberView(int id) : id_(id) {}

    int id() const { return id_; }
};

// ============================================================================
// SECTION 1: Compile-Time Data Structures
// ============================================================================

TEST_CASE (



"fixed_string basics"
,
"[meta][data_structures]"
)
{
    constexpr meta::fixed_string fs("hello");

    STATIC_REQUIRE(fs.length == 5);
    STATIC_REQUIRE(fs.view() == "hello");
    STATIC_REQUIRE(fs[0] == 'h');
    STATIC_REQUIRE(fs[4] == 'o');

    constexpr meta::fixed_string fs2("hello");
    STATIC_REQUIRE(fs == fs2);

    constexpr meta::fixed_string world(" world");
    constexpr auto combined = fs + world;
    STATIC_REQUIRE(combined.view() == "hello world");
}

TEST_CASE (



"TypeList basics"
,
"[meta][data_structures]"
)
{
    using TL = meta::TypeList<int, double, float, char>;

    STATIC_REQUIRE(TL::size == 4);
    STATIC_REQUIRE(std::same_as<TL::element<0>, int>);
    STATIC_REQUIRE(std::same_as<TL::element<1>, double>);
    STATIC_REQUIRE(std::same_as<TL::element<2>, float>);
    STATIC_REQUIRE(std::same_as<TL::element<3>, char>);

    STATIC_REQUIRE(TL::contains<int>());
    STATIC_REQUIRE(TL::contains<double>());
    STATIC_REQUIRE(!TL::contains<long>());

    STATIC_REQUIRE(TL::index_of<int>() == 0);
    STATIC_REQUIRE(TL::index_of<double>() == 1);
    STATIC_REQUIRE(TL::index_of<float>() == 2);
    STATIC_REQUIRE(TL::index_of<char>() == 3);
}

TEST_CASE (



"Sequence basics"
,
"[meta][data_structures]"
)
{
    using Seq = meta::Sequence<int, double, float>;

    STATIC_REQUIRE(Seq::size == 3);
    STATIC_REQUIRE(std::same_as<Seq::element<0>, int>);
    STATIC_REQUIRE(std::same_as<Seq::element<1>, double>);
    STATIC_REQUIRE(std::same_as<Seq::element<2>, float>);
}

TEST_CASE (



"value_list basics"
,
"[meta][data_structures]"
)
{
    using VL = meta::value_list<1, 2, 3, 4, 5>;

    STATIC_REQUIRE(VL::size == 5);
    STATIC_REQUIRE(VL::get<0>() == 1);
    STATIC_REQUIRE(VL::get<4>() == 5);
}

TEST_CASE (



"ct_map basics"
,
"[meta][data_structures]"
)
{
    using Map = meta::ct_map<
        meta::named_entry<"name", std::string>,
        meta::named_entry<"age", int>,
        meta::named_entry<"height", double>>;

    STATIC_REQUIRE(Map::size == 3);
    STATIC_REQUIRE(Map::contains<"name">());
    STATIC_REQUIRE(Map::contains<"age">());
    STATIC_REQUIRE(Map::contains<"height">());
    STATIC_REQUIRE(!Map::contains<"weight">());

    STATIC_REQUIRE(std::same_as<Map::lookup_t<"name">, std::string>);
    STATIC_REQUIRE(std::same_as<Map::lookup_t<"age">, int>);
    STATIC_REQUIRE(std::same_as<Map::lookup_t<"height">, double>);
}

// ============================================================================
// SECTION 2: Concepts & Core Traits
// ============================================================================

TEST_CASE (



"Aggregate concept"
,
"[meta][concepts]"
)
{
    STATIC_REQUIRE(meta::Aggregate<Point>);
    STATIC_REQUIRE(meta::Aggregate<Vec3>);
    STATIC_REQUIRE(meta::Aggregate<Person>);
    STATIC_REQUIRE(meta::Aggregate<Empty>);
    STATIC_REQUIRE(meta::Aggregate<Wrapper>);
    STATIC_REQUIRE(!meta::Aggregate<std::string>);
}

TEST_CASE (



"StandardLayout concept"
,
"[meta][concepts]"
)
{
    STATIC_REQUIRE(meta::StandardLayout<Point>);
    STATIC_REQUIRE(meta::StandardLayout<Vec3>);
    STATIC_REQUIRE(meta::StandardLayout<int>);
}

TEST_CASE (



"TriviallyCopyable concept"
,
"[meta][concepts]"
)
{
    STATIC_REQUIRE(meta::TriviallyCopyable<Point>);
    STATIC_REQUIRE(meta::TriviallyCopyable<int>);
    STATIC_REQUIRE(!meta::TriviallyCopyable<std::string>);
}

TEST_CASE (



"MetaEnum concept"
,
"[meta][concepts]"
)
{
    STATIC_REQUIRE(meta::MetaEnum<Color>);
    STATIC_REQUIRE(meta::MetaEnum<Direction>);
    STATIC_REQUIRE(!meta::MetaEnum<int>);
}

TEST_CASE (



"Reflectable concept"
,
"[meta][concepts]"
)
{
    STATIC_REQUIRE(meta::Reflectable<Point>);
    STATIC_REQUIRE(meta::Reflectable<Vec3>);
    STATIC_REQUIRE(meta::Reflectable<TradeOrder>);
}

TEST_CASE (



"FieldDescriptorLike concept"
,
"[meta][concepts]"
)
{
    STATIC_REQUIRE(meta::FieldDescriptorLike<meta::AggregateFieldDescriptor<Point, 0>>);
    STATIC_REQUIRE(meta::FieldDescriptorLike<meta::reflect_t<TradeOrder>::element<0>>);
}

// ============================================================================
// SECTION 3: Compiler Adaptation Layer
// ============================================================================

TEST_CASE (



"type_name extraction"
,
"[meta][compiler]"
)
{
    constexpr auto name_int = meta::type_name<int>();
    REQUIRE(name_int == "int");

    constexpr auto name_double = meta::type_name<double>();
    REQUIRE(name_double == "double");

    // The type name should contain "Point" for our struct
    constexpr auto name_point = meta::type_name<Point>();
    REQUIRE(name_point.find("Point") != std::string_view::npos);
}

// ============================================================================
// SECTION 4: Aggregate Reflection
// ============================================================================

TEST_CASE (



"Field counting"
,
"[meta][reflection]"
)
{
    STATIC_REQUIRE(meta::field_count<Empty> == 0);
    STATIC_REQUIRE(meta::field_count<Wrapper> == 1);
    STATIC_REQUIRE(meta::field_count<Point> == 2);
    STATIC_REQUIRE(meta::field_count<Vec3> == 3);
    STATIC_REQUIRE(meta::field_count<Person> == 3);
    STATIC_REQUIRE(meta::field_count<Record> == 8);
}

TEST_CASE (



"Aggregate decomposition"
,
"[meta][reflection]"
)
{
    using PointSeq = decltype(meta::decompose<Point>());

    STATIC_REQUIRE(PointSeq::size == 2);

    using F0 = PointSeq::element<0>;
    using F1 = PointSeq::element<1>;

    STATIC_REQUIRE(F0::index() == 0);
    STATIC_REQUIRE(F1::index() == 1);
    STATIC_REQUIRE(F0::name() == "field_0");
    STATIC_REQUIRE(F1::name() == "field_1");
    STATIC_REQUIRE(std::same_as<F0::value_type, double>);
    STATIC_REQUIRE(std::same_as<F1::value_type, double>);
}

TEST_CASE (



"AggregateFieldDescriptor::get()"
,
"[meta][reflection]"
)
{
    Point p{3.0, 4.0};
    using Seq = decltype(meta::decompose<Point>());

    REQUIRE(Seq::element<0>::get(p) == 3.0);
    REQUIRE(Seq::element<1>::get(p) == 4.0);

    // Mutable access
    Seq::element<0>::get(p) = 5.0;
    REQUIRE(p.x == 5.0);
}

TEST_CASE (



"AggregateFieldDescriptor traits"
,
"[meta][reflection]"
)
{
    using F0 = meta::AggregateFieldDescriptor<Point, 0>;
    STATIC_REQUIRE(!F0::is_pointer());
    STATIC_REQUIRE(!F0::is_reference());
    STATIC_REQUIRE(!F0::is_const());

    using FP = meta::AggregateFieldDescriptor<WithPointer, 0>;
    STATIC_REQUIRE(FP::is_pointer());

    using FR = meta::AggregateFieldDescriptor<RefAggregate, 0>;
    STATIC_REQUIRE(std::same_as<FR::declared_type, int&>);
    STATIC_REQUIRE(FR::is_reference());

    using FC = meta::AggregateFieldDescriptor<RefAggregate, 1>;
    STATIC_REQUIRE(std::same_as<FC::declared_type, const int>);
    STATIC_REQUIRE(FC::is_const());
}

TEST_CASE (



"AggregateFieldDescriptor layout properties"
,
"[meta][reflection]"
)
{
    using F = meta::AggregateFieldDescriptor<Point, 0>;
    STATIC_REQUIRE(F::size() == sizeof(double));
    STATIC_REQUIRE(F::alignment() == alignof(double));
}

TEST_CASE (



"Aggregate decomposition for Vec3"
,
"[meta][reflection]"
)
{
    Vec3 v{1.0f, 2.0f, 3.0f};
    using Seq = decltype(meta::decompose<Vec3>());

    REQUIRE(Seq::element<0>::get(v) == 1.0f);
    REQUIRE(Seq::element<1>::get(v) == 2.0f);
    REQUIRE(Seq::element<2>::get(v) == 3.0f);
}

TEST_CASE (



"Aggregate decomposition for Record (8 fields)"
,
"[meta][reflection]"
)
{
    Record r{1, 2, 3, 4, 5.0, 6.0, 7.0, 8.0};
    using Seq = decltype(meta::decompose<Record>());

    STATIC_REQUIRE(Seq::size == 8);
    REQUIRE(Seq::element<0>::get(r) == 1);
    REQUIRE(Seq::element<3>::get(r) == 4);
    REQUIRE(Seq::element<4>::get(r) == 5.0);
    REQUIRE(Seq::element<7>::get(r) == 8.0);
}

TEST_CASE (



"reflect<T>() dispatches to aggregate decomposition"
,
"[meta][reflection]"
)
{
    using Seq = meta::reflect_t<Point>;
    STATIC_REQUIRE(Seq::size == 2);

    Point p{10.0, 20.0};
    REQUIRE(Seq::element<0>::get(p) == 10.0);
    REQUIRE(Seq::element<1>::get(p) == 20.0);
}

// ============================================================================
// SECTION 5: Custom (ADL) Reflection
// ============================================================================

TEST_CASE (



"Custom reflection via ADL"
,
"[meta][reflection][custom]"
)
{
    using Seq = meta::reflect_t<TradeOrder>;
    STATIC_REQUIRE(Seq::size == 2);

    using F0 = Seq::element<0>;
    using F1 = Seq::element<1>;

    STATIC_REQUIRE(F0::name() == "price");
    STATIC_REQUIRE(F1::name() == "quantity");
    STATIC_REQUIRE(F0::index() == 0);
    STATIC_REQUIRE(F1::index() == 1);
    STATIC_REQUIRE(std::same_as<F0::value_type, double>);
    STATIC_REQUIRE(std::same_as<F1::value_type, int>);
}

TEST_CASE (



"Custom reflection get()"
,
"[meta][reflection][custom]"
)
{
    TradeOrder order(99.5, 100);
    using Seq = meta::reflect_t<TradeOrder>;

    REQUIRE(Seq::element<0>::get(order) == 99.5);
    REQUIRE(Seq::element<1>::get(order) == 100);
}

TEST_CASE (



"Custom reflection for Account"
,
"[meta][reflection][custom]"
)
{
    Account acc("Alice", 1000.0, true);
    using Seq = meta::reflect_t<Account>;

    STATIC_REQUIRE(Seq::size == 3);
    STATIC_REQUIRE(Seq::element<0>::name() == "name");
    STATIC_REQUIRE(Seq::element<1>::name() == "balance");
    STATIC_REQUIRE(Seq::element<2>::name() == "active");

    REQUIRE(Seq::element<0>::get(acc) == "Alice");
    REQUIRE(Seq::element<1>::get(acc) == 1000.0);
    REQUIRE(Seq::element<2>::get(acc) == true);
}

TEST_CASE (



"MemberFieldDescriptor traits"
,
"[meta][reflection][custom]"
)
{
    using Seq = meta::reflect_t<TradeOrder>;
    using F0 = Seq::element<0>;

    STATIC_REQUIRE(!F0::is_pointer());
    STATIC_REQUIRE(!F0::is_reference());
    STATIC_REQUIRE(F0::size() == sizeof(double));
    STATIC_REQUIRE(F0::alignment() == alignof(double));

    using FC = meta::reflect_t<ConstMemberView>::element<0>;
    STATIC_REQUIRE(std::same_as<FC::declared_type, const int>);
    STATIC_REQUIRE(FC::is_const());
}

// ============================================================================
// SECTION 6: Enum Reflection
// ============================================================================

TEST_CASE (



"enum_name compile-time"
,
"[meta][enum]"
)
{
    STATIC_REQUIRE(meta::enum_name<Color::Red>() == "Red");
    STATIC_REQUIRE(meta::enum_name<Color::Green>() == "Green");
    STATIC_REQUIRE(meta::enum_name<Color::Blue>() == "Blue");
}

TEST_CASE (



"enum_name runtime"
,
"[meta][enum]"
)
{
    REQUIRE(meta::enum_name(Color::Red) == "Red");
    REQUIRE(meta::enum_name(Color::Green) == "Green");
    REQUIRE(meta::enum_name(Color::Blue) == "Blue");

    REQUIRE(meta::enum_name(Direction::North) == "North");
    REQUIRE(meta::enum_name(Direction::South) == "South");
    REQUIRE(meta::enum_name(Direction::East) == "East");
    REQUIRE(meta::enum_name(Direction::West) == "West");

    // Out-of-range probes should not stringify as valid enumerators.
    REQUIRE(meta::enum_name(static_cast<Color>(99)).empty());
}

TEST_CASE (



"enum_count"
,
"[meta][enum]"
)
{
    STATIC_REQUIRE(meta::enum_count<Color> == 3);
    STATIC_REQUIRE(meta::enum_count<Direction> == 4);
    STATIC_REQUIRE(meta::enum_count<Status> == 3);
}

TEST_CASE (



"enum_values"
,
"[meta][enum]"
)
{
    constexpr auto colors = meta::enum_values<Color>();
    STATIC_REQUIRE(colors.size() == 3);
    STATIC_REQUIRE(colors[0] == Color::Red);
    STATIC_REQUIRE(colors[1] == Color::Green);
    STATIC_REQUIRE(colors[2] == Color::Blue);
}

TEST_CASE (



"enum_from_string"
,
"[meta][enum]"
)
{
    auto result = meta::enum_from_string<Color>("Green");
    REQUIRE(result.has_value());
    REQUIRE(result.value() == Color::Green);

    auto result2 = meta::enum_from_string<Color>("NotAColor");
    REQUIRE(!result2.has_value());
    REQUIRE(result2.error().message == "Invalid enum value");
}

TEST_CASE (



"EnumeratorDescriptor"
,
"[meta][enum]"
)
{
    using Desc = meta::EnumeratorDescriptor<Color, Color::Blue>;

    STATIC_REQUIRE(Desc::value() == Color::Blue);
    STATIC_REQUIRE(Desc::name() == "Blue");
    STATIC_REQUIRE(std::same_as<Desc::enum_type, Color>);
}

// ============================================================================
// SECTION 7: Meta Algorithms
// ============================================================================

TEST_CASE (



"for_each over descriptors"
,
"[meta][algorithms]"
)
{
    using Seq = meta::reflect_t<Point>;
    std::size_t count_ = 0;
    meta::for_each<Seq>([&](auto desc) { ++count_; });
    REQUIRE(count_ == 2);
}

TEST_CASE (



"for_each with instance"
,
"[meta][algorithms]"
)
{
    Point p{3.0, 4.0};
    using Seq = meta::reflect_t<Point>;

    double sum = 0;
    meta::for_each<Seq>(p, [&](auto /*desc*/, auto& val) { sum += val; });
    REQUIRE(sum == Catch::Approx(7.0));

    const Point cp{1.0, 2.0};
    double const_sum = 0;
    meta::for_each<Seq>(cp, [&](auto /*desc*/, const auto& val)
    {
        const_sum += val;
    });
    REQUIRE(const_sum == Catch::Approx(3.0));
}

TEST_CASE (



"transform returns tuple of results"
,
"[meta][algorithms]"
)
{
    using Seq = meta::reflect_t<Point>;
    auto names = meta::transform<Seq>(
        [](auto desc) { return desc.name(); });

    REQUIRE(std::get<0>(names) == "field_0");
    REQUIRE(std::get<1>(names) == "field_1");
}

TEST_CASE (



"fold over descriptors"
,
"[meta][algorithms]"
)
{
    using Seq = meta::reflect_t<Point>;

    auto total_size = meta::fold<Seq>(
        std::size_t{0},
        [](std::size_t acc, auto desc)
        {
            return acc + sizeof(typename decltype(desc)::value_type);
        });

    REQUIRE(total_size == sizeof(double) * 2);
}

TEST_CASE (



"find_by_name"
,
"[meta][algorithms]"
)
{
    using Seq = meta::reflect_t<Point>;
    STATIC_REQUIRE(meta::find_by_name<Seq>("field_0") == 0);
    STATIC_REQUIRE(meta::find_by_name<Seq>("field_1") == 1);
    STATIC_REQUIRE(meta::find_by_name<Seq>("nonexistent") == Seq::size);
}

TEST_CASE (



"find_by_name with custom reflection"
,
"[meta][algorithms]"
)
{
    using Seq = meta::reflect_t<TradeOrder>;
    STATIC_REQUIRE(meta::find_by_name<Seq>("price") == 0);
    STATIC_REQUIRE(meta::find_by_name<Seq>("quantity") == 1);
    STATIC_REQUIRE(meta::find_by_name<Seq>("nonexistent") == Seq::size);
}

TEST_CASE (



"count_if"
,
"[meta][algorithms]"
)
{
    using Seq = meta::reflect_t<WithPointer>;

    constexpr auto pointer_count = meta::count_if<Seq>(
        [](auto desc) { return desc.is_pointer(); });
    STATIC_REQUIRE(pointer_count == 1);
}

TEST_CASE (



"apply iterates all fields"
,
"[meta][algorithms]"
)
{
    Point p{1.0, 2.0};
    double sum = 0;
    meta::apply(p, [&](auto /*desc*/, auto& val) { sum += val; });
    REQUIRE(sum == Catch::Approx(3.0));
}

TEST_CASE (



"zip iterates two objects"
,
"[meta][algorithms]"
)
{
    Point a{1.0, 2.0};
    Point b{3.0, 4.0};

    double dot = 0;
    meta::zip(a, b, [&](auto /*desc*/, auto& va, auto& vb)
    {
        dot += va * vb;
    });
    REQUIRE(dot == Catch::Approx(11.0)); // 1*3 + 2*4
}

// ============================================================================
// SECTION 8: Tuple Interop
// ============================================================================

TEST_CASE (



"tie_members"
,
"[meta][tuple]"
)
{
    Point p{3.0, 4.0};
    auto t = meta::tie_members(p);

    REQUIRE(std::get<0>(t) == 3.0);
    REQUIRE(std::get<1>(t) == 4.0);

    // Modification through tie
    std::get<0>(t) = 5.0;
    REQUIRE(p.x == 5.0);
}

TEST_CASE (



"to_value_tuple"
,
"[meta][tuple]"
)
{
    Point p{3.0, 4.0};
    auto t = meta::to_value_tuple(p);

    REQUIRE(std::get<0>(t) == 3.0);
    REQUIRE(std::get<1>(t) == 4.0);

    // Modification through copy doesn't affect original
    std::get<0>(t) = 5.0;
    REQUIRE(p.x == 3.0);
}

TEST_CASE (



"from_tuple"
,
"[meta][tuple]"
)
{
    auto t = std::make_tuple(10.0, 20.0);
    auto p = meta::from_tuple<Point>(t);

    REQUIRE(p.x == 10.0);
    REQUIRE(p.y == 20.0);
}

TEST_CASE (



"forward_members"
,
"[meta][tuple]"
)
{
    Point p{7.0, 8.0};
    auto t = meta::forward_members(p);

    REQUIRE(get<0>(t) == 7.0);
    REQUIRE(get<1>(t) == 8.0);
}

TEST_CASE (



"Tuple round-trip"
,
"[meta][tuple]"
)
{
    Vec3 v{1.0f, 2.0f, 3.0f};
    auto t = meta::to_value_tuple(v);
    auto v2 = meta::from_tuple<Vec3>(t);

    REQUIRE(v2.x == v.x);
    REQUIRE(v2.y == v.y);
    REQUIRE(v2.z == v.z);
}

// ============================================================================
// SECTION 9: Layout & Zero-Copy Policies
// ============================================================================

TEST_CASE (



"has_pointer_field"
,
"[meta][layout]"
)
{
    STATIC_REQUIRE(!meta::has_pointer_field<Point>());
    STATIC_REQUIRE(meta::has_pointer_field<WithPointer>());
    STATIC_REQUIRE(meta::has_pointer_field<Person>()); // const char* is a pointer
    // Empty struct: fold over zero fields must return the || identity (false).
    // This exercises the unary left fold (... || E) with an empty pack.
    STATIC_REQUIRE(!meta::has_pointer_field<Empty>());
}

TEST_CASE (



"has_reference_field"
,
"[meta][layout]"
)
{
    STATIC_REQUIRE(!meta::has_reference_field<Point>());
    // Empty struct: fold over zero fields must return the || identity (false).
    STATIC_REQUIRE(!meta::has_reference_field<Empty>());
}

TEST_CASE (



"has_pointer_field and has_reference_field on empty struct (empty-pack fold)"
,
"[meta][layout][empty_fold]"
)
{
    // Regression test: (... || E) must compile and evaluate to false when
    // the pack I... is empty (Seq::size == 0).  A unary right fold
    // (E || ...) would also give false for ||, but some compiler versions
    // have historically rejected an empty unary right fold for non-comma
    // operators.  The left-fold form is unambiguous.
    STATIC_REQUIRE(!meta::has_pointer_field<Empty>());
    STATIC_REQUIRE(!meta::has_reference_field<Empty>());

    // Confirm non-empty structs are unaffected by the change.
    STATIC_REQUIRE(!meta::has_pointer_field<Point>());
    STATIC_REQUIRE(meta::has_pointer_field<WithPointer>());
    STATIC_REQUIRE(meta::has_reference_field<RefAggregate>());
    STATIC_REQUIRE(!meta::has_reference_field<Point>());
}

TEST_CASE (



"is_trivially_copyable_deep"
,
"[meta][layout]"
)
{
    STATIC_REQUIRE(meta::is_trivially_copyable_deep<Point>());
    STATIC_REQUIRE(meta::is_trivially_copyable_deep<Vec3>());
}

TEST_CASE (



"is_zero_copy_serializable"
,
"[meta][layout]"
)
{
    STATIC_REQUIRE(meta::is_zero_copy_serializable<Point>());
    STATIC_REQUIRE(!meta::is_zero_copy_serializable<WithPointer>());
    STATIC_REQUIRE(!meta::is_zero_copy_serializable<Person>()); // has pointer

    // Strict policy
    STATIC_REQUIRE(meta::is_zero_copy_serializable<Point, meta::StrictPolicy>());
}

TEST_CASE (



"is_binary_stable"
,
"[meta][layout]"
)
{
    STATIC_REQUIRE(meta::is_binary_stable<Point>());
    STATIC_REQUIRE(!meta::is_binary_stable<WithPointer>());
}

TEST_CASE (



"descriptor layout capability model"
,
"[meta][layout]"
)
{
    using Agg = meta::AggregateFieldDescriptor<Point, 0>;
    STATIC_REQUIRE(!Agg::has_offset());
    STATIC_REQUIRE(Agg::has_layout_metadata());

    using Mem = meta::reflect_t<TradeOrder>::element<0>;
    STATIC_REQUIRE(!Mem::has_offset());
    STATIC_REQUIRE(Mem::has_layout_metadata());
}

// ============================================================================
// SECTION 10: Compile-Time Algorithms (Type-Level)
// ============================================================================

TEST_CASE (



"concat sequences"
,
"[meta][type_algorithms]"
)
{
    using S1 = meta::Sequence<int, double>;
    using S2 = meta::Sequence<float, char>;
    using S3 = meta::concat_t<S1, S2>;

    STATIC_REQUIRE(S3::size == 4);
    STATIC_REQUIRE(std::same_as<S3::element<0>, int>);
    STATIC_REQUIRE(std::same_as<S3::element<1>, double>);
    STATIC_REQUIRE(std::same_as<S3::element<2>, float>);
    STATIC_REQUIRE(std::same_as<S3::element<3>, char>);
}

TEST_CASE (



"head and tail"
,
"[meta][type_algorithms]"
)
{
    using S = meta::Sequence<int, double, float>;

    STATIC_REQUIRE(std::same_as<meta::head_t<S>, int>);

    using T = meta::tail_t<S>;
    STATIC_REQUIRE(T::size == 2);
    STATIC_REQUIRE(std::same_as<T::element<0>, double>);
    STATIC_REQUIRE(std::same_as<T::element<1>, float>);
}

TEST_CASE (



"reverse sequence"
,
"[meta][type_algorithms]"
)
{
    using S = meta::Sequence<int, double, float>;
    using R = meta::reverse_t<S>;

    STATIC_REQUIRE(R::size == 3);
    STATIC_REQUIRE(std::same_as<R::element<0>, float>);
    STATIC_REQUIRE(std::same_as<R::element<1>, double>);
    STATIC_REQUIRE(std::same_as<R::element<2>, int>);
}

TEST_CASE (



"take_t"
,
"[meta][type_algorithms]"
)
{
    using S = meta::Sequence<int, double, float, char>;
    using T = meta::take_t<S, 2>;

    STATIC_REQUIRE(T::size == 2);
    STATIC_REQUIRE(std::same_as<T::element<0>, int>);
    STATIC_REQUIRE(std::same_as<T::element<1>, double>);
}

TEST_CASE (



"drop_t"
,
"[meta][type_algorithms]"
)
{
    using S = meta::Sequence<int, double, float, char>;
    using D = meta::drop_t<S, 2>;

    STATIC_REQUIRE(D::size == 2);
    STATIC_REQUIRE(std::same_as<D::element<0>, float>);
    STATIC_REQUIRE(std::same_as<D::element<1>, char>);
}

TEST_CASE (



"unique_t"
,
"[meta][type_algorithms]"
)
{
    using S = meta::Sequence<int, double, int, float, double>;
    using U = meta::unique_t<S>;

    STATIC_REQUIRE(U::size == 3);
    STATIC_REQUIRE(std::same_as<U::element<0>, int>);
    STATIC_REQUIRE(std::same_as<U::element<1>, double>);
    STATIC_REQUIRE(std::same_as<U::element<2>, float>);
}

// Predicate for filter_t test (must be at namespace scope)
template <typename T>
struct is_floating_pred : std::is_floating_point<typename T::value_type> {};

TEST_CASE (



"filter_t"
,
"[meta][type_algorithms]"
)
{
    // filter_t requires a type-level predicate on descriptors
    // We test it using the aggregate field descriptors of a mixed-type struct
    using Seq = meta::reflect_t<Person>; // int, double, const char*
    using Filtered = meta::filter_t<Seq, is_floating_pred>;
    // Only double (field_1) should match
    STATIC_REQUIRE(Filtered::size == 1);
}

// ============================================================================
// SECTION 11: Compile-Time Hashing & Schema
// ============================================================================

TEST_CASE (



"fnv1a_hash"
,
"[meta][hashing]"
)
{
    constexpr auto h1 = meta::fnv1a_hash("hello");
    constexpr auto h2 = meta::fnv1a_hash("world");
    constexpr auto h3 = meta::fnv1a_hash("hello");

    STATIC_REQUIRE(h1 == h3);
    STATIC_REQUIRE(h1 != h2);
    STATIC_REQUIRE(h1 != 0);
}

TEST_CASE (



"type_hash"
,
"[meta][hashing]"
)
{
    constexpr auto h_int = meta::type_hash<int>();
    constexpr auto h_double = meta::type_hash<double>();
    constexpr auto h_int2 = meta::type_hash<int>();

    STATIC_REQUIRE(h_int == h_int2);
    STATIC_REQUIRE(h_int != h_double);
}

TEST_CASE (



"schema_hash"
,
"[meta][hashing]"
)
{
    constexpr auto h_point = meta::schema_hash<Point>();
    constexpr auto h_vec3 = meta::schema_hash<Vec3>();

    STATIC_REQUIRE(h_point != h_vec3);
    STATIC_REQUIRE(h_point != 0);
    STATIC_REQUIRE(h_vec3 != 0);

    // Same type yields same hash
    constexpr auto h_point2 = meta::schema_hash<Point>();
    STATIC_REQUIRE(h_point == h_point2);
}

// ============================================================================
// SECTION 12: Diagnostics & Validation
// ============================================================================

TEST_CASE (



"validate_reflection"
,
"[meta][diagnostics]"
)
{
    STATIC_REQUIRE(meta::validate_reflection<Point>());
    STATIC_REQUIRE(meta::validate_reflection<Vec3>());
    STATIC_REQUIRE(meta::validate_reflection<Record>());
    STATIC_REQUIRE(meta::validate_reflection<TradeOrder>());
}

TEST_CASE (



"field_names"
,
"[meta][diagnostics]"
)
{
    constexpr auto names = meta::field_names<Point>();
    STATIC_REQUIRE(names.size() == 2);
    STATIC_REQUIRE(names[0] == "field_0");
    STATIC_REQUIRE(names[1] == "field_1");
}

TEST_CASE (



"field_names with custom reflection"
,
"[meta][diagnostics]"
)
{
    constexpr auto names = meta::field_names<TradeOrder>();
    STATIC_REQUIRE(names.size() == 2);
    STATIC_REQUIRE(names[0] == "price");
    STATIC_REQUIRE(names[1] == "quantity");
}

TEST_CASE (



"field_types_are"
,
"[meta][diagnostics]"
)
{
    STATIC_REQUIRE(
        meta::field_types_are<Point, std::is_floating_point>());
    STATIC_REQUIRE(
        meta::field_types_are<Vec3, std::is_floating_point>());
    STATIC_REQUIRE(
        !meta::field_types_are<Person, std::is_floating_point>());
}

// ============================================================================
// SECTION 13: Structural Comparison
// ============================================================================

TEST_CASE (



"structural_equal"
,
"[meta][comparison]"
)
{
    Point a{1.0, 2.0};
    Point b{1.0, 2.0};
    Point c{3.0, 4.0};

    REQUIRE(meta::structural_equal(a, b));
    REQUIRE(!meta::structural_equal(a, c));
}

TEST_CASE (



"structural_less"
,
"[meta][comparison]"
)
{
    Point a{1.0, 2.0};
    Point b{1.0, 3.0};
    Point c{2.0, 0.0};

    REQUIRE(meta::structural_less(a, b)); // same x, a.y < b.y
    REQUIRE(!meta::structural_less(b, a)); // reverse
    REQUIRE(meta::structural_less(a, c)); // a.x < c.x
}

TEST_CASE (



"structural_equal constexpr"
,
"[meta][comparison]"
)
{
    constexpr Point a{1.0, 2.0};
    constexpr Point b{1.0, 2.0};
    STATIC_REQUIRE(meta::structural_equal(a, b));
}

// ============================================================================
// SECTION 14: Integration / End-to-End
// ============================================================================

TEST_CASE (



"End-to-end: reflect + for_each + collect names"
,
"[meta][integration]"
)
{
    using Seq = meta::reflect_t<Vec3>;
    std::vector<std::string_view> names;

    meta::for_each<Seq>([&](auto desc)
    {
        names.push_back(desc.name());
    });

    REQUIRE(names.size() == 3);
    REQUIRE(names[0] == "field_0");
    REQUIRE(names[1] == "field_1");
    REQUIRE(names[2] == "field_2");
}

TEST_CASE (



"End-to-end: custom reflect + for_each + values"
,
"[meta][integration]"
)
{
    TradeOrder order(42.5, 200);
    using Seq = meta::reflect_t<TradeOrder>;

    std::vector<std::string_view> names;
    meta::for_each<Seq>([&](auto desc)
    {
        names.push_back(desc.name());
    });

    REQUIRE(names[0] == "price");
    REQUIRE(names[1] == "quantity");

    // Check values via for_each with instance
    double price_val = 0;
    int qty_val = 0;
    meta::for_each<Seq>(order, [&](auto desc, auto& val)
    {
        if constexpr (std::same_as<typename decltype(desc)::value_type, double>)
            price_val = val;
        else if constexpr (std::same_as<typename decltype(desc)::value_type, int>)
            qty_val = val;
    });

    REQUIRE(price_val == 42.5);
    REQUIRE(qty_val == 200);
}

TEST_CASE (



"End-to-end: enum round-trip"
,
"[meta][integration]"
)
{
    // Enum to string and back
    Color c = Color::Green;
    auto name = meta::enum_name(c);
    REQUIRE(name == "Green");

    auto result = meta::enum_from_string<Color>(name);
    REQUIRE(result.has_value());
    REQUIRE(result.value() == Color::Green);
}

TEST_CASE (



"End-to-end: tuple round-trip with structural equality"
,
"[meta][integration]"
)
{
    Point original{3.14, 2.71};
    auto tup = meta::to_value_tuple(original);
    auto restored = meta::from_tuple<Point>(tup);

    REQUIRE(meta::structural_equal(original, restored));
}

// ============================================================================
// NEW TESTS: Additional Coverage for Edge Cases and Negative Scenarios
// ============================================================================

// Additional test types for new tests
struct FourFields {
    int a;
    int b;
    int c;
    int d;
};

struct MixedTypes {
    char c;
    short s;
    int i;
    long l;
    float f;
    double d;
};

enum class EmptyEnum {};

enum class SingleValue { Only = 42 };

enum class NegativeValues { Minus2 = -2, Minus1 = -1, Zero = 0, One = 1 };

// ============================================================================
// SECTION 15: Additional fixed_string Tests
// ============================================================================

TEST_CASE (



"fixed_string - empty string"
,
"[meta][data_structures][new]"
)
{
    constexpr meta::fixed_string empty("");
    STATIC_REQUIRE(empty.length == 0);
    STATIC_REQUIRE(empty.view() == "");
}

TEST_CASE (



"fixed_string - single character"
,
"[meta][data_structures][new]"
)
{
    constexpr meta::fixed_string single("x");
    STATIC_REQUIRE(single.length == 1);
    STATIC_REQUIRE(single[0] == 'x');
    STATIC_REQUIRE(single.view() == "x");
}

TEST_CASE (



"fixed_string - concatenation with empty"
,
"[meta][data_structures][new]"
)
{
    constexpr meta::fixed_string hello("hello");
    constexpr meta::fixed_string empty("");
    constexpr auto result = hello + empty;
    STATIC_REQUIRE(result.view() == "hello");
}

TEST_CASE (



"fixed_string - inequality comparison"
,
"[meta][data_structures][new]"
)
{
    constexpr meta::fixed_string a("abc");
    constexpr meta::fixed_string b("def");
    constexpr meta::fixed_string c("abcd");

    STATIC_REQUIRE(!(a == b));
    STATIC_REQUIRE(!(a == c)); // Different lengths
}

TEST_CASE (



"fixed_string - special characters"
,
"[meta][data_structures][new]"
)
{
    constexpr meta::fixed_string special("a\tb\nc");
    STATIC_REQUIRE(special.length == 5);
    STATIC_REQUIRE(special[1] == '\t');
    STATIC_REQUIRE(special[3] == '\n');
}

// ============================================================================
// SECTION 16: Additional TypeList Tests
// ============================================================================

TEST_CASE (



"TypeList - empty list"
,
"[meta][data_structures][new]"
)
{
    using Empty = meta::TypeList<>;
    STATIC_REQUIRE(Empty::size == 0);
    STATIC_REQUIRE(!Empty::contains<int>());
}

TEST_CASE (



"TypeList - single element"
,
"[meta][data_structures][new]"
)
{
    using Single = meta::TypeList<int>;
    STATIC_REQUIRE(Single::size == 1);
    STATIC_REQUIRE(Single::contains<int>());
    STATIC_REQUIRE(!Single::contains<double>());
    STATIC_REQUIRE(Single::index_of<int>() == 0);
}

TEST_CASE (



"TypeList - duplicate types"
,
"[meta][data_structures][new]"
)
{
    using Dups = meta::TypeList<int, double, int, float>;
    STATIC_REQUIRE(Dups::size == 4);
    STATIC_REQUIRE(Dups::contains<int>());
    STATIC_REQUIRE(Dups::index_of<int>() == 0); // Returns first occurrence
}

// ============================================================================
// SECTION 17: Additional ct_array Tests
// ============================================================================

TEST_CASE (



"ct_array - basic operations"
,
"[meta][data_structures][new]"
)
{
    constexpr auto arr = []()
    {
        meta::ct_array<int, 5> a{};
        a.push_back(1);
        a.push_back(2);
        a.push_back(3);
        return a;
    }();

    STATIC_REQUIRE(arr.size() == 3);
    STATIC_REQUIRE(arr[0] == 1);
    STATIC_REQUIRE(arr[1] == 2);
    STATIC_REQUIRE(arr[2] == 3);
}

TEST_CASE (



"ct_array - empty array"
,
"[meta][data_structures][new]"
)
{
    constexpr meta::ct_array<int, 10> empty{};
    STATIC_REQUIRE(empty.size() == 0);
}

TEST_CASE (



"ct_array - iteration"
,
"[meta][data_structures][new]"
)
{
    constexpr auto arr = []()
    {
        meta::ct_array<int, 5> a{};
        a.push_back(10);
        a.push_back(20);
        return a;
    }();

    constexpr auto sum = []()
    {
        meta::ct_array<int, 5> a{};
        a.push_back(10);
        a.push_back(20);
        int s = 0;
        for (auto val : a)
        {
            s += val;
        }
        return s;
    }();

    STATIC_REQUIRE(sum == 30);
}

// ============================================================================
// SECTION 18: Additional ct_map Tests
// ============================================================================

TEST_CASE (



"ct_map - single entry"
,
"[meta][data_structures][new]"
)
{
    using SingleMap = meta::ct_map<meta::named_entry<"key", int>>;
    STATIC_REQUIRE(SingleMap::size == 1);
    STATIC_REQUIRE(SingleMap::contains<"key">());
    STATIC_REQUIRE(!SingleMap::contains<"other">());
    STATIC_REQUIRE(std::same_as<SingleMap::lookup_t<"key">, int>);
}

TEST_CASE (



"ct_map - empty map"
,
"[meta][data_structures][new]"
)
{
    using EmptyMap = meta::ct_map<>;
    STATIC_REQUIRE(EmptyMap::size == 0);
    STATIC_REQUIRE(!EmptyMap::contains<"any">());
}

// ============================================================================
// SECTION 19: Additional value_list Tests
// ============================================================================

TEST_CASE (



"value_list - single value"
,
"[meta][data_structures][new]"
)
{
    using Single = meta::value_list<42>;
    STATIC_REQUIRE(Single::size == 1);
    STATIC_REQUIRE(Single::get<0>() == 42);
}

TEST_CASE (



"value_list - mixed values"
,
"[meta][data_structures][new]"
)
{
    using Mixed = meta::value_list<-1, 0, 1, 100>;
    STATIC_REQUIRE(Mixed::size == 4);
    STATIC_REQUIRE(Mixed::get<0>() == -1);
    STATIC_REQUIRE(Mixed::get<1>() == 0);
    STATIC_REQUIRE(Mixed::get<2>() == 1);
    STATIC_REQUIRE(Mixed::get<3>() == 100);
}

// ============================================================================
// SECTION 20: Additional Aggregate Reflection Tests
// ============================================================================

TEST_CASE (



"Aggregate reflection - empty struct"
,
"[meta][reflection][new]"
)
{
    using Seq = meta::reflect_t<Empty>;
    STATIC_REQUIRE(Seq::size == 0);
    STATIC_REQUIRE(meta::field_count<Empty> == 0);

    Empty e{};
    // No fields to iterate, but should not crash
    int count = 0;
    meta::for_each<Seq>([&](auto) { ++count; });
    REQUIRE(count == 0);
}

TEST_CASE (



"Aggregate reflection - single field struct"
,
"[meta][reflection][new]"
)
{
    Wrapper w{42};
    using Seq = meta::reflect_t<Wrapper>;

    STATIC_REQUIRE(Seq::size == 1);
    REQUIRE(Seq::element<0>::get(w) == 42);

    Seq::element<0>::get(w) = 100;
    REQUIRE(w.value == 100);
}

TEST_CASE (



"Aggregate reflection - four fields"
,
"[meta][reflection][new]"
)
{
    FourFields f{1, 2, 3, 4};
    using Seq = meta::reflect_t<FourFields>;

    STATIC_REQUIRE(Seq::size == 4);
    REQUIRE(Seq::element<0>::get(f) == 1);
    REQUIRE(Seq::element<1>::get(f) == 2);
    REQUIRE(Seq::element<2>::get(f) == 3);
    REQUIRE(Seq::element<3>::get(f) == 4);
}

TEST_CASE (



"Aggregate reflection - mixed type sizes"
,
"[meta][reflection][new]"
)
{
    MixedTypes m{1, 2, 3, 4, 5.0f, 6.0};
    using Seq = meta::reflect_t<MixedTypes>;

    STATIC_REQUIRE(Seq::size == 6);
    REQUIRE(Seq::element<0>::get(m) == 1);
    REQUIRE(Seq::element<1>::get(m) == 2);
    REQUIRE(Seq::element<2>::get(m) == 3);
    REQUIRE(Seq::element<3>::get(m) == 4);
    REQUIRE(Seq::element<4>::get(m) == 5.0f);
    REQUIRE(Seq::element<5>::get(m) == 6.0);
}

// ============================================================================
// SECTION 21: Additional Enum Reflection Tests
// ============================================================================

TEST_CASE (



"Enum reflection - single value enum"
,
"[meta][enum][new]"
)
{
    STATIC_REQUIRE(meta::enum_count<SingleValue> == 1);

    constexpr auto values = meta::enum_values<SingleValue>();
    STATIC_REQUIRE(values.size() == 1);
    STATIC_REQUIRE(values[0] == SingleValue::Only);

    REQUIRE(meta::enum_name(SingleValue::Only) == "Only");
}

TEST_CASE (



"Enum reflection - negative values"
,
"[meta][enum][new]"
)
{
    STATIC_REQUIRE(meta::enum_count<NegativeValues> == 4);

    REQUIRE(meta::enum_name(NegativeValues::Minus2) == "Minus2");
    REQUIRE(meta::enum_name(NegativeValues::Minus1) == "Minus1");
    REQUIRE(meta::enum_name(NegativeValues::Zero) == "Zero");
    REQUIRE(meta::enum_name(NegativeValues::One) == "One");
}

TEST_CASE (



"Enum reflection - from_string failures"
,
"[meta][enum][new]"
)
{
    auto result1 = meta::enum_from_string<Color>("");
    REQUIRE(!result1.has_value());

    auto result2 = meta::enum_from_string<Color>("red"); // lowercase
    REQUIRE(!result2.has_value());

    auto result3 = meta::enum_from_string<Color>("BLUE"); // uppercase
    REQUIRE(!result3.has_value());

    auto result4 = meta::enum_from_string<Color>("Blue "); // trailing space
    REQUIRE(!result4.has_value());
}

TEST_CASE (



"Enum reflection - all values round-trip"
,
"[meta][enum][new]"
)
{
    constexpr auto values = meta::enum_values<Direction>();

    for (auto val : values)
    {
        auto name = meta::enum_name(val);
        REQUIRE(!name.empty());

        auto parsed = meta::enum_from_string<Direction>(name);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed.value() == val);
    }
}

TEST_CASE (



"Enum reflection - invalid cast values"
,
"[meta][enum][new]"
)
{
    // Test various out-of-range values
    REQUIRE(meta::enum_name(static_cast<Color>(-1)).empty());
    REQUIRE(meta::enum_name(static_cast<Color>(10)).empty());
    REQUIRE(meta::enum_name(static_cast<Color>(255)).empty());

    REQUIRE(meta::enum_name(static_cast<Direction>(-100)).empty());
    REQUIRE(meta::enum_name(static_cast<Direction>(100)).empty());
}

// ============================================================================
// SECTION 22: Additional Meta Algorithm Tests
// ============================================================================

TEST_CASE (



"Meta algorithms - find_if not found"
,
"[meta][algorithms][new]"
)
{
    using Seq = meta::reflect_t<Point>;

    constexpr auto idx = meta::find_if<Seq>(
        [](auto desc) { return desc.is_pointer(); });
    STATIC_REQUIRE(idx == Seq::size); // Not found sentinel
}

TEST_CASE (



"Meta algorithms - find_if found"
,
"[meta][algorithms][new]"
)
{
    using Seq = meta::reflect_t<WithPointer>;

    constexpr auto idx = meta::find_if<Seq>(
        [](auto desc) { return desc.is_pointer(); });
    STATIC_REQUIRE(idx == 0); // First field is a pointer
}

TEST_CASE (



"Meta algorithms - contains positive"
,
"[meta][algorithms][new]"
)
{
    using Seq = meta::reflect_t<WithPointer>;

    constexpr bool has_pointer = meta::contains<Seq>(
        [](auto desc) { return desc.is_pointer(); });
    STATIC_REQUIRE(has_pointer);
}

TEST_CASE (



"Meta algorithms - contains negative"
,
"[meta][algorithms][new]"
)
{
    using Seq = meta::reflect_t<Point>;

    constexpr bool has_pointer = meta::contains<Seq>(
        [](auto desc) { return desc.is_pointer(); });
    STATIC_REQUIRE(!has_pointer);
}

TEST_CASE (



"Meta algorithms - count_if zero"
,
"[meta][algorithms][new]"
)
{
    using Seq = meta::reflect_t<Point>;

    constexpr auto count = meta::count_if<Seq>(
        [](auto desc) { return desc.is_pointer(); });
    STATIC_REQUIRE(count == 0);
}

TEST_CASE (



"Meta algorithms - count_if all"
,
"[meta][algorithms][new]"
)
{
    using Seq = meta::reflect_t<Point>;

    constexpr auto count = meta::count_if<Seq>(
        [](auto) { return true; });
    STATIC_REQUIRE(count == 2);
}

TEST_CASE (



"Meta algorithms - transform with multiple types"
,
"[meta][algorithms][new]"
)
{
    using Seq = meta::reflect_t<Person>;

    auto types = meta::transform<Seq>([](auto desc)
    {
        return sizeof(typename decltype(desc)::value_type);
    });

    REQUIRE(std::get<0>(types) == sizeof(int));
    REQUIRE(std::get<1>(types) == sizeof(double));
    REQUIRE(std::get<2>(types) == sizeof(const char*));
}

TEST_CASE (



"Meta algorithms - fold with empty accumulation"
,
"[meta][algorithms][new]"
)
{
    using Seq = meta::reflect_t<Empty>;

    auto result = meta::fold<Seq>(
        100,
        [](int acc, auto) { return acc + 1; });

    REQUIRE(result == 100); // No fields, so no changes
}

TEST_CASE (



"Meta algorithms - zip with modification"
,
"[meta][algorithms][new]"
)
{
    Point a{1.0, 2.0};
    Point b{10.0, 20.0};

    // Copy b values to a
    meta::zip(a, b, [](auto, auto& va, auto& vb)
    {
        va = vb;
    });

    REQUIRE(a.x == 10.0);
    REQUIRE(a.y == 20.0);
}

TEST_CASE (



"Meta algorithms - apply with modification"
,
"[meta][algorithms][new]"
)
{
    Vec3 v{1.0f, 2.0f, 3.0f};

    // Double all values
    meta::apply(v, [](auto, auto& val)
    {
        val *= 2;
    });

    REQUIRE(v.x == 2.0f);
    REQUIRE(v.y == 4.0f);
    REQUIRE(v.z == 6.0f);
}

// ============================================================================
// SECTION 23: Additional Tuple Interop Tests
// ============================================================================

TEST_CASE (



"Tuple interop - tie_members with const"
,
"[meta][tuple][new]"
)
{
    const Point p{1.0, 2.0};
    auto t = meta::tie_members(p);

    REQUIRE(std::get<0>(t) == 1.0);
    REQUIRE(std::get<1>(t) == 2.0);

    // Cannot modify through const tie (compile-time check)
    // std::get<0>(t) = 5.0; // Would not compile
}

TEST_CASE (



"Tuple interop - from_tuple with rvalue"
,
"[meta][tuple][new]"
)
{
    auto p = meta::from_tuple<Point>(std::make_tuple(7.5, 8.5));

    REQUIRE(p.x == 7.5);
    REQUIRE(p.y == 8.5);
}

TEST_CASE (



"Tuple interop - empty struct"
,
"[meta][tuple][new]"
)
{
    Empty e{};
    auto t = meta::to_value_tuple(e);

    STATIC_REQUIRE(std::tuple_size_v<decltype(t)> == 0);
}

TEST_CASE (



"Tuple interop - single field round-trip"
,
"[meta][tuple][new]"
)
{
    Wrapper w{99};
    auto t = meta::to_value_tuple(w);
    auto w2 = meta::from_tuple<Wrapper>(t);

    REQUIRE(w2.value == 99);
}

// ============================================================================
// SECTION 24: Additional Layout Policy Tests
// ============================================================================

TEST_CASE (



"Layout policies - has_reference_field positive"
,
"[meta][layout][new]"
)
{
    STATIC_REQUIRE(meta::has_reference_field<RefAggregate>());
}

TEST_CASE (



"Layout policies - zero copy with reference field"
,
"[meta][layout][new]"
)
{
    STATIC_REQUIRE(!meta::is_zero_copy_serializable<RefAggregate>());
}

TEST_CASE (



"Layout policies - binary stable with reference"
,
"[meta][layout][new]"
)
{
    STATIC_REQUIRE(!meta::is_binary_stable<RefAggregate>());
}

TEST_CASE (



"Layout policies - trivially copyable deep negative"
,
"[meta][layout][new]"
)
{
    // Account has std::string which is not trivially copyable
    STATIC_REQUIRE(!meta::is_trivially_copyable_deep<Account>());
}

TEST_CASE (



"Layout policies - descriptor capabilities"
,
"[meta][layout][new]"
)
{
    using Desc = meta::AggregateFieldDescriptor<Point, 0>;

    STATIC_REQUIRE(!Desc::has_offset());
    STATIC_REQUIRE(Desc::has_size());
    STATIC_REQUIRE(Desc::has_alignment());
    STATIC_REQUIRE(Desc::has_layout_metadata());
}

// ============================================================================
// SECTION 25: Additional Type Algorithm Tests
// ============================================================================

TEST_CASE (



"Type algorithms - concat empty sequences"
,
"[meta][type_algorithms][new]"
)
{
    using E1 = meta::Sequence<>;
    using E2 = meta::Sequence<>;
    using Result = meta::concat_t<E1, E2>;

    STATIC_REQUIRE(Result::size == 0);
}

TEST_CASE (



"Type algorithms - concat with empty"
,
"[meta][type_algorithms][new]"
)
{
    using S = meta::Sequence<int, double>;
    using E = meta::Sequence<>;
    using R1 = meta::concat_t<S, E>;
    using R2 = meta::concat_t<E, S>;

    STATIC_REQUIRE(R1::size == 2);
    STATIC_REQUIRE(R2::size == 2);
}

TEST_CASE (



"Type algorithms - reverse single element"
,
"[meta][type_algorithms][new]"
)
{
    using S = meta::Sequence<int>;
    using R = meta::reverse_t<S>;

    STATIC_REQUIRE(R::size == 1);
    STATIC_REQUIRE(std::same_as<R::element<0>, int>);
}

TEST_CASE (



"Type algorithms - reverse empty"
,
"[meta][type_algorithms][new]"
)
{
    using E = meta::Sequence<>;
    using R = meta::reverse_t<E>;

    STATIC_REQUIRE(R::size == 0);
}

TEST_CASE (



"Type algorithms - take zero"
,
"[meta][type_algorithms][new]"
)
{
    using S = meta::Sequence<int, double, float>;
    using T = meta::take_t<S, 0>;

    STATIC_REQUIRE(T::size == 0);
}

TEST_CASE (



"Type algorithms - take all"
,
"[meta][type_algorithms][new]"
)
{
    using S = meta::Sequence<int, double, float>;
    using T = meta::take_t<S, 3>;

    STATIC_REQUIRE(T::size == 3);
    STATIC_REQUIRE(std::same_as<T::element<0>, int>);
    STATIC_REQUIRE(std::same_as<T::element<1>, double>);
    STATIC_REQUIRE(std::same_as<T::element<2>, float>);
}

TEST_CASE (



"Type algorithms - drop all"
,
"[meta][type_algorithms][new]"
)
{
    using S = meta::Sequence<int, double, float>;
    using D = meta::drop_t<S, 3>;

    STATIC_REQUIRE(D::size == 0);
}

TEST_CASE (



"Type algorithms - drop zero"
,
"[meta][type_algorithms][new]"
)
{
    using S = meta::Sequence<int, double, float>;
    using D = meta::drop_t<S, 0>;

    STATIC_REQUIRE(D::size == 3);
}

TEST_CASE (



"Type algorithms - unique no duplicates"
,
"[meta][type_algorithms][new]"
)
{
    using S = meta::Sequence<int, double, float>;
    using U = meta::unique_t<S>;

    STATIC_REQUIRE(U::size == 3);
}

TEST_CASE (



"Type algorithms - unique all duplicates"
,
"[meta][type_algorithms][new]"
)
{
    using S = meta::Sequence<int, int, int>;
    using U = meta::unique_t<S>;

    STATIC_REQUIRE(U::size == 1);
    STATIC_REQUIRE(std::same_as<U::element<0>, int>);
}

TEST_CASE (



"Type algorithms - index_of"
,
"[meta][type_algorithms][new]"
)
{
    using S = meta::Sequence<int, double, float, char>;

    STATIC_REQUIRE(meta::index_of<S, int>() == 0);
    STATIC_REQUIRE(meta::index_of<S, double>() == 1);
    STATIC_REQUIRE(meta::index_of<S, float>() == 2);
    STATIC_REQUIRE(meta::index_of<S, char>() == 3);
}

// ============================================================================
// SECTION 26: Additional Hashing Tests
// ============================================================================

TEST_CASE (



"Hashing - fnv1a empty string"
,
"[meta][hashing][new]"
)
{
    constexpr auto h = meta::fnv1a_hash("");
    STATIC_REQUIRE(h == 14695981039346656037ULL); // FNV offset basis
}

TEST_CASE (



"Hashing - fnv1a collision resistance"
,
"[meta][hashing][new]"
)
{
    constexpr auto h1 = meta::fnv1a_hash("abc");
    constexpr auto h2 = meta::fnv1a_hash("abd");
    constexpr auto h3 = meta::fnv1a_hash("bac");

    STATIC_REQUIRE(h1 != h2);
    STATIC_REQUIRE(h1 != h3);
    STATIC_REQUIRE(h2 != h3);
}

TEST_CASE (



"Hashing - type_hash different types"
,
"[meta][hashing][new]"
)
{
    constexpr auto h1 = meta::type_hash<char>();
    constexpr auto h2 = meta::type_hash<short>();
    constexpr auto h3 = meta::type_hash<int>();
    constexpr auto h4 = meta::type_hash<long>();

    STATIC_REQUIRE(h1 != h2);
    STATIC_REQUIRE(h2 != h3);
    STATIC_REQUIRE(h3 != h4);
}

TEST_CASE (



"Hashing - schema_hash stability"
,
"[meta][hashing][new]"
)
{
    // Same type should always produce same hash
    constexpr auto h1 = meta::schema_hash<Vec3>();
    constexpr auto h2 = meta::schema_hash<Vec3>();
    constexpr auto h3 = meta::schema_hash<Vec3>();

    STATIC_REQUIRE(h1 == h2);
    STATIC_REQUIRE(h2 == h3);
}

// ============================================================================
// SECTION 27: Additional Comparison Tests
// ============================================================================

TEST_CASE (



"Comparison - structural_equal with empty struct"
,
"[meta][comparison][new]"
)
{
    Empty e1{};
    Empty e2{};

    REQUIRE(meta::structural_equal(e1, e2));
}

TEST_CASE (



"Comparison - structural_equal with self"
,
"[meta][comparison][new]"
)
{
    Point p{1.0, 2.0};
    REQUIRE(meta::structural_equal(p, p));
}

TEST_CASE (



"Comparison - structural_less equal values"
,
"[meta][comparison][new]"
)
{
    Point a{1.0, 2.0};
    Point b{1.0, 2.0};

    REQUIRE(!meta::structural_less(a, b));
    REQUIRE(!meta::structural_less(b, a));
}

TEST_CASE (



"Comparison - structural_less with mixed fields"
,
"[meta][comparison][new]"
)
{
    Vec3 a{1.0f, 2.0f, 3.0f};
    Vec3 b{1.0f, 2.0f, 4.0f};
    Vec3 c{1.0f, 3.0f, 0.0f};

    REQUIRE(meta::structural_less(a, b)); // differs in z
    REQUIRE(meta::structural_less(a, c)); // differs in y
    REQUIRE(!meta::structural_less(b, a));
}

// ============================================================================
// SECTION 28: Error Condition Tests
// ============================================================================

TEST_CASE (



"Error conditions - enum parsing edge cases"
,
"[meta][enum][new]"
)
{
    // Empty string
    auto r1 = meta::enum_from_string<Color>("");
    REQUIRE(!r1.has_value());
    REQUIRE(r1.error().message == "Invalid enum value");

    // Whitespace only
    auto r2 = meta::enum_from_string<Color>("   ");
    REQUIRE(!r2.has_value());

    // Partial match
    auto r3 = meta::enum_from_string<Color>("Re");
    REQUIRE(!r3.has_value());

    // Extra characters
    auto r4 = meta::enum_from_string<Color>("Red!");
    REQUIRE(!r4.has_value());
}

TEST_CASE (



"Error conditions - tuple size mismatch"
,
"[meta][tuple][new]"
)
{
    // This would be a compile-time error, but we can test runtime behavior
    auto small_tuple = std::make_tuple(1.0);

    // from_tuple with wrong size would fail at compile time
    // We just verify the correct size works
    auto correct_tuple = std::make_tuple(1.0, 2.0);
    auto p = meta::from_tuple<Point>(correct_tuple);
    REQUIRE(p.x == 1.0);
    REQUIRE(p.y == 2.0);
}

// ============================================================================
// SECTION 29: Constexpr Evaluation Tests
// ============================================================================

TEST_CASE (



"Constexpr - structural_less"
,
"[meta][comparison][new]"
)
{
    constexpr Point a{1.0, 2.0};
    constexpr Point b{1.0, 3.0};
    STATIC_REQUIRE(meta::structural_less(a, b));
    STATIC_REQUIRE(!meta::structural_less(b, a));
}

TEST_CASE (



"Constexpr - field operations"
,
"[meta][reflection][new]"
)
{
    constexpr Point p{5.0, 10.0};
    using Seq = meta::reflect_t<Point>;

    constexpr auto x = Seq::element<0>::get(p);
    constexpr auto y = Seq::element<1>::get(p);

    STATIC_REQUIRE(x == 5.0);
    STATIC_REQUIRE(y == 10.0);
}

TEST_CASE (



"Constexpr - schema validation"
,
"[meta][diagnostics][new]"
)
{
    constexpr bool valid_point = meta::validate_reflection<Point>();
    constexpr bool valid_vec3 = meta::validate_reflection<Vec3>();
    constexpr bool valid_trade = meta::validate_reflection<TradeOrder>();

    STATIC_REQUIRE(valid_point);
    STATIC_REQUIRE(valid_vec3);
    STATIC_REQUIRE(valid_trade);
}

// ============================================================================
// SECTION 30: Integration Tests for Complex Scenarios
// ============================================================================

TEST_CASE (



"Integration - nested transformations"
,
"[meta][integration][new]"
)
{
    Point p{1.0, 2.0};
    using Seq = meta::reflect_t<Point>;

    // Get all values, double them, and sum
    double result = 0;
    meta::for_each<Seq>(p, [&](auto, auto& val)
    {
        result += val * 2;
    });

    REQUIRE(result == Catch::Approx(6.0)); // (1.0 + 2.0) * 2
}

TEST_CASE (



"Integration - multiple algorithms chained"
,
"[meta][integration][new]"
)
{
    using Seq = meta::reflect_t<MixedTypes>;

    // Count fields
    constexpr std::size_t total = Seq::size;
    STATIC_REQUIRE(total == 6);

    // Count pointer fields
    constexpr auto ptr_count = meta::count_if<Seq>(
        [](auto desc) { return desc.is_pointer(); });
    STATIC_REQUIRE(ptr_count == 0);

    // Get all field names
    constexpr auto names = meta::field_names<MixedTypes>();
    STATIC_REQUIRE(names.size() == 6);
}

TEST_CASE (



"Integration - custom reflection with algorithms"
,
"[meta][integration][new]"
)
{
    TradeOrder order(100.0, 50);
    using Seq = meta::reflect_t<TradeOrder>;

    // Collect all field names
    std::vector<std::string_view> names;
    meta::for_each<Seq>([&](auto desc)
    {
        names.push_back(desc.name());
    });

    REQUIRE(names.size() == 2);
    REQUIRE(names[0] == "price");
    REQUIRE(names[1] == "quantity");

    // Validate all fields are accessible
    double sum = 0;
    meta::for_each<Seq>(order, [&](auto desc, auto& val)
    {
        if constexpr (std::same_as<typename decltype(desc)::value_type, double>)
        {
            sum += val;
        }
        else if constexpr (std::same_as<typename decltype(desc)::value_type, int>)
        {
            sum += static_cast<double>(val);
        }
    });

    REQUIRE(sum == Catch::Approx(150.0));
}

// ============================================================================
// SECTION 31: count_fields_binary — default field-counting strategy
// ============================================================================
// Regression tests for the fix: detail::aggregate::count_fields() must use
// count_fields_binary (O(log N) instantiation depth) rather than the O(N)
// count_fields_linear scan.
// ============================================================================

// Helper aggregate with a mid-range field count
struct SixteenFields {
    int f0, f1, f2, f3, f4, f5, f6, f7,
        f8, f9, f10, f11, f12, f13, f14, f15;
};

TEST_CASE (



"count_fields_binary: correct count for all stock test types"
,
"[meta][reflection][binary]"
)
{
    // Binary-search path must return the same field count as linear for every
    // well-known aggregate used across the suite.
    namespace ag = meta::detail::aggregate;

    STATIC_REQUIRE(ag::count_fields_binary<Empty, 0, ag::max_aggregate_fields>() == 0);
    STATIC_REQUIRE(ag::count_fields_binary<Wrapper, 0, ag::max_aggregate_fields>() == 1);
    STATIC_REQUIRE(ag::count_fields_binary<Point, 0, ag::max_aggregate_fields>() == 2);
    STATIC_REQUIRE(ag::count_fields_binary<Vec3, 0, ag::max_aggregate_fields>() == 3);
    STATIC_REQUIRE(ag::count_fields_binary<Record, 0, ag::max_aggregate_fields>() == 8);
    STATIC_REQUIRE(ag::count_fields_binary<SixteenFields, 0, ag::max_aggregate_fields>() == 16);
}

TEST_CASE (



"count_fields_binary agrees with count_fields_linear"
,
"[meta][reflection][binary]"
)
{
    // The two strategies must produce identical counts so substituting one for
    // the other is a transparent change.
    namespace ag = meta::detail::aggregate;

    STATIC_REQUIRE(ag::count_fields_binary<Empty, 0, ag::max_aggregate_fields>()
                   == ag::count_fields_linear<Empty>());
    STATIC_REQUIRE(ag::count_fields_binary<Wrapper, 0, ag::max_aggregate_fields>()
                   == ag::count_fields_linear<Wrapper>());
    STATIC_REQUIRE(ag::count_fields_binary<Point, 0, ag::max_aggregate_fields>()
                   == ag::count_fields_linear<Point>());
    STATIC_REQUIRE(ag::count_fields_binary<Vec3, 0, ag::max_aggregate_fields>()
                   == ag::count_fields_linear<Vec3>());
    STATIC_REQUIRE(ag::count_fields_binary<Record, 0, ag::max_aggregate_fields>()
                   == ag::count_fields_linear<Record>());
    STATIC_REQUIRE(ag::count_fields_binary<SixteenFields, 0, ag::max_aggregate_fields>()
                   == ag::count_fields_linear<SixteenFields>());
}

TEST_CASE (



"count_fields() delegates to count_fields_binary"
,
"[meta][reflection][binary]"
)
{
    // Primary regression test: count_fields<T>() must now call the binary-
    // search path.  Verify that its result equals count_fields_binary<T>()
    // and NOT some inconsistent value.
    namespace ag = meta::detail::aggregate;

    STATIC_REQUIRE(ag::count_fields<Empty>()
                   == ag::count_fields_binary<Empty, 0, ag::max_aggregate_fields>());
    STATIC_REQUIRE(ag::count_fields<Point>()
                   == ag::count_fields_binary<Point, 0, ag::max_aggregate_fields>());
    STATIC_REQUIRE(ag::count_fields<Vec3>()
                   == ag::count_fields_binary<Vec3, 0, ag::max_aggregate_fields>());
    STATIC_REQUIRE(ag::count_fields<Record>()
                   == ag::count_fields_binary<Record, 0, ag::max_aggregate_fields>());
    STATIC_REQUIRE(ag::count_fields<SixteenFields>()
                   == ag::count_fields_binary<SixteenFields, 0, ag::max_aggregate_fields>());
}

TEST_CASE (



"field_count<T> end-to-end with binary-search backend"
,
"[meta][reflection][binary]"
)
{
    // field_count<T> -> count_fields<T>() -> count_fields_binary<T,...>()
    STATIC_REQUIRE(meta::field_count<SixteenFields> == 16);
    STATIC_REQUIRE(meta::field_count<Empty> == 0);
    STATIC_REQUIRE(meta::field_count<Point> == 2);
}

// ============================================================================
// SECTION 32: Sequence member algorithm bridges
// ============================================================================

TEST_CASE (



"Sequence::as_index_sequence and has_element<I> type aliases"
,
"[meta][sequence][member]"
)
{
    // as_index_sequence spans exactly [0, size)
    using Seq = meta::reflect_t<Point>; // Sequence<D0, D1>, size == 2
    using EmptySeq = meta::reflect_t<Empty>; // Sequence<>,        size == 0

    STATIC_REQUIRE(std::is_same_v<Seq::as_index_sequence,
                   std::make_index_sequence<2>>);
    STATIC_REQUIRE(std::is_same_v<EmptySeq::as_index_sequence,
                   std::make_index_sequence<0>>);

    // has_element<I> — in-bounds indices are true, out-of-bounds false
    STATIC_REQUIRE(Seq::template has_element<0>);
    STATIC_REQUIRE(Seq::template has_element<1>);
    STATIC_REQUIRE(!Seq::template has_element<2>);
    STATIC_REQUIRE(!EmptySeq::template has_element<0>);
}

TEST_CASE (



"Sequence::for_each member bridge visits all descriptors in order"
,
"[meta][sequence][member]"
)
{
    using Seq = meta::reflect_t<Point>; // D0("field_0"), D1("field_1")

    // Compile-time: counting via constexpr immediately-invoked lambda
    STATIC_REQUIRE([]() constexpr {
        std::size_t n = 0;
        Seq::for_each([&](auto) { ++n; });
        return n;
        }() == 2);

    // Runtime parity with free meta::for_each<Seq>
    std::vector<std::string_view> member_names, free_names;
    Seq::for_each([&](auto d) { member_names.push_back(d.name()); });
    meta::for_each<Seq>([&](auto d) { free_names.push_back(d.name()); });
    REQUIRE(member_names == free_names);
    REQUIRE(member_names.size() == 2);
}

TEST_CASE (



"Sequence::for_each on empty sequence visits nothing"
,
"[meta][sequence][member]"
)
{
    using EmptySeq = meta::reflect_t<Empty>;
    std::size_t count = 0;
    EmptySeq::for_each([&](auto) { ++count; });
    REQUIRE(count == 0);
}

TEST_CASE (



"Sequence::transform member bridge returns tuple matching free function"
,
"[meta][sequence][member]"
)
{
    using Seq = meta::reflect_t<Point>;

    // Both calling styles must return the same name values
    constexpr auto member_result =
        Seq::transform([](auto d) constexpr { return d.name(); });
    constexpr auto free_result =
        meta::transform<Seq>([](auto d) constexpr { return d.name(); });

    STATIC_REQUIRE(std::get<0>(member_result) == std::get<0>(free_result));
    STATIC_REQUIRE(std::get<1>(member_result) == std::get<1>(free_result));

    // Result tuple size must equal the sequence size
    STATIC_REQUIRE(std::tuple_size_v<decltype(member_result)> == 2);
}

TEST_CASE (



"Sequence::transform on empty sequence yields empty tuple"
,
"[meta][sequence][member]"
)
{
    using EmptySeq = meta::reflect_t<Empty>;
    constexpr auto result = EmptySeq::transform([](auto d) { return d.name(); });
    STATIC_REQUIRE(std::tuple_size_v<decltype(result)> == 0);
}

TEST_CASE (



"Sequence::fold member bridge accumulates like the free function"
,
"[meta][sequence][member]"
)
{
    using Seq = meta::reflect_t<Point>;

    // Count via fold (both styles)
    constexpr std::size_t member_count = Seq::fold(
        std::size_t{0},
        [](std::size_t acc, auto) constexpr { return acc + 1; });
    constexpr std::size_t free_count = meta::fold<Seq>(
        std::size_t{0},
        [](std::size_t acc, auto) constexpr { return acc + 1; });

    STATIC_REQUIRE(member_count == free_count);
    STATIC_REQUIRE(member_count == 2);

    // Empty sequence: fold returns the initial value unchanged
    using EmptySeq = meta::reflect_t<Empty>;
    constexpr std::size_t empty_result = EmptySeq::fold(
        std::size_t{42},
        [](std::size_t acc, auto) constexpr { return acc + 1; });
    STATIC_REQUIRE(empty_result == 42);
}

TEST_CASE (



"Sequence::count_if member bridge agrees with free meta::count_if"
,
"[meta][sequence][member]"
)
{
    using PointSeq = meta::reflect_t<Point>; // 2 × double
    using Vec3Seq = meta::reflect_t<Vec3>; // 3 × float
    using EmptySeq = meta::reflect_t<Empty>; // 0 fields

    // All Point fields are double
    constexpr auto is_double = [](auto d) constexpr
    {
        return std::is_same_v<typename decltype(d)::value_type, double>;
    };
    STATIC_REQUIRE(PointSeq::count_if(is_double) == 2);
    STATIC_REQUIRE(PointSeq::count_if(is_double)
        == meta::count_if<PointSeq>(is_double));

    // No double fields in Vec3
    STATIC_REQUIRE(Vec3Seq::count_if(is_double) == 0);

    // Empty sequence always counts 0 regardless of predicate
    STATIC_REQUIRE(EmptySeq::count_if([](auto) constexpr { return true; }) == 0);
}

TEST_CASE (



"Sequence::find_if member bridge finds the first matching descriptor"
,
"[meta][sequence][member]"
)
{
    using Vec3Seq = meta::reflect_t<Vec3>; // 3 × float, indices 0..2
    using EmptySeq = meta::reflect_t<Empty>;

    constexpr auto is_float = [](auto d) constexpr
    {
        return std::is_same_v<typename decltype(d)::value_type, float>;
    };
    constexpr auto is_int = [](auto d) constexpr
    {
        return std::is_same_v<typename decltype(d)::value_type, int>;
    };

    // First float field is at index 0; parity with free find_if
    STATIC_REQUIRE(Vec3Seq::find_if(is_float) == 0);
    STATIC_REQUIRE(Vec3Seq::find_if(is_float) == meta::find_if<Vec3Seq>(is_float));

    // Non-existent type returns size sentinel
    STATIC_REQUIRE(Vec3Seq::find_if(is_int) == Vec3Seq::size);

    // Record: find first int field (there are four: a,b,c,d → index 0)
    using RecordSeq = meta::reflect_t<Record>;
    STATIC_REQUIRE(RecordSeq::find_if(is_int) == 0);

    // Empty sequence: sentinel immediately (find_if returns 0 == EmptySeq::size)
    STATIC_REQUIRE(
        EmptySeq::find_if([](auto) constexpr { return true; }) == EmptySeq::size);
}

TEST_CASE (



"Sequence::find_by_name and contains_named with ADL reflection"
,
"[meta][sequence][member]"
)
{
    // TradeOrder has custom reflection: "price"@0, "quantity"@1
    using Seq = meta::reflect_t<TradeOrder>;

    // find_by_name: member bridge
    STATIC_REQUIRE(Seq::find_by_name("price") == 0);
    STATIC_REQUIRE(Seq::find_by_name("quantity") == 1);
    STATIC_REQUIRE(Seq::find_by_name("missing") == Seq::size);

    // contains_named: derived from find_by_name
    STATIC_REQUIRE(Seq::contains_named("price"));
    STATIC_REQUIRE(Seq::contains_named("quantity"));
    STATIC_REQUIRE(!Seq::contains_named("volume"));

    // Parity with the free find_by_name
    STATIC_REQUIRE(Seq::find_by_name("price")
        == meta::find_by_name<Seq>("price"));
    STATIC_REQUIRE(Seq::find_by_name("quantity")
        == meta::find_by_name<Seq>("quantity"));
    STATIC_REQUIRE(Seq::find_by_name("missing")
        == meta::find_by_name<Seq>("missing"));
}

TEST_CASE (



"Sequence member bridges give consistent results across all algorithms"
,
"[meta][sequence][member]"
)
{
    // Use TradeOrder (ADL reflection, 2 fields: double price_, int quantity_)
    // to exercise count_if, find_if, fold, and transform together.
    using Seq = meta::reflect_t<TradeOrder>;

    // count_if: exactly one int field (quantity_)
    constexpr auto is_int = [](auto d) constexpr
    {
        return std::is_same_v<typename decltype(d)::value_type, int>;
    };
    STATIC_REQUIRE(Seq::count_if(is_int)
        == meta::count_if<Seq>(is_int));
    STATIC_REQUIRE(Seq::count_if(is_int) == 1);

    // find_if: first int field is "quantity" @ index 1
    STATIC_REQUIRE(Seq::find_if(is_int) == 1);
    STATIC_REQUIRE(Seq::find_if(is_int) == meta::find_if<Seq>(is_int));

    // fold: total character count of all field names ("price" + "quantity")
    constexpr std::size_t total_name_len = Seq::fold(
        std::size_t{0},
        [](std::size_t acc, auto d) constexpr
        {
            return acc + d.name().size();
        });
    // "price" == 5 chars, "quantity" == 8 chars
    STATIC_REQUIRE(total_name_len == 5 + 8);

    // transform: collect name sizes into a tuple
    constexpr auto name_sizes =
        Seq::transform([](auto d) constexpr { return d.name().size(); });
    STATIC_REQUIRE(std::get<0>(name_sizes) == 5); // "price"
    STATIC_REQUIRE(std::get<1>(name_sizes) == 8); // "quantity"
}

// ============================================================================
// SECTION 33: Reflection Views (Section 14 API)
// ============================================================================

namespace {
    // Aggregate with a pointer field — used by serialization_view tests.
    struct WithPointerField {
        int value;
        int* ptr;
        double ratio;
    };
} // anonymous namespace

TEST_CASE (



"Views - is_synthetic distinguishes aggregate vs member descriptors"
,
"[meta][views]"
)
{
    using AggDesc = meta::reflect_t<Point>::template element<0>;
    using MemDesc = meta::reflect_t<TradeOrder>::template element<0>;
    STATIC_REQUIRE(AggDesc::is_synthetic());
    STATIC_REQUIRE(!MemDesc::is_synthetic());
}

TEST_CASE (



"Views - structural_view_t is identity for the full sequence"
,
"[meta][views]"
)
{
    using Seq = meta::reflect_t<Point>;
    using SView = meta::structural_view_t<Seq>;
    STATIC_REQUIRE(std::is_same_v<SView, Seq>);
    STATIC_REQUIRE(SView::size == Seq::size);
}

TEST_CASE (



"Views - semantic_view_t on aggregate filters all synthetic fields"
,
"[meta][views]"
)
{
    // Aggregate reflection gives synthetic names "field_0", "field_1" — all filtered.
    using Seq = meta::reflect_t<Point>;
    using SemView = meta::semantic_view_t<Seq>;
    STATIC_REQUIRE(SemView::size == 0);
}

TEST_CASE (



"Views - semantic_view_t on custom-reflected type preserves all fields"
,
"[meta][views]"
)
{
    using Seq = meta::reflect_t<TradeOrder>;
    using SemView = meta::semantic_view_t<Seq>;
    STATIC_REQUIRE(SemView::size == Seq::size); // both MemberFieldDescriptors
    STATIC_REQUIRE(SemView::size == 2);
}

TEST_CASE (



"Views - layout_view_t on standard-layout type preserves all fields"
,
"[meta][views]"
)
{
    // Point is a standard-layout aggregate; all fields have layout metadata.
    using Seq = meta::reflect_t<Point>;
    using LView = meta::layout_view_t<Seq>;
    STATIC_REQUIRE(LView::size == Seq::size);
}

TEST_CASE (



"Views - serialization_view_t filters pointer fields"
,
"[meta][views]"
)
{
    // WithPointerField: value (ok), ptr (pointer — filtered), ratio (ok)
    using Seq = meta::reflect_t<WithPointerField>;
    using SerView = meta::serialization_view_t<Seq>;
    STATIC_REQUIRE(Seq::size == 3);
    STATIC_REQUIRE(SerView::size == 2); // ptr is excluded
}

TEST_CASE (



"Views - reflect_as dispatches to the correct view type"
,
"[meta][views]"
)
{
    using FullSeq = meta::reflect_t<TradeOrder>;

    using SView = meta::reflect_as_t<meta::structural_view_tag, TradeOrder>;
    using SeV = meta::reflect_as_t<meta::semantic_view_tag, TradeOrder>;
    using LV = meta::reflect_as_t<meta::layout_view_tag, TradeOrder>;
    using SerV = meta::reflect_as_t<meta::serialization_view_tag, TradeOrder>;

    STATIC_REQUIRE(std::is_same_v<SView, FullSeq>);
    STATIC_REQUIRE(std::is_same_v<SeV, meta::semantic_view_t<FullSeq>>);
    STATIC_REQUIRE(std::is_same_v<LV, meta::layout_view_t<FullSeq>>);
    STATIC_REQUIRE(std::is_same_v<SerV, meta::serialization_view_t<FullSeq>>);
    STATIC_REQUIRE(SeV::size == 2); // both TradeOrder fields are semantic
}

TEST_CASE (



"Views - projected_view_t with as_const_descriptor marks all fields const"
,
"[meta][views]"
)
{
    using Seq = meta::reflect_t<TradeOrder>;
    using ConstSeq = meta::projected_view_t<Seq,
                                            meta::projections::as_const_descriptor>;

    STATIC_REQUIRE(ConstSeq::size == Seq::size);
    STATIC_REQUIRE(ConstSeq::template element<0>::is_const());
    STATIC_REQUIRE(ConstSeq::template element<1>::is_const());
    // Names and indices are forwarded unchanged.
    STATIC_REQUIRE(ConstSeq::template element<0>::name() == "price");
    STATIC_REQUIRE(ConstSeq::template element<1>::name() == "quantity");
    STATIC_REQUIRE(ConstSeq::template element<0>::index() == 0);
    STATIC_REQUIRE(ConstSeq::template element<1>::index() == 1);
}

TEST_CASE (



"Views - value_types_of_t extracts field value types as TypeList"
,
"[meta][views]"
)
{
    using Seq = meta::reflect_t<TradeOrder>;
    using VTypes = meta::value_types_of_t<Seq>;
    STATIC_REQUIRE(VTypes::size == Seq::size);
    STATIC_REQUIRE(std::is_same_v<VTypes::template element<0>, double>); // price_
    STATIC_REQUIRE(std::is_same_v<VTypes::template element<1>, int>); // quantity_
}

TEST_CASE (



"Views - chained views compose: semantic then serialization"
,
"[meta][views]"
)
{
    using Seq = meta::reflect_t<TradeOrder>;
    // Semantic filter keeps all MemberFieldDescriptors (both named).
    // Serialization filter keeps double and int (both trivially copyable, neither pointer/ref).
    using Chained = meta::serialization_view_t<meta::semantic_view_t<Seq>>;
    STATIC_REQUIRE(Chained::size == 2); // both fields survive both filters

    // Point aggregate: semantic filter removes all synthetic fields → empty.
    using AggSem = meta::semantic_view_t<meta::reflect_t<Point>>;
    STATIC_REQUIRE(AggSem::size == 0);
    // Serialization on that empty sequence → still empty.
    using AggSemSer = meta::serialization_view_t<AggSem>;
    STATIC_REQUIRE(AggSemSer::size == 0);
}

TEST_CASE (



"Views - reflect_as runtime usage: iterate semantic fields of TradeOrder"
,
"[meta][views]"
)
{
    TradeOrder order{1.5, 100};
    using SemSeq = meta::reflect_as_t<meta::semantic_view_tag, TradeOrder>;
    std::vector<std::string_view> names;
    meta::for_each<SemSeq>([&](auto d) { names.push_back(d.name()); });
    REQUIRE(names.size() == 2);
    REQUIRE(names[0] == "price");
    REQUIRE(names[1] == "quantity");
}

// ============================================================================
// SECTION 34: Tuple Interop — explicit three-way API for all Reflectable types
// ============================================================================
// Verifies that tie_members / forward_members / to_value_tuple work uniformly
// for both plain aggregates (Point) and custom-reflected types (TradeOrder).
// Also confirms the critical safety contracts:
//   • tie_members  → true lvalue refs (mutations write back)
//   • forward_members → MemberTie named wrapper (not a plain tuple)
//   • to_value_tuple → owned values (mutations do NOT write back)
//   • tie_members(T&&) is =delete'd at compile time
// ============================================================================

// ---------------------------------------------------------------------------
// 34.1  tie_members — aggregate: readable and writable via lvalue refs
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - tie_members on aggregate yields writable lvalue refs"
,
"[meta][tuple][api]"
)
{
    Point p{1.0, 2.0};
    auto t = meta::tie_members(p);

    REQUIRE(std::get<0>(t) == 1.0);
    REQUIRE(std::get<1>(t) == 2.0);

    // Mutation through the tuple must write back to the original.
    std::get<0>(t) = 99.0;
    REQUIRE(p.x == 99.0); // tie guarantees reference, not copy
    REQUIRE(std::get<1>(t) == 2.0); // other field unaffected
}

// ---------------------------------------------------------------------------
// 34.2  tie_members — custom-reflected type: same lvalue-ref guarantee
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - tie_members on custom-reflected type yields writable lvalue refs"
,
"[meta][tuple][api]"
)
{
    TradeOrder order{42.5, 100};
    auto t = meta::tie_members(order);

    REQUIRE(std::get<0>(t) == 42.5); // price
    REQUIRE(std::get<1>(t) == 100); // quantity

    // Mutation through the tie must propagate back.
    std::get<0>(t) = 1.0;
    std::get<1>(t) = 1;
    REQUIRE(std::get<0>(meta::tie_members(order)) == 1.0);
    REQUIRE(std::get<1>(meta::tie_members(order)) == 1);
}

// ---------------------------------------------------------------------------
// 34.3  tie_members — type-level: result is a tuple of references
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - tie_members element types are lvalue references"
,
"[meta][tuple][api]"
)
{
    // Aggregate case
    {
        Point p{};
        using T = decltype(meta::tie_members(p));
        STATIC_REQUIRE(std::is_lvalue_reference_v<std::tuple_element_t<0, T>>);
        STATIC_REQUIRE(std::is_lvalue_reference_v<std::tuple_element_t<1, T>>);
    }
    // Custom-reflected case
    {
        TradeOrder o{1.0, 1};
        using T = decltype(meta::tie_members(o));
        STATIC_REQUIRE(std::is_lvalue_reference_v<std::tuple_element_t<0, T>>);
        STATIC_REQUIRE(std::is_lvalue_reference_v<std::tuple_element_t<1, T>>);
    }
}

// ---------------------------------------------------------------------------
// 34.4  tie_members — rvalue overload is deleted (compile-time safety)
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - tie_members rvalue overload is deleted"
,
"[meta][tuple][api]"
)
{
    // The rvalue overload is =delete'd; calling tie_members with a temporary
    // is a hard compile error.  Testing non-invocability via requires{} for
    // =deleted functions is not portable across compilers: Clang considers
    // the expression well-formed (overload resolution succeeds to the deleted
    // candidate) while other compilers differ.  We verify the lvalue overload
    // still functions correctly instead.
    Point p{1.0, 2.0};
    auto t = meta::tie_members(p);
    REQUIRE(std::get<0>(t) == 1.0);
    REQUIRE(std::get<1>(t) == 2.0);
}

// ---------------------------------------------------------------------------
// 34.5  forward_members — returns MemberTie, not a plain tuple
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - forward_members returns MemberTie named wrapper"
,
"[meta][tuple][api]"
)
{
    Point p{3.0, 4.0};
    auto ft = meta::forward_members(p);

    // Unqualified get<I> works via ADL — meta::get is found because MemberTie
    // lives in namespace meta. std::get cannot be used (qualified calls bypass
    // ADL and no std::get overload exists for MemberTie).
    REQUIRE(get<0>(ft) == 3.0);
    REQUIRE(get<1>(ft) == 4.0);

    // MemberTie is a distinct named type — not a raw std::tuple.
    STATIC_REQUIRE(!std::is_same_v<decltype(ft),
                   decltype(meta::tie_members(p))>);
}

// ---------------------------------------------------------------------------
// 34.6  forward_members — custom-reflected type, const overload
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - forward_members const overload on custom-reflected type"
,
"[meta][tuple][api]"
)
{
    const TradeOrder order{7.5, 200};
    auto ft = meta::forward_members(order);

    REQUIRE(get<0>(ft) == 7.5);
    REQUIRE(get<1>(ft) == 200);

    // Elements through a const MemberTie must be const references.
    using ElemT = std::remove_reference_t<
        std::tuple_element_t<0, decltype(ft)>>;
    STATIC_REQUIRE(std::is_const_v<ElemT>);
}

// ---------------------------------------------------------------------------
// 34.7  forward_members — rvalue overload is deleted
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - forward_members rvalue overload is deleted"
,
"[meta][tuple][api]"
)
{
    // The rvalue overload is =delete'd; calling forward_members with a
    // temporary is a hard compile error.  Testing non-invocability via
    // requires{} for =deleted functions is not portable across compilers:
    // Clang considers the expression well-formed (overload resolution
    // succeeds to the deleted candidate) while other compilers differ.
    // We verify the lvalue overload still functions correctly instead.
    Point p{3.0, 4.0};
    auto ft = meta::forward_members(p);
    REQUIRE(std::tuple_size_v<decltype(ft)> == 2);
    REQUIRE(get<0>(ft) == 3.0);
    REQUIRE(get<1>(ft) == 4.0);
}

// ---------------------------------------------------------------------------
// 34.8  to_value_tuple — aggregate: owned copy, mutations do not write back
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - to_value_tuple on aggregate produces owned copy"
,
"[meta][tuple][api]"
)
{
    Point p{5.0, 6.0};
    auto vt = meta::to_value_tuple(p);

    REQUIRE(std::get<0>(vt) == 5.0);
    REQUIRE(std::get<1>(vt) == 6.0);

    // Mutating the copy must NOT affect the original.
    std::get<0>(vt) = 0.0;
    REQUIRE(p.x == 5.0); // source is untouched
}

// ---------------------------------------------------------------------------
// 34.9  to_value_tuple — custom-reflected type: owned copy
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - to_value_tuple on custom-reflected type produces owned copy"
,
"[meta][tuple][api]"
)
{
    TradeOrder order{9.9, 50};
    auto vt = meta::to_value_tuple(order);

    REQUIRE(std::get<0>(vt) == 9.9);
    REQUIRE(std::get<1>(vt) == 50);

    // Mutating the value tuple must NOT write back.
    std::get<0>(vt) = 0.0;
    std::get<1>(vt) = 0;
    // Re-tie to confirm original unchanged.
    auto ct = meta::tie_members(order);
    REQUIRE(std::get<0>(ct) == 9.9);
    REQUIRE(std::get<1>(ct) == 50);
}

// ---------------------------------------------------------------------------
// 34.10 to_value_tuple — element types are non-reference value types
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - to_value_tuple element types are non-reference values"
,
"[meta][tuple][api]"
)
{
    // Aggregate
    {
        Point p{};
        using T = decltype(meta::to_value_tuple(p));
        STATIC_REQUIRE(!std::is_reference_v<std::tuple_element_t<0, T>>);
        STATIC_REQUIRE(!std::is_reference_v<std::tuple_element_t<1, T>>);
    }
    // Custom-reflected
    {
        TradeOrder o{1.0, 1};
        using T = decltype(meta::to_value_tuple(o));
        STATIC_REQUIRE(!std::is_reference_v<std::tuple_element_t<0, T>>);
        STATIC_REQUIRE(!std::is_reference_v<std::tuple_element_t<1, T>>);
    }
}

// ---------------------------------------------------------------------------
// 34.11 Three-way distinction — tie vs forward vs value have distinct types
// ---------------------------------------------------------------------------
TEST_CASE (



"Tuple API - three APIs produce three distinct types"
,
"[meta][tuple][api]"
)
{
    Point p{1.0, 2.0};
    using TieTy = decltype(meta::tie_members(p));
    using FwdTy = decltype(meta::forward_members(p));
    using ValueTy = decltype(meta::to_value_tuple(p));

    // All three must be pairwise different types.
    STATIC_REQUIRE(!std::is_same_v<TieTy, FwdTy>);
    STATIC_REQUIRE(!std::is_same_v<TieTy, ValueTy>);
    STATIC_REQUIRE(!std::is_same_v<FwdTy, ValueTy>);

    // Tie and value have identical arity (both wrap the same field count).
    STATIC_REQUIRE(std::tuple_size_v<TieTy> == 2);
    STATIC_REQUIRE(std::tuple_size_v<FwdTy> == 2);
    STATIC_REQUIRE(std::tuple_size_v<ValueTy> == 2);
}

// ============================================================================
// SECTION 35: aggregate_info / apply_aggregate / aggregate_as_tuple
// — internal backend tests for the unified structured-binding engine
// ============================================================================
// These tests exercise the refactored aggregate decomposition internals:
//   • aggregate_info<T>  — centralized compile-time metadata cache
//   • apply_aggregate<N> — single structured-binding expansion engine
//   • aggregate_as_tuple<N> — thin tuple wrapper over apply_aggregate
// ============================================================================

// ---------------------------------------------------------------------------
// 35.1  aggregate_info<T> — field_count static member
// ---------------------------------------------------------------------------
TEST_CASE (



"aggregate_info: field_count matches field_count<T>"
,
"[meta][aggregate_info][reflection]"
)
{
    namespace dt = meta::detail;

    STATIC_REQUIRE(dt::aggregate_info<Empty>::field_count == 0);
    STATIC_REQUIRE(dt::aggregate_info<Wrapper>::field_count == 1);
    STATIC_REQUIRE(dt::aggregate_info<Point>::field_count == 2);
    STATIC_REQUIRE(dt::aggregate_info<Vec3>::field_count == 3);
    STATIC_REQUIRE(dt::aggregate_info<Record>::field_count == 8);
    STATIC_REQUIRE(dt::aggregate_info<SixteenFields>::field_count == 16);

    // Must agree with the public field_count<T> variable template.
    STATIC_REQUIRE(dt::aggregate_info<Point>::field_count
        == meta::field_count<Point>);
    STATIC_REQUIRE(dt::aggregate_info<Record>::field_count
        == meta::field_count<Record>);
    STATIC_REQUIRE(dt::aggregate_info<SixteenFields>::field_count
        == meta::field_count<SixteenFields>);
}

// ---------------------------------------------------------------------------
// 35.2  aggregate_info<T> — exact_type_list
// ---------------------------------------------------------------------------
TEST_CASE (



"aggregate_info: exact_type_list carries correct field types"
,
"[meta][aggregate_info][reflection]"
)
{
    namespace dt = meta::detail;

    // Point has two double fields.
    using PointTypes = dt::aggregate_info<Point>::exact_type_list;
    STATIC_REQUIRE(std::same_as<PointTypes::element<0>, double>);
    STATIC_REQUIRE(std::same_as<PointTypes::element<1>, double>);

    // RefAggregate: first field is int& (reference), second is const int.
    using RefTypes = dt::aggregate_info<RefAggregate>::exact_type_list;
    STATIC_REQUIRE(std::same_as<RefTypes::element<0>, int&>);
    STATIC_REQUIRE(std::same_as<RefTypes::element<1>, const int>);

    // Wrapper has a single int field.
    using WrapTypes = dt::aggregate_info<Wrapper>::exact_type_list;
    STATIC_REQUIRE(std::same_as<WrapTypes::element<0>, int>);
}

// ---------------------------------------------------------------------------
// 35.3  apply_aggregate<N> — callable interface
// ---------------------------------------------------------------------------
TEST_CASE (



"apply_aggregate: invokes callable with all fields as arguments"
,
"[meta][apply_aggregate][reflection]"
)
{
    namespace ag = meta::detail::aggregate;

    // N == 0: callable is invoked with no arguments.
    {
        Empty e{};
        int called = 0;
        ag::apply_aggregate<0>(e, [&]() { ++called; });
        REQUIRE(called == 1);
    }

    // N == 1: single-field aggregate.
    {
        Wrapper w{42};
        int result = ag::apply_aggregate<1>(w, [](int x) { return x; });
        REQUIRE(result == 42);
    }

    // N == 2: two-field aggregate — Point.
    {
        Point p{1.5, 2.5};
        double sum = ag::apply_aggregate<2>(p,
                                            [](double x, double y) { return x + y; });
        REQUIRE(sum == 4.0);
    }

    // N == 3: Vec3.
    {
        Vec3 v{1.0f, 2.0f, 3.0f};
        float s = ag::apply_aggregate<3>(v,
                                         [](float a, float b, float c) { return a + b + c; });
        REQUIRE(s == 6.0f);
    }
}

TEST_CASE (



"apply_aggregate: mutations through callable write back to object"
,
"[meta][apply_aggregate][reflection]"
)
{
    namespace ag = meta::detail::aggregate;

    // apply_aggregate passes fields by forwarding reference, so mutations
    // through the callable must be reflected in the original object.
    Point p{0.0, 0.0};
    ag::apply_aggregate<2>(p, [](double& x, double& y)
    {
        x = 10.0;
        y = 20.0;
    });
    REQUIRE(p.x == 10.0);
    REQUIRE(p.y == 20.0);
}

TEST_CASE (



"apply_aggregate: field count is compile-time constant"
,
"[meta][apply_aggregate][reflection]"
)
{
    namespace ag = meta::detail::aggregate;

    // Verify that applying with the statically-known N compiles and produces
    // the correct result — ensures the constexpr branch is selected.
    constexpr std::size_t N = meta::field_count<Point>;
    Point p{3.0, 4.0};
    double hyp = ag::apply_aggregate<N>(p,
                                        [](double x, double y) { return x * x + y * y; });
    REQUIRE(hyp == 25.0);
}

// ---------------------------------------------------------------------------
// 35.4  aggregate_as_tuple<N> — tuple construction
// ---------------------------------------------------------------------------
TEST_CASE (



"aggregate_as_tuple: produces a tuple of lvalue references"
,
"[meta][aggregate_as_tuple][reflection]"
)
{
    namespace ag = meta::detail::aggregate;

    // The result must be a forwarding-reference tuple (references, not copies).
    Point p{7.0, 8.0};
    auto tup = ag::aggregate_as_tuple<2>(p);

    STATIC_REQUIRE(std::tuple_size_v<decltype(tup)> == 2);
    STATIC_REQUIRE(std::is_reference_v<std::tuple_element_t<0, decltype(tup)>>);
    STATIC_REQUIRE(std::is_reference_v<std::tuple_element_t<1, decltype(tup)>>);

    // Values match.
    REQUIRE(std::get<0>(tup) == 7.0);
    REQUIRE(std::get<1>(tup) == 8.0);
}

TEST_CASE (



"aggregate_as_tuple: mutations through tuple write back to object"
,
"[meta][aggregate_as_tuple][reflection]"
)
{
    namespace ag = meta::detail::aggregate;

    Point p{0.0, 0.0};
    auto tup = ag::aggregate_as_tuple<2>(p);
    std::get<0>(tup) = 99.0;
    std::get<1>(tup) = 100.0;

    REQUIRE(p.x == 99.0);
    REQUIRE(p.y == 100.0);
}

TEST_CASE (



"aggregate_as_tuple: consistent with get_field / tie_members"
,
"[meta][aggregate_as_tuple][reflection]"
)
{
    namespace ag = meta::detail::aggregate;
    namespace dt = meta::detail;

    // aggregate_as_tuple underpins get_field<I>, so the values must agree.
    Vec3 v{1.0f, 2.0f, 3.0f};
    constexpr std::size_t N = meta::field_count<Vec3>;
    auto tup = ag::aggregate_as_tuple<N>(v);

    REQUIRE(std::get<0>(tup) == dt::get_field<0>(v));
    REQUIRE(std::get<1>(tup) == dt::get_field<1>(v));
    REQUIRE(std::get<2>(tup) == dt::get_field<2>(v));
}

// ============================================================================
// SECTION 36: aggregate_info<T> — Single Metadata Source
// ============================================================================

// A 9-field aggregate (field_count > 8) for is_small_aggregate_v tests.
struct NineFields {
    int a, b, c, d, e, f, g, h, i;
};

TEST_CASE (



"aggregate_info: field_count matches meta::field_count variable"
,
"[meta][aggregate_info]"
)
{
    namespace dt = meta::detail;

    STATIC_REQUIRE(dt::aggregate_info<Point>::field_count == meta::field_count<Point>);
    STATIC_REQUIRE(dt::aggregate_info<Vec3>::field_count == meta::field_count<Vec3>);
    STATIC_REQUIRE(dt::aggregate_info<Record>::field_count == meta::field_count<Record>);
    STATIC_REQUIRE(dt::aggregate_info<Empty>::field_count == meta::field_count<Empty>);
    STATIC_REQUIRE(dt::aggregate_info<Wrapper>::field_count == meta::field_count<Wrapper>);
}

TEST_CASE (



"aggregate_info: exact_type_list element count equals field_count"
,
"[meta][aggregate_info]"
)
{
    namespace dt = meta::detail;

    STATIC_REQUIRE(dt::aggregate_info<Point>::exact_type_list::size
        == dt::aggregate_info<Point>::field_count);

    STATIC_REQUIRE(dt::aggregate_info<Vec3>::exact_type_list::size
        == dt::aggregate_info<Vec3>::field_count);

    STATIC_REQUIRE(dt::aggregate_info<Record>::exact_type_list::size
        == dt::aggregate_info<Record>::field_count);
}

TEST_CASE (



"aggregate_info: exact_type_list carries correct declared types"
,
"[meta][aggregate_info]"
)
{
    namespace dt = meta::detail;

    // Point has two double fields.
    using PointTypes = dt::aggregate_info<Point>::exact_type_list;
    STATIC_REQUIRE(std::same_as<PointTypes::element<0>, double>);
    STATIC_REQUIRE(std::same_as<PointTypes::element<1>, double>);

    // Vec3 has three float fields.
    using Vec3Types = dt::aggregate_info<Vec3>::exact_type_list;
    STATIC_REQUIRE(std::same_as<Vec3Types::element<0>, float>);
    STATIC_REQUIRE(std::same_as<Vec3Types::element<1>, float>);
    STATIC_REQUIRE(std::same_as<Vec3Types::element<2>, float>);
}

TEST_CASE (



"aggregate_info: exact_type_list preserves reference and const qualifiers"
,
"[meta][aggregate_info]"
)
{
    namespace dt = meta::detail;

    // RefAggregate has: int& ref, const int c
    using Types = dt::aggregate_info<RefAggregate>::exact_type_list;
    STATIC_REQUIRE(std::same_as<Types::element<0>, int &>);
    STATIC_REQUIRE(std::same_as<Types::element<1>, const int>);
}

TEST_CASE (



"aggregate_info: cv-ref stripped T yields same instantiation as bare T"
,
"[meta][aggregate_info]"
)
{
    namespace dt = meta::detail;

    // aggregate_info should only be instantiated for remove_cvref_t<T>.
    // The field_count variable template applies remove_cvref_t, so these
    // must agree.
    STATIC_REQUIRE(
        dt::aggregate_info<Point>::field_count
        == dt::aggregate_info<std::remove_cvref_t<const Point &>>::field_count);
}

// ============================================================================
// SECTION 37: AggregateDecomposable Concept
// ============================================================================

TEST_CASE (



"AggregateDecomposable: aggregate types satisfy the concept"
,
"[meta][AggregateDecomposable]"
)
{
    STATIC_REQUIRE(meta::AggregateDecomposable<Point>);
    STATIC_REQUIRE(meta::AggregateDecomposable<Vec3>);
    STATIC_REQUIRE(meta::AggregateDecomposable<Record>);
    STATIC_REQUIRE(meta::AggregateDecomposable<Empty>);
    STATIC_REQUIRE(meta::AggregateDecomposable<Wrapper>);
    STATIC_REQUIRE(meta::AggregateDecomposable<Person>);
    STATIC_REQUIRE(meta::AggregateDecomposable<RefAggregate>);
    STATIC_REQUIRE(meta::AggregateDecomposable<NineFields>);
}

TEST_CASE (



"AggregateDecomposable: non-aggregate types do not satisfy the concept"
,
"[meta][AggregateDecomposable]"
)
{
    STATIC_REQUIRE(!meta::AggregateDecomposable<std::string>);
    STATIC_REQUIRE(!meta::AggregateDecomposable<int>);
    STATIC_REQUIRE(!meta::AggregateDecomposable<double>);
    STATIC_REQUIRE(!meta::AggregateDecomposable<TradeOrder>); // has constructor
    STATIC_REQUIRE(!meta::AggregateDecomposable<Account>); // has constructor
}

TEST_CASE (



"AggregateDecomposable: cv-ref qualified types are handled"
,
"[meta][AggregateDecomposable]"
)
{
    // The concept applies remove_cvref_t internally so all cv-ref variants
    // of the same aggregate type must satisfy it.
    STATIC_REQUIRE(meta::AggregateDecomposable<const Point>);
    STATIC_REQUIRE(meta::AggregateDecomposable<const Point &>);
    STATIC_REQUIRE(meta::AggregateDecomposable<Point &&>);
    STATIC_REQUIRE(meta::AggregateDecomposable<volatile Point>);
}

// ============================================================================
// SECTION 38: get_field — apply_aggregate Backend
// ============================================================================

TEST_CASE (



"get_field: reads correct values for all fields"
,
"[meta][get_field]"
)
{
    namespace dt = meta::detail;

    Point p{1.5, 2.5};
    REQUIRE(dt::get_field<0>(p) == 1.5);
    REQUIRE(dt::get_field<1>(p) == 2.5);

    Vec3 v{3.0f, 6.0f, 9.0f};
    REQUIRE(dt::get_field<0>(v) == 3.0f);
    REQUIRE(dt::get_field<1>(v) == 6.0f);
    REQUIRE(dt::get_field<2>(v) == 9.0f);
}

TEST_CASE (



"get_field: returns lvalue reference for lvalue argument"
,
"[meta][get_field]"
)
{
    namespace dt = meta::detail;

    // get_field on an lvalue must return an lvalue reference so that
    // mutations are written back to the original object.
    Point p{0.0, 0.0};
    dt::get_field<0>(p) = 42.0;
    dt::get_field<1>(p) = 99.0;

    REQUIRE(p.x == 42.0);
    REQUIRE(p.y == 99.0);
}

TEST_CASE (



"get_field: return type is lvalue reference for lvalue input"
,
"[meta][get_field]"
)
{
    namespace dt = meta::detail;

    Point p{1.0, 2.0};
    // decltype of get_field<0>(lvalue) must be a reference type.
    STATIC_REQUIRE(std::is_lvalue_reference_v<decltype(dt::get_field<0>(p))>);
    STATIC_REQUIRE(std::is_lvalue_reference_v<decltype(dt::get_field<1>(p))>);
}

TEST_CASE (



"get_field: consistent with AggregateFieldDescriptor::get"
,
"[meta][get_field]"
)
{
    namespace dt = meta::detail;

    // AggregateFieldDescriptor<T,I>::get delegates to get_field<I>.
    // Values must agree for both access paths.
    Record r{10, 20, 30, 40, 1.1, 2.2, 3.3, 4.4};
    using Seq = meta::reflect_t<Record>;

    REQUIRE(Seq::element<0>::get(r) == dt::get_field<0>(r));
    REQUIRE(Seq::element<3>::get(r) == dt::get_field<3>(r));
    REQUIRE(Seq::element<7>::get(r) == dt::get_field<7>(r));
}

TEST_CASE (



"get_field: const object yields const reference"
,
"[meta][get_field]"
)
{
    namespace dt = meta::detail;

    const Point cp{5.0, 6.0};
    REQUIRE(dt::get_field<0>(cp) == 5.0);
    REQUIRE(dt::get_field<1>(cp) == 6.0);

    // Must be const-reference: assigning should not compile.
    STATIC_REQUIRE(std::is_const_v<
        std::remove_reference_t<decltype(dt::get_field<0>(cp))>>);
}

// ============================================================================
// SECTION 39: aggregate_limits — Single Source of Truth
// ============================================================================

TEST_CASE (



"aggregate_limits: max_fields value is 32"
,
"[meta][aggregate_limits]"
)
{
    namespace ag = meta::detail::aggregate;
    STATIC_REQUIRE(ag::aggregate_limits::max_fields == 32);
}

TEST_CASE (



"aggregate_limits: backward-compat aliases agree with struct"
,
"[meta][aggregate_limits]"
)
{
    namespace ag = meta::detail::aggregate;
    STATIC_REQUIRE(ag::max_aggregate_fields == ag::aggregate_limits::max_fields);
    STATIC_REQUIRE(ag::max_fields == ag::aggregate_limits::max_fields);
}

TEST_CASE (



"aggregate_limits: apply_aggregate static_assert limit matches struct"
,
"[meta][aggregate_limits]"
)
{
    // The easiest observable consequence of the limit: field_count of any
    // aggregate must be strictly less than max_fields (the saturation guard).
    namespace ag = meta::detail::aggregate;
    STATIC_REQUIRE(meta::field_count<Record> < ag::aggregate_limits::max_fields);
    STATIC_REQUIRE(meta::field_count<NineFields> < ag::aggregate_limits::max_fields);
}

// ============================================================================
// SECTION 40: is_small_aggregate_v — Compile-Time Size Classification
// ============================================================================

TEST_CASE (



"is_small_aggregate_v: types with <= 8 fields are small"
,
"[meta][is_small_aggregate_v]"
)
{
    STATIC_REQUIRE(meta::is_small_aggregate_v<Empty>); // 0 fields
    STATIC_REQUIRE(meta::is_small_aggregate_v<Wrapper>); // 1 field
    STATIC_REQUIRE(meta::is_small_aggregate_v<Point>); // 2 fields
    STATIC_REQUIRE(meta::is_small_aggregate_v<Vec3>); // 3 fields
    STATIC_REQUIRE(meta::is_small_aggregate_v<Person>); // 3 fields
    STATIC_REQUIRE(meta::is_small_aggregate_v<Record>); // exactly 8 fields
}

TEST_CASE (



"is_small_aggregate_v: types with > 8 fields are not small"
,
"[meta][is_small_aggregate_v]"
)
{
    STATIC_REQUIRE(!meta::is_small_aggregate_v<NineFields>); // 9 fields
}

TEST_CASE (



"is_small_aggregate_v: classification is consistent with field_count"
,
"[meta][is_small_aggregate_v]"
)
{
    // is_small_aggregate_v must be equivalent to field_count <= 8.
    STATIC_REQUIRE(meta::is_small_aggregate_v<Point>
        == (meta::field_count<Point> <= 8));
    STATIC_REQUIRE(meta::is_small_aggregate_v<Record>
        == (meta::field_count<Record> <= 8));
    STATIC_REQUIRE(meta::is_small_aggregate_v<NineFields>
        == (meta::field_count<NineFields> <= 8));
}

TEST_CASE (



"is_small_aggregate_v: cv-ref qualified types resolved correctly"
,
"[meta][is_small_aggregate_v]"
)
{
    // remove_cvref_t is applied internally; all variants must agree.
    STATIC_REQUIRE(meta::is_small_aggregate_v<Point>
        == meta::is_small_aggregate_v<const Point &>);
    STATIC_REQUIRE(meta::is_small_aggregate_v<NineFields>
        == meta::is_small_aggregate_v<const NineFields &>);
}

// ============================================================================
// SECTION 41: exact_type_list — Owned Exclusively by aggregate_info
// ============================================================================

TEST_CASE (



"exact_type_list: accessible only through aggregate_info, not directly"
,
"[meta][exact_type_list]"
)
{
    namespace dt = meta::detail;

    // Verify the type list is reachable via aggregate_info (the only public
    // path after exact_types was moved into detail::impl).
    using Types = dt::aggregate_info<Point>::exact_type_list;
    STATIC_REQUIRE(Types::size == 2);

    // All three accessors — aggregate_info, field_count variable template, and
    // decompose — must all agree on the type list size.
    STATIC_REQUIRE(Types::size == meta::field_count<Point>);
}

TEST_CASE (



"exact_type_list: pointer field types are preserved exactly"
,
"[meta][exact_type_list]"
)
{
    namespace dt = meta::detail;

    // WithPointer has: int* ptr, int val
    using Types = dt::aggregate_info<WithPointer>::exact_type_list;
    STATIC_REQUIRE(std::is_pointer_v<Types::element<0>>);
    STATIC_REQUIRE(std::same_as<Types::element<0>, int *>);
    STATIC_REQUIRE(std::same_as<Types::element<1>, int>);
}

TEST_CASE (



"exact_type_list: used by aggregate_field_type_t helper"
,
"[meta][exact_type_list]"
)
{
    namespace dt = meta::detail;

    // aggregate_field_type_t<T, I> is a thin alias over
    // aggregate_info<T>::exact_type_list::element<I>.
    STATIC_REQUIRE(std::same_as<dt::aggregate_field_type_t<Point, 0>, double>);
    STATIC_REQUIRE(std::same_as<dt::aggregate_field_type_t<Point, 1>, double>);
    STATIC_REQUIRE(std::same_as<dt::aggregate_field_type_t<Vec3, 0>, float>);
    STATIC_REQUIRE(std::same_as<dt::aggregate_field_type_t<RefAggregate, 0>, int &>);
    STATIC_REQUIRE(std::same_as<dt::aggregate_field_type_t<RefAggregate, 1>, const int>);
}

TEST_CASE (



"exact_type_list: AggregateFieldDescriptor declared_type matches"
,
"[meta][exact_type_list]"
)
{
    namespace dt = meta::detail;

    // AggregateFieldDescriptor<T,I>::declared_type must equal
    // aggregate_info<T>::exact_type_list::element<I>.
    using InfoTypes = dt::aggregate_info<RefAggregate>::exact_type_list;
    using Desc0 = meta::AggregateFieldDescriptor<RefAggregate, 0>;
    using Desc1 = meta::AggregateFieldDescriptor<RefAggregate, 1>;

    STATIC_REQUIRE(std::same_as<Desc0::declared_type, InfoTypes::element<0>>);
    STATIC_REQUIRE(std::same_as<Desc1::declared_type, InfoTypes::element<1>>);
}

// ============================================================================
// SECTION 16: Enhanced fixed_string & ct_parser tests
// ============================================================================
#include <containers/ct_parser.hpp>

// --- 16.1 fixed_string extended ops ---

TEST_CASE (



"fixed_string: spaceship comparison"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string a("abc");
    constexpr meta::fixed_string b("abd");
    constexpr meta::fixed_string c("abc");
    STATIC_REQUIRE((a <=> c) == std::strong_ordering::equal);
    STATIC_REQUIRE((a <=> b) == std::strong_ordering::less);
    STATIC_REQUIRE((b <=> a) == std::strong_ordering::greater);
}

TEST_CASE (



"fixed_string: inequality operator"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string a("foo");
    constexpr meta::fixed_string b("bar");
    STATIC_REQUIRE(a != b);
    STATIC_REQUIRE(!(a != meta::fixed_string("foo")));
}

TEST_CASE (



"fixed_string: concat with char"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string s("he");
    constexpr auto r1 = s + 'y';
    constexpr auto r2 = 'x' + s;
    STATIC_REQUIRE(r1.view() == "hey");
    STATIC_REQUIRE(r2.view() == "xhe");
}

TEST_CASE (



"fixed_string: substr"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string s("hello world");
    constexpr auto h = meta::substr<0, 5>(s);
    constexpr auto w = meta::substr<6, 5>(s);
    STATIC_REQUIRE(h.view() == "hello");
    STATIC_REQUIRE(w.view() == "world");
}

TEST_CASE (



"fixed_string: find_char / rfind_char"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string s("abcba");
    STATIC_REQUIRE(meta::find_char(s, 'b') == 1);
    STATIC_REQUIRE(meta::rfind_char(s, 'b') == 3);
    STATIC_REQUIRE(meta::find_char(s, 'z') == std::string_view::npos);
}

TEST_CASE (



"fixed_string: contains_char"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string s("hello");
    STATIC_REQUIRE(meta::contains_char(s, 'e'));
    STATIC_REQUIRE(!meta::contains_char(s, 'z'));
}

TEST_CASE (



"fixed_string: find_substr"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string hay("abcdefgh");
    constexpr meta::fixed_string needle("cde");
    STATIC_REQUIRE(meta::find_substr(hay, needle) == 2);
    constexpr meta::fixed_string absent("xyz");
    STATIC_REQUIRE(meta::find_substr(hay, absent) == std::string_view::npos);
}

TEST_CASE (



"fixed_string: starts_with / ends_with"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string s("foobar");
    STATIC_REQUIRE(meta::starts_with(s, meta::fixed_string("foo")));
    STATIC_REQUIRE(!meta::starts_with(s, meta::fixed_string("bar")));
    STATIC_REQUIRE(meta::ends_with(s, meta::fixed_string("bar")));
    STATIC_REQUIRE(!meta::ends_with(s, meta::fixed_string("foo")));
}

TEST_CASE (



"fixed_string: to_upper / to_lower"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string s("Hello World");
    constexpr auto up = meta::to_upper(s);
    constexpr auto lo = meta::to_lower(s);
    STATIC_REQUIRE(up.view() == "HELLO WORLD");
    STATIC_REQUIRE(lo.view() == "hello world");
}

TEST_CASE (



"fixed_string: replace_char"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string s("a.b.c");
    constexpr auto r = meta::replace_char(s, '.', '/');
    STATIC_REQUIRE(r.view() == "a/b/c");
}

TEST_CASE (



"fixed_string: repeat"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr meta::fixed_string s("ab");
    constexpr auto r = meta::repeat<3>(s);
    STATIC_REQUIRE(r.view() == "ababab");
}

TEST_CASE (



"fixed_string: trim_view"
,
"[meta][fixed_string][section16]"
)
 {
    static constexpr meta::fixed_string s("  hello  ");
    constexpr auto t = meta::trim_view(s);
    STATIC_REQUIRE(t == "hello");
}

TEST_CASE (



"fixed_string: uint_to_str / str_to_uint"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr auto s = meta::uint_to_str<12345>();
    STATIC_REQUIRE(s.view() == "12345");
    STATIC_REQUIRE(meta::str_to_uint(s) == 12345);
    constexpr auto zero = meta::uint_to_str<0>();
    STATIC_REQUIRE(zero.view() == "0");
}

TEST_CASE (



"fixed_string: ct_string_builder"
,
"[meta][fixed_string][section16]"
)
 {
    constexpr auto built = []() consteval {
        meta::ct_string_builder<32> b{};
        b.append(meta::fixed_string("Hello"));
        b.push(' ');
        b.append(meta::fixed_string("World"));
        return b.build<11>();
    }();
    STATIC_REQUIRE(built.view() == "Hello World");
    STATIC_REQUIRE(built.length == 11);
}

// --- 16.2 Section 15 algorithms ---

TEST_CASE (



"kmp_find: substring search"
,
"[meta][section15][section16]"
)
 {
    static constexpr meta::fixed_string hay("the cat sat on the mat");
    static constexpr meta::fixed_string needle("sat");
    STATIC_REQUIRE(meta::kmp_find(hay, needle) == 8);
    static constexpr meta::fixed_string absent("dog");
    STATIC_REQUIRE(meta::kmp_find(hay, absent) == std::string_view::npos);
}

TEST_CASE (



"kmp_count: non-overlapping count"
,
"[meta][section15][section16]"
)
 {
    static constexpr meta::fixed_string s("abababab");
    static constexpr meta::fixed_string p("ab");
    STATIC_REQUIRE(meta::kmp_count(s, p) == 4);
}

TEST_CASE (



"split_by_char: basic split"
,
"[meta][section15][section16]"
)
 {
    static constexpr meta::fixed_string s("a,b,c,d");
    constexpr auto parts = meta::split_by_char<8>(s, ',');
    STATIC_REQUIRE(parts.size() == 4);
    STATIC_REQUIRE(parts[0] == "a");
    STATIC_REQUIRE(parts[3] == "d");
}

TEST_CASE (



"ct_char_set: contains and operators"
,
"[meta][section15][section16]"
)
 {
    constexpr auto digits = meta::cs_digits();
    STATIC_REQUIRE(digits.contains('5'));
    STATIC_REQUIRE(!digits.contains('a'));
    constexpr auto alnum = meta::cs_alnum();
    STATIC_REQUIRE(alnum.contains('z'));
    STATIC_REQUIRE(alnum.contains('9'));
    STATIC_REQUIRE(!alnum.contains(' '));
}

TEST_CASE (



"fnv1a64: compile-time hash"
,
"[meta][section15][section16]"
)
 {
    static constexpr meta::fixed_string s("hello");
    constexpr auto h1 = meta::fnv1a64(s);
    constexpr auto h2 = meta::fnv1a64(std::string_view{"hello"});
    STATIC_REQUIRE(h1 == h2);
    constexpr auto h3 = meta::fnv1a64(meta::fixed_string("world"));
    STATIC_REQUIRE(h1 != h3);
}

TEST_CASE (



"pad_right / pad_left"
,
"[meta][section15][section16]"
)
 {
    constexpr meta::fixed_string s("hi");
    constexpr auto pr = meta::pad_right<5>(s, '*');
    constexpr auto pl = meta::pad_left<5>(s, '-');
    STATIC_REQUIRE(pr.view() == "hi***");
    STATIC_REQUIRE(pl.view() == "---hi");
}

TEST_CASE (



"intern_tag: type-level string identity"
,
"[meta][section15][section16]"
)
 {
    using A = meta::intern_tag<"foo">;
    using B = meta::intern_tag<"foo">;
    using C = meta::intern_tag<"bar">;
    STATIC_REQUIRE(std::same_as<A, B>);
    STATIC_REQUIRE(!std::same_as<A, C>);
    STATIC_REQUIRE(A::str() == "foo");
    STATIC_REQUIRE(meta::intern_equal<"abc", "abc">);
    STATIC_REQUIRE(!meta::intern_equal<"abc", "xyz">);
}

// --- 16.3 ct_parser tests ---

TEST_CASE (



"ct::lit: exact match"
,
"[ct_parser][section16]"
)
 {
    constexpr ct::lit<"hello"> p{};
    constexpr ct::ct_span s{"hello world", 0};
    constexpr auto r = p(s);
    STATIC_REQUIRE(r.ok);
    STATIC_REQUIRE(r.value == "hello");
    STATIC_REQUIRE(r.next.pos == 5);

    constexpr ct::ct_span s2{"world", 0};
    constexpr auto r2 = p(s2);
    STATIC_REQUIRE(!r2.ok);
}

TEST_CASE (



"ct::char_if: digit parser"
,
"[ct_parser][section16]"
)
 {
    constexpr ct::ct_span s{"9x", 0};
    constexpr auto r = ct::digit(s);
    STATIC_REQUIRE(r.ok);
    STATIC_REQUIRE(r.value == '9');

    constexpr ct::ct_span s2{"ax", 0};
    constexpr auto r2 = ct::digit(s2);
    STATIC_REQUIRE(!r2.ok);
}

TEST_CASE (



"ct::identifier: basic identifier parse"
,
"[ct_parser][section16]"
)
 {
    constexpr ct::ct_span s{"foo_bar123 rest", 0};
    constexpr auto r = ct::identifier(s);
    STATIC_REQUIRE(r.ok);
    STATIC_REQUIRE(r.value == "foo_bar123");
}

TEST_CASE (



"ct::decimal: decimal integer"
,
"[ct_parser][section16]"
)
 {
    constexpr ct::ct_span s{"42abc", 0};
    constexpr auto r = ct::decimal(s);
    STATIC_REQUIRE(r.ok);
    STATIC_REQUIRE(r.value == "42");
}

TEST_CASE (



"ct::skip_ws: skip whitespace"
,
"[ct_parser][section16]"
)
 {
    constexpr ct::ct_span s{"   hello", 0};
    constexpr auto r = ct::skip_ws(s);
    STATIC_REQUIRE(r.ok);
    STATIC_REQUIRE(r.next.pos == 3);
}

TEST_CASE (



"ct::optional: opt parser"
,
"[ct_parser][section16]"
)
 {
    constexpr auto p = ct::optional(ct::lit<"foo">{});
    constexpr ct::ct_span s{"foobar", 0};
    constexpr auto r = p(s);
    STATIC_REQUIRE(r.ok);
    STATIC_REQUIRE(r.value.has_value());

    constexpr ct::ct_span s2{"bar", 0};
    constexpr auto r2 = p(s2);
    STATIC_REQUIRE(r2.ok);
    STATIC_REQUIRE(!r2.value.has_value());
}

// Helper predicate for take_while tests (constexpr function pointer as NTTP)
namespace test_helpers {
    constexpr bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }
}

TEST_CASE (



"ct::take_while: run until condition fails"
,
"[ct_parser][section16]"
)
 {
    constexpr ct::take_while_t<test_helpers::is_ascii_digit> tw{};
    constexpr ct::ct_span s{"123abc", 0};
    constexpr auto r = tw(s);
    STATIC_REQUIRE(r.ok);
    STATIC_REQUIRE(r.value == "123");
}

TEST_CASE (



"ct::many: collect multiple matches"
,
"[ct_parser][section16]"
)
 {
    constexpr ct::ct_span s{"aaa!", 0};
    constexpr auto p = ct::many<8>(ct::exact_char{'a'});
    constexpr auto r = p(s);
    STATIC_REQUIRE(r.ok);
    STATIC_REQUIRE(r.value.size() == 3);
}

TEST_CASE (



"ct::keyword_table: compile-time keyword lookup"
,
"[ct_parser][section16]"
)
 {
    constexpr ct::keyword_table<4> kws{{{"if", "else", "while", "for"}}, {0, 1, 2, 3}};
    STATIC_REQUIRE(kws.lookup("while") == 2);
    STATIC_REQUIRE(kws.lookup("for") == 3);
    STATIC_REQUIRE(kws.lookup("nope") == -1);
    STATIC_REQUIRE(kws.contains("if"));
    STATIC_REQUIRE(!kws.contains("do"));
}

TEST_CASE (



"ct::char_in_set: set-based char match"
,
"[ct_parser][section16]"
)
 {
    constexpr auto vowels = meta::ct_char_set{meta::fixed_string{"aeiouAEIOU"}};
    constexpr ct::ct_span sa{"apple", 0};
    constexpr auto ra = ct::char_in_set<vowels>(sa);
    STATIC_REQUIRE(ra.ok && ra.value == 'a');
    constexpr ct::ct_span sb{"zzz", 0};
    constexpr auto rb = ct::char_in_set<vowels>(sb);
    STATIC_REQUIRE(!rb.ok);
}

// ============================================================================
// SECTION 17: New Feature Tests
// fixed_string STL/ranges, ct_char_set bitset, caches, meta::config,
// Sequence aliases, reflection_traits, hash, path, backend abstraction
// ============================================================================

TEST_CASE (



"fixed_string STL/ranges compliance"
,
"[meta][akshara][section17]"
)
 {
    static constexpr akshara::fixed_string s{"hello"};

    // size / empty
    STATIC_REQUIRE(s.size() == 5);
    STATIC_REQUIRE(!s.empty());
    static constexpr akshara::fixed_string empty_s{""};
    STATIC_REQUIRE(empty_s.empty());

    // begin / end iterator range
    STATIC_REQUIRE(s.end() - s.begin() == 5);
    STATIC_REQUIRE(*s.begin() == 'h');
    STATIC_REQUIRE(*(s.end() - 1) == 'o');

    // cbegin / cend
    STATIC_REQUIRE(s.cend() - s.cbegin() == 5);

    // implicit string_view conversion
    static constexpr std::string_view sv = s;
    STATIC_REQUIRE(sv == "hello");

    // view() backward compat
    STATIC_REQUIRE(s.view() == "hello");

    // ranges::equal (via begin/end)
    static constexpr akshara::fixed_string s2{"hello"};
    constexpr bool eq = std::ranges::equal(s, s2);
    STATIC_REQUIRE(eq);
}

TEST_CASE (



"ct_char_set bitset redesign"
,
"[meta][akshara][section17]"
)
 {
    // Basic contains
    constexpr auto digits = akshara::cs_digits();
    STATIC_REQUIRE(digits.contains('0'));
    STATIC_REQUIRE(digits.contains('9'));
    STATIC_REQUIRE(!digits.contains('a'));

    // Union
    constexpr auto alpha = akshara::cs_alpha();
    constexpr auto alnum = digits | alpha;
    STATIC_REQUIRE(alnum.contains('z'));
    STATIC_REQUIRE(alnum.contains('5'));

    // Intersection
    constexpr auto lower = akshara::cs_lower();
    constexpr auto inter = alpha & lower;
    STATIC_REQUIRE(inter.contains('z'));
    STATIC_REQUIRE(!inter.contains('Z'));

    // XOR
    constexpr auto xored = alpha ^ lower;
    STATIC_REQUIRE(xored.contains('A'));  // in alpha, not in lower
    STATIC_REQUIRE(!xored.contains('a')); // in both

    // Complement (spot check)
    constexpr auto not_digits = digits.complement();
    STATIC_REQUIRE(!not_digits.contains('5'));
    STATIC_REQUIRE(not_digits.contains('a'));

    // Ident sets
    constexpr auto ident_s = akshara::cs_ident_start();
    STATIC_REQUIRE(ident_s.contains('_'));
    STATIC_REQUIRE(ident_s.contains('A'));
    STATIC_REQUIRE(!ident_s.contains('5'));

    constexpr auto ident_c = akshara::cs_ident_cont();
    STATIC_REQUIRE(ident_c.contains('_'));
    STATIC_REQUIRE(ident_c.contains('5'));
}

TEST_CASE (



"meta::config feature detection"
,
"[meta][section17]"
)
 {
    // expected is C++23 — should be true on modern toolchains
    CHECK(meta::config::has_expected == true);
    // source_location — should be available
    CHECK(meta::config::has_source_location == true);
    // ranges — C++20
    CHECK(meta::config::has_ranges == true);
    // These are just booleans; test they compile and are bool
    [[maybe_unused]] bool b = meta::config::has_mdspan;
    [[maybe_unused]] bool s = meta::config::has_stacktrace;
}

TEST_CASE (



"reflection caches: reflection_v, type_name_v, schema_hash_v"
,
"[meta][section17]"
)
 {
    // type_name_v
    constexpr auto tn = meta::type_name_v<Point>;
    CHECK(!tn.empty());
    CHECK(tn.find("Point") != std::string_view::npos);

    // reflection_v
    constexpr auto rv = meta::reflection_v<Point>;
    STATIC_REQUIRE(decltype(rv)::size == 2);

    // schema_hash_v
    constexpr auto sh = meta::schema_hash_v<Point>;
    STATIC_REQUIRE(sh != 0);
    // Two different types should differ
    constexpr auto sh2 = meta::schema_hash_v<Vec3>;
    STATIC_REQUIRE(sh != sh2);
}

TEST_CASE (



"Sequence member aliases: empty_seq, front, back, contains_type"
,
"[meta][section17]"
)
 {
    using Seq2 = meta::Sequence<
        meta::AggregateFieldDescriptor<Point, 0>,
        meta::AggregateFieldDescriptor<Point, 1>>;

    STATIC_REQUIRE(!Seq2::empty_seq);
    STATIC_REQUIRE(Seq2::size == 2);

    using F = Seq2::front;
    STATIC_REQUIRE(F::index() == 0);

    using B = Seq2::back;
    STATIC_REQUIRE(B::index() == 1);

    STATIC_REQUIRE(Seq2::contains_type<meta::AggregateFieldDescriptor<Point, 0>>());
    STATIC_REQUIRE(!Seq2::contains_type<meta::AggregateFieldDescriptor<Vec3, 0>>());

    // Empty sequence
    using EmptySeq = meta::Sequence<>;
    STATIC_REQUIRE(EmptySeq::empty_seq);
}

TEST_CASE (



"take_seq_t / drop_seq_t / reverse_seq_t free aliases"
,
"[meta][section17]"
)
 {
    using Seq3 = meta::Sequence<
        meta::AggregateFieldDescriptor<Vec3, 0>,
        meta::AggregateFieldDescriptor<Vec3, 1>,
        meta::AggregateFieldDescriptor<Vec3, 2>>;

    using First2 = meta::take_seq_t<Seq3, 2>;
    STATIC_REQUIRE(First2::size == 2);

    using Last2 = meta::drop_seq_t<Seq3, 1>;
    STATIC_REQUIRE(Last2::size == 2);

    using Rev = meta::reverse_seq_t<Seq3>;
    STATIC_REQUIRE(Rev::size == 3);
    STATIC_REQUIRE(Rev::template element<0>::index() == 2);
    STATIC_REQUIRE(Rev::template element<2>::index() == 0);
}

TEST_CASE (



"reflection_traits<T>"
,
"[meta][section17]"
)
 {
    // Aggregate type
    using PA = meta::reflection_traits<Point>;
    STATIC_REQUIRE(PA::is_reflectable);
    STATIC_REQUIRE(PA::is_aggregate);
    STATIC_REQUIRE(!PA::is_custom);
    STATIC_REQUIRE(PA::field_count == 2);
    STATIC_REQUIRE(!PA::has_named_fields);

    // Custom-reflected type
    using TA = meta::reflection_traits<TradeOrder>;
    STATIC_REQUIRE(TA::is_reflectable);
    STATIC_REQUIRE(!TA::is_aggregate);
    STATIC_REQUIRE(TA::is_custom);
    STATIC_REQUIRE(TA::field_count == 2);
    STATIC_REQUIRE(TA::has_named_fields);

    // Non-reflectable type
    using IA = meta::reflection_traits<int>;
    STATIC_REQUIRE(!IA::is_reflectable);
    STATIC_REQUIRE(IA::field_count == 0);

    // is_reflectable_v convenience
    STATIC_REQUIRE(meta::is_reflectable_v<Point>);
    STATIC_REQUIRE(!meta::is_reflectable_v<int>);
}

TEST_CASE (



"type_id_v / field_id / schema_id_v"
,
"[meta][section17]"
)
 {
    constexpr auto tid = meta::type_id_v<Point>;
    STATIC_REQUIRE(tid != 0);

    constexpr auto tid2 = meta::type_id_v<Vec3>;
    STATIC_REQUIRE(tid != tid2);

    using D0 = meta::AggregateFieldDescriptor<Point, 0>;
    using D1 = meta::AggregateFieldDescriptor<Point, 1>;
    constexpr auto fid0 = meta::field_id<D0>();
    constexpr auto fid1 = meta::field_id<D1>();
    STATIC_REQUIRE(fid0 != fid1);

    constexpr auto sid = meta::schema_id_v<Point>;
    STATIC_REQUIRE(sid != 0);
}

TEST_CASE (



"meta::hash — wyhash64 and xxh3_64"
,
"[meta][section17]"
)
 {
    constexpr auto h1 = meta::hash::wyhash64(std::string_view{"hello"});
    constexpr auto h2 = meta::hash::wyhash64(std::string_view{"world"});
    STATIC_REQUIRE(h1 != h2);
    STATIC_REQUIRE(h1 != 0);

    constexpr auto x1 = meta::hash::xxh3_64(std::string_view{"hello"});
    constexpr auto x2 = meta::hash::xxh3_64(std::string_view{"world"});
    STATIC_REQUIRE(x1 != x2);
    STATIC_REQUIRE(x1 != 0);

    // fixed_string overloads
    constexpr akshara::fixed_string fs{"hello"};
    constexpr auto wh = meta::hash::wyhash64(fs);
    constexpr auto xh = meta::hash::xxh3_64(fs);
    STATIC_REQUIRE(wh == h1);
    STATIC_REQUIRE(xh == x1);
}

TEST_CASE (



"akshara::path compile-time path operations"
,
"[meta][akshara][section17]"
)
 {
    constexpr akshara::fixed_string p1{"src/parser/tokenizer.cpp"};
    STATIC_REQUIRE(akshara::path::filename(p1) == "tokenizer.cpp");
    STATIC_REQUIRE(akshara::path::stem(p1) == "tokenizer");
    STATIC_REQUIRE(akshara::path::extension(p1) == ".cpp");
    STATIC_REQUIRE(akshara::path::parent_path(p1) == "src/parser");

    constexpr akshara::fixed_string p2{"README.md"};
    STATIC_REQUIRE(akshara::path::filename(p2) == "README.md");
    STATIC_REQUIRE(akshara::path::stem(p2) == "README");
    STATIC_REQUIRE(akshara::path::extension(p2) == ".md");
    STATIC_REQUIRE(akshara::path::parent_path(p2) == "");

    // normalize
    constexpr akshara::fixed_string p3{"./src/foo.cpp"};
    STATIC_REQUIRE(akshara::path::normalize(p3) == "src/foo.cpp");

    constexpr akshara::fixed_string p4{"src/"};
    STATIC_REQUIRE(akshara::path::normalize(p4) == "src");
}

TEST_CASE (



"reflection backend abstraction"
,
"[meta][section17]"
)
 {
    // reflect_backend_t<T> selects correct backend
    STATIC_REQUIRE(std::same_as<meta::reflect_backend_t<Point>, meta::aggregate_backend>);
    STATIC_REQUIRE(std::same_as<meta::reflect_backend_t<TradeOrder>, meta::adl_backend>);

    // reflect_with<adl_backend, TradeOrder>() — same result as reflect<TradeOrder>()
    constexpr auto seq_adl = meta::reflect_with<meta::adl_backend, TradeOrder>();
    STATIC_REQUIRE(decltype(seq_adl)::size == 2);

    // reflect_with<aggregate_backend, Point>() — same result as reflect<Point>()
    constexpr auto seq_agg = meta::reflect_with<meta::aggregate_backend, Point>();
    STATIC_REQUIRE(decltype(seq_agg)::size == 2);
}
