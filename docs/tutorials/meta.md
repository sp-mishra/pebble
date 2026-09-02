# Tutorial: The Alchemist of Pebble Island — Compile-Time Reflection with Meta & Akshara

Welcome to **Pebble Island**. As the Royal Alchemist and Systems Architect, your duty is to inspect, transform, and
serialize data structures without paying any runtime penalty.

In traditional C++, writing a JSON serializer, a database ORM, or a binary packet encoder requires either:

1. Painful, error-prone manual boilerplate for every field.
2. Clunky, intrusive macros (`BOOST_HANA_DEFINE_STRUCT`, `REFLECT(...)`).
3. Heavy runtime reflection (RTTI, virtual functions, heap allocations).

Pebble provides a modern alternative: **`meta.hpp`** and **`akshara.hpp`** — single-header, zero-dependency C++23
compile-time reflection, string, and metaprogramming systems with **zero macros, zero virtual dispatch, and zero runtime
overhead**.

This tutorial assumes **zero prior knowledge of advanced template metaprogramming**. We will build from first
principles, starting with compile-time strings and advancing to automatic struct reflection, enum-to-string mapping,
tuple transformation, Structure-of-Arrays (SoA) layout transforms, and compile-time schema hashing.

---

## Table of Contents

1. [The Philosophy: Reflection Without Macros](#1-the-philosophy-reflection-without-macros)
2. [The One-File Compilation Blueprint](#2-the-one-file-compilation-blueprint)
3. [Act 1: The Indestructible Word (Compile-Time Strings with Akshara)](#act-1-the-indestructible-word-compile-time-strings-with-akshara)
4. [Act 2: The X-Ray Mirror (Automatic Aggregate Reflection)](#act-2-the-x-ray-mirror-automatic-aggregate-reflection)
5. [Act 3: The Secret Archives (Semantic ADL Reflection for Private Fields)](#act-3-the-secret-archives-semantic-adl-reflection-for-private-fields)
6. [Act 4: The Chameleon Runes (Enum Introspection & Serialization)](#act-4-the-chameleon-runes-enum-introspection--serialization)
7. [Act 5: The Transmutation Circle (Meta Algorithms: for_each, transform, fold)](#act-5-the-transmutation-circle-meta-algorithms-for_each-transform-fold)
8. [Act 6: The Universal Bridge (Tuple Interop & Destructuring)](#act-6-the-universal-bridge-tuple-interop--destructuring)
9. [Act 7: The Cache Transmuter (Structure-of-Arrays SoA Transforms)](#act-7-the-cache-transmuter-structure-of-arrays-soa-transforms)
10. [Act 8: The Seal of Integrity (Compile-Time Schema Fingerprinting)](#act-8-the-seal-of-integrity-compile-time-schema-fingerprinting)
11. [Quick API Reference & Cheat Sheet](#11-quick-api-reference--cheat-sheet)

---

## 1. The Philosophy: Reflection Without Macros

How does `meta.hpp` inspect a plain C++ struct like `struct Player { std::string name; int level; double health; };`
without any macros or annotations?

1. **Brace-Initialization Probing**: At compile time, template concepts probe how many fields a struct has using modern
   structured bindings.
2. **Type Extraction**: Structured bindings decompose the object into typed references.
3. **Symbol Demangling**: `__PRETTY_FUNCTION__` / `__FUNCSIG__` is parsed at compile time via `akshara` to extract clean
   member and type names.
4. **Constexpr Fold Execution**: Loops over fields are unrolled at compile time by the optimizer into direct pointer
   offsets.

---

## 2. The One-File Compilation Blueprint

Everything you need lives in `include/meta/meta.hpp` and `include/meta/akshara.hpp`.

Save the following template as `main.cpp` and compile with any C++23 compiler:

```cpp
#include "meta/meta.hpp"
#include "meta/akshara.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace meta;
using namespace akshara::literals;

int main() {
    std::cout << "Pebble Meta & Akshara reflection system ready!\n";
    return 0;
}
```

---

## Act 1: The Indestructible Word (Compile-Time Strings with Akshara)

### The Concept

*Akshara* (Sanskrit: अक्षर) means "that which does not perish". `akshara::fixed_string<N>` allows strings to be passed
as Non-Type Template Parameters (NTTP), concatenated, sliced, searched, and hashed entirely at compile time.

### The Code

```cpp
void act1_compile_time_strings() {
    // 1. Literal compile-time strings via ""_fs
    constexpr auto greeting = "Hello, Pebble Island!"_fs;
    static_assert(greeting.size() == 21);

    // 2. Compile-time string concatenation
    constexpr auto prefix = "KEY_"_fs;
    constexpr auto suffix = "USER_42"_fs;
    constexpr auto full_key = prefix + suffix;
    static_assert(full_key == "KEY_USER_42"_fs);

    // 3. Compile-time Substring & Search
    constexpr auto sub = full_key.substr<0, 3>();
    static_assert(sub == "KEY"_fs);
    static_assert(full_key.contains("USER"_fs));

    // 4. Compile-time FNV-1a Hashing
    constexpr uint64_t hash_val = akshara::fnv1a64("PlayerPosition");

    std::cout << "[Act 1] String: " << full_key.c_str() << " (Hash: " << hash_val << ")\n";
}
```

---

## Act 2: The X-Ray Mirror (Automatic Aggregate Reflection)

### The Problem

You have an unannotated third-party struct. You want to inspect its field count, field types, and field values
automatically.

### The Code

```cpp
struct Hero {
    std::string name;
    int health;
    double mana;
    bool is_alive;
};

void act2_automatic_reflection() {
    Hero hero{"Eldrin", 100, 45.5, true};

    // 1. Query field count at compile time
    constexpr std::size_t num_fields = meta::member_count<Hero>;
    std::cout << "[Act 2] Hero has " << num_fields << " fields.\n";

    // 2. Iterate over all fields and print name + value
    meta::for_each_member(hero, [](std::string_view name, const auto& value) {
        std::cout << "  - Field [" << name << "] = " << value << "\n";
    });

    // 3. Query type name of struct and fields
    std::cout << "  Struct Type: " << meta::type_name<Hero>() << "\n";
}
```

---

## Act 3: The Secret Archives (Semantic ADL Reflection for Private Fields)

### The Problem

Some classes encapsulate their state with `private` members. To reflect them without exposing public getters/setters,
declare an ADL hidden-friend `reflect_members`.

### The Code

```cpp
class TreasureChest {
private:
    uint32_t gold_coins_{5000};
    std::string secret_code_{"X-MARKS-THE-SPOT"};

public:
    // Hidden-friend semantic reflection descriptor
    friend constexpr auto reflect_members(TreasureChest&) {
        return meta::describe_members(
            meta::member("gold_coins", &TreasureChest::gold_coins_),
            meta::member("secret_code", &TreasureChest::secret_code_)
        );
    }
};

void act3_private_member_reflection() {
    TreasureChest chest;

    std::cout << "[Act 3] Inspecting Encapsulated Vault:\n";
    meta::for_each_member(chest, [](std::string_view name, const auto& value) {
        std::cout << "  - " << name << " -> " << value << "\n";
    });
}
```

---

## Act 4: The Chameleon Runes (Enum Introspection & Serialization)

### The Problem

Converting C++ `enum class` values to strings (and parsing strings back to enums) usually requires maintaining brittle
`switch` statements. `meta.hpp` automates this at compile time.

### The Code

```cpp
enum class IslandFaction : uint8_t {
    Merchants,
    Pirates,
    RoyalNavy,
    Alchemists
};

void act4_enum_reflection() {
    // 1. Enum to String
    IslandFaction f = IslandFaction::RoyalNavy;
    std::cout << "[Act 4] Faction Name: " << meta::enum_name(f) << "\n";

    // 2. String to Enum with std::expected (No Exceptions!)
    auto parsed = meta::enum_cast<IslandFaction>("Alchemists");
    if (parsed) {
        std::cout << "  Successfully parsed: " << static_cast<int>(*parsed) << "\n";
    }

    // 3. Iterate all enum entries
    std::cout << "  All Available Factions:\n";
    meta::enum_for_each<IslandFaction>([](std::string_view name, IslandFaction val) {
        std::cout << "    * " << name << " = " << static_cast<int>(val) << "\n";
    });
}
```

---

## Act 5: The Transmutation Circle (Meta Algorithms: for_each, transform, fold)

### The Problem

Write a generic JSON serializer that can print **any** C++ struct without knowing its types in advance.

### The Code

```cpp
template <typename T>
void print_as_json(const T& obj) {
    std::cout << "{\n";
    bool first = true;
    meta::for_each_member(obj, [&first](std::string_view name, const auto& value) {
        if (!first) std::cout << ",\n";
        first = false;
        std::cout << "  \"" << name << "\": ";
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string> ||
                      std::is_same_v<std::decay_t<decltype(value)>, const char*>) {
            std::cout << "\"" << value << "\"";
        } else if constexpr (std::is_same_v<std::decay_t<decltype(value)>, bool>) {
            std::cout << (value ? "true" : "false");
        } else {
            std::cout << value;
        }
    });
    std::cout << "\n}\n";
}

struct ShipManifest {
    std::string captain;
    int cargo_tons;
    bool armed;
};

void act5_generic_serialization() {
    ShipManifest ship{"Avery", 120, true};
    std::cout << "[Act 5] Auto-Generated JSON:\n";
    print_as_json(ship);
}
```

---

## Act 6: The Universal Bridge (Tuple Interop & Destructuring)

### The Concept

Convert any struct into a `std::tuple` of values or references for pattern matching, hashing, or structured comparisons.

### The Code

```cpp
struct Point3D {
    float x{1.0f};
    float y{2.0f};
    float z{3.0f};
};

void act6_tuple_interop() {
    Point3D p{10.0f, 20.0f, 30.0f};

    // 1. Convert struct to std::tuple of values
    auto val_tuple = meta::to_value_tuple(p);
    std::cout << "[Act 6] Tuple 1st element: " << std::get<0>(val_tuple) << "\n";

    // 2. Tie struct members as references (mutate via tuple)
    auto ref_tuple = meta::tie_members(p);
    std::get<0>(ref_tuple) = 99.0f; // Modifies p.x directly!
    std::cout << "  Mutated p.x: " << p.x << "\n";

    // 3. Reconstruct struct from tuple
    auto new_p = meta::from_tuple<Point3D>(std::make_tuple(5.0f, 6.0f, 7.0f));
    std::cout << "  Reconstructed Point: (" << new_p.x << ", " << new_p.y << ", " << new_p.z << ")\n";
}
```

---

## Act 7: The Cache Transmuter (Structure-of-Arrays SoA Transforms)

### The Problem

Storing particles as an Array of Structs (`std::vector<Particle>`) destroys SIMD and cache locality during physics
loops.

`meta::soa_vector<T>` automatically decomposes any struct `T` into parallel contiguous column buffers at compile time.

```
Array of Structs (AoS) - Cache Inefficient:
[x,y,z,mass,id] [x,y,z,mass,id] [x,y,z,mass,id]

Structure of Arrays (SoA) - Cache Friendly / SIMD Ready:
X:    [x0, x1, x2, ...]
Y:    [y0, y1, y2, ...]
Z:    [z0, z1, z2, ...]
Mass: [m0, m1, m2, ...]
```

### The Code

```cpp
struct Particle {
    float x, y, z;
    float mass;
};

void act7_soa_optimization() {
    meta::soa_vector<Particle> particles;

    particles.push_back(Particle{1.0f, 2.0f, 3.0f, 0.5f});
    particles.push_back(Particle{4.0f, 5.0f, 6.0f, 1.2f});

    std::cout << "[Act 7] SoA Particle Count: " << particles.size() << "\n";

    // Access continuous column buffers directly for SIMD
    auto& x_column = particles.get_column<0>();
    std::cout << "  Contiguous X array: [" << x_column[0] << ", " << x_column[1] << "]\n";
}
```

---

## Act 8: The Seal of Integrity (Compile-Time Schema Fingerprinting)

### The Concept

When saving binary files or sending network packets, how do you verify that the sender and receiver share the exact same
struct layout?

`meta::schema_hash<T>()` computes a deterministic 64-bit FNV-1a fingerprint based on field names, field types, and field
offsets at compile time.

### The Code

```cpp
struct PacketV1 {
    uint32_t seq;
    uint32_t payload_len;
};

struct PacketV2 {
    uint32_t seq;
    uint32_t payload_len;
    uint64_t timestamp; // Schema modified!
};

void act8_schema_hashing() {
    constexpr uint64_t v1_hash = meta::schema_hash<PacketV1>();
    constexpr uint64_t v2_hash = meta::schema_hash<PacketV2>();

    std::cout << "[Act 8] PacketV1 Schema Fingerprint: 0x" << std::hex << v1_hash << "\n";
    std::cout << "        PacketV2 Schema Fingerprint: 0x" << std::hex << v2_hash << "\n";

    static_assert(v1_hash != v2_hash, "Schema evolution detected!");
}
```

---

## 11. Quick API Reference & Cheat Sheet

| Category                | API                                                                               | Description                                      |
|-------------------------|-----------------------------------------------------------------------------------|--------------------------------------------------|
| **Compile-Time String** | `"text"_fs`, `fixed_string<N>`, `akshara::fnv1a64`                                | NTTP-capable constexpr string manipulation       |
| **Field Count**         | `meta::member_count<T>`                                                           | Compile-time count of fields in struct `T`       |
| **Field Iteration**     | `meta::for_each_member(obj, fn)`                                                  | Unrolled compile-time visitor over all fields    |
| **Type Names**          | `meta::type_name<T>()`, `meta::type_name_fs<T>()`                                 | Clean constexpr type demangling                  |
| **Enum Reflection**     | `meta::enum_name(e)`, `meta::enum_cast<E>(str)`, `meta::enum_for_each<E>(fn)`     | String-to-enum / enum-to-string mapping          |
| **Tuple Interop**       | `meta::to_value_tuple(obj)`, `meta::tie_members(obj)`, `meta::from_tuple<T>(tup)` | Zero-copy tuple conversion and reference binding |
| **SoA Memory**          | `meta::soa_vector<T>`                                                             | Automatic Structure-of-Arrays cache optimizer    |
| **Schema Hashing**      | `meta::type_hash<T>()`, `meta::schema_hash<T>()`                                  | Structural FNV-1a binary schema fingerprinting   |
| **Binary Predicates**   | `meta::is_binary_stable<T>`, `meta::is_zero_copy_safe<T>`                         | Compile-time memory safety checks                |
