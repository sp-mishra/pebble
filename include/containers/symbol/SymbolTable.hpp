#pragma once


// ============================================================================
// SymbolTable — concurrent symbol registry with namespace index
//
// Three cooperating components:
//
//   symbol_entry     — plain data: name (interned), address, version, id
//
//   SymbolTable<Mutex>
//     Primary lookup store. Keys are interned string_views so the hot-path
//     resolve() does a pointer-equality hash lookup under a shared lock —
//     no heap allocation, no string copy.
//     Supports: register, resolve, resolve_versioned, unregister, bulk
//     register from range, snapshot/rollback.
//
//   NamespaceIndex
//     Secondary, non-owning index built on NAryTree<string_view, symbol_entry*>.
//     Splits qualified names ("lithe::runtime::linker::foo") on "::" and
//     maintains a trie of namespace components. Used for enumeration and
//     lowest-common-ancestor path queries — not for the O(1) resolve path.
//
// Thread safety
// -------------
//   SymbolTable  : concurrent reads, exclusive writes (shared_mutex).
//   NamespaceIndex: same shared_mutex pattern; insert holds write lock,
//                   enumerate/depth/path hold read lock.
//
// Lock acquisition order
// ----------------------
//   SymbolTable::mtx_ must always be acquired before InternPool::mtx_.
//   Any code path that inverts this order risks deadlock.
//
// Usage
// -----
//   symtab::SymbolTable<> tbl;
//   tbl.register_symbol("lithe::runtime::foo", reinterpret_cast<void*>(&foo));
//   void* p = tbl.resolve("lithe::runtime::foo");
// ============================================================================

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "containers/symbol/InternPool.hpp"
#include "containers/tree/NAryTree.hpp"

namespace symtab {
    // ============================================================================
    // Errors
    // ============================================================================

    enum class SymError : std::uint8_t {
        AlreadyRegistered, // name exists at same or newer version
        NotFound, // resolve / unregister on unknown symbol
        InvalidName, // empty name
        SnapshotUnderflow, // rollback to a size larger than current
    };

    template <typename T>
    using SymResult = std::expected<T, SymError>;

    // ============================================================================
    // Strong id
    // ============================================================================

    struct SymbolId {
        std::uint32_t value{0};
        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }

