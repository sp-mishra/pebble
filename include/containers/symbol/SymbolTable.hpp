#pragma once


// ============================================================================
// SymbolTable — concurrent symbol registry with namespace index
//
// Three cooperating components:
//
//   basic_symbol_entry<Value> — plain data: name (interned), address/value, version, id
//   symbol_entry              — alias for basic_symbol_entry<void*>
//
//   SymbolTable<Value, Mutex, Map, Vector>
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
//                  String interning occurs outside the table write lock to
//                  maximize concurrency across threads.
//   NamespaceIndex: same shared_mutex pattern; insert holds write lock,
//                   enumerate/depth/path hold read lock.
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

#include "containers/dynamic/SmallVector.hpp"
#include "containers/symbol/InternPool.hpp"
#include "containers/tree/NAryTree.hpp"

namespace symtab {
    // ============================================================================
    // Errors
    // ============================================================================

    enum class SymError : std::uint8_t {
        AlreadyRegistered, // name exists at same or newer version
        NotFound,          // resolve / unregister on unknown symbol
        InvalidName,       // empty name
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
    // basic_symbol_entry — plain data, name pointer is stable (owned by InternPool)
    // ============================================================================

    template <typename Value = void*>
    struct basic_symbol_entry {
        std::string_view name{}; // stable pointer into InternPool
        Value address{};
        std::uint32_t version{0};
        std::uint32_t id{0}; // assigned by SymbolTable; 0 = not yet registered
    };

    using symbol_entry = basic_symbol_entry<void*>;

    // ============================================================================
    // SymbolTable
    // ============================================================================

    template <
        typename Value = void*,
        typename Mutex = std::shared_mutex,
        typename Map   = std::unordered_map<std::string_view, basic_symbol_entry<Value>, StringHash, StringEqual>,
        typename Vector = containers::SmallVector<std::string_view, 64>
    >
    class SymbolTable {
    public:
        using value_type = Value;
        using mutex_type = Mutex;
        using map_type   = Map;
        using vector_type = Vector;
        using entry_type = basic_symbol_entry<Value>;

        SymbolTable() = default;

        ~SymbolTable() = default;

        SymbolTable(const SymbolTable&) = delete;

        SymbolTable& operator=(const SymbolTable&) = delete;

        // Move ctor / assign: thread-safe move using scoped lock on both tables.
        SymbolTable(SymbolTable&& other) noexcept {
            std::scoped_lock lk(mtx_, other.mtx_);
            pool_ = std::move(other.pool_);
            map_ = std::move(other.map_);
            insertion_order_ = std::move(other.insertion_order_);
            next_id_ = other.next_id_;
            other.next_id_ = 1;
        }

        SymbolTable& operator=(SymbolTable&& other) noexcept {
            if (this != &other) {
                std::scoped_lock lk(mtx_, other.mtx_);
                pool_ = std::move(other.pool_);
                map_ = std::move(other.map_);
                insertion_order_ = std::move(other.insertion_order_);
                next_id_ = other.next_id_;
                other.next_id_ = 1;
            }
            return *this;
        }

        // -------------------------------------------------------------------------
        // register_symbol
        //   Returns a new SymbolId on success.
        //   Returns SymError::AlreadyRegistered if the name already exists with
        //   version >= the requested version (downgrade / same-version rejected).
        //   Returns SymError::InvalidName for empty names.
        // -------------------------------------------------------------------------
        [[nodiscard]] SymResult<SymbolId>
        register_symbol(const std::string_view name, Value addr, const std::uint32_t version = 0) {
            if (name.empty()) return std::unexpected(SymError::InvalidName);

            // Intern outside the table write lock to allow concurrent interning across threads.
            auto intern_res = pool_.intern(name);
            if (!intern_res) return std::unexpected(SymError::InvalidName);
            const auto interned = *intern_res;

            std::unique_lock wl(mtx_);
            if (const auto it = map_.find(interned); it != map_.end()) {
                if (it->second.version >= version)
                    return std::unexpected(SymError::AlreadyRegistered);
                // Version upgrade: replace in place; id is stable.
                it->second.address = addr;
                it->second.version = version;
                return SymbolId{it->second.id};
            }

            const auto id = next_id_++;
            map_.emplace(interned, entry_type{interned, addr, version, id});
            insertion_order_.push_back(interned);
            return SymbolId{id};
        }

