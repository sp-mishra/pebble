#pragma once

// vakya/types.hpp — Vākya type-term representation (opt-in, additive).
//
// C++23, header-only, no virtual, no macros. Opt-in; not pulled by vakya.hpp.
// Namespace: vakya::types
//
// τ ::= κ (primitive) | α (variable) | C(τ…) (constructor) | (τ…→τ) (callable)
//       | ∀ᾱ.τ (quantified) | alias(name, τ) | tensor | effect | capability | ownership | opaque
//
// type_ref = generational_handle<type_tag> into a slot_map over a LinearArena.
// Canonical structural equality: interned-handle identity after canonicalize().
// Extension: specialise type_descriptor<Ctor> with stable_id >= kTypeKindExtensionBase.
//
// Internal libraries used:
//   smriti::pools::LinearArena      — type-node arena storage
//   kosha::core::Cache / ShardedCache — interning + canonicalize memo
//   containers::slot_map            — type_ref handle store
//   containers::generational_handle — type_ref handle type
//   containers::dynamic::SmallVector — child lists
//   containers::symbol::InternPool  — type/constructor names

#include "vakya/vakya.hpp"
#include "containers/handle/generational_handle.hpp"
#include "containers/associative/slot_map.hpp"
#include "containers/dynamic/SmallVector.hpp"
#include "containers/cache/kosha.hpp"
#include "mem/arena.hpp"

