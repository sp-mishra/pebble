#pragma once

// languages/generic/ir/interning.hpp — Name interning + structural hash-consing for IR.
//
// C++23, header-only, no virtual, no macros. Namespace: lang
//
// ir_interner — combines InternPool string interning with subtree deduplication.
//   intern_name(sv)    → symbol_id (InternPool-backed; pointer-stable).
//   dedup(hash, id)    → ir_node_id: returns existing id if structurally identical node seen,
//                        else records id and returns it (new).
//
// Enabled via a Dedup policy tag on ir_module (default = no interner, zero cost).
// InternPool lives in containers/symbol/InternPool.hpp; namespace symtab.

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include "node.hpp"
#include "../../../containers/symbol/InternPool.hpp"

namespace lang {
    class ir_interner {
    public:
        // Intern a name string — returns a stable symbol_id (index into the pool).
        // Repeated calls with the same string return the same id (pointer-equality guarantee).
        [[nodiscard]] symbol_id intern_name(std::string_view sv) {
            const auto result = pool_.intern(sv);
            if (!result) return k_null_symbol;
            const auto interned = *result;
            // Map stable pointer to a dense uint32 id.
            const auto key = reinterpret_cast<std::uintptr_t>(interned.data());
            auto [it, inserted] = name_to_id_.emplace(key, next_symbol_id_);
            if (inserted) ++next_symbol_id_;
            return it->second;
        }

        // Dedup: if structural_hash already registered, return existing node id.
        // Otherwise register new_id and return it.
        [[nodiscard]] ir_node_id dedup(std::uint64_t structural_hash, ir_node_id new_id) {
            auto [it, inserted] = hash_to_node_.emplace(structural_hash, new_id);
            return it->second;
        }

        // Clear all interning state (name pool survives — strings remain stable).
        void reset_dedup() noexcept { hash_to_node_.clear(); }

        void clear() {
            hash_to_node_.clear();
            name_to_id_.clear();
            next_symbol_id_ = k_null_symbol + 1;
        }

    private:
        symtab::InternPool pool_;
        std::unordered_map<std::uintptr_t, symbol_id> name_to_id_;
        std::unordered_map<std::uint64_t, ir_node_id> hash_to_node_;
        symbol_id next_symbol_id_ = k_null_symbol + 1;
    };
} // namespace lang