        // -------------------------------------------------------------------------
        // resolve — O(1) hot path, shared lock only, no allocation.
        // Returns default value (e.g. nullptr) if the symbol is not registered.
        // -------------------------------------------------------------------------
        [[nodiscard]] Value resolve(const std::string_view name) const {
            std::shared_lock rl(mtx_);
            const auto it = map_.find(name);
            return (it != map_.end()) ? it->second.address : Value{};
        }

        // resolve_versioned — same as resolve but also checks version matches exactly.
        [[nodiscard]] Value resolve_versioned(const std::string_view name,
                                              const std::uint32_t version) const {
            std::shared_lock rl(mtx_);
            const auto it = map_.find(name);
            if (it == map_.end()) return Value{};
            return (it->second.version == version) ? it->second.address : Value{};
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
            auto it_order = std::remove(insertion_order_.begin(), insertion_order_.end(), interned);
            insertion_order_.erase(it_order, insertion_order_.end());
            return true;
        }

        // -------------------------------------------------------------------------
        // register_range — bulk insert from any input range of symbol entries.
        // Acquires the write lock once for the entire range.
        // -------------------------------------------------------------------------
        template <std::ranges::input_range R>
            requires std::same_as<std::ranges::range_value_t<R>, entry_type>
        std::size_t register_range(R&& entries) {
            std::size_t count = 0;
            for (auto&& e : entries) {
                if (e.name.empty()) continue;
                (void)pool_.intern(e.name);
            }
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
                const auto sv = insertion_order_.back();
                insertion_order_.pop_back();
                map_.erase(sv);
            }
            return {};
        }

        // -------------------------------------------------------------------------
        // lookup_entry — returns a copy of the entry for inspection.
        // -------------------------------------------------------------------------
        [[nodiscard]] SymResult<entry_type> lookup_entry(const std::string_view name) const {
            std::shared_lock rl(mtx_);
            const auto it = map_.find(name);
            if (it == map_.end()) return std::unexpected(SymError::NotFound);
            return it->second;
        }

        // Direct reference to underlying pool.
        [[nodiscard]] InternPool& pool() noexcept { return pool_; }
        [[nodiscard]] const InternPool& pool() const noexcept { return pool_; }

    private:
        // Assumes mtx_ is already held (write lock).
        [[nodiscard]] SymResult<SymbolId>
        register_symbol_locked_(const std::string_view name, Value addr, const std::uint32_t version) {
            if (name.empty()) return std::unexpected(SymError::InvalidName);
            auto intern_res = pool_.intern(name);
            if (!intern_res) return std::unexpected(SymError::InvalidName);
            const auto interned = *intern_res;

            if (const auto it = map_.find(interned); it != map_.end()) {
                if (it->second.version >= version)
                    return std::unexpected(SymError::AlreadyRegistered);
                it->second.address = addr;
                it->second.version = version;
                return SymbolId{it->second.id};
            }
            const auto id = next_id_++;
            map_.emplace(interned, entry_type{interned, addr, version, id});
            insertion_order_.push_back(interned);
            return SymbolId{id};
        }

        mutable Mutex mtx_;
        InternPool pool_;
        Map map_;
        Vector insertion_order_;
        std::uint32_t next_id_{1};
    };

    // ============================================================================
    // DelimiterScanner — zero-allocation delimiter iterator for string_view
    // ============================================================================

    class DelimiterScanner {
    public:
        constexpr DelimiterScanner(const std::string_view str, const std::string_view delim) noexcept
            : str_(str), delim_(delim) {}

        struct Iterator {
            std::string_view str{};
            std::string_view delim{};
            std::size_t start{0};
            std::size_t end{0};
            bool done{false};

            constexpr Iterator() noexcept : done(true) {}
            constexpr Iterator(const std::string_view s, const std::string_view d) noexcept
                : str(s), delim(d), start(0), done(s.empty()) {
                if (!done) {
                    end = str.find(delim, start);
                }
            }