        constexpr auto operator<=>(const SymbolId&) const noexcept = default;
    };

    static constexpr SymbolId INVALID_SYMBOL_ID{};

    // ============================================================================
    // symbol_entry — plain data, name pointer is stable (owned by InternPool)
    // ============================================================================

    struct symbol_entry {
        std::string_view name; // stable pointer into InternPool
        void* address{nullptr};
        std::uint32_t version{0};
        std::uint32_t id{0}; // assigned by SymbolTable; 0 = not yet registered
    };

    // ============================================================================
    // SymbolTable
    //
    // Move operations are deleted: map_ and insertion_order_ hold string_view
    // keys into pool_ storage. Moving pool_ would relocate its unordered_set
    // nodes, leaving all existing string_view keys dangling.
    // ============================================================================

    template <typename Mutex = std::shared_mutex>
    class SymbolTable {
    public:
        SymbolTable() = default;

        ~SymbolTable() = default;

        SymbolTable(const SymbolTable&) = delete;

        SymbolTable& operator=(const SymbolTable&) = delete;

        SymbolTable(SymbolTable&&) = delete;

        SymbolTable& operator=(SymbolTable&&) = delete;

        // -------------------------------------------------------------------------
        // register_symbol
        //   Returns a new SymbolId on success.
        //   Returns SymError::AlreadyRegistered if the name already exists with
        //   version >= the requested version (downgrade / same-version rejected).
        //   Returns SymError::InvalidName for empty names.
        // -------------------------------------------------------------------------
        [[nodiscard]] SymResult<SymbolId>
        register_symbol(const std::string_view name, void* addr, const std::uint32_t version = 0) {
            if (name.empty()) return std::unexpected(SymError::InvalidName);

            std::unique_lock wl(mtx_);
            // Lock order: SymbolTable::mtx_ held here, then InternPool::mtx_ inside
            // intern_or_throw. Never acquire these in reverse order elsewhere.
            auto interned = pool_.intern_or_throw(name);

            if (const auto it = map_.find(interned); it != map_.end()) {
                if (it->second.version >= version)
                    return std::unexpected(SymError::AlreadyRegistered);
                // Version upgrade: replace in place; id is stable.
                it->second.address = addr;
                it->second.version = version;
                return SymbolId{it->second.id};
            }

            const auto id = next_id_++;
            map_.emplace(interned, symbol_entry{interned, addr, version, id});
            insertion_order_.push_back(interned);
            return SymbolId{id};
        }

        // -------------------------------------------------------------------------
        // resolve — O(1) hot path, shared lock only, no allocation.
        // The map uses StringHash/StringEqual (transparent), so the string_view
        // key is looked up by value without constructing std::string.
        // Returns nullptr if the symbol is not registered.
        // -------------------------------------------------------------------------
        [[nodiscard]] void* resolve(const std::string_view name) const {
            std::shared_lock rl(mtx_);
            const auto it = map_.find(name);
            return (it != map_.end()) ? it->second.address : nullptr;
        }

        // resolve_versioned — same as resolve but also checks version matches exactly.
        [[nodiscard]] void* resolve_versioned(const std::string_view name,
                                              const std::uint32_t version) const {
            std::shared_lock rl(mtx_);
            const auto it = map_.find(name);
            if (it == map_.end()) return nullptr;
            return (it->second.version == version) ? it->second.address : nullptr;
        }

        // -------------------------------------------------------------------------
        // contains / size
        // -------------------------------------------------------------------------
        [[nodiscard]] bool contains(const std::string_view name) const {
            std::shared_lock rl(mtx_);
            return map_.contains(name);
        }

        [[nodiscard]] std::size_t size() const {
            std::shared_lock rl(mtx_);
            return map_.size();
        }

        // -------------------------------------------------------------------------
        // unregister — returns true if the symbol existed and was removed.
        // -------------------------------------------------------------------------
        bool unregister(const std::string_view name) {
            std::unique_lock wl(mtx_);
            const auto it = map_.find(name);
            if (it == map_.end()) return false;
            const auto interned = it->first;
            map_.erase(it);
            std::erase(insertion_order_, interned);
            return true;
        }

        // -------------------------------------------------------------------------
        // register_range — bulk insert from any input range of symbol_entry.
        // Acquires the write lock once for the entire range.
        // Returns the count of successfully registered symbols.
        // -------------------------------------------------------------------------
        template <std::ranges::input_range R>
            requires std::same_as < std::ranges::range_value_t < R >



        ,
        symbol_entry
        >
        std::size_t register_range(R&& entries) {
            std::size_t count = 0;
            std::unique_lock wl(mtx_);
            for (auto&& e : entries) {
                if (register_symbol_locked_(e.name, e.address, e.version).has_value())
                    ++count;
            }
            return count;
        }

        // -------------------------------------------------------------------------
        // snapshot / rollback
        //   snapshot() returns the current number of registered symbols.
        //   rollback(n) removes all symbols registered after the snapshot,
        //   in reverse insertion order.
        //
        //   Note: InternPool is NOT rolled back. Strings interned for rolled-back
        //   symbols persist for the pool's lifetime. This is intentional — pool
        //   grows monotonically; callers must not rely on pool size matching map size.
        // -------------------------------------------------------------------------
        [[nodiscard]] std::size_t snapshot() const {
            std::shared_lock rl(mtx_);
            return map_.size();
        }

        SymResult<void> rollback(const std::size_t target_size) {
            std::unique_lock wl(mtx_);
            if (target_size > map_.size())
                return std::unexpected(SymError::SnapshotUnderflow);
            while (map_.size() > target_size) {
                auto sv = insertion_order_.back();
                insertion_order_.pop_back();
                map_.erase(sv);
            }
            return {};
        }

        // -------------------------------------------------------------------------
        // lookup_entry — returns a copy of the symbol_entry for inspection.
        // -------------------------------------------------------------------------
        [[nodiscard]] SymResult<symbol_entry> lookup_entry(const std::string_view name) const {
            std::shared_lock rl(mtx_);
            const auto it = map_.find(name);
            if (it == map_.end()) return std::unexpected(SymError::NotFound);
            return it->second;
        }

    private:
        // Assumes mtx_ is already held (write lock). Used by register_range.
        [[nodiscard]] SymResult<SymbolId>
        register_symbol_locked_(const std::string_view name, void* addr, const std::uint32_t version) {
            if (name.empty()) return std::unexpected(SymError::InvalidName);
            auto interned = pool_.intern_or_throw(name);
            if (const auto it = map_.find(interned); it != map_.end()) {
                if (it->second.version >= version)
                    return std::unexpected(SymError::AlreadyRegistered);
                it->second.address = addr;
                it->second.version = version;
                return SymbolId{it->second.id};
            }
            const auto id = next_id_++;
            map_.emplace(interned, symbol_entry{interned, addr, version, id});
            insertion_order_.push_back(interned);
            return SymbolId{id};
        }

        mutable Mutex mtx_;
        InternPool pool_;

        // key: interned string_view (pointer into pool_, stable forever)
        // value: symbol_entry carries its own id — no separate id_map_ needed
        std::unordered_map<std::string_view, symbol_entry, StringHash, StringEqual> map_;

        // insertion_order_ tracks registration sequence for snapshot/rollback.
        std::vector<std::string_view> insertion_order_;
        std::uint32_t next_id_{1};
    };

    // ============================================================================
    // NamespaceIndex
    //
    // Wraps NAryTree<string_view, symbol_entry*> to provide namespace-scoped
    // enumeration. Splits "a::b::c::sym" into components ["a","b","c","sym"],
    // walks/creates interior nodes for the namespace prefix, and attaches the
    // symbol_entry* to the leaf.
    //
    // The tree node data is the component string_view (interned, stable).
    // The node metadata is symbol_entry* — non-null only on leaves that represent
    // a registered symbol; nullptr on pure-namespace interior nodes.
    //
    // Requires an InternPool& reference. Prefix strings for node_cache_ keys are
    // interned into the pool so node_cache_ uses string_view keys with no heap
    // allocation on lookup.
    // ============================================================================

    class NamespaceIndex {
        using Meta = symbol_entry*; // nullptr for ns nodes
        using Tree = NAryTree<std::string_view, Meta>;
        using TNode = Tree::TreeNode;

    public:
        // Default ctor: owns an internal InternPool for prefix strings.
        NamespaceIndex() : owned_pool_(std::in_place), pool_(*owned_pool_) {
            init_root_();
        }

        // External pool ctor: caller manages pool lifetime; must outlive this index.
        explicit NamespaceIndex(InternPool& pool) : pool_(pool) {
            init_root_();
        }

        NamespaceIndex(const NamespaceIndex&) = delete;

        NamespaceIndex& operator=(const NamespaceIndex&) = delete;

        // -------------------------------------------------------------------------
        // insert — registers a symbol in the namespace trie.
        //   entry.name must be an interned string_view.
        //   The last component is the symbol name; all prior components are
        //   namespace segments.
        // -------------------------------------------------------------------------
        void insert(symbol_entry* entry) {
            if (!entry || entry->name.empty()) return;
            std::unique_lock wl(mtx_);

            const auto components = split_name(entry->name);
            TNode* parent = root_node_;

            // Walk / create namespace nodes for all but the last component.
            for (std::size_t i = 0; i + 1 < components.size(); ++i) {
                const auto seg = components[i];
                // Intern the prefix string to get a stable string_view key.
                auto cache_key = intern_prefix_(components, i + 1);
                if (auto it = node_cache_.find(cache_key); it != node_cache_.end()) {
                    parent = it->second;
                }
                else {
                    TNode* ns_node = tree_.insert(parent, seg, nullptr);
                    node_cache_.emplace(cache_key, ns_node);
                    parent = ns_node;
                }
            }

            // Leaf: the symbol itself. entry->name is already interned.
            // Guard against duplicate inserts: if this full name is already in the
            // cache, do not create a second leaf node.
            if (node_cache_.contains(entry->name)) return;
            const auto sym_name = components.back();
            TNode* leaf = tree_.insert(parent, sym_name, entry);
            node_cache_.emplace(entry->name, leaf);
        }

        // -------------------------------------------------------------------------
        // enumerate — returns all symbol_entry* under a namespace prefix.
        //   ns_prefix="" returns everything; "lithe::runtime" returns all symbols
        //   registered under that subtree.
        // -------------------------------------------------------------------------
        [[nodiscard]] std::vector<symbol_entry*> enumerate(const std::string_view ns_prefix) const {
            std::shared_lock rl(mtx_);
            const TNode* subtree_root = find_ns_node(ns_prefix);
            if (!subtree_root) return {};

            std::vector<symbol_entry*> result;
            std::queue<const TNode*> q;
            q.push(subtree_root);
            while (!q.empty()) {
                const TNode* cur = q.front();
                q.pop();
                if (cur->metadata) result.push_back(cur->metadata);
                for (const auto& child : cur->children)
                    q.push(child.get());
            }
            return result;
        }

        // -------------------------------------------------------------------------
        // depth — depth of a namespace node in the trie (1 = top-level, 2 = one
        // level nested, etc.). Returns 0 if the prefix is not found.
        // The virtual root ("") is not counted.
        // -------------------------------------------------------------------------
        [[nodiscard]] std::size_t depth(const std::string_view ns_prefix) const {
            std::shared_lock rl(mtx_);
            const TNode* node = find_ns_node(ns_prefix);
            if (!node || node == root_node_) return 0;
            // Count hops from node up to (not including) the virtual root.
            std::size_t d = 0;
            const TNode* cur = node;
            while (cur && cur != root_node_) {
                ++d;
                cur = cur->parent;
            }
            return d;
        }

        // -------------------------------------------------------------------------
        // path — namespace component path between two prefixes via LCA.
        //   Returns the sequence of component names traversed from from_ns to to_ns.
        //   Returns nullopt if either prefix is unknown.
        // -------------------------------------------------------------------------
        [[nodiscard]] std::optional<std::vector<std::string_view>>
        path(const std::string_view from_ns, const std::string_view to_ns) const {
            std::shared_lock rl(mtx_);
            TNode* a = find_ns_node(from_ns);
            TNode* b = find_ns_node(to_ns);
            if (!a || !b) return std::nullopt;

            const TNode* lca = tree_.lowest_common_ancestor(a, b);
            if (!lca) return std::nullopt;

            // Collect path: a → lca (reverse), then lca → b.
            auto up = path_to_node(a, lca);
            const auto down = path_from_node(lca, b);

            std::vector<std::string_view> result;
            for (const auto& it : std::views::reverse(up))
                result.push_back(it->data);
            for (const auto* n : down)
                result.push_back(n->data);
            return result;
        }

    private:
        // Split "a::b::c" → ["a","b","c"].
        static std::vector<std::string_view> split_name(const std::string_view name) {
            std::vector<std::string_view> parts;
            std::size_t start = 0;
            while (start < name.size()) {
                const auto pos = name.find("::", start);
                if (pos == std::string_view::npos) {
                    parts.push_back(name.substr(start));
                    break;
                }
                parts.push_back(name.substr(start, pos - start));
                start = pos + 2;
            }
            return parts;
        }

        // Intern "a::b::c" (first `count` components) into pool_, returning a
        // stable string_view suitable as a node_cache_ key.
        std::string_view intern_prefix_(const std::vector<std::string_view>& parts,
                                        const std::size_t count) const {
            std::string s;
            for (std::size_t i = 0; i < count; ++i) {
                if (i) s += "::";
                s += parts[i];
            }
            return pool_.intern_or_throw(s);
        }

        // Find the tree node for a given namespace prefix (empty = root).
        // No heap allocation: node_cache_ uses string_view keys.
        [[nodiscard]] TNode* find_ns_node(const std::string_view ns) const {
            const auto it = node_cache_.find(ns);
            return (it != node_cache_.end()) ? it->second : nullptr;
        }

        // Collect nodes from `start` up to (not including) `stop`.
        static std::vector<TNode*> path_to_node(TNode* start, const TNode* stop) {
            std::vector<TNode*> path;
            TNode* cur = start;
            while (cur && cur != stop) {
                path.push_back(cur);
                cur = cur->parent;
            }
            return path;
        }

        // Collect nodes from `lca` down to `target` by walking parent pointers
        // of `target` until we hit `lca`, then reverse.
        static std::vector<TNode*> path_from_node(const TNode* lca, TNode* target) {
            std::vector<TNode*> path;
            TNode* cur = target;
            while (cur && cur != lca) {
                path.push_back(cur);
                cur = cur->parent;
            }
            std::ranges::reverse(path);
            return path;
        }

        void init_root_() {
            root_node_ = tree_.insert(nullptr, std::string_view{""}, nullptr);
            node_cache_[std::string_view{""}] = root_node_;
        }

        std::optional<InternPool> owned_pool_; // non-empty when default-constructed
        InternPool& pool_;
        mutable std::shared_mutex mtx_;
        Tree tree_;
        TNode* root_node_{nullptr};
        // Cache: interned prefix string_view → TNode* for O(1) namespace lookups.
        // Keys are stable string_views into pool_ (or the static "" for the root).
        std::unordered_map<std::string_view, TNode*, StringHash, StringEqual> node_cache_;
    };
} // namespace symtab
