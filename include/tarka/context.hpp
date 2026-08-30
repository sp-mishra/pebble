#pragma once
// =============================================================================
// tarka/context.hpp — Arena + Hash-consed Term/Sort (CSE)
//
// Namespace:  tarka
// Provides:
//   SortImpl    — arena-owned immutable sort node
//   TermImpl    — arena-owned immutable term node
//   Context     — owns BumpPool arena + ShardedLRUCache interning tables
//
// Design:
//   - Arena: smriti::BumpPool<SystemRAMDomain> for bump allocation of nodes.
//   - Interning: kosha::ShardedLRUCache<uint64_t, *Impl> with structural
//     equality verify-on-hit (hash collision → structural check before accept).
//   - Context is non-copyable, movable. Term construction is single-threaded
//     per Context; interned nodes are immutable so cross-thread reads need no lock.
//   - Checkpoint/restore uses saved arena offset for scoped scratch terms.
//   - Operator overloads on Term recover Context from ptr_->ctx_.
// =============================================================================

#include "tarka/term.hpp"

#include "mem/smriti.hpp"
#include "mem/arena.hpp"
#include "containers/cache/kosha.hpp"
#include "containers/dynamic/SmallVector.hpp"

#include <cassert>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace tarka {
    // =========================================================================
    // Structural hash / equality helpers
    // =========================================================================

    namespace detail {
        [[nodiscard]] inline std::uint64_t mix64(std::uint64_t h) noexcept {
            h ^= h >> 33u;
            h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33u;
            h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33u;
            return h;
        }

        [[nodiscard]] inline std::uint64_t hash_combine(std::uint64_t h, std::uint64_t v) noexcept {
            return mix64(h ^ (v + 0x9e3779b97f4a7c15ULL + (h << 6u) + (h >> 2u)));
        }
    } // namespace detail

    // =========================================================================
    // SortImpl — arena-owned immutable sort node
    // =========================================================================

    struct SortImpl {
        SortKind kind;
        std::uint32_t scalar_param; // bitvec width
        std::uint16_t param_count; // number of sort_params stored after this struct
        std::uint8_t _pad[1] = {};
        std::uint64_t hash;
        Context* ctx_; // back-pointer (for Sort::ctx())
        const SortImpl* coll_next = nullptr; // intern collision chain (same hash bucket)
        // sort_params follow in memory: Sort params_[param_count]
        // Accessed via: reinterpret_cast<const Sort*>(this + 1)
    };

    // =========================================================================
    // TermImpl — arena-owned immutable term node
    // =========================================================================

    struct TermImpl {
        Op op;
        std::uint16_t child_count;
        std::uint32_t node_id = 0; // dense monotonic id (assigned at intern; enables SparseSet-keyed theory state)
        Sort sort_;
        std::uint64_t hash;
        std::uint64_t payload_hash; // for Lit/Sym nodes; 0 otherwise
        Context* ctx_; // back-pointer for operator overloads
        const TermImpl* coll_next = nullptr; // intern collision chain (same hash bucket)
        // children follow in memory: Term children_[child_count]
        // Accessed via: reinterpret_cast<const Term*>(this + 1)
    };

    // =========================================================================
    // Sort member implementations (need TermImpl/SortImpl defs)
    // =========================================================================

    [[nodiscard]] inline SortKind Sort::kind() const noexcept {
        assert(ptr_);
        return ptr_->kind;
    }

    [[nodiscard]] inline std::uint32_t Sort::scalar_param() const noexcept {
        assert(ptr_);
        return ptr_->scalar_param;
    }

    [[nodiscard]] inline std::span<const Sort> Sort::sort_params() const noexcept {
        assert(ptr_);
        return {reinterpret_cast<const Sort*>(ptr_ + 1), ptr_->param_count};
    }

    // =========================================================================
    // Term member implementations
    // =========================================================================

    [[nodiscard]] inline Op Term::op() const noexcept {
        assert(ptr_);
        return ptr_->op;
    }

    [[nodiscard]] inline Sort Term::sort() const noexcept {
        assert(ptr_);
        return ptr_->sort_;
    }

    [[nodiscard]] inline std::span<const Term> Term::children() const noexcept {
        assert(ptr_);
        return {reinterpret_cast<const Term*>(ptr_ + 1), ptr_->child_count};
    }

    [[nodiscard]] inline Context& Term::ctx() const noexcept {
        assert(ptr_);
        return *ptr_->ctx_;
    }

    // =========================================================================
    // Context
    // =========================================================================

    class Context {
    public:
        static constexpr std::size_t kDefaultArena = 4 * 1024 * 1024; // 4 MB

        explicit Context(std::size_t arena_bytes = kDefaultArena)
            : arena_(arena_bytes)
              , sort_table_(512)
              , term_table_(4096) {}

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        Context(Context&&) = delete;
        Context& operator=(Context&&) = delete;

        // ---------------------------------------------------------------------
        // Sort construction
        // ---------------------------------------------------------------------

        [[nodiscard]] Sort make_sort(SortKind kind,
                                     std::span<const Sort> sort_params = {},
                                     std::uint32_t scalar_param = 0) {
            const std::uint64_t h = sort_hash(kind, sort_params, scalar_param);

            // Probe the intern table with a full structural verify over the whole
            // collision chain (hash equal ≠ structurally equal).
            if (const SortImpl** head = sort_table_.find(h)) {
                for (const SortImpl* e = *head; e; e = e->coll_next)
                    if (sort_equal(e, kind, sort_params, scalar_param))
                        return Sort{e, h};
                // Hash present but no structural match — a genuine collision;
                // fall through and prepend the newcomer to this chain.
            }

            // Allocate new SortImpl + trailing Sort params in arena
            const std::size_t param_bytes = sort_params.size() * sizeof(Sort);
            const std::size_t total = sizeof(SortImpl) + param_bytes;
            void* raw = arena_.allocate(total, alignof(SortImpl));
            assert(raw && "tarka::Context sort arena exhausted");

            auto* impl = new(raw) SortImpl{};
            impl->kind = kind;
            impl->scalar_param = scalar_param;
            impl->param_count = static_cast<std::uint16_t>(sort_params.size());
            impl->hash = h;
            impl->ctx_ = this;

            // Copy sort_params after the struct
            if (!sort_params.empty()) {
                auto* dest = reinterpret_cast<Sort*>(impl + 1);
                std::memcpy(dest, sort_params.data(), param_bytes);
            }

            // Prepend to the collision chain for this hash bucket.
            if (const SortImpl** head = sort_table_.find(h)) {
                impl->coll_next = *head;
                sort_table_.insert_or_assign(h, impl);
            } else {
                sort_table_.insert(h, impl);
            }
            journal_sort(h);
            return Sort{impl, h};
        }

        // Convenience: base sorts
        [[nodiscard]] Sort bool_sort() { return make_sort(SortKind::Bool); }
        [[nodiscard]] Sort int_sort() { return make_sort(SortKind::Int); }
        [[nodiscard]] Sort real_sort() { return make_sort(SortKind::Real); }
        [[nodiscard]] Sort string_sort() { return make_sort(SortKind::String); }

        [[nodiscard]] Sort bv_sort(std::uint32_t width) {
            return make_sort(SortKind::BitVec, {}, width);
        }

        [[nodiscard]] Sort array_sort(Sort index_sort, Sort elem_sort) {
            const Sort params[2] = {index_sort, elem_sort};
            return make_sort(SortKind::Array, params, 0);
        }

        [[nodiscard]] Sort function_sort(std::span<const Sort> domain_sorts, Sort range_sort) {
            // ≤7-arg function sorts (domain + range) stay stack-resident; wider
            // spills to heap. Sort is 16B, so 8·16 = 128 bytes of inline budget.
            containers::dynamic::SmallVector<Sort, 8 * sizeof(Sort)> params;
            params.reserve(domain_sorts.size() + 1);
            for (const Sort& s : domain_sorts) params.push_back(s);
            params.push_back(range_sort);
            return make_sort(SortKind::Function,
                             std::span<const Sort>{params.data(), params.size()}, 0);
        }

        // ---------------------------------------------------------------------
        // Term construction
        // ---------------------------------------------------------------------

        [[nodiscard]] Term make_term(Op op,
                                     Sort sort,
                                     std::span<const Term> children,
                                     std::uint64_t payload_hash = 0) {
            const std::uint64_t h = term_hash(op, sort, children, payload_hash);

            if (const TermImpl** head = term_table_.find(h)) {
                for (const TermImpl* e = *head; e; e = e->coll_next)
                    if (term_equal(e, op, sort, children, payload_hash))
                        return Term{e, h};
            }

            const std::size_t child_bytes = children.size() * sizeof(Term);
            const std::size_t total = sizeof(TermImpl) + child_bytes;
            void* raw = arena_.allocate(total, alignof(TermImpl));
            assert(raw && "tarka::Context term arena exhausted");

            auto* impl = new(raw) TermImpl{};
            impl->op = op;
            impl->child_count = static_cast<std::uint16_t>(children.size());
            impl->node_id = next_node_id_++;
            impl->sort_ = sort;
            impl->hash = h;
            impl->payload_hash = payload_hash;
            impl->ctx_ = this;

            if (!children.empty()) {
                auto* dest = reinterpret_cast<Term*>(impl + 1);
                std::memcpy(dest, children.data(), child_bytes);
            }

            if (const TermImpl** head = term_table_.find(h)) {
                impl->coll_next = *head;
                term_table_.insert_or_assign(h, impl);
            } else {
                term_table_.insert(h, impl);
            }
            journal_term(h);
            return Term{impl, h};
        }

        [[nodiscard]] Term make_term(Op op,
                                     Sort sort,
                                     std::initializer_list<Term> children,
                                     std::uint64_t payload_hash = 0) {
            return make_term(op, sort, std::span<const Term>(children.begin(), children.size()), payload_hash);
        }

        // Symbolic variable
        //
        // Two distinct names can share an FNV-1a payload hash. We probe a chain of
        // rehashed keys (ph, ph2, ph3, …) until we find either the name already
        // stored under that key (reuse it) or a free key (claim it). This makes
        // symbol interning collision-safe for any number of aliasing names, not
        // just two — symbol_name() stays faithful and equal names still intern to
        // the same payload key.
        [[nodiscard]] Term make_symbol(std::string_view name, Sort sort) {
            std::uint64_t key = symbol_payload_hash(name);
            for (;;) {
                const std::string* existing = symbols_.find(key);
                if (!existing) {
                    symbols_.insert(key, std::string{name});
                    journal_symbol(key);
                    return make_term(Op::Sym, sort, {}, key);
                }
                if (*existing == name)
                    return make_term(Op::Sym, sort, {}, key);
                // Occupied by a different name — rehash to the next chain slot.
                key = detail::hash_combine(key, detail::mix64(key ^ 0xDEADC0DEULL));
                if (key == 0) key = 1;
            }
        }

        // Literal: bool
        [[nodiscard]] Term make_bool(bool v) {
            return make_term(v ? Op::True : Op::False, make_sort(SortKind::Bool), {}, 0);
        }

        // Literal: bitvector (≤64 bit)
        [[nodiscard]] Term make_value(std::uint64_t bits, Sort bv_sort_) {
            assert(bv_sort_.valid() && bv_sort_.kind() == SortKind::BitVec);
            const std::uint64_t ph = detail::hash_combine(bits, bv_sort_.hash());
            bv_literals_.insert(ph, bv_value{bits, bv_sort_.scalar_param()});
            return make_term(Op::Lit, bv_sort_, {}, ph);
        }

        // Literal: int64
        [[nodiscard]] Term make_int(std::int64_t v, Sort int_sort_) {
            assert(int_sort_.valid() && int_sort_.kind() == SortKind::Int);
            const std::uint64_t ph = detail::hash_combine(static_cast<std::uint64_t>(v), 0x494e5400ULL);
            int_literals_.insert(ph, v);
            return make_term(Op::Lit, int_sort_, {}, ph);
        }

        // Literal: real/rational
        [[nodiscard]] Term make_real(rational v, Sort real_sort_) {
            assert(real_sort_.valid() && real_sort_.kind() == SortKind::Real);
            const std::uint64_t ph = detail::hash_combine(
                static_cast<std::uint64_t>(v.num),
                detail::hash_combine(static_cast<std::uint64_t>(v.den), 0x5245414cULL));
            real_literals_.insert(ph, v);
            return make_term(Op::Lit, real_sort_, {}, ph);
        }

        [[nodiscard]] Term make_real(std::int64_t num, std::int64_t den = 1, Sort real_sort_ = {}) {
            if (!real_sort_.valid()) real_sort_ = real_sort();
            return make_real(rational{num, den}, real_sort_);
        }

        // Retrieve symbol name by payload_hash
        [[nodiscard]] std::string_view symbol_name(std::uint64_t ph) const noexcept {
            if (const std::string* s = symbols_.find(ph)) return *s;
            return {};
        }

        // Retrieve bv_value by payload_hash
        [[nodiscard]] std::optional<bv_value> bv_literal(std::uint64_t ph) const noexcept {
            if (const bv_value* v = bv_literals_.find(ph)) return *v;
            return std::nullopt;
        }

        // Retrieve int64 literal by payload_hash
        [[nodiscard]] std::optional<std::int64_t> int_literal(std::uint64_t ph) const noexcept {
            if (const std::int64_t* v = int_literals_.find(ph)) return *v;
            return std::nullopt;
        }

        // Retrieve real/rational literal by payload_hash
        [[nodiscard]] std::optional<rational> real_literal(std::uint64_t ph) const noexcept {
            if (const rational* v = real_literals_.find(ph)) return *v;
            return std::nullopt;
        }

        // ---------------------------------------------------------------------
        // Structured-term convenience constructors
        // ---------------------------------------------------------------------

        // if-then-else: cond must be Bool; then/else must share a sort.
        [[nodiscard]] Term make_ite(Term cond, Term then_t, Term else_t) {
            assert(cond.sort().kind() == SortKind::Bool);
            assert(then_t.sort() == else_t.sort());
            const Term children[3] = {cond, then_t, else_t};
            return make_term(Op::Ite, then_t.sort(), children);
        }

        // Quantifiers: bound_vars are Sym terms scoped over a Bool body. The
        // quantified term itself is Bool-sorted.
        [[nodiscard]] Term make_forall(std::span<const Term> bound_vars, Term body) {
            return make_quantifier(Op::Forall, bound_vars, body);
        }

        [[nodiscard]] Term make_exists(std::span<const Term> bound_vars, Term body) {
            return make_quantifier(Op::Exists, bound_vars, body);
        }

        // ---------------------------------------------------------------------
        // Checkpoint / rollback (for scoped scratch terms)
        //
        // rollback() rewinds the arena to the checkpoint offset AND erases every
        // term/sort/symbol/literal interned after the checkpoint from the caches,
        // so no cache entry ever points into freed arena memory. Pre-checkpoint
        // entries are untouched. Checkpoints are a stack: each rollback pops back
        // to the matching depth. Handles obtained after the checkpoint must not
        // be used after rollback.
        // ---------------------------------------------------------------------

        struct checkpoint_t {
            std::size_t arena_offset;
            std::size_t journal_mark;
        };

        [[nodiscard]] checkpoint_t checkpoint() noexcept {
            ++checkpoint_depth_;
            return {arena_.used_bytes(), journal_.size()};
        }

        void rollback(checkpoint_t cp) noexcept {
            // Evict intern-table entries created after the checkpoint (LIFO) before
            // the arena memory they name is reclaimed. Within one hash bucket the
            // chain head is the newest node, and the journal replays newest-first,
            // so popping the head each time removes exactly the right node.
            for (std::size_t i = journal_.size(); i > cp.journal_mark; --i) {
                const journal_entry& e = journal_[i - 1];
                switch (e.kind) {
                    case journal_kind::term: {
                        if (const TermImpl** head = term_table_.find(e.hash)) {
                            const TermImpl* next = (*head)->coll_next;
                            if (next) term_table_.insert_or_assign(e.hash, next);
                            else term_table_.erase(e.hash);
                        }
                        break;
                    }
                    case journal_kind::sort: {
                        if (const SortImpl** head = sort_table_.find(e.hash)) {
                            const SortImpl* next = (*head)->coll_next;
                            if (next) sort_table_.insert_or_assign(e.hash, next);
                            else sort_table_.erase(e.hash);
                        }
                        break;
                    }
                    case journal_kind::symbol: symbols_.erase(e.hash); break;
                }
            }
            journal_.resize(cp.journal_mark);
            if (checkpoint_depth_ > 0) --checkpoint_depth_;
            arena_.rollback(smriti::pools::LinearArena::Checkpoint{cp.arena_offset});
        }

    private:
        using ArenaPool = smriti::pools::LinearArena;
        // Interning is a *correctness* structure, not a cache: an interned node
        // must never be evicted (its address is a stable identity used as a map
        // key throughout the solver). So the tables are permanent, non-evicting
        // FlatHashStorage keyed on the structural hash, with each slot holding the
        // head of an intrusive collision chain (coll_next) walked with a full
        // structural compare on hit — a hash collision can never alias two
        // distinct formulas.
        using SortTable = kosha::core::FlatHashStorage<std::uint64_t, const SortImpl*>;
        using TermTable = kosha::core::FlatHashStorage<std::uint64_t, const TermImpl*>;

        template <typename V>
        using FlatMap = kosha::FlatHashStorage<std::uint64_t, V>;

        // Rollback journal: records what each interning added, so rollback can
        // undo cache insertions in reverse. Only populated while a checkpoint is
        // live has no bearing — recording is unconditional and cheap (one push).
        enum class journal_kind : std::uint8_t { term, sort, symbol };
        struct journal_entry {
            std::uint64_t hash;
            journal_kind kind;
        };

        ArenaPool arena_;
        SortTable sort_table_;
        TermTable term_table_;
        std::uint32_t next_node_id_ = 1; // 0 reserved as "unassigned"

        FlatMap<std::string> symbols_;
        FlatMap<bv_value> bv_literals_;
        FlatMap<std::int64_t> int_literals_;
        FlatMap<rational> real_literals_;

        std::vector<journal_entry> journal_;
        std::size_t checkpoint_depth_ = 0; // journal only records while a checkpoint is live

        // Journal an interning only while a checkpoint is live. On the common
        // non-incremental path (no checkpoint) interned nodes are permanent, so
        // recording them would be a pure leak.
        void journal_term(std::uint64_t h) {
            if (checkpoint_depth_ > 0) journal_.push_back({h, journal_kind::term});
        }
        void journal_sort(std::uint64_t h) {
            if (checkpoint_depth_ > 0) journal_.push_back({h, journal_kind::sort});
        }
        void journal_symbol(std::uint64_t h) {
            if (checkpoint_depth_ > 0) journal_.push_back({h, journal_kind::symbol});
        }

        [[nodiscard]] Term make_quantifier(Op q, std::span<const Term> bound_vars, Term body) {
            // Children layout: [bound_vars..., body]. Body determines nothing about
            // sort — a quantified formula is Bool.
            containers::dynamic::SmallVector<Term, 8 * sizeof(Term)> ch;
            ch.reserve(bound_vars.size() + 1);
            for (const Term& v : bound_vars) ch.push_back(v);
            ch.push_back(body);
            Context& c = *this;
            return make_term(q, c.bool_sort(),
                             std::span<const Term>{ch.data(), ch.size()});
        }

        // ------------------------------------------------------------------
        // Hash functions
        // ------------------------------------------------------------------

        [[nodiscard]] static std::uint64_t sort_hash(SortKind kind,
                                                     std::span<const Sort> params,
                                                     std::uint32_t scalar) noexcept {
            std::uint64_t h = detail::mix64(static_cast<std::uint64_t>(kind) ^ 0xDEAD5041ULL);
            h = detail::hash_combine(h, static_cast<std::uint64_t>(scalar));
            for (const Sort& p : params)
                h = detail::hash_combine(h, p.hash_);
            return h == 0 ? 1 : h; // zero is invalid
        }

        [[nodiscard]] static std::uint64_t term_hash(Op op, Sort sort,
                                                     std::span<const Term> children,
                                                     std::uint64_t payload_hash) noexcept {
            std::uint64_t h = detail::mix64(static_cast<std::uint64_t>(static_cast<std::uint16_t>(op)) ^ 0xFACE7A42ULL);
            h = detail::hash_combine(h, sort.hash_);
            h = detail::hash_combine(h, payload_hash);
            for (const Term& c : children)
                h = detail::hash_combine(h, c.hash_);
            return h == 0 ? 1 : h;
        }

        [[nodiscard]] static std::uint64_t symbol_payload_hash(std::string_view name) noexcept {
            std::uint64_t h = 0xCBF29CE484222325ULL;
            for (char c : name) h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001B3ULL;
            return h == 0 ? 1 : h;
        }

        // ------------------------------------------------------------------
        // Structural equality (for hash-collision safety)
        // ------------------------------------------------------------------

        [[nodiscard]] static bool sort_equal(const SortImpl* impl, SortKind kind,
                                             std::span<const Sort> params, std::uint32_t scalar) noexcept {
            if (impl->kind != kind || impl->scalar_param != scalar) return false;
            if (impl->param_count != params.size()) return false;
            const auto* stored = reinterpret_cast<const Sort*>(impl + 1);
            for (std::size_t i = 0; i < params.size(); ++i)
                if (stored[i] != params[i]) return false;
            return true;
        }

        [[nodiscard]] static bool term_equal(const TermImpl* impl, Op op, Sort sort,
                                             std::span<const Term> children,
                                             std::uint64_t payload_hash) noexcept {
            if (impl->op != op || impl->sort_ != sort) return false;
            if (impl->payload_hash != payload_hash) return false;
            if (impl->child_count != children.size()) return false;
            const auto* stored = reinterpret_cast<const Term*>(impl + 1);
            for (std::size_t i = 0; i < children.size(); ++i)
                if (stored[i].ptr_ != children[i].ptr_) return false;
            return true;
        }
    };

    // =========================================================================
    // Term operator implementations (recover Context from ptr_->ctx_)
    // =========================================================================

    inline Term Term::operator&&(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::And, c.bool_sort(), children);
    }

    inline Term Term::operator||(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Or, c.bool_sort(), children);
    }

    inline Term Term::operator!() const {
        assert(ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[1] = {*this};
        return c.make_term(Op::Not, c.bool_sort(), children);
    }

    inline Term Term::operator==(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Eq, c.bool_sort(), children);
    }

    inline Term Term::operator!=(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Distinct, c.bool_sort(), children);
    }

    inline Term Term::operator+(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Add, ptr_->sort_, children);
    }

    inline Term Term::operator-(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Sub, ptr_->sort_, children);
    }

    inline Term Term::operator*(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Mul, ptr_->sort_, children);
    }

    inline Term Term::operator/(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Div, ptr_->sort_, children);
    }

    inline Term Term::operator%(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Mod, ptr_->sort_, children);
    }

    inline Term Term::implies(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Implies, c.bool_sort(), children);
    }

    inline Term Term::operator<(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Lt, c.bool_sort(), children);
    }

    inline Term Term::operator<=(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Le, c.bool_sort(), children);
    }

    inline Term Term::operator>(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Gt, c.bool_sort(), children);
    }

    inline Term Term::operator>=(Term rhs) const {
        assert(ptr_ && rhs.ptr_);
        Context& c = *ptr_->ctx_;
        const Term children[2] = {*this, rhs};
        return c.make_term(Op::Ge, c.bool_sort(), children);
    }

    // =========================================================================
    // get_op_info — runtime op metadata (declared in term.hpp)
    // =========================================================================

    template <Op O>
    [[nodiscard]] constexpr op_info make_op_info() noexcept {
        return {
            op_descriptor<O>::stable_id,
            op_descriptor<O>::symbol,
            op_descriptor<O>::arity,
            op_descriptor<O>::is_commutative,
            op_descriptor<O>::theory_bits
        };
    }

    [[nodiscard]] inline op_info get_op_info(Op o) noexcept {
        switch (o) {
            case Op::Lit: return make_op_info<Op::Lit>();
            case Op::Sym: return make_op_info<Op::Sym>();
            case Op::Forall: return make_op_info<Op::Forall>();
            case Op::Exists: return make_op_info<Op::Exists>();
            case Op::True: return make_op_info<Op::True>();
            case Op::False: return make_op_info<Op::False>();
            case Op::Not: return make_op_info<Op::Not>();
            case Op::And: return make_op_info<Op::And>();
            case Op::Or: return make_op_info<Op::Or>();
            case Op::Xor: return make_op_info<Op::Xor>();
            case Op::Implies: return make_op_info<Op::Implies>();
            case Op::Ite: return make_op_info<Op::Ite>();
            case Op::Eq: return make_op_info<Op::Eq>();
            case Op::Distinct: return make_op_info<Op::Distinct>();
            case Op::Add: return make_op_info<Op::Add>();
            case Op::Sub: return make_op_info<Op::Sub>();
            case Op::Mul: return make_op_info<Op::Mul>();
            case Op::Div: return make_op_info<Op::Div>();
            case Op::Mod: return make_op_info<Op::Mod>();
            case Op::Neg: return make_op_info<Op::Neg>();
            case Op::Lt: return make_op_info<Op::Lt>();
            case Op::Le: return make_op_info<Op::Le>();
            case Op::Gt: return make_op_info<Op::Gt>();
            case Op::Ge: return make_op_info<Op::Ge>();
            case Op::BvAdd: return make_op_info<Op::BvAdd>();
            case Op::BvSub: return make_op_info<Op::BvSub>();
            case Op::BvMul: return make_op_info<Op::BvMul>();
            case Op::BvUdiv: return make_op_info<Op::BvUdiv>();
            case Op::BvSdiv: return make_op_info<Op::BvSdiv>();
            case Op::BvUrem: return make_op_info<Op::BvUrem>();
            case Op::BvSrem: return make_op_info<Op::BvSrem>();
            case Op::BvNeg: return make_op_info<Op::BvNeg>();
            case Op::BvAnd: return make_op_info<Op::BvAnd>();
            case Op::BvOr: return make_op_info<Op::BvOr>();
            case Op::BvXor: return make_op_info<Op::BvXor>();
            case Op::BvNot: return make_op_info<Op::BvNot>();
            case Op::BvShl: return make_op_info<Op::BvShl>();
            case Op::BvLshr: return make_op_info<Op::BvLshr>();
            case Op::BvAshr: return make_op_info<Op::BvAshr>();
            case Op::BvUlt: return make_op_info<Op::BvUlt>();
            case Op::BvUle: return make_op_info<Op::BvUle>();
            case Op::BvSlt: return make_op_info<Op::BvSlt>();
            case Op::BvSle: return make_op_info<Op::BvSle>();
            case Op::BvConcat: return make_op_info<Op::BvConcat>();
            case Op::BvExtract: return make_op_info<Op::BvExtract>();
            case Op::BvZeroExt: return make_op_info<Op::BvZeroExt>();
            case Op::BvSignExt: return make_op_info<Op::BvSignExt>();
            case Op::Select: return make_op_info<Op::Select>();
            case Op::Store: return make_op_info<Op::Store>();
            case Op::Apply: return make_op_info<Op::Apply>();
            default: {
                const auto id = static_cast<std::uint16_t>(o);
                return {id, "?", -1, false, theory_bit(theory_family::core)};
            }
        }
    }
} // namespace tarka
