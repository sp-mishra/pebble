# EasyRules: Modern C++23 Rule Engine

## Overview

EasyRules is a high-performance, header-only rule engine built with modern C++23 principles. It provides a
zero-overhead, type-safe DSL for declarative business logic without virtual functions or runtime polymorphism overhead.

## Architecture

### Design Principles

1. **No Virtual Functions**: Uses CRTP (Curiously Recurring Template Pattern) for extensibility instead of virtual
   inheritance. This eliminates vtable indirection and enables compiler optimizations.

2. **Compile-Time DSL**: Leverages C++23 concepts and `constexpr` to validate rules at compile-time where possible,
   catching errors early.

3. **Type Safety**: All facts are stored in a `std::variant<int, bool, std::string, double>` with compile-time type
   checking via `std::expected<T, RuleError>` for error handling.

4. **Zero-Overhead Abstractions**: Facts storage uses `std::unordered_map` with transparent hashing for O (1) lookup.
   Rule evaluation is iterative with early exit on failure.

### Core Components

#### Facts

The type-safe fact store. Supports get/set/remove operations with type checking.

```cpp
ExecutionContext ctx;
ctx.facts.set("age", 25);
auto result = ctx.facts.get<int>("age");
if (result.has_value()) {
    std::cout << *result << '\n';  // 25
} else {
    // Handle error: TypeMismatch, FactNotFound, etc.
}
```

**Features:**

- Snapshots: Immutable string representation of all facts for auditing
- Bulk operations: Set multiple facts at once
- Iteration: Range-based for loop over all facts
- get_or: Default values for missing facts

#### Rules & DSL

Rules are defined using the fluent DSL:

```cpp
engine.when("rule_name", predicate)
    .then([](ExecutionContext& ctx) {
        // action code
    });
```

**Predicates** are callable objects returning `bool`. Common patterns:

- `fact<T>("key") == value`: Equality check
- `fact<T>("key") > value`: Comparison
- `pred1 && pred2`: Logical AND
- Lambda predicates: `[](const Facts&) { return ...; }`

#### Listeners & CRTP

Extend the engine via listeners without virtual functions:

```cpp
struct MyListener : public RuleListener<MyListener> {
    bool before_evaluate_impl(const Rule&, const ExecutionContext&) {
        return true;  // false to veto
    }
    void on_success_impl(const Rule&, ExecutionContext&) { }
    void on_failure_impl(const Rule&, ExecutionContext&) { }
    void on_skipped_impl(const Rule&) { }
};

MyListener listener;
engine.add_listener(listener);
```

**Callbacks:**

- `before_evaluate_impl()`: Pre-evaluation hook; return false to skip/veto rule
- `on_success_impl()`: Called when rule fires
- `on_failure_impl()`: Called when predicate fails
- `on_skipped_impl()`: Called when rule is skipped

## Performance Characteristics

### Time Complexity

- Rule evaluation: O (r) where r is the number of rules (iterates each once)
- Predicate evaluation: O (1) for most predicates, O (n) for collection checks
- Fact lookup: O (1) average (unordered_map with transparent hashing)

### Space Complexity

- O (f) for facts storage (f = number of facts)
- O (r) for rules storage
- No per-rule allocation overhead beyond the rule object itself

### Optimization Techniques

1. **Fact Snapshots with Double-Buffering**: Satisfies snapshot caching without copying
2. **Transparent Hashing**: String view hashing allows lookups without allocation
3. **Early Exit**: Rules stop evaluating on first predicate failure
4. **Compile-Time Validation**: C++23 concepts catch misuse before instantiation

## Type Safety & Error Handling

Facts are stored in `std::variant` for heterogeneous storage. All access goes through `std::expected<T, RuleError>`:

```cpp
enum class RuleError {
    InvalidPredicate,      // Predicate failed
    FactNotFound,          // Key doesn't exist
    TypeMismatch,          // Requested wrong type
    ValidationFailed,      // Custom validation failed
    ActivationLimitReached // Iteration limit hit
};
```

**Type Checking:**

```cpp
ctx.facts.set("age", 25);
auto result = ctx.facts.get<int>("age");  // Ok
auto bad = ctx.facts.get<std::string>("age");  // Error: TypeMismatch
```

## Listener System

### Built-in Listeners

**AuditListener**: Basic event logging

```cpp
AuditListener audit;
engine.add_listener(audit);
engine.run(ctx);
const auto& history = audit.get_history();  // vector<AuditEvent>
```

**EnhancedAuditListener**: Statistics and performance metrics

```cpp
EnhancedAuditListener audit;
engine.add_listener(audit);
engine.run(ctx);
auto stats = audit.get_rule_statistics();  // vector<RuleStatistic>
// Each stat has: name, execution_count, total_time, avg_time
```

### Custom Listeners

Use CRTP to avoid virtual functions:

```cpp
struct PerfListener : public RuleListener<PerfListener> {
    void on_success_impl(const Rule& rule, ExecutionContext& ctx) {
        std::cout << "Rule " << rule.name << " fired\n";
    }
};
```

## Usage Example

```cpp
#include "rules/easy_rules.hpp"
using namespace easy_rules;
using namespace easy_rules::dsl;

int main() {
    ExecutionContext ctx;
    ctx.facts.set("temperature", 28);
    ctx.facts.set("humidity", 65);

    EasyRuleEngine engine;
    engine.when("too_hot", fact<int>("temperature") > 30)
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("alert", std::string("temperature high"));
        });

    engine.when("comfortable", 
        (fact<int>("temperature") >= 20 && fact<int>("temperature") <= 28) &&
        (fact<int>("humidity") >= 40 && fact<int>("humidity") <= 60))
        .then([](ExecutionContext& ctx) {
            ctx.facts.set("comfort_level", std::string("optimal"));
        });

    engine.run(ctx);
    
    auto comfort = ctx.facts.get<std::string>("comfort_level");
    if (comfort.has_value()) {
        std::cout << *comfort << '\n';  // "optimal"
    }
}
```

## DSL API Reference

### Fact Factory Functions

- `fact<T>(name)`: Create fact reference for type T
- `string_fact(name)`: Create string fact reference (alias)
- `numeric_fact<T>(name)`: Create numeric fact reference

### Operators

All fact references support comparison operators:

- `==`, `!=`: Equality
- `<`, `<=`, `>`, `>=`: Comparison
- `&&`, `||`: Logical operations

### Collection Operations

- `in(container)(fact_ref)`: Check if fact value is in container (requires FactRef interface)

## Thread Safety

The engine is **not thread-safe** by default. For multi-threaded usage:

- Protect engine.run () with mutex
- or use `concurrent_run()` (if implemented)

## Limitations & Future Work

1. **No I/O Context**: Rule actions cannot perform async operations
2. **Simple Type System**: Facts limited to int, bool, string, double
3. **No Rule Transactions**: Changes are immediate, no rollback
4. **in () API**: Collection operations require FactRef interface completion

## Related Concepts

- **CRTP**: Compile-time polymorphism without virtual functions
- **std::expected**: Error handling without exceptions
- **C++23 Concepts**: Compile-time type constraints
- **Transparent Hashing**: String view-based lookups