#include <cassert>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace vakya::types {
    // ============================================================================
    // type_kind — open enum, extension band >= kTypeKindExtensionBase
    // ============================================================================

    enum class type_kind : std::uint32_t {
        primitive = 0,
        variable = 1,
        constructor = 2,
        callable = 3, // (τ…→τ)
        quantified = 4, // ∀ᾱ.τ
        alias = 5,
        tensor = 6,
        effect = 7,
        capability = 8,
        ownership = 9,
        opaque = 10,
    };

    inline constexpr std::uint32_t kTypeKindExtensionBase = emit::kExtensionIdBase;

    // ============================================================================
    // Variance for subtyping
    // ============================================================================

    enum class variance : std::uint8_t { covariant = 0, contravariant = 1, invariant = 2 };

    // ============================================================================
    // type_tag / type_ref — phantom-typed stable handle
    // ============================================================================

    struct type_tag {};

    using type_ref = containers::generational_handle<type_tag, std::uint32_t>;

    // ============================================================================
    // type_descriptor<Ctor> — single source of truth for constructor metadata.
    // Built-in primitive tags use stable_id < kTypeKindExtensionBase.
    // Downstream constructors specialise this with stable_id >= kTypeKindExtensionBase.
    // ============================================================================

    template <class Ctor>
    struct type_descriptor {
        // Derived specialisations must provide:
        //   static constexpr std::uint32_t   stable_id;
        //   static constexpr std::uint8_t    arity;       // kTypeVariadicArity for variadic
        //   static constexpr std::string_view symbol;
        //   static constexpr std::span<const variance> variance_vec; // per-argument
    };

    inline constexpr std::uint8_t kTypeVariadicArity = emit::kVariadicArity; // 0xFF

    // ============================================================================
    // Built-in type constructor tags (stable_id < 1000)
    // ============================================================================

    // Primitive types
    struct integer_type_tag {};

    struct float_type_tag {};

    struct bool_type_tag {};

    struct char_type_tag {};

    struct string_type_tag {};

    struct void_type_tag {};

    struct dynamic_type_tag {};

    // Composite types
    struct array_type_tag {};

    struct vector_type_tag {};

    struct tuple_type_tag {};

    struct struct_type_tag {};

    struct union_type_tag {};

    struct function_type_tag {};

    struct optional_type_tag {};

    struct result_type_tag {};

    struct map_type_tag {};

    struct list_type_tag {};

    struct tensor_type_tag {};

    // Built-in descriptor specialisations
    template <>
    struct type_descriptor<integer_type_tag> {
        static constexpr std::uint32_t stable_id = 1;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Integer";
    };

    template <>
    struct type_descriptor<float_type_tag> {
        static constexpr std::uint32_t stable_id = 2;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Float";
    };

    template <>
    struct type_descriptor<bool_type_tag> {
        static constexpr std::uint32_t stable_id = 3;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Bool";
    };

    template <>
    struct type_descriptor<char_type_tag> {
        static constexpr std::uint32_t stable_id = 4;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Char";
    };

    template <>
    struct type_descriptor<string_type_tag> {
        static constexpr std::uint32_t stable_id = 5;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "String";
    };

    template <>
    struct type_descriptor<void_type_tag> {
        static constexpr std::uint32_t stable_id = 6;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Void";
    };

    template <>
    struct type_descriptor<dynamic_type_tag> {
        static constexpr std::uint32_t stable_id = 7;
        static constexpr std::uint8_t arity = 0;
        static constexpr std::string_view symbol = "Dynamic";
    };

    template <>
    struct type_descriptor<array_type_tag> {
        static constexpr std::uint32_t stable_id = 10;
        static constexpr std::uint8_t arity = 2; // element type + size type
        static constexpr std::string_view symbol = "Array";
    };

    template <>
    struct type_descriptor<vector_type_tag> {
        static constexpr std::uint32_t stable_id = 11;
        static constexpr std::uint8_t arity = 1;
        static constexpr std::string_view symbol = "Vector";
    };

    template <>
    struct type_descriptor<tuple_type_tag> {
        static constexpr std::uint32_t stable_id = 12;
        static constexpr std::uint8_t arity = kTypeVariadicArity;
        static constexpr std::string_view symbol = "Tuple";
    };

    template <>
    struct type_descriptor<struct_type_tag> {
        static constexpr std::uint32_t stable_id = 13;
        static constexpr std::uint8_t arity = kTypeVariadicArity;
        static constexpr std::string_view symbol = "Struct";
    };

    template <>
    struct type_descriptor<union_type_tag> {
        static constexpr std::uint32_t stable_id = 14;
        static constexpr std::uint8_t arity = kTypeVariadicArity;
        static constexpr std::string_view symbol = "Union";
    };

    template <>
    struct type_descriptor<function_type_tag> {
        static constexpr std::uint32_t stable_id = 15;
        static constexpr std::uint8_t arity = kTypeVariadicArity; // params… + return
        static constexpr std::string_view symbol = "->";
    };

    template <>
    struct type_descriptor<optional_type_tag> {
        static constexpr std::uint32_t stable_id = 16;
        static constexpr std::uint8_t arity = 1;
        static constexpr std::string_view symbol = "Optional";
    };

    template <>
    struct type_descriptor<result_type_tag> {
        static constexpr std::uint32_t stable_id = 17;
        static constexpr std::uint8_t arity = 2;
        static constexpr std::string_view symbol = "Result";
    };

    template <>
    struct type_descriptor<map_type_tag> {
        static constexpr std::uint32_t stable_id = 18;
        static constexpr std::uint8_t arity = 2;
        static constexpr std::string_view symbol = "Map";
    };

    template <>
    struct type_descriptor<list_type_tag> {
        static constexpr std::uint32_t stable_id = 19;
        static constexpr std::uint8_t arity = 1;
        static constexpr std::string_view symbol = "List";
    };

    template <>
    struct type_descriptor<tensor_type_tag> {
        static constexpr std::uint32_t stable_id = 20;
        static constexpr std::uint8_t arity = kTypeVariadicArity; // element type + shape dims
        static constexpr std::string_view symbol = "Tensor";
    };

    // ============================================================================
    // type_var_id — index into the substitution's union-find
    // ============================================================================

    using type_var_id = std::uint32_t;
    inline constexpr type_var_id kInvalidTypeVarId = std::numeric_limits<type_var_id>::max();

    // ============================================================================
    // type_node — flat tagged-union value (arena-stored, interned by type_arena)
    // ============================================================================

    struct type_node {
        type_kind kind = type_kind::primitive;
        std::uint32_t descriptor_stable_id = 0;

        // Children stored as handles (already interned).  SmallVector keeps small lists inline.
        containers::dynamic::SmallVector<type_ref, 32> children{};

        // Payload variants (only one active per kind):
        type_var_id var_id = kInvalidTypeVarId; // kind == variable
        std::uint64_t alias_name_hash = 0; // kind == alias
        type_ref alias_def{}; // kind == alias — definition

        // Quantified vars stored separately:
        containers::dynamic::SmallVector<type_var_id, 16> quantified_vars{};

        // Extra payload hash (tensor dims, effect id, etc.) for user extension kinds.
        std::uint64_t payload_hash = 0;

        // Cached FNV-1a hash; 0 = not yet computed. type_hash() sets this on first call.
        mutable std::uint64_t cached_hash = 0;

        [[nodiscard]] bool operator==(const type_node& o) const noexcept {
            if (kind != o.kind) return false;
            if (descriptor_stable_id != o.descriptor_stable_id) return false;
            if (var_id != o.var_id) return false;
            if (alias_name_hash != o.alias_name_hash) return false;
            if (alias_def != o.alias_def) return false;
            if (payload_hash != o.payload_hash) return false;
            if (children.size() != o.children.size()) return false;
            for (std::size_t i = 0; i < children.size(); ++i) {
                if (children[i] != o.children[i]) return false;
            }
            if (quantified_vars.size() != o.quantified_vars.size()) return false;
            for (std::size_t i = 0; i < quantified_vars.size(); ++i) {
                if (quantified_vars[i] != o.quantified_vars[i]) return false;
            }
            return true;
        }
    };

    // ============================================================================
    // type_hash — FNV-1a over descriptor.stable_id + child handles + payload
    // ============================================================================

    inline std::uint64_t type_hash(const type_node& n) noexcept {
        if (n.cached_hash != 0) return n.cached_hash;

        constexpr std::uint64_t kFNVBasis = 14695981039346656037ULL;
        constexpr std::uint64_t kFNVPrime = 1099511628211ULL;

        auto fnv_mix = [&](std::uint64_t h, std::uint64_t v) noexcept -> std::uint64_t {
            for (int b = 0; b < 8; ++b) {
                h ^= (v & 0xFFu);
                h *= kFNVPrime;
                v >>= 8;
            }
            return h;
        };

        std::uint64_t h = kFNVBasis;
        h = fnv_mix(h, static_cast<std::uint64_t>(n.kind));
        h = fnv_mix(h, static_cast<std::uint64_t>(n.descriptor_stable_id));
        h = fnv_mix(h, static_cast<std::uint64_t>(n.var_id));
        h = fnv_mix(h, n.alias_name_hash);
        h = fnv_mix(h, n.payload_hash);

        // Use child handle .index field (stable for an interned node) as the hash input.
        for (const type_ref& c : n.children) {
            h = fnv_mix(h, static_cast<std::uint64_t>(c.index));
        }
        for (type_var_id v : n.quantified_vars) {
            h = fnv_mix(h, static_cast<std::uint64_t>(v));
        }

        // Avoid storing 0 so the cache sentinel remains unambiguous.
        if (h == 0) h = 1;
        n.cached_hash = h;
        return h;
    }

    // ============================================================================
    // type_arena — owns all type_node storage + interning index
    // ============================================================================

    // Intern cache: uint64_t hash -> type_ref
    using type_intern_cache_t = kosha::core::Cache<
        std::uint64_t,
        type_ref,
        kosha::core::LRUPolicy<std::uint64_t>
    >;

    // Canonicalize memo: type_ref.index -> type_ref
    using type_canon_cache_t = kosha::core::Cache<
        std::uint32_t,
        type_ref,
        kosha::core::LRUPolicy<std::uint32_t>
    >;

    enum class type_ir_kind : std::uint32_t {};   // opaque; kind encoded in type_node

    // View of type_arena as lang::ir_module<type_ir_kind, type_ref, handle_store<type_tag>>.
    // Nodes are NOT copied — the view holds a const pointer to the arena's slot_map.
    // child adjacency is derived from type_node::children (stored as type_ref handles).
    //
    // Usage:
    //   auto view = arena.as_ir_module_view();
    //   auto adj  = view.as_egraph_view();   // feeds generic egraph tooling
    //   std::uint32_t n = view.size();
    struct type_ir_module_view {
        const containers::slot_map<type_node, type_ref>* store = nullptr;

        [[nodiscard]] const type_node* find(type_ref ref) const noexcept {
            if (!store) return nullptr;
            return store->find(ref);
        }

        [[nodiscard]] std::uint32_t size() const noexcept {
            return store ? static_cast<std::uint32_t>(store->size()) : 0;
        }

        [[nodiscard]] bool empty() const noexcept {
            return !store || store->empty();
        }

        // adj(ref) — child type_ref handles of this type node.
        [[nodiscard]] containers::dynamic::SmallVector<type_ref, 8>
        adj(type_ref ref) const noexcept {
            containers::dynamic::SmallVector<type_ref, 8> out;
            const type_node* n = find(ref);
            if (!n) return out;
            for (const type_ref& c : n->children) out.push_back(c);
            return out;
        }

        // as_egraph_view() — returns self (view is already adjacency-capable).
        [[nodiscard]] const type_ir_module_view& as_egraph_view() const noexcept { return *this; }
        [[nodiscard]] const type_ir_module_view& as_adjacency()   const noexcept { return *this; }
    };

    class type_arena {
    public:
        explicit type_arena(std::size_t capacity = 1 << 20)
            : arena_{capacity},
              intern_cache_{4096},
              canon_cache_{4096} {}

        type_arena(const type_arena&) = delete;
        type_arena& operator=(const type_arena&) = delete;
        type_arena(type_arena&&) = delete;
        type_arena& operator=(type_arena&&) = delete;

        // Intern a type_node: returns the canonical type_ref for this node.
        // If a structurally equal node is already interned, returns its handle.
        [[nodiscard]] type_ref intern(type_node node) {
            const std::uint64_t h = type_hash(node);

            // Check existing bucket (collision chain is short in practice)
            if (auto cached = intern_cache_.get(h); cached) {
                const type_ref candidate = *cached;
                if (const type_node* existing = store_.find(candidate)) {
                    if (*existing == node) return candidate;
                }
            }

            // New node — insert into slot_map
            type_ref ref = store_.insert(std::move(node));
            (void)intern_cache_.put(h, ref);
            return ref;
        }

        // Intern a variable type (fresh var).
        [[nodiscard]] type_ref intern_variable(type_var_id vid) {
            type_node n;
            n.kind = type_kind::variable;
            n.descriptor_stable_id = 0;
            n.var_id = vid;
            return intern(std::move(n));
        }

        // Intern a primitive (nullary) type from a built-in tag.
        template <class Ctor>
        [[nodiscard]] type_ref intern_primitive() {
            type_node n;
            n.kind = type_kind::primitive;
            n.descriptor_stable_id = type_descriptor<Ctor>::stable_id;
            return intern(std::move(n));
        }

        // Intern a constructor application: C(τ…).
        template <class Ctor>
        [[nodiscard]] type_ref intern_constructor(std::span<const type_ref> children_span) {
            type_node n;
            n.kind = type_kind::constructor;
            n.descriptor_stable_id = type_descriptor<Ctor>::stable_id;
            for (const type_ref& c : children_span) n.children.push_back(c);
            return intern(std::move(n));
        }

        // Intern a callable: (params… → return).
        [[nodiscard]] type_ref intern_callable(std::span<const type_ref> params,
                                               type_ref return_type) {
            type_node n;
            n.kind = type_kind::callable;
            n.descriptor_stable_id = type_descriptor<function_type_tag>::stable_id;
            for (const type_ref& p : params) n.children.push_back(p);
            n.children.push_back(return_type);
            return intern(std::move(n));
        }

        // Intern a quantified type: ∀ᾱ.τ.
        [[nodiscard]] type_ref intern_quantified(std::span<const type_var_id> vars,
                                                 type_ref body) {
            type_node n;
            n.kind = type_kind::quantified;
            n.descriptor_stable_id = 0;
            for (type_var_id v : vars) n.quantified_vars.push_back(v);
            n.children.push_back(body);
            return intern(std::move(n));
        }

        // Intern an alias: name=alias_name_hash, definition=def.
        [[nodiscard]] type_ref intern_alias(std::uint64_t name_hash, type_ref def) {
            type_node n;
            n.kind = type_kind::alias;
            n.descriptor_stable_id = 0;
            n.alias_name_hash = name_hash;
            n.alias_def = def;
            n.children.push_back(def);
            return intern(std::move(n));
        }

        // ---- IR alignment views (Stage 9) -----------------------------------

        // as_ir_module_view() — non-copying view of vakya's type store as a
        // generic IR module. Exposes size(), find(type_ref), adj(type_ref), and
        // as_egraph_view(). The underlying slot_map backing is not duplicated.
        [[nodiscard]] type_ir_module_view as_ir_module_view() const noexcept {
            return type_ir_module_view{&store_};
        }

        [[nodiscard]] type_ir_module_view as_egraph_view() const noexcept {
            return type_ir_module_view{&store_};
        }

        // Retrieve a node by handle (nullptr if stale).
        [[nodiscard]] const type_node* get(type_ref ref) const noexcept {
            return store_.find(ref);
        }

        // Structural equality via interned identity.
        [[nodiscard]] bool type_equal(type_ref a, type_ref b) const noexcept {
            return a == b;
        }

        // Canonicalize: expand aliases transitively, re-intern.
        enum class canon_error { alias_cycle };

        [[nodiscard]] std::expected<type_ref, canon_error>
        canonicalize(type_ref t) {
            if (auto cached = canon_cache_.get(t.index)) {
                return *cached;
            }
            containers::dynamic::SmallVector<type_ref, 32> visiting;
            auto result = expand_canonical(t, visiting);
            if (result) {
                (void)canon_cache_.put(t.index, *result);
            }
            return result;
        }

    private:
        [[nodiscard]] std::expected<type_ref, canon_error>
        expand_canonical(type_ref t,
                         containers::dynamic::SmallVector<type_ref, 32>& visiting) {
            const type_node* n = store_.find(t);
            if (!n) return t;

            if (n->kind != type_kind::alias) {
                type_node rebuilt;
                rebuilt.kind = n->kind;
                rebuilt.descriptor_stable_id = n->descriptor_stable_id;
                rebuilt.var_id = n->var_id;
                rebuilt.alias_name_hash = n->alias_name_hash;
                rebuilt.alias_def = n->alias_def;
                rebuilt.payload_hash = n->payload_hash;
                for (type_var_id v : n->quantified_vars) rebuilt.quantified_vars.push_back(v);

                for (const type_ref& c : n->children) {
                    auto cr = expand_canonical(c, visiting);
                    if (!cr) return cr;
                    rebuilt.children.push_back(*cr);
                }
                return intern(std::move(rebuilt));
            }

            // Alias kind: cycle check
            for (std::size_t i = 0; i < visiting.size(); ++i) {
                if (visiting[i] == t) return std::unexpected(canon_error::alias_cycle);
            }
            visiting.push_back(t);
            auto result = expand_canonical(n->alias_def, visiting);
            visiting.pop_back();
            return result;
        }

        smriti::pools::LinearArena arena_;
        containers::slot_map<type_node, type_ref> store_;
        type_intern_cache_t intern_cache_;
        type_canon_cache_t canon_cache_;
    };

    // ============================================================================
    // Convenience: make a fresh type-variable id
    // ============================================================================

    class type_var_generator {
        std::uint32_t next_ = 0;

    public:
        [[nodiscard]] type_var_id fresh() noexcept { return next_++; }
        [[nodiscard]] std::uint32_t count() const noexcept { return next_; }
        // Advance counter to at least min_next — used to sync after subst.make_var()
        // allocations that bypass gen.fresh().
        void sync_to(std::uint32_t min_next) noexcept {
            if (next_ < min_next) next_ = min_next;
        }
    };

    // ============================================================================
    // kosha_dedup_adapter — satisfies the generic Dedup seam with kosha
    // ============================================================================
    //
    // The generic ir_module Dedup policy interface:
    //   dedup(hash, ref) → type_ref   — returns existing ref if hash seen; else inserts
    //   insert(hash, ref) → void      — record hash→ref mapping
    //
    // kosha_dedup_adapter wraps type_intern_cache_t so the same cache that drives
    // type_arena::intern() also satisfies the generic Dedup seam without duplication.

    struct kosha_dedup_adapter {
        type_intern_cache_t* cache = nullptr;

        // Returns the cached ref if hash has been seen; otherwise inserts and returns ref.
        [[nodiscard]] type_ref dedup(std::uint64_t hash, type_ref ref) {
            if (!cache) return ref;
            if (auto existing = cache->get(hash); existing) return *existing;
            (void)cache->put(hash, ref);
            return ref;
        }

        void insert(std::uint64_t hash, type_ref ref) {
            if (cache) (void)cache->put(hash, ref);
        }
    };

} // namespace vakya::types