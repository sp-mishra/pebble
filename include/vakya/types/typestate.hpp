#pragma once

// =============================================================================
// vakya/types/typestate.hpp — affine typestate protocols (opt-in)
//
// C++23, header-only, no virtual, no macros.
// Namespace: vakya::types
//
// A protocol is a finite state machine over a resource's lifecycle:
//   File: Closed --open--> Open --read/write--> Open --close--> Closed
// Each method is legal only from certain states and moves the resource to a new
// state; a `consumes` transition makes the old handle affine (unusable after).
//
// protocol_descriptor is registered in a descriptor_registry so protocols are
// enumerable and looked up by stable_id — the same registry pattern as effects /
// capabilities. transitions are stored contiguously and referenced by [begin,end).
//
// check_transition(proto, from, method) → next state, or a rejection. This is the
// fact the rule solver consults; a rejected transition (illegal method for the
// current state, or use-after-consume) is a type error surfaced at reasoning time.
//
// affine_scope is an RAII tracker of live (region-carried) resource handles within
// a lexical scope; on scope exit it can report handles left in a non-terminal
// state (a leaked resource). It reuses region_ref as the resource identity.
//
// Dependencies: vakya/types/opt_handles.hpp, vakya/constraints.hpp,
//               containers/descriptor_registry.hpp
// =============================================================================

#include "vakya/types/opt_handles.hpp"
#include "vakya/constraints.hpp"
#include "containers/descriptor_registry.hpp"
#include "containers/dynamic/SmallVector.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace vakya::types {
    // ============================================================================
    // transition — a single (from, method) → (to, consumes) edge.
    // ============================================================================

    struct transition {
        typestate_id from = kNoTypestate;
        std::uint64_t method_hash = 0; // hash of the method name
        typestate_id to = kNoTypestate;
        bool consumes = false; // true: `from`-handle is affine after this edge
    };

    // ============================================================================
    // protocol_descriptor — registrable protocol metadata.
    //
    // transitions are non-owning: the descriptor points at a caller-owned array
    // (typically a constexpr static table). initial/terminal classify states.
    // ============================================================================

    enum class protocol_category : std::uint32_t { builtin = 0, extension = 1 };

    struct protocol_descriptor {
        std::uint32_t stable_id = 0;
        std::uint64_t name_hash = 0;
        protocol_category category = protocol_category::builtin;

        std::string_view symbol{};
        const transition* transitions = nullptr;
        std::uint32_t transition_count = 0;
        typestate_id initial = kNoTypestate;
        typestate_id terminal = kNoTypestate; // a "closed / released" resting state

        [[nodiscard]] std::span<const transition> edges() const noexcept {
            return {transitions, transition_count};
        }
    };

    static_assert(containers::RegistrableDescriptor<protocol_descriptor>);

    using protocol_registry = containers::descriptor_registry<protocol_descriptor>;

    // ============================================================================
    // transition_result — outcome of a check_transition query.
    // ============================================================================

    enum class transition_status : std::uint8_t {
        ok = 0,             // legal; `next` is the resulting state
        illegal_method = 1, // no edge from `from` for this method
        consumed = 2,       // the handle was already consumed (affine violation)
    };

    struct transition_result {
        transition_status status = transition_status::illegal_method;
        typestate_id next = kNoTypestate;
        bool consumes = false;
    };

    // ============================================================================
    // check_transition — resolve (proto, from, method) against the protocol table.
    // ============================================================================

    [[nodiscard]] inline transition_result
    check_transition(const protocol_descriptor& proto,
                     typestate_id from,
                     std::uint64_t method_hash) noexcept {
        for (const transition& t : proto.edges()) {
            if (t.from == from && t.method_hash == method_hash) {
                return transition_result{transition_status::ok, t.to, t.consumes};
            }
        }
        return transition_result{transition_status::illegal_method, from, false};
    }

    // ============================================================================
    // register_protocol — publish a protocol from a caller-owned transition table.
    // ============================================================================

    inline containers::descriptor_handle
    register_protocol(protocol_registry& reg,
                      std::uint32_t stable_id,
                      std::string_view name,
                      std::span<const transition> edges,
                      typestate_id initial,
                      typestate_id terminal,
                      protocol_category cat = protocol_category::builtin) {
        protocol_descriptor d;
        d.stable_id = stable_id;
        d.name_hash = containers::desc_name_hash(name);
        d.category = cat;
        d.symbol = name;
        d.transitions = edges.data();
        d.transition_count = static_cast<std::uint32_t>(edges.size());
        d.initial = initial;
        d.terminal = terminal;
        return reg.register_desc(d);
    }

    // ============================================================================
    // kTransitionKind — ext-band constraint "method legal from state" (rule solver).
    // extension band +23. The payload packs (from << 32 | method_lo) is insufficient for a
    // full obligation; typestate checking runs through check_transition directly at
    // analysis time. The kind exists so a residual (dynamic-state) obligation can be
    // routed to the SMT band when the state is not statically known.
    // ============================================================================

    inline constexpr constraint_kind kTransitionKind =
        static_cast<constraint_kind>(kConstraintKindExtensionBase + 23);

    // ============================================================================
    // affine_scope — RAII tracker of live resource handles in a lexical scope.
    //
    // Each tracked resource is (region_ref identity, current typestate_id). track()
    // registers a resource; advance() applies a checked transition (updating state
    // and marking consumed handles); leaked() reports resources not in the terminal
    // state at scope end. Zero allocation until the first track() (SmallVector inline
    // storage). No destructor side effects — the consumer decides what to do with
    // leaked(); Vakya only reports the neutral fact.
    // ============================================================================

    class affine_scope {
    public:
        struct tracked {
            region_ref resource{};
            typestate_id state = kNoTypestate;
            bool consumed = false;
        };

        void track(region_ref resource, typestate_id initial) {
            live_.push_back(tracked{resource, initial, false});
        }

        // Apply a transition to a tracked resource. Returns the transition_result;
        // updates the tracked state on success, flags consumed handles.
        [[nodiscard]] transition_result
        advance(const protocol_descriptor& proto, region_ref resource,
                std::uint64_t method_hash) {
            for (tracked& t : live_) {
                if (t.resource != resource) continue;
                if (t.consumed) {
                    return transition_result{transition_status::consumed, t.state, false};
                }
                const transition_result tr = check_transition(proto, t.state, method_hash);
                if (tr.status == transition_status::ok) {
                    t.state = tr.next;
                    if (tr.consumes) t.consumed = true;
                }
                return tr;
            }
            // Untracked resource: treat as illegal (unknown state).
            return transition_result{transition_status::illegal_method, kNoTypestate, false};
        }

        // Resources not in the terminal state (and not consumed) at scope end.
        [[nodiscard]] containers::dynamic::SmallVector<region_ref, 4>
        leaked(typestate_id terminal) const {
            containers::dynamic::SmallVector<region_ref, 4> out;
            for (const tracked& t : live_) {
                if (!t.consumed && t.state != terminal) out.push_back(t.resource);
            }
            return out;
        }

        [[nodiscard]] std::span<const tracked> resources() const noexcept {
            return {live_.data(), live_.size()};
        }

    private:
        containers::dynamic::SmallVector<tracked, 4> live_;
    };
} // namespace vakya::types
