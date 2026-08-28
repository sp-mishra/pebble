#pragma once

// vakya/constraints.hpp — Constraint algebra, constraint graph, solver interfaces.
//
// C++23, header-only, no virtual, no macros. Opt-in; not pulled by vakya.hpp.
// Namespace: vakya::types  (extends types.hpp + unification.hpp)
//
// constraint: POD record {kind, operands, payload}; identity via slot_map handle.
// constraint_graph: LiteGraph + LiteGraphAlgorithms for SCC/topo dependency analysis.
// constraint_solver<S>: concept for solver backends.
// composite_solver<Solvers...>: variadic [[no_unique_address]] fold, zero erasure.
// unification_solver: adapter wrapping unify() as a constraint_solver.
// any_solver: type-erased boundary for tooling/plugin paths.

#include "vakya/unification.hpp"
#include "containers/graph/LiteGraph.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace vakya::types {
    // ============================================================================
    // constraint_kind — open enum, extension band >= kExtensionIdBase
    // ============================================================================

    enum class constraint_kind : std::uint32_t {
        same_type = 0,
        convertible = 1,
        subtype = 2,
        implements = 3, // implements(T, Trait)
        same_rank = 4,
        broadcastable = 5,
        compatible = 6,
        requires_cap = 7, // requires_cap(T, Capability)
        user = 8, // user-defined, extension band
        // Extension ids start at kExtensionIdBase (1000)
    };

    inline constexpr std::uint32_t kConstraintKindExtensionBase = emit::kExtensionIdBase;

    // ============================================================================
    // constraint — POD operand record
    // ============================================================================

    struct constraint_tag {};

    using constraint_ref = containers::generational_handle<constraint_tag, std::uint32_t>;

    struct constraint {
        constraint_kind kind = constraint_kind::same_type;
        // Operand types (usually 1 or 2 type_refs)
        containers::dynamic::SmallVector<type_ref, 4> operands{};
        // Optional string payload for trait/capability names (interned via InternPool in practice)
        std::uint64_t trait_name_hash = 0; // for implements / requires_cap
        std::uint64_t payload = 0; // generic user payload
        // Handle back to the originating constraint in a constraint_store (optional).
        // Set by callers that maintain a constraint_store; left default otherwise.
        constraint_ref source{};

        [[nodiscard]] bool operator==(const constraint& o) const noexcept {
            if (kind != o.kind) return false;
            if (trait_name_hash != o.trait_name_hash) return false;
            if (payload != o.payload) return false;
            if (operands.size() != o.operands.size()) return false;
            for (std::size_t i = 0; i < operands.size(); ++i) {
                if (operands[i] != o.operands[i]) return false;
            }
            return true;
        }
    };

    // ============================================================================
    // constraint_store — slot_map of constraints
    // ============================================================================

    using constraint_store = containers::slot_map<constraint, constraint_ref>;

    // ============================================================================
    // solve_status — result classification
    // ============================================================================

    enum class solve_status : std::uint8_t {
        solved,
        unsatisfiable,
        ambiguous,
        deferred,
    };

    // Join two statuses: unsatisfiable > ambiguous > deferred > solved
    [[nodiscard]] inline solve_status join_status(solve_status a, solve_status b) noexcept {
        return static_cast<solve_status>(std::max(
            static_cast<std::uint8_t>(a),
            static_cast<std::uint8_t>(b)));
    }

    // ============================================================================
    // diagnostic — lightweight solver diagnostic (full model in diagnostics.hpp)
    // ============================================================================

    struct solver_diagnostic {
        std::string message;
        constraint_ref source{};
    };

    // ============================================================================
    // solve_result
    // ============================================================================

    struct solve_result {
        solve_status status = solve_status::solved;
        subst_delta substitution;
        std::vector<solver_diagnostic> diagnostics;
    };

    // ============================================================================
    // solve_context
    // ============================================================================

    struct solve_context {
        type_arena* arena = nullptr;
        substitution* subst = nullptr;
    };

    // ============================================================================
    // constraint_solver<S> concept — static polymorphism, no vtable
    // ============================================================================

    template <class S>
    concept constraint_solver =
        requires(S& s, std::span<const constraint> batch, solve_context ctx,
                 constraint_kind k) {
            { s.solve(batch, ctx) } -> std::same_as<solve_result>;
            { s.handles(k) } -> std::same_as<bool>;
        };

    // ============================================================================
    // composite_solver<Solvers...> — variadic [[no_unique_address]] fold
    // Routes each constraint to the first solver whose handles(kind) is true.
    // Threads the substitution left-to-right across solvers.
    // ============================================================================

    namespace detail {
        template <class... Solvers>
        struct solver_tuple_base {
            std::tuple<Solvers...> solvers;
            explicit solver_tuple_base(Solvers... s) : solvers(std::forward<Solvers>(s)...) {}
        };

        template <std::size_t I, class... Solvers>
        bool dispatch_constraint(std::tuple<Solvers...>& solvers,
                                 const constraint& c,
                                 solve_context& ctx,
                                 solve_result& accum) {
            if constexpr (I < sizeof...(Solvers)) {
                auto& s = std::get < I > (solvers);
                if (s.handles(c.kind)) {
                    const constraint arr[1] = {c};
                    solve_result r = s.solve(std::span<const constraint>(arr, 1), ctx);
                    // Thread substitution
                    for (auto& br : r.substitution) accum.substitution.push_back(br);
                    accum.diagnostics.insert(
                        accum.diagnostics.end(),
                        r.diagnostics.begin(),
                        r.diagnostics.end());
                    accum.status = join_status(accum.status, r.status);
                    return true;
                }
                return dispatch_constraint<I + 1>(solvers, c, ctx, accum);
            }
            else {
                // No solver handles this kind: treat as deferred
                accum.status = join_status(accum.status, solve_status::deferred);
                return false;
            }
        }
    } // namespace detail

    template <constraint_solver... Solvers>
    class composite_solver {
    public:
        explicit composite_solver(Solvers... s) : solvers_(std::forward<Solvers>(s)...) {}

        [[nodiscard]] solve_result solve(std::span<const constraint> batch,
                                         solve_context ctx) {
            solve_result accum;
            for (const constraint& c : batch) {
                if (accum.status == solve_status::unsatisfiable) break;
                detail::dispatch_constraint<0>(solvers_, c, ctx, accum);
            }
            return accum;
        }

        [[nodiscard]] bool handles(constraint_kind k) {
            return handles_impl(k, std::index_sequence_for < Solvers...>{});
        }

    private:
        template <std::size_t... Is>
        bool handles_impl(constraint_kind k, std::index_sequence<Is...>) {
            return (std::get < Is > (solvers_).handles(k) ||
            ...
            )
            ;
        }

        std::tuple<Solvers...> solvers_;
    };

    // ============================================================================
    // unification_solver — adapter wrapping unify() as a constraint_solver
    // Handles: same_type, convertible, subtype (invariant subtype = same_type)
    // ============================================================================

    class unification_solver {
    public:
        [[nodiscard]] bool handles(constraint_kind k) const noexcept {
            return k == constraint_kind::same_type ||
                k == constraint_kind::convertible ||
                k == constraint_kind::subtype;
        }

        [[nodiscard]] solve_result solve(std::span<const constraint> batch,
                                         solve_context ctx) {
            solve_result result;
            if (!ctx.arena || !ctx.subst) {
                result.status = solve_status::deferred;
                return result;
            }

            for (const constraint& c : batch) {
                if (!handles(c.kind)) continue;
                if (c.operands.size() < 2) continue;

                auto r = unify(c.operands[0], c.operands[1], *ctx.subst, *ctx.arena);
                if (!r) {
                    result.status = solve_status::unsatisfiable;
                    result.diagnostics.push_back(solver_diagnostic{
                        "unification failed: " + unify_error_message(r.error()),
                        constraint_ref{}
                    });
                    break;
                }
                for (auto& br : *r) result.substitution.push_back(br);
            }
            return result;
        }

    private:
        [[nodiscard]] static std::string unify_error_message(const unify_error& e) {
            switch (e.kind) {
            case unify_error_kind::constructor_clash: return "constructor clash";
            case unify_error_kind::arity_mismatch: return "arity mismatch";
            case unify_error_kind::infinite_type: return "infinite type (occurs check)";
            case unify_error_kind::kind_mismatch: return "kind mismatch";
            }
            return "unknown unify error";
        }
    };

    static_assert(constraint_solver<unification_solver>);

    // ============================================================================
    // constraint_graph — LiteGraph wrapper for dependency/ordering constraints
    // Nodes = type-variable indices (uint32_t as node payload)
    // Edges = constraints (constraint_ref as edge payload)
    // ============================================================================

    using constraint_graph_t = litegraph::Graph<std::uint32_t, constraint_ref, litegraph::Directed>;
    using constraint_graph_node_id = litegraph::NodeId;
    using constraint_graph_edge_id = litegraph::EdgeId;

    // ============================================================================
    // any_solver — type-erased boundary for tooling/plugin paths only
    // (NOT for the hot path; use composite_solver<...> there)
    // ============================================================================

    class any_solver {
    public:
        template <constraint_solver S>
        explicit any_solver(S s)
            : solve_fn_([s = std::move(s)](std::span<const constraint> batch,
                                           solve_context ctx) mutable {
                  return s.solve(batch, ctx);
              }),
              handles_fn_([s_ptr = std::make_shared<S>(std::move(s))](constraint_kind k) {
                  return s_ptr->handles(k);
              }) {}

        [[nodiscard]] solve_result solve(std::span<const constraint> batch,
                                         solve_context ctx) {
            return solve_fn_(batch, ctx);
        }

        [[nodiscard]] bool handles(constraint_kind k) { return handles_fn_(k); }

    private:
        std::function<solve_result(std::span<const constraint>, solve_context)> solve_fn_;
        std::function<bool(constraint_kind)> handles_fn_;
    };
} // namespace vakya::types