            constexpr std::string_view operator*() const noexcept {
                if (end == std::string_view::npos) {
                    return str.substr(start);
                }
                return str.substr(start, end - start);
            }

            constexpr Iterator& operator++() noexcept {
                if (end == std::string_view::npos) {
                    done = true;
                } else {
                    start = end + delim.size();
                    end = str.find(delim, start);
                }
                return *this;
            }

            constexpr bool operator==(const Iterator& other) const noexcept {
                if (done && other.done) return true;
                return done == other.done && start == other.start && str == other.str;
            }
        };

        [[nodiscard]] constexpr Iterator begin() const noexcept { return Iterator(str_, delim_); }
        [[nodiscard]] constexpr Iterator end() const noexcept { return Iterator(); }

    private:
        std::string_view str_;
        std::string_view delim_;
    };

    // ============================================================================
    // NamespaceIndex
    //
    // Wraps NAryTree<string_view, symbol_entry*> to provide namespace-scoped
    // enumeration without heap allocations during parsing.
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
        // -------------------------------------------------------------------------
        void insert(symbol_entry* entry) {
            if (!entry || entry->name.empty()) return;
            std::unique_lock wl(mtx_);

            TNode* parent = root_node_;
            std::size_t start = 0;

            while (true) {
                const auto pos = entry->name.find("::", start);
                if (pos == std::string_view::npos) {
                    // Leaf: the symbol name component
                    const auto sym_name = entry->name.substr(start);
                    if (node_cache_.contains(entry->name)) return;
                    TNode* leaf = tree_.insert(parent, sym_name, entry);
                    node_cache_.emplace(entry->name, leaf);
                    break;
                }

                // Interior namespace segment
                const auto seg = entry->name.substr(start, pos - start);
                const auto prefix_view = entry->name.substr(0, pos);

                if (auto it = node_cache_.find(prefix_view); it != node_cache_.end()) {
                    parent = it->second;
                } else {
                    auto interned_prefix = pool_.intern_or_throw(prefix_view);
                    TNode* ns_node = tree_.insert(parent, seg, nullptr);
                    node_cache_.emplace(interned_prefix, ns_node);
                    parent = ns_node;
                }
                start = pos + 2;
            }
        }

        // -------------------------------------------------------------------------
        // enumerate — returns all symbol_entry* under a namespace prefix.
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
        // depth — depth of a namespace node in the trie.
        // -------------------------------------------------------------------------
        [[nodiscard]] std::size_t depth(const std::string_view ns_prefix) const {
            std::shared_lock rl(mtx_);
            const TNode* node = find_ns_node(ns_prefix);
            if (!node || node == root_node_) return 0;

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
            containers::SmallVector<TNode*, 32> up;
            TNode* cur = a;
            while (cur && cur != lca) {
                up.push_back(cur);
                cur = cur->parent;
            }

            containers::SmallVector<TNode*, 32> down;
            cur = b;
            while (cur && cur != lca) {
                down.push_back(cur);
                cur = cur->parent;
            }
            std::ranges::reverse(down);

            std::vector<std::string_view> result;
            result.reserve(up.size() + down.size());
            for (const auto& it : std::views::reverse(up))
                result.push_back(it->data);
            for (const auto* n : down)
                result.push_back(n->data);
            return result;
        }

    private:
        [[nodiscard]] TNode* find_ns_node(const std::string_view ns) const {
            const auto it = node_cache_.find(ns);
            return (it != node_cache_.end()) ? it->second : nullptr;
        }

        void init_root_() {
            root_node_ = tree_.insert(nullptr, std::string_view{""}, nullptr);
            node_cache_[std::string_view{""}] = root_node_;
        }

        std::optional<InternPool> owned_pool_;
        InternPool& pool_;
        mutable std::shared_mutex mtx_;
        Tree tree_;
        TNode* root_node_{nullptr};
        std::unordered_map<std::string_view, TNode*, StringHash, StringEqual> node_cache_;
    };
} // namespace symtab
