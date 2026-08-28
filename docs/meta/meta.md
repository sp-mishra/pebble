# `meta.hpp` — C++23 Reflection & Compile-Time Metaprogramming System

> **Header:** `include/utils/meta.hpp`  
> **Namespace:** `meta`  
> **Standard required:** C++23 (`-std=c++2b`)  
> **Dependencies:** Standard library only (`<tuple>`, `<type_traits>`, `<array>`, `<expected>`, …)

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Philosophy](#2-philosophy)
3. [Executive Summary](#3-executive-summary)
4. [Public-Facing API](#4-public-facing-api)
5. [Detailed Usage with Examples](#5-detailed-usage-with-examples)
    - 5.1 [Compile-Time Data Structures](#51-compile-time-data-structures)
    - 5.2 [Concepts & Traits](#52-concepts--traits)
    - 5.3 [Type Names](#53-type-names)
    - 5.4 [Aggregate Reflection](#54-aggregate-reflection)
    - 5.5 [Custom ADL Reflection](#55-custom-adl-reflection)
    - 5.6 [Enum Reflection](#56-enum-reflection)
    - 5.7 [Meta Algorithms](#57-meta-algorithms)
    - 5.8 [Tuple Interop](#58-tuple-interop)
    - 5.9 [Layout & Serialisation Policies](#59-layout--serialisation-policies)
    - 5.10 [Compile-Time Sequence Algorithms](#510-compile-time-sequence-algorithms)
    - 5.11 [Hashing & Schema Fingerprinting](#511-hashing--schema-fingerprinting)
    - 5.12 [Diagnostics & Validation](#512-diagnostics--validation)
    - 5.13 [Structural Comparison](#513-structural-comparison)
    - 5.14 [Reflection Views & Projections](#514-reflection-views--projections)
6. [New in This Release](#6-new-in-this-release)
    - 6.1 [Feature Detection — meta::config](#61-feature-detection--metaconfg)
    - 6.2 [Reflection Cache Variables](#62-reflection-cache-variables)
    - 6.3 [source_location Diagnostics](#63-source_location-diagnostics)
    - 6.4 [Sequence Modernization](#64-sequence-modernization)
    - 6.5 [reflection_traits\<T\>](#65-reflection_traitst)
    - 6.6 [Compile-Time IDs](#66-compile-time-ids)
    - 6.7 [Advanced Hashing — meta::hash](#67-advanced-hashing--metahash)
    - 6.8 [Reflection Backend Abstraction](#68-reflection-backend-abstraction)
7. [Design Details](#7-design-details)
8. [Gaps & Future Work](#8-gaps--future-work)

---

## 1. Introduction

`meta.hpp` is a **single-header, zero-dependency, zero-overhead** reflection and compile-time metaprogramming library
for C++23. It provides:

- **Automatic structural introspection** of aggregate types (plain structs and simple classes) using C++17
  structured-binding decomposition — no source annotation required.
- **User-defined semantic reflection** of non-aggregate or opaque types via an ADL-discovered `reflect_members`
  hidden-friend function.
- **Enum introspection** — name/value tables, runtime lookup, and `std::expected`-based string-to-enum conversion.
- **A rich algorithm layer** — fold-expression-based `for_each`, `transform`, `filter`, `fold`, `find`, `zip`, and more
  operating directly on reflected `Sequence` types.
- **Tuple interop** — `tie_members`, `forward_members`, `to_value_tuple`, `from_tuple`.
- **Compile-time sequence algebra** — concat, head/tail, reverse, take, drop, unique.
- **Structural hashing** — FNV-1a type and schema fingerprinting, safe for version checks.
- **Layout & serialisation predicates** — zero-copy and binary-stability checks.
- **Reflection views** — semantic, layout, and serialisation filters on top of any reflected sequence.

Everything lives in `namespace meta`. There are no macros, no virtual dispatch, no RTTI, and no code generation step.

---

## 2. Philosophy

### No macros, no registration

Traditional C++ reflection libraries (Boost.Hana, Magic Get, reflect-cpp) require either intrusive macros such as
`BOOST_HANA_DEFINE_STRUCT` or external code-generation passes. `meta.hpp` uses only language features: structured
bindings, `consteval`, concepts, fold expressions, and `__PRETTY_FUNCTION__` / `__FUNCSIG__` for compile-time string
extraction.

### Two-tier reflection model

| Tier               | Mechanism                                    | When to use                                                                                 |
|--------------------|----------------------------------------------|---------------------------------------------------------------------------------------------|
| **Aggregate tier** | C++17 structured bindings + brace-init probe | Plain `struct` / POD; no annotation needed                                                  |
| **Semantic tier**  | ADL hidden-friend `reflect_members`          | Non-aggregates, classes with private members (via friend), types needing stable field names |

The two tiers are unified behind the `Reflectable<T>` concept and the single `reflect<T>()` / `reflect_t<T>` entry
point.

### Zero overhead at runtime

All reflection metadata (field counts, names, types, descriptors) is computed entirely at compile time. The generated
machine code is identical to hand-written field access. LTO can reduce everything to a sequence of direct member
dereferences.

### Safety over convenience for mutable views

Operations that produce references to object internals (`tie_members`, `forward_members`) accept only lvalue references
and have rvalue overloads that are explicitly `=delete`'d. This prevents accidental dangling references at the call site
rather than in a debugger.

### Compiler portability through an adaptation layer

Section 3 (the compiler adaptation layer) wraps `__PRETTY_FUNCTION__`, `__FUNCSIG__`, and Clang-specific cast-spelling
artefacts behind a single internal API surface. Application code never calls the compiler layer directly.

---

## 3. Executive Summary

```
#include "utils/meta.hpp"
```

| Feature               | Entry point                              | Returns                         |
|-----------------------|------------------------------------------|---------------------------------|
| Reflect a type        | `meta::reflect<T>()`                     | `Sequence<Descriptors...>`      |
| Reflect alias         | `meta::reflect_t<T>`                     | same (type alias)               |
| Aggregate field count | `meta::field_count<T>`                   | `std::size_t` (constexpr)       |
| Per-descriptor access | `Seq::element<I>::name()` / `::get(obj)` | `string_view` / field ref       |
| Iterate fields        | `meta::for_each<Seq>(obj, fn)`           | void                            |
| Map fields to tuple   | `meta::transform<Seq>(fn)`               | `std::tuple<...>`               |
| Filter descriptors    | `meta::filter_t<Seq, Pred>`              | `Sequence<...>`                 |
| Enum name             | `meta::enum_name<Value>()`               | `string_view`                   |
| Enum from string      | `meta::enum_from_string<E>(sv)`          | `std::expected<E, parse_error>` |
| Tuple of lvalue refs  | `meta::tie_members(obj)`                 | `std::tuple<F0&, F1&, ...>`     |
| Named view wrapper    | `meta::forward_members(obj)`             | `MemberTie<tuple<...>>`         |
| Owned value tuple     | `meta::to_value_tuple(obj)`              | `std::tuple<F0, F1, ...>`       |
| Structural equality   | `meta::structural_equal(a, b)`           | `bool`                          |
| Structural hash       | `meta::schema_hash<T>()`                 | `uint64_t`                      |
| Binary-safe predicate | `meta::is_binary_stable<T>()`            | `bool` (consteval)              |
| Semantic view         | `meta::semantic_view_t<Seq>`             | filtered `Sequence<...>`        |
| Type name             | `meta::type_name<T>()`                   | `string_view`                   |

---

## 4. Public-Facing API

This section lists every non-`detail` symbol exported from `namespace meta`.

### 4.1 Compile-Time Data Structures

| Symbol                    | Kind            | Purpose                                                                                      |
|---------------------------|-----------------|----------------------------------------------------------------------------------------------|
| `fixed_string<N>`         | struct template | Compile-time string literal; supports `+`, `==`, `[]`, `.view()`                             |
| `TypeList<Ts...>`         | struct template | Heterogeneous type container; `::size`, `::element<I>`, `::contains<T>()`, `::index_of<T>()` |
| `Sequence<Ds...>`         | struct template | Core descriptor container; full algorithm interface (see §4.7)                               |
| `ct_pair<Key, Value>`     | struct template | Compile-time key-value pair                                                                  |
| `named_entry<Key, Value>` | struct template | `fixed_string`-keyed entry for `ct_map`                                                      |
| `ct_map<Entries...>`      | struct template | Compile-time map; `::contains<Key>()`, `::lookup_t<Key>`                                     |
| `value_list<Vs...>`       | struct template | Heterogeneous compile-time value pack; `::get<I>()`                                          |
| `ct_array<T, Capacity>`   | struct template | `consteval` fixed-capacity array; `push_back`, `[]`, `begin`, `end`                          |

### 4.2 Concepts

| Concept                    | Satisfied when                                                                                                                |
|----------------------------|-------------------------------------------------------------------------------------------------------------------------------|
| `Aggregate<T>`             | `std::is_aggregate_v<remove_cvref_t<T>>`                                                                                      |
| `StandardLayout<T>`        | `std::is_standard_layout_v<remove_cvref_t<T>>`                                                                                |
| `TriviallyCopyable<T>`     | `std::is_trivially_copyable_v<remove_cvref_t<T>>`                                                                             |
| `MetaEnum<E>`              | `std::is_enum_v<E>`                                                                                                           |
| `AggregateDecomposable<T>` | `Aggregate<T>` **and** `detail::aggregate_info<T>::exact_type_list` is valid (i.e. structured-binding decomposition succeeds) |
| `Reflectable<T>`           | `Aggregate<T>` **or** has ADL `reflect_members`                                                                               |
| `FieldDescriptorLike<D>`   | D has `index()`, `name()`, `owner_type`, `declared_type`, `value_type`, `is_synthetic()`                                      |
| `DescriptorSequence<S>`    | S has `::size` convertible to `std::size_t`                                                                                   |

### 4.3 Compiler / Type Name

| Symbol           | Signature               | Notes                                                        |
|------------------|-------------------------|--------------------------------------------------------------|
| `type_name<T>()` | `consteval string_view` | Extracts T's name from `__PRETTY_FUNCTION__` / `__FUNCSIG__` |
| `type_tag<T>`    | struct                  | ADL dispatch tag used for custom reflection                  |

### 4.4 Aggregate Reflection

| Symbol                           | Signature                                               | Notes                                                                             |
|----------------------------------|---------------------------------------------------------|-----------------------------------------------------------------------------------|
| `decompose<T>()`                 | `consteval Sequence<AggregateFieldDescriptor<T,0>,...>` | Requires `AggregateDecomposable<T>` (stricter than `Aggregate<T>`); max 32 fields |
| `field_count<T>`                 | `constexpr size_t`                                      | Inline variable; number of aggregate fields                                       |
| `AggregateFieldDescriptor<T, I>` | struct                                                  | Descriptor for the I-th aggregate field (see §4.6)                                |

### 4.5 Custom ADL Reflection

| Symbol                                   | Signature                 | Notes                                               |
|------------------------------------------|---------------------------|-----------------------------------------------------|
| `MemberFieldDescriptor<T, I, Ptr, Name>` | struct                    | Pointer-to-member based descriptor                  |
| `field<I, Ptr, Name>()`                  | `consteval auto`          | Factory: creates `MemberFieldDescriptor`            |
| `make_sequence(ds...)`                   | `consteval Sequence<...>` | Builds a `Sequence` from a pack of descriptors      |
| `reflect<T>()`                           | `consteval Sequence<...>` | Unified entry point; dispatches to ADL or aggregate |
| `reflect_t<T>`                           | type alias                | `decltype(reflect<T>())`                            |

### 4.6 Descriptor Trait Summary

Both `AggregateFieldDescriptor` and `MemberFieldDescriptor` satisfy `FieldDescriptorLike` and expose:

| Member            | Type                       | Description                                                                       |
|-------------------|----------------------------|-----------------------------------------------------------------------------------|
| `owner_type`      | type alias                 | The owning struct/class                                                           |
| `declared_type`   | type alias                 | Exact type as declared (may be reference)                                         |
| `value_type`      | type alias                 | `remove_cvref_t<declared_type>`                                                   |
| `index()`         | `consteval size_t`         | 0-based positional index                                                          |
| `name()`          | `consteval string_view`    | Field name (synthetic `"field_N"` or user-assigned)                               |
| `get(obj)`        | `constexpr decltype(auto)` | Projection; preserves value category                                              |
| `is_synthetic()`  | `consteval bool`           | `true` for aggregate decomposition, `false` for custom                            |
| `is_reference()`  | `consteval bool`           | Whether declared type is a reference                                              |
| `is_pointer()`    | `consteval bool`           | Whether value type is a pointer                                                   |
| `is_const()`      | `consteval bool`           | Whether field is `const`-qualified                                                |
| `has_offset()`    | `consteval bool`           | `false` (not currently exposed)                                                   |
| `has_size()`      | `consteval bool`           | Always `true` for `MemberFieldDescriptor`; `true` for `StandardLayout` aggregates |
| `has_alignment()` | `consteval bool`           | Same conditions as `has_size()`                                                   |
| `size()`          | `consteval size_t`         | `sizeof(value_type)`                                                              |
| `alignment()`     | `consteval size_t`         | `alignof(value_type)`                                                             |

### 4.7 Enum Reflection

| Symbol                       | Signature                            | Notes                                                   |
|------------------------------|--------------------------------------|---------------------------------------------------------|
| `EnumeratorDescriptor<E, V>` | struct                               | Single enumerator: `value()`, `name()`, `as_constant()` |
| `enum_name<V>()`             | `consteval string_view`              | Name of compile-time enumerator `V`                     |
| `enum_name(E v)`             | `constexpr string_view`              | Runtime enum → name; `{}` if not found                  |
| `enum_count<E>`              | `constexpr size_t`                   | Number of valid enumerators in [-128, 256]              |
| `enum_values<E>()`           | `consteval array<E,N>`               | All valid enum values                                   |
| `parse_error`                | struct                               | `{string_view message}`                                 |
| `enum_from_string<E>(sv)`    | `constexpr expected<E, parse_error>` | String → enum with error handling                       |

Default scan range is `[−128, 256]`. Customise per-call: `enum_name<E, Min, Max>(v)`.

### 4.8 Meta Algorithms (free functions)

| Symbol                        | Signature          | Description                                          |
|-------------------------------|--------------------|------------------------------------------------------|
| `for_each<Seq>(fn)`           | `constexpr void`   | Invoke `fn(D{})` for each descriptor                 |
| `for_each<Seq>(obj, fn)`      | `constexpr void`   | Invoke `fn(D{}, D::get(obj))` for each field         |
| `transform<Seq>(fn)`          | `constexpr tuple`  | Map descriptors: returns `tuple{fn(D{})...}`         |
| `transform_t<Seq, Transform>` | type alias         | Type-level map                                       |
| `filter_t<Seq, Pred>`         | type alias         | Compile-time filter keeping `Pred<D>::value == true` |
| `fold<Seq>(init, fn)`         | `constexpr auto`   | Left fold: `result = fn(result, D{})`                |
| `find_if<Seq>(pred)`          | `consteval size_t` | Index of first match; `Seq::size` = not found        |
| `find_by_name<Seq>(name)`     | `consteval size_t` | Index by field name                                  |
| `contains<Seq>(pred)`         | `consteval bool`   | True if any descriptor matches                       |
| `count_if<Seq>(pred)`         | `consteval size_t` | Count matching descriptors                           |
| `apply(obj, fn)`              | `constexpr void`   | `for_each` over `reflect_t<T>`                       |
| `zip(a, b, fn)`               | `constexpr void`   | `fn(D{}, D::get(a), D::get(b))` for each field       |

`Sequence<Ds...>` also exposes all algorithms as **static member functions** (`Seq::for_each(fn)`, `Seq::transform(fn)`,
`Seq::fold(init, fn)`, `Seq::count_if(pred)`, `Seq::find_if(pred)`, `Seq::find_by_name(sv)`, `Seq::contains_named(sv)`).

### 4.9 Tuple Interop

| Symbol                         | Signature                  | Description                                           |
|--------------------------------|----------------------------|-------------------------------------------------------|
| `MemberTie<Tup>`               | struct                     | Named non-owning wrapper around a tuple of references |
| `tie_members(T& obj)`          | `constexpr tuple<F0&,...>` | Lvalue refs; write-back; rvalue overload `=delete`    |
| `forward_members(T&& obj)`     | `constexpr MemberTie<...>` | View; lvalue-only; rvalue overload `=delete`          |
| `to_value_tuple(const T& obj)` | `constexpr tuple<F0,...>`  | Deep copy; safe to store                              |
| `from_tuple<T>(tup)`           | `constexpr T`              | Construct aggregate from matching tuple               |

### 4.10 Layout & Serialisation Predicates

| Symbol                                   | Signature        | Description                                                       |
|------------------------------------------|------------------|-------------------------------------------------------------------|
| `has_pointer_field<T>()`                 | `consteval bool` | Any field is a raw pointer                                        |
| `has_reference_field<T>()`               | `consteval bool` | Any field is a reference                                          |
| `is_trivially_copyable_deep<T>()`        | `consteval bool` | `T` and all fields are trivially copyable                         |
| `is_zero_copy_serializable<T, Policy>()` | `consteval bool` | Safe to `memcpy`; no pointers/references                          |
| `is_binary_stable<T, Policy>()`          | `consteval bool` | Zero-copy AND `is_standard_layout_v`                              |
| `DefaultPolicy`                          | struct           | Relaxed: no pointer/reference, trivially copyable                 |
| `StrictPolicy`                           | struct           | Strict: additionally requires standard layout + deep trivial copy |

### 4.11 Compile-Time Sequence Algebra

| Symbol               | Description                                 |
|----------------------|---------------------------------------------|
| `concat_t<S1, S2>`   | Concatenate two `Sequence` types            |
| `head_t<Seq>`        | First descriptor type                       |
| `tail_t<Seq>`        | All but first descriptor                    |
| `reverse_t<Seq>`     | Reverse order                               |
| `take_t<Seq, N>`     | First N descriptors                         |
| `drop_t<Seq, N>`     | Drop first N descriptors                    |
| `unique_t<Seq>`      | Remove duplicate descriptor types           |
| `index_of<Seq, T>()` | `consteval size_t` — index of type T in Seq |

### 4.12 Hashing & Schema

| Symbol             | Signature            | Description                                            |
|--------------------|----------------------|--------------------------------------------------------|
| `fnv1a_hash(sv)`   | `consteval uint64_t` | FNV-1a 64-bit hash of a string                         |
| `type_hash<T>()`   | `consteval uint64_t` | Hash of `type_name<T>()`                               |
| `schema_hash<T>()` | `consteval uint64_t` | Hash of type name + all field names + field type names |

### 4.13 Diagnostics & Validation

| Symbol                       | Signature                         | Description                                           |
|------------------------------|-----------------------------------|-------------------------------------------------------|
| `validate_reflection<T>()`   | `consteval bool`                  | Checks descriptor indices are sequential from 0       |
| `field_names<T>()`           | `consteval array<string_view, N>` | Array of all field names                              |
| `field_types_are<T, Pred>()` | `consteval bool`                  | All `value_type`s satisfy unary type predicate `Pred` |

### 4.14 Comparison

| Symbol                   | Signature        | Description                                   |
|--------------------------|------------------|-----------------------------------------------|
| `structural_equal(a, b)` | `constexpr bool` | Field-by-field equality                       |
| `structural_less(a, b)`  | `constexpr bool` | Lexicographic less-than over reflected fields |

### 4.15 Reflection Views & Projections

| Symbol                                | Description                                           |
|---------------------------------------|-------------------------------------------------------|
| `structural_view_tag`                 | Identity; all fields                                  |
| `semantic_view_tag`                   | Non-synthetic fields only                             |
| `layout_view_tag`                     | Fields with size/alignment metadata                   |
| `serialization_view_tag`              | Non-pointer, non-reference, trivially-copyable fields |
| `structural_view_t<Seq>`              | `Seq` (identity)                                      |
| `semantic_view_t<Seq>`                | `filter_t<Seq, is_semantic_field>`                    |
| `layout_view_t<Seq>`                  | `filter_t<Seq, has_layout_metadata_pred>`             |
| `serialization_view_t<Seq>`           | `filter_t<Seq, is_serializable_field>`                |
| `reflect_as<ViewTag, T>()`            | View-gated reflection entry point                     |
| `reflect_as_t<ViewTag, T>`            | Type alias for `decltype(reflect_as<ViewTag,T>())`    |
| `projected_view_t<Seq, Proj>`         | Type-level descriptor transform                       |
| `value_types_of_t<Seq>`               | `TypeList` of `value_type` for each descriptor        |
| `projections::as_const_descriptor<D>` | Projection that makes every field `const`             |

---

## 5. Detailed Usage with Examples

### 5.1 Compile-Time Data Structures

#### `fixed_string`

```cpp
constexpr auto greeting = meta::fixed_string("hello");
static_assert(greeting.length == 5);
static_assert(greeting.view() == "hello");

// Concatenation
constexpr auto s = meta::fixed_string("foo") + meta::fixed_string("_bar");
static_assert(s.view() == "foo_bar");
```

`fixed_string` is the backbone of named descriptors. It is used as a NTTP (non-type template parameter) for
`MemberFieldDescriptor`.

#### `TypeList`

```cpp
using Nums = meta::TypeList<int, float, double>;
static_assert(Nums::size == 3);
static_assert(Nums::contains<float>());
static_assert(Nums::index_of<double>() == 2);
using Third = Nums::element<2>;  // double
```

#### `Sequence`

```cpp
// Usually obtained from reflect<T>() — rarely constructed manually.
// But for illustration:
using MySeq = meta::Sequence<D0, D1, D2>;
static_assert(MySeq::size == 3);
static_assert(MySeq::has_element<2>);

// Member algorithms
MySeq::for_each([](auto d) { /* called with D0{}, D1{}, D2{} */ });
constexpr auto idx = MySeq::find_by_name("price");
constexpr bool has = MySeq::contains_named("qty");
```

#### `ct_map`

```cpp
using Registry = meta::ct_map<
    meta::named_entry<"alpha", int>,
    meta::named_entry<"beta",  float>
>;
static_assert(Registry::contains<"alpha">());
using BetaType = Registry::lookup_t<"beta">;  // float
```

#### `ct_array`

```cpp
consteval auto build() {
    meta::ct_array<int, 8> arr;
    arr.push_back(10);
    arr.push_back(20);
    return arr;
}
constexpr auto arr = build();
static_assert(arr.size() == 2);
static_assert(arr[0] == 10);
```

---

### 5.2 Concepts & Traits

```cpp
struct Point { double x, y; };         // aggregate
class Opaque { int v; };                // NOT aggregate (private member)

static_assert(meta::Aggregate<Point>);
static_assert(!meta::Aggregate<Opaque>);
static_assert(meta::Reflectable<Point>); // via aggregate path
// Opaque can become Reflectable if it defines reflect_members (see §5.5)

enum class Color { Red, Green, Blue };
static_assert(meta::MetaEnum<Color>);
```

---

### 5.3 Type Names

```cpp
static_assert(meta::type_name<int>() == "int");
static_assert(meta::type_name<std::string_view>() == "std::string_view");

// Useful for runtime logging
template <typename T>
void log_type() {
    std::println("Type: {}", meta::type_name<T>());
}
```

> **Note:** The string returned is compiler-dependent in formatting but stable within one binary. Do not persist across
> compiler versions.

---

### 5.4 Aggregate Reflection

Any plain struct with up to 32 public non-static data members is reflectable automatically.

```cpp
struct Vec3 {
    float x, y, z;
};

// Field count
static_assert(meta::field_count<Vec3> == 3);

// Decompose to Sequence
using Seq = meta::reflect_t<Vec3>;
static_assert(Seq::size == 3);

// Access individual descriptor
using D0 = Seq::element<0>;
static_assert(D0::name() == "field_0");    // synthetic positional name
static_assert(D0::index() == 0);
static_assert(D0::is_synthetic());
static_assert(std::same_as<D0::value_type, float>);

// Runtime field access via descriptor
Vec3 v{1.f, 2.f, 3.f};
float x = D0::get(v);   // x == 1.f

D0::get(v) = 10.f;      // mutates through the reference
assert(v.x == 10.f);
```

Aggregate field names are synthetic positional placeholders (`field_0`, `field_1`, …). For semantic names use custom
reflection (§5.5).

---

### 5.5 Custom ADL Reflection

For non-aggregates, classes with private members, or any type where you want stable semantic field names, define
`reflect_members` as a hidden friend:

```cpp
#include "utils/meta.hpp"

struct TradeOrder {
    double price;
    int    quantity;

    // ADL-discoverable hidden friend — no macro required.
    friend consteval auto reflect_members(meta::type_tag<TradeOrder>) {
        return meta::make_sequence(
            meta::field<0, &TradeOrder::price,    "price">(),
            meta::field<1, &TradeOrder::quantity, "quantity">()
        );
    }
};

static_assert(meta::Reflectable<TradeOrder>);

using Seq = meta::reflect_t<TradeOrder>;
static_assert(Seq::size == 2);

using PriceD = Seq::element<0>;
static_assert(PriceD::name()  == "price");
static_assert(!PriceD::is_synthetic());
static_assert(std::same_as<PriceD::value_type, double>);

TradeOrder order{99.5, 10};
double p = PriceD::get(order);   // p == 99.5
```

The `field<Index, Ptr, Name>()` factory deduces the owner type from the pointer-to-member type, so you never repeat the
class name.

**Private-member reflection** is achieved by making `reflect_members` a friend, giving it access to private
pointer-to-members:

```cpp
class Account {
    int id_;
    double balance_;
public:
    explicit Account(int id, double balance) : id_(id), balance_(balance) {}

    friend consteval auto reflect_members(meta::type_tag<Account>) {
        return meta::make_sequence(
            meta::field<0, &Account::id_,      "id">(),
            meta::field<1, &Account::balance_, "balance">()
        );
    }
};
```

---

### 5.6 Enum Reflection

```cpp
enum class Direction { North, South, East, West };

// Compile-time name lookup
static_assert(meta::enum_name<Direction::North>() == "North");

// Runtime name lookup
Direction d = Direction::East;
std::string_view name = meta::enum_name(d);  // "East"

// Count
static_assert(meta::enum_count<Direction> == 4);

// All values
constexpr auto vals = meta::enum_values<Direction>();
// vals == {North, South, East, West} (in numeric order)

// String → enum (returns std::expected)
auto result = meta::enum_from_string<Direction>("South");
if (result) {
    Direction dir = *result;  // Direction::South
} else {
    std::println("Error: {}", result.error().message);
}

// Custom scan range for sparse enums
enum class Status : int { OK = 0, Timeout = 100, Error = 200 };
constexpr std::size_t n = meta::enum_count<Status, 0, 250>;
std::string_view s = meta::enum_name<Status, 0, 250>(Status::Timeout);
```

---

### 5.7 Meta Algorithms

#### `for_each` — iterate descriptors

```cpp
struct Config { int width; int height; bool fullscreen; };

// Descriptor-only iteration (compile-time)
using Seq = meta::reflect_t<Config>;
meta::for_each<Seq>([](auto descriptor) {
    std::println("field: {}", descriptor.name());
});

// Instance iteration (runtime values)
Config cfg{1920, 1080, true};
meta::for_each<Seq>(cfg, [](auto descriptor, auto &&value) {
    std::println("{} = {}", descriptor.name(), value);
});

// Shorthand using apply()
meta::apply(cfg, [](auto descriptor, auto &&value) {
    std::println("{} = {}", descriptor.name(), value);
});
```

#### `transform` — map to tuple

```cpp
Config cfg{1920, 1080, true};
auto names = meta::transform<Seq>([](auto d) { return d.name(); });
// names == tuple{"field_0", "field_1", "field_2"}
```

#### `fold` — accumulate

```cpp
// Count pointer fields
constexpr auto ptr_count = meta::fold<Seq>(std::size_t{0},
    [](std::size_t acc, auto d) {
        return acc + (d.is_pointer() ? 1 : 0);
    });
```

#### `filter_t` — compile-time filter

```cpp
// Keep only non-synthetic (user-named) fields
template <typename D> struct is_user_named
    : std::bool_constant<!D::is_synthetic()> {};

using UserFields = meta::filter_t<meta::reflect_t<TradeOrder>, is_user_named>;
static_assert(UserFields::size == 2); // both have user names
```

#### `find_if` / `find_by_name`

```cpp
using Seq = meta::reflect_t<TradeOrder>;

constexpr auto idx = meta::find_by_name<Seq>("quantity");
static_assert(idx == 1);

TradeOrder order{99.5, 10};
auto &qty = Seq::element<idx>::get(order);  // reference to quantity
```

#### `zip` — pair two objects

```cpp
TradeOrder a{99.5, 10}, b{100.0, 5};
meta::zip(a, b, [](auto d, auto &&va, auto &&vb) {
    std::println("{}: {} vs {}", d.name(), va, vb);
});
```

---

### 5.8 Tuple Interop

#### `tie_members` — writable reference tuple

```cpp
TradeOrder order{99.5, 10};
auto t = meta::tie_members(order);

std::get<0>(t) = 105.0;   // writes back to order.price
assert(order.price == 105.0);

// Rvalue is compile error:
// meta::tie_members(TradeOrder{99.5, 10});  // ERROR: =delete'd
```

#### `forward_members` — category-preserving named view

```cpp
TradeOrder order{99.5, 10};
auto view = meta::forward_members(order);

// Tuple-like access — use UNQUALIFIED get (ADL finds meta::get).
// std::get<I>(view) does NOT compile; meta::get lives in namespace meta.
double p = get<0>(view);   // ✅  99.5 — ADL resolves to meta::get
get<1>(view) = 20;         // ✅  writes back to order.quantity

// Structured bindings work normally:
auto [price, qty] = view;  // ✅

// std::tuple_size / std::tuple_element work:
static_assert(std::tuple_size_v<decltype(view)> == 2);

// Rvalue is compile error:
// meta::forward_members(TradeOrder{99.5, 10});  // ERROR: =delete'd
```

`MemberTie` is distinct from `std::tuple` — it cannot accidentally decay into a plain tuple reference, making the
ephemeral, non-owning nature visible at the type level.

> **⚠️ Important — use unqualified `get`, not `std::get`:**  
> `MemberTie`'s `get` overloads live in `namespace meta`, not `namespace std`.  
> Structured bindings and ADL-based code work correctly, but a *qualified*  
> call `std::get<0>(view)` will **not compile** — the overload is not in `namespace std`.  
> Always use unqualified `get<I>(view)` (or rely on structured bindings):
> ```cpp
> using std::get;          // or just leave unqualified — ADL finds meta::get
> auto p = get<0>(view);   // ✅  ADL resolves to meta::get
> // std::get<0>(view);    // ❌  does not compile
> ```

#### `to_value_tuple` — owned deep copy

```cpp
TradeOrder order{99.5, 10};
auto snap = meta::to_value_tuple(order);  // std::tuple<double, int>

// Safe to store, return, pass freely
order.price = 200.0;
assert(std::get<0>(snap) == 99.5);  // independent copy
```

#### `from_tuple` — construct aggregate from tuple

```cpp
auto tup = std::make_tuple(1.5f, 2.5f, 3.5f);
auto v   = meta::from_tuple<Vec3>(tup);
assert(v.x == 1.5f && v.y == 2.5f && v.z == 3.5f);
```

---

### 5.9 Layout & Serialisation Policies

```cpp
struct Packet { uint32_t id; float value; };
struct WithPointer { int *ptr; float data; };
struct WithRef    { int &ref; float data; };

static_assert( meta::is_zero_copy_serializable<Packet>());
static_assert(!meta::is_zero_copy_serializable<WithPointer>());
static_assert(!meta::is_zero_copy_serializable<WithRef>());

// Strict policy additionally requires standard layout + deep trivial copy
static_assert( meta::is_binary_stable<Packet>());
static_assert( meta::is_binary_stable<Packet, meta::StrictPolicy>());

// Structural introspection
static_assert(!meta::has_pointer_field<Packet>());
static_assert( meta::has_pointer_field<WithPointer>());
static_assert( meta::is_trivially_copyable_deep<Packet>());
```

These predicates are all `consteval` — usable in `static_assert`, `if constexpr`, and concept requirements.

---

### 5.10 Compile-Time Sequence Algorithms

```cpp
using A = meta::Sequence<D0, D1>;
using B = meta::Sequence<D2, D3>;

// Concatenate
using AB = meta::concat_t<A, B>;
static_assert(AB::size == 4);

// Head / tail
using First = meta::head_t<AB>;     // D0
using Rest  = meta::tail_t<AB>;     // Sequence<D1,D2,D3>

// Reverse
using Rev = meta::reverse_t<AB>;    // Sequence<D3,D2,D1,D0>

// Take / drop
using Take2 = meta::take_t<AB, 2>; // Sequence<D0,D1>
using Drop2 = meta::drop_t<AB, 2>; // Sequence<D2,D3>

// Unique
using Dups   = meta::Sequence<D0, D1, D0, D2>;
using NoDups = meta::unique_t<Dups>; // Sequence<D0,D1,D2>

// Index of type
constexpr auto i = meta::index_of<AB, D2>(); // 2
```

---

### 5.11 Hashing & Schema Fingerprinting

```cpp
struct Record { int id; double value; };

constexpr uint64_t hash = meta::schema_hash<Record>();

// Use for version checks in serialisation headers:
constexpr uint64_t RECORD_SCHEMA_V1 = meta::schema_hash<Record>();
static_assert(meta::schema_hash<Record>() == RECORD_SCHEMA_V1,
    "Record schema has changed — update the serialiser");

// FNV-1a directly
constexpr uint64_t h = meta::fnv1a_hash("hello");

// Type hash
constexpr uint64_t th = meta::type_hash<int>();
```

---

### 5.12 Diagnostics & Validation

```cpp
// Verify a custom reflect_members is correctly indexed
static_assert(meta::validate_reflection<TradeOrder>());

// Get all field names as an array
constexpr auto names = meta::field_names<TradeOrder>();
static_assert(names[0] == "price");
static_assert(names[1] == "quantity");

// Check all fields satisfy a predicate
template <typename T>
struct is_arithmetic : std::bool_constant<std::is_arithmetic_v<T>> {};

struct AllArith { int a; double b; float c; };
static_assert(meta::field_types_are<AllArith, is_arithmetic>());

struct Mixed { int a; std::string b; };
static_assert(!meta::field_types_are<Mixed, is_arithmetic>());
```

---

### 5.13 Structural Comparison

```cpp
struct Color { uint8_t r, g, b; };

Color red{255, 0, 0}, also_red{255, 0, 0}, blue{0, 0, 255};

assert( meta::structural_equal(red, also_red));
assert(!meta::structural_equal(red, blue));

// Lexicographic less-than over (r, g, b)
assert( meta::structural_less(blue, red));  // {0,0,255} < {255,0,0}
assert(!meta::structural_less(red, blue));
```

`structural_less` enables `meta::Reflectable` types to be used in sorted containers without manually writing
`operator<`:

```cpp
auto cmp = [](const Color &a, const Color &b) {
    return meta::structural_less(a, b);
};
std::set<Color, decltype(cmp)> palette(cmp);
```

---

### 5.14 Reflection Views & Projections

#### Semantic view — suppress synthetic names

```cpp
// An aggregate's fields have synthetic names "field_0", "field_1", etc.
// Mixing with custom-reflected types in a generic algorithm requires
// filtering out synthetic fields when only user-named fields matter.

using AllFields  = meta::reflect_t<TradeOrder>;
using UserFields = meta::semantic_view_t<AllFields>;

// TradeOrder is custom-reflected, so all fields are semantic:
static_assert(UserFields::size == AllFields::size);
```

#### Layout view — only fields with known size/alignment

```cpp
using LayoutFields = meta::layout_view_t<meta::reflect_t<TradeOrder>>;
// Keeps only fields where has_layout_metadata() is true.
// For MemberFieldDescriptor this is always all fields.
```

#### Serialisation view — trivially-copyable, non-pointer, non-reference

```cpp
struct Mixed { int id; std::string *name; double score; };
using Ser = meta::serialization_view_t<meta::reflect_t<Mixed>>;
// Seq<id, score> — 'name' is a pointer, filtered out
static_assert(Ser::size == 2);
```

#### `reflect_as` — view-gated entry point

```cpp
using Semantic = meta::reflect_as_t<meta::semantic_view_tag, TradeOrder>;
// Equivalent to semantic_view_t<reflect_t<TradeOrder>>

auto seq = meta::reflect_as<meta::serialization_view_tag, Mixed>();
// Returns serialization_view_t<reflect_t<Mixed>>
```

#### `as_const_descriptor` projection

```cpp
// Make every field appear const — useful for read-only serialisers
using ConstSeq = meta::projected_view_t<
    meta::reflect_t<TradeOrder>,
    meta::projections::as_const_descriptor
>;

TradeOrder order{99.5, 10};
auto &p = ConstSeq::element<0>::get(order);
// p is const double& — cannot mutate through it
```

#### Extract value types

```cpp
using VT = meta::value_types_of_t<meta::reflect_t<TradeOrder>>;
// VT == TypeList<double, int>
static_assert(std::same_as<VT::element<0>, double>);
```

---

## 7. Design Details

### 7.1 Field Counting for Aggregates

Aggregate field counting uses **brace-init constructibility probing** with a sentinel `AnyType` that converts to any
non-aggregate type via a `consteval operator T&`. The algorithm:

1. `is_constructible_n<T>(index_sequence<0,…,N-1>)` — true if `T{any, any, …}` (N elements) is well-formed.
2. **Binary search** (`count_fields_binary`) over `[0, max_aggregate_fields]` (default 32) finds the highest
   constructible arity in O(log N) template instantiation depth. A **linear fallback** (`count_fields_linear`) is used
   for non-monotonic cases (aggregates with reference members).
3. The limit can be raised by changing `max_aggregate_fields` in the header.

### 7.2 Structured Binding Engine (`tie_impl`)

Because the number of structured-binding names must be a compile-time constant, `tie_impl<N, T>` is an explicit
`if constexpr` chain from `N=0` to `N=32`. Each branch:

- Destructures `auto &&[f0, ..., fN-1] = obj`
- Returns `std::forward_as_tuple(f0, …, fN-1)`

This is macro-free and produces zero-overhead field access under optimisation.

`exact_types<N, T>` mirrors the same structure but captures `decltype(fI)` rather than forwarding, giving the
authoritative declared type (preserving references, const, etc.) without going through `std::tuple_element`.

### 7.3 ADL Reflection Dispatch

The `detail::no_custom_reflection` fallback ensures `reflect_members(type_tag<T>{})` is always findable via ADL — it
returns a sentinel type. User-defined hidden friends take precedence as non-template exact matches. The
`HasCustomReflection<T>` concept checks that the returned type is not the sentinel. `reflect<T>()` then dispatches:

```
if HasCustomReflection<T>       → invoke_reflect_members<T>()
else if AggregateDecomposable<T> → decompose<T>()
else                             → static_assert
```

`AggregateDecomposable<T>` is a stricter gate than bare `Aggregate<T>`: it additionally requires that
`detail::aggregate_info<T>::exact_type_list` is well-formed (i.e. the structured-binding decomposition actually succeeds
at the implementation level). This ensures that `decompose<T>()` is constrained on `AggregateDecomposable<T>` as a
`requires` clause, so an unsatisfied decomposition produces a concept constraint diagnostic **at the call site** rather
than a hard error deep inside template instantiation.

### 7.4 Enum Scanning

Enum scanning uses `__PRETTY_FUNCTION__` / `__FUNCSIG__` with the enum value embedded as a template argument. Valid
enumerators produce identifier-like strings; out-of-range probes produce cast-expression strings `(Direction)42`.
`is_valid_enum_name` distinguishes these by:

1. Rejecting empty strings, strings starting with `(`.
2. On Clang: additionally rejecting cast-like spellings (parentheses, explicit cast tokens, or numeric-only identifiers)
   via `looks_like_cast_spelling`.
3. Verifying the cleaned identifier starts with a letter or underscore and contains only valid identifier characters.

Scanning is done lazily as a `constexpr` lookup table built once per `(E, Min, Max)` instantiation.

### 7.5 `forward_members` / `tie_members` — Rvalue Safety

The overload design uses a **single forwarding-reference template** constrained by `std::is_lvalue_reference_v<T>`:

- `T = SomeType&` (mutable lvalue): `is_lvalue_reference_v<T>` = `true` → lvalue overload selected.
- `T = const SomeType&` (const lvalue): same.
- `T = SomeType` (rvalue): `is_lvalue_reference_v<T>` = `false` → `= delete`'d overload.

Two separate overloads (`T&` and `const T&`) were rejected because `const T&` binds rvalues in C++, silently defeating
the safety guarantee. The forwarding-reference + `is_lvalue_reference_v` pattern is the correct way to express "accept
lvalues only" without falling into that trap.

### 7.6 `MemberTie` — Named Wrapper Type

`MemberTie<Tup>` wraps the `std::tuple<Field0&, ...>` produced by `forward_members`. Its purpose is to prevent the
result from silently decaying into a plain `std::tuple` in generic code, which would lose the "non-owning, ephemeral"
semantic signal.

It satisfies the structured-binding protocol via:

- `std::tuple_size<MemberTie<Tup>>` — specialisation in `namespace std`
- `std::tuple_element<I, MemberTie<Tup>>` — specialisation in `namespace std`
- `get<I>(MemberTie<Tup>&)` / `get<I>(const MemberTie<Tup>&)` / `get<I>(MemberTie<Tup>&&)` — **overloads
  in `namespace meta`** (not `namespace std`)

Per `[namespace.std]`, only template *specializations* for user-defined types may be added to `namespace std`; injecting
new function overloads there is undefined behaviour. The `get` overloads are therefore placed in `namespace meta`, where
ADL finds them automatically because `MemberTie` is declared in that namespace. Calling `std::get<I>(membertie)`
bypasses ADL and will **not compile**.

### 7.7 Sequence as an Algorithm Facade

`Sequence` exposes its own `for_each`, `transform`, `fold`, `count_if`, `find_if`, `find_by_name`, `contains_named` as
static member functions with implementations that deliberately do **not** delegate to the free functions in Section 7.
This avoids a forward-declaration dependency (the algorithms in Section 7 come after `Sequence` in the file) and keeps
`Sequence` self-contained.

### 7.8 Compiler Adaptation Layer

Section 3 (`detail::compiler`) abstracts all compiler-specific string extraction. Application code and even Section 4–14
only call `type_name_raw`, `enum_name_raw`, `clean_enum_name`, and `is_valid_enum_name` through the layer — never
`__PRETTY_FUNCTION__` directly. This makes porting to a new compiler (e.g. EDG / ICC) a one-file change.

---

## 8. Gaps & Future Work

### 8.1 Synthetic Names for Aggregates

Aggregate field names are positional placeholders (`field_0`, `field_1`, …), not the actual C++ identifier names. C++23
does not expose source-level member names without reflection TS (`std::meta`). Until P2996 (reflection) is standardised,
the only way to get semantic names is through custom `reflect_members` (Section 5.5).

**Mitigation:** Use `is_synthetic()` to detect and skip positional names in generic code. The `semantic_view_t` filter (
Section 5.14) does this automatically.

### 8.2 Aggregate Field Limit

The structured-binding engine supports up to `max_aggregate_fields = 32` fields. The constant is `inline constexpr` and
can be raised in the header. Raising it requires adding more branches to `tie_impl` and `exact_types` — these are
mechanical additions.

**Future:** Code-generate these branches or, once P2996 lands, replace the entire decomposition engine with native
reflection.

### 8.3 Nested Reflection

There is no built-in recursive descent. Reflecting a `struct Outer { Inner i; int x; }` gives descriptors for `i` and
`x`, but `Inner`'s fields are not automatically expanded. Users can traverse nested types manually:

```cpp
// Manual recursive descent
meta::for_each<meta::reflect_t<Outer>>(obj, [](auto d, auto &&v) {
    if constexpr (meta::Reflectable<std::remove_cvref_t<decltype(v)>>) {
        meta::apply(v, [&](auto nd, auto &&nv) { /* nested */ });
    }
});
```

**Future:** A `flatten_t<Seq>` or `deep_apply` utility.

### 8.4 Inherited Members

The aggregate decomposition engine counts fields visible in brace-init, which for simple aggregates includes base-class
members only when the base is itself an aggregate with no user-provided constructor and the derived struct uses
aggregate initialisation. Complex inheritance hierarchies, virtual bases, or non-trivial base classes are not supported.

### 8.5 Enum Scanning Range

The default range `[-128, 256]` covers most `int`-sized enums but misses large sparse enums (e.g. Win32 `HRESULT`).
Users must specify `<Min, Max>` explicitly for such cases. There is no automatic range detection.

**Future:** A `REFLECTABLE_ENUM(E, Min, Max)` annotation helper or range auto-detection via binary search.

### 8.6 Field Offset Metadata

`has_offset()` returns `false` for all current descriptors. Offsets can in principle be computed with `offsetof` for
standard-layout types. Adding an `OffsetAwareFieldDescriptor` or a specialised `StandardLayoutDescriptor` with
`offset()` is a natural extension.

### 8.7 `schema_hash` Stability

`schema_hash<T>()` hashes `type_name<T>()` plus field names and types. `type_name<T>()` is compiler-dependent in
formatting (e.g. `std::__cxx11::basic_string` vs `std::string`). Schema hashes should **not** be persisted across
compilers or standard library versions.

**Future:** A stable canonical type-name layer that normalises common aliases.

### 8.8 C++26 / Reflection TS Readiness

When P2996 (`std::meta`) is adopted, the aggregate decomposition engine (Sections 3–4) can be replaced wholesale with
`std::meta::members_of`, and `type_name<T>()` can use `std::meta::identifier_of`. The API surface (Section 4) is
designed to be forward-compatible: `reflect<T>()` still returns a `Sequence<Descriptors...>`, only the implementation
changes.

### 8.9 Parallel / SIMD Field Iteration

The current algorithm layer is scalar. A `simd_apply` or vectorised `zip` that processes multiple objects at once using
SIMD intrinsics would be valuable for hot serialisation paths. This requires layout-safe (standard-layout, no-pointer)
types, which `is_binary_stable` already identifies.

---

## 6. New in This Release

### 6.1 Feature Detection — meta::config

`namespace meta::config` exposes `inline constexpr bool` flags backed by `__cpp_*` feature-test macros:

```cpp
meta::config::has_ranges          // __cpp_lib_ranges >= 201911
meta::config::has_expected        // __cpp_lib_expected >= 202211
meta::config::has_mdspan          // __cpp_lib_mdspan >= 202207
meta::config::has_source_location // __cpp_lib_source_location >= 201907
meta::config::has_stacktrace      // __cpp_lib_stacktrace >= 202011
meta::config::has_flat_map        // __cpp_lib_flat_map >= 202207
meta::config::has_print           // __cpp_lib_print >= 202207
```

Use these to guard optional functionality without `#ifdef` cascades.

### 6.2 Reflection Cache Variables

Repeated calls to `reflect<T>()`, `type_name<T>()`, and `schema_hash<T>()` in large metaprograms can explode template
instantiation depth. Cache variables collapse each to a single instantiation per type:

```cpp
template <typename T>
inline constexpr auto             reflection_v  = reflect<T>();

template <typename T>
inline constexpr std::string_view type_name_v   = type_name<T>();

template <typename T>  // requires Reflectable<T>
inline constexpr std::uint64_t    schema_hash_v = schema_hash<T>();
```

Internal library code should prefer `reflection_v<T>` over `reflect<T>()` where the result is used more than once.

### 6.3 source_location Diagnostics

`reflect<T>()` now accepts an optional `std::source_location` parameter:

```cpp
template <typename T>
consteval auto reflect(
    std::source_location loc = std::source_location::current());
```

When reflection fails, the error message includes the call site's type, file, and line — directly usable in Xcode's
error navigator.

### 6.4 Sequence Modernization

`Sequence<Descriptors...>` gained ergonomic member aliases:

| Member                    | Equivalent free-function form             |
|---------------------------|-------------------------------------------|
| `Seq::empty_seq`          | `Seq::size == 0`                          |
| `Seq::front`              | `head_t<Seq>`                             |
| `Seq::back`               | last element type                         |
| `Seq::contains_type<T>()` | `(std::same_as<T, Descriptors> \|\| ...)` |

Free alias templates (for take/drop/reverse — cannot be member aliases in C++23):

```cpp
template <typename Seq, std::size_t N>
using take_seq_t = take_t<Seq, N>;

template <typename Seq, std::size_t N>
using drop_seq_t = drop_t<Seq, N>;

template <typename Seq>
using reverse_seq_t = reverse_t<Seq>;
```

### 6.5 reflection_traits\<T\>

A uniform trait struct exposing reflection metadata:

```cpp
template <typename T>
struct reflection_traits {
    static constexpr bool        is_reflectable;   // satisfies Reflectable<T>
    static constexpr bool        is_aggregate;     // plain aggregate
    static constexpr bool        is_custom;        // has ADL reflect_members()
    static constexpr std::size_t field_count;      // 0 if not reflectable
    static constexpr bool        has_named_fields; // false for aggregate tier
};

template <typename T>
inline constexpr bool is_reflectable_v = reflection_traits<T>::is_reflectable;
```

### 6.6 Compile-Time IDs

Stable compile-time identities for types, fields, and schemas:

```cpp
template <typename T>
inline constexpr std::uint64_t type_id_v = type_hash<T>();  // hash of type name

template <typename D>  // D is a field descriptor
consteval std::uint64_t field_id() noexcept;                 // hash of owner + name + index

template <typename T>  // requires Reflectable<T>
inline constexpr std::uint64_t schema_id_v = schema_hash_v<T>;
```

### 6.7 Advanced Hashing — meta::hash

`namespace meta::hash` provides additional constexpr-safe hash functions alongside the existing FNV-1a:

```cpp
namespace meta::hash {
    // wyhash-inspired 64-bit hash
    constexpr std::uint64_t wyhash64(std::string_view sv) noexcept;
    template <std::size_t N>
    constexpr std::uint64_t wyhash64(const akshara::fixed_string<N>&) noexcept;

    // XXH3-inspired 64-bit hash
    constexpr std::uint64_t xxh3_64(std::string_view sv) noexcept;
    template <std::size_t N>
    constexpr std::uint64_t xxh3_64(const akshara::fixed_string<N>&) noexcept;

    // Re-exports from akshara / meta
    using meta::fnv1a_hash;
    using akshara::fnv1a64;
}
```

Both are portable constexpr adaptations — no `__uint128_t`, no compiler intrinsics — usable in `consteval` contexts. For
schema versioning, prefer `fnv1a64` (stable, well-known). For hash table keys where avalanche quality matters, prefer
`wyhash64` or `xxh3_64`.

### 6.8 Reflection Backend Abstraction

Decouples `reflect<T>()` from its implementation strategy. Enables a clean migration path to P2996 `std::meta` without
any API change:

```cpp
// Backend tags
using aggregate_backend      = detail::aggregate_backend_tag;
using adl_backend            = detail::adl_backend_tag;
using std_reflection_backend = detail::std_reflection_backend_tag; // future P2996

// Backend selector
template <typename T>
using reflect_backend_t = ...;  // aggregate_backend or adl_backend

// Explicit dispatch (advanced)
template <typename Backend, typename T>
consteval auto reflect_with();
```

**Usage:**

```cpp
// Verify which backend is used
static_assert(std::same_as<meta::reflect_backend_t<Point>,      meta::aggregate_backend>);
static_assert(std::same_as<meta::reflect_backend_t<TradeOrder>, meta::adl_backend>);

// Explicit backend (rarely needed)
auto seq = meta::reflect_with<meta::adl_backend, TradeOrder>();
```

When P2996 lands, only `reflect_backend_t<T>` and the backend dispatch need updating. All caller code remains unchanged.

---
